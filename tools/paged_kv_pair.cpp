// Production-library C6 helper: piece-wise SSB5 prefill and in-process decode pairs.
// Linked only to superslm. The Python driver owns manifests, locks and atomic records.
#if defined(SUPERSLM_ENABLE_KV_PAGES_TEST_SEAMS) || defined(SUPERSLM_ENABLE_BAD_ALLOC_INJECTION) || \
    defined(SUPERSLM_ENABLE_MATVEC_TEST_SEAMS) || defined(SUPERSLM_ENABLE_MATMUL_DISPATCH_INSTRUMENT) || \
    defined(SUPERSLM_ENABLE_GPU_CHUNK_DISPATCH_INSTRUMENT) || \
    defined(SUPERSLM_ENABLE_GPU_BENCH_PRE_BATCHING) || defined(SUPERSLM_ENABLE_GPU_API_FAILURE_INJECTION) || \
    defined(SUPERSLM_GPU_ALLOC_FAULT_INJECTION) || defined(SUPERSLM_FORCE_SCALAR_MATMUL)
#error A C6 production pair must not carry a seam or injection definition.
#endif
#include "pkv_budget_c_helpers.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace pkv;
using namespace pkv::bc;
using clock_type = std::chrono::steady_clock;

void Require(bool okay, const std::string& why) { if (!okay || State().failures) throw std::runtime_error(why); }
int Number(const char* s) { size_t at=0; int v=std::stoi(s,&at); Require(at==std::string(s).size(),"invalid integer"); return v; }
uint32_t Seed(const char* s) { Require(s[0]!='-',"seed out of range"); size_t at=0;
	const auto value=std::stoull(s,&at);Require(at==std::string(s).size()&&value<=std::numeric_limits<uint32_t>::max(),"seed out of range");
	return static_cast<uint32_t>(value); }
const Fixture& Model(const char* file,int cap,int layers,int heads,int dim) {
	const Fixture& fx=GetArtifact(file,Geometry{static_cast<uint32_t>(layers),static_cast<uint32_t>(heads),static_cast<uint32_t>(dim),cap});
	Require(fx.ok,"model did not load");return fx;
}
std::vector<uint8_t> Blob(const char* path) {
	std::vector<uint8_t> bytes;Require(ReadAll(path,&bytes)&&!bytes.empty(),std::string("missing blob: ")+path);return bytes;
}
void Write(const char* path,const std::vector<uint8_t>& bytes) {
	std::ofstream out(path,std::ios::binary|std::ios::trunc);Require(bool(out),"cannot open blob output");
	out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));Require(bool(out),"blob write failed");
}
void Prefill(int argc,char** argv) {
	// prefill MODEL CAP LAYERS HEADS DIM WIDTH START COUNT INPUT|- OUTPUT
	Require(argc==12,"prefill argument count");
	const Fixture& fx=Model(argv[2],Number(argv[3]),Number(argv[4]),Number(argv[5]),Number(argv[6]));
	const int width=Number(argv[7]),start=Number(argv[8]),count=Number(argv[9]);
	Require(width>512&&start>=0&&count>0&&count<=4096&&start+count<=width-512,"piece outside prompt or over 4096");
	LegacyPool pool(fx.model,1);Require(pool.status==SSLM_OK,"legacy pool failed");sslm_seq seq=nullptr;
	if(std::string(argv[10])=="-") Require(start==0&&sslm_seq_create(fx.model,&pool.pool,&seq)==SSLM_OK,"new sequence failed");
	else {auto blob=Blob(argv[10]);Require(sslm_seq_restore(fx.model,&pool.pool,blob.data(),blob.size(),&seq)==SSLM_OK,"restore previous piece failed");}
	const auto stream=Stream(79,width-512,kVocab);
	const std::vector<int32_t> part(stream.begin()+start,stream.begin()+start+count);
	Require(PrefillAll(fx.model,seq,part,kChunk)==SSLM_OK,"production prefill failed");
	Write(argv[11],Save(seq));Require(State().failures==0,"save failed");sslm_seq_release(seq);
	std::cout<<"PREFILL_OK start="<<start<<" count="<<count<<" model_sha="<<fx.sha<<"\n";
}
struct Arm {
	const Fixture& fx;std::unique_ptr<LegacyPool> legacy;std::unique_ptr<PagePool> page;
	sslm_prefix world=nullptr,persona=nullptr;sslm_seq seq=nullptr;
	explicit Arm(const Fixture& f):fx(f){}
	sslm_kv_pool* Pool() {return legacy?&legacy->pool:&page->pool;}
	~Arm(){if(seq)sslm_seq_release(seq);if(persona)sslm_prefix_release(persona);if(world)sslm_prefix_release(world);}
	void Restore(const char* path){legacy=std::make_unique<LegacyPool>(fx.model,1);Require(legacy->status==SSLM_OK,"pool failed");
		auto blob=Blob(path);Require(IsMagic(blob,"SSB5"),"prompt blob is not SSB5");
		Require(sslm_seq_restore(fx.model,Pool(),blob.data(),blob.size(),&seq)==SSLM_OK,"prompt restore failed");}
	void Shared(){page=std::make_unique<PagePool>(fx.model,static_cast<uint32_t>(fx.R(1000)+fx.R(200)+fx.R(512)+8));
		Require(page->status==SSLM_OK,"shared pool failed");
		Require(sslm_prefix_begin_budgeted(fx.model,Pool(),1000,&world)==SSLM_OK,"world begin failed");
		Require(PrefixPrefillAll(fx.model,world,WorldTokens(),kChunk)==SSLM_OK,"world prefill failed");
		Require(sslm_prefix_freeze(world)==SSLM_OK,"world freeze failed");
		Require(sslm_prefix_begin_from(world,200,&persona)==SSLM_OK,"persona begin failed");
		Require(PrefixPrefillAll(fx.model,persona,PersonaTokens(0),kChunk)==SSLM_OK,"persona prefill failed");
		Require(sslm_prefix_freeze(persona)==SSLM_OK,"persona freeze failed");
		Require(sslm_seq_create_budgeted(fx.model,Pool(),512,&seq)==SSLM_OK,"budget sequence failed");
		Require(sslm_seq_adopt_prefix(seq,persona)==SSLM_OK,"persona adopt failed");}
	void Contiguous(){legacy=std::make_unique<LegacyPool>(fx.model,1);Require(legacy->status==SSLM_OK,"contiguous pool failed");
		Require(sslm_seq_create(fx.model,Pool(),&seq)==SSLM_OK,"contiguous sequence failed");
		auto prompt=WorldTokens();auto persona_tokens=PersonaTokens(0);prompt.insert(prompt.end(),persona_tokens.begin(),persona_tokens.end());
		Require(PrefillAll(fx.model,seq,prompt,kChunk)==SSLM_OK,"contiguous prefill failed");}
	int32_t Ready(){sslm_status status=SSLM_OK;int32_t token=Token(fx.model,seq,&status);Require(status==SSLM_OK&&token>=0,"ready token failed");return token;}
	std::pair<double,int32_t> Decode(bool mutant=false){sslm_decode_params p{};p.layer_budget=static_cast<int32_t>(fx.geo.layers);sslm_seq batch[1]={seq};int32_t token=-1;
		const auto begin=clock_type::now();const sslm_status status=sslm_decode_step(fx.model,batch,1,&p,nullptr,&token);
		if(mutant){const double own=std::chrono::duration<double,std::nano>(clock_type::now()-begin).count();
			const auto until=clock_type::now()+std::chrono::nanoseconds(static_cast<int64_t>(own*0.1765));
			while(clock_type::now()<until){} }
		const double ns=std::chrono::duration<double,std::nano>(clock_type::now()-begin).count();
		Require(status==SSLM_OK&&token>=0,"one-call token decode failed");return {ns,token};}
};
void Pairs(int argc,char** argv,bool shared){
	// pair79 A B CAP_A CAP_B LAYERS HEADS DIM BLOB_A BLOB_B PAIRS SEED OUT
	// pair79b MODEL CAP LAYERS HEADS DIM PAIRS SEED OUT
	Require(argc==(shared?10:14),"pair argument count");
	const Fixture* fa=nullptr;const Fixture* fb=nullptr;int pairs=0;uint32_t seed=0;const char* output=nullptr;
	if(shared){fa=&Model(argv[2],Number(argv[3]),Number(argv[4]),Number(argv[5]),Number(argv[6]));fb=fa;
		pairs=Number(argv[7]);seed=Seed(argv[8]);output=argv[9];}
	else {fa=&Model(argv[2],Number(argv[4]),Number(argv[6]),Number(argv[7]),Number(argv[8]));
		fb=&Model(argv[3],Number(argv[5]),Number(argv[6]),Number(argv[7]),Number(argv[8]));
		pairs=Number(argv[11]);seed=Seed(argv[12]);output=argv[13];}
	Require(pairs>0&&pairs<=508,"pair count must be 1..508");
	Arm a(*fa),b(*fb);
	if(shared){a.Shared();b.Contiguous();}else{a.Restore(argv[9]);b.Restore(argv[10]);}
	Require(a.Ready()==b.Ready(),"ready tokens differ");
	for(int i=0;i<4;++i)Require(a.Decode().second==b.Decode().second,"warmup tokens differ");
	std::ofstream out(output,std::ios::binary|std::ios::trunc);Require(bool(out),"pair output cannot open");
	std::mt19937 random(seed);
	const char* mutant=std::getenv("SUPERSLM_PAGED_KV_79_SLOW_ARM");
	const bool slow_a=mutant&&std::string(mutant)=="A",slow_b=mutant&&std::string(mutant)=="B";
	for(int i=0;i<pairs;++i){std::pair<double,int32_t> x,y;const bool first=(random()&1)!=0;
		if(first){x=a.Decode(slow_a);y=b.Decode(slow_b);}else{y=b.Decode(slow_b);x=a.Decode(slow_a);}
		Require(x.second==y.second,"token-stream mismatch at pair "+std::to_string(i));
		out<<i<<','<<(first?"A":"B")<<','<<x.first<<','<<y.first<<','<<x.second<<','<<y.second<<'\n';
		Require(bool(out),"pair output write failed");}
	out.flush();Require(bool(out),"pair output flush failed");
	std::cout<<"PAIR_OK kind="<<(shared?"79b":"79")<<" pairs="<<pairs<<" model_a="<<fa->sha<<" model_b="<<fb->sha<<"\n";
}
}
int main(int argc,char** argv){
	try{Require(argc>=2,"mode required");const std::string mode=argv[1];
		if(mode=="identity") {Require(argc==2,"identity argument count");
#if defined(PKV_C6_IDENTITY_PROBE)
			std::cout<<"PKV_PAIR_PRODUCTION_V2 defs=PKV_C6_IDENTITY_PROBE\n";
#else
			std::cout<<"PKV_PAIR_PRODUCTION_V2 defs=none\n";
#endif
		}
		else if(mode=="prefill")Prefill(argc,argv);else if(mode=="pair79")Pairs(argc,argv,false);else if(mode=="pair79b")Pairs(argc,argv,true);else throw std::runtime_error("unknown mode");
		return 0;
	}catch(const std::exception& error){std::cerr<<"INFRASTRUCTURE: "<<error.what()<<'\n';return 40;}
}
