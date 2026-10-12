// C6 7.4: paired calls at two caps on one model; fixed timer vitality precedes grading.
#include "pkv_budget_c_helpers.h"
#include "superslm/model.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define PKV_74_X86 1
#include <immintrin.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#endif
#else
#define PKV_74_X86 0
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace {
using namespace pkv;
using namespace pkv::bc;
constexpr int kPairs = 205;
enum class Verdict { Pass, Fail, NoResult };
[[noreturn]] void Refuse(int code,const char* why);
std::string locked74_path;
void Lock74(const char* path=nullptr) {
#if defined(_WIN32)
	if(!path){CreateDirectoryA("D:\\_scratch\\SuperSLM",nullptr);path="D:\\_scratch\\SuperSLM\\pkv-c6-timing.lock";}
	for(int attempt=0;attempt<10;++attempt){
		HANDLE handle=CreateFileA(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(handle!=INVALID_HANDLE_VALUE){
			const auto pid=std::to_string(GetCurrentProcessId());DWORD written=0;
			const bool okay=WriteFile(handle,pid.data(),static_cast<DWORD>(pid.size()),&written,nullptr)&&written==pid.size();
			CloseHandle(handle);
			if(!okay){DeleteFileA(path);Refuse(77,"C6 timing lock owner write failed");}
			locked74_path=path;std::atexit([]{DeleteFileA(locked74_path.c_str());});return;
		}
		if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)Refuse(77,"C6 timing lock create failed");
		HANDLE old=CreateFileA(path,GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(old==INVALID_HANDLE_VALUE){Sleep(50);continue;}
		char buffer[32]{};DWORD count=0;
		const bool read=ReadFile(old,buffer,sizeof(buffer)-1,&count,nullptr)!=0;
		char* end=nullptr;errno=0;const unsigned long owner=read?std::strtoul(buffer,&end,10):0;
		if(!read||!count){CloseHandle(old);Sleep(50);continue;}
		if(buffer[0]<'0'||buffer[0]>'9'||errno==ERANGE||!owner||owner>std::numeric_limits<DWORD>::max()||end!=buffer+count){
			CloseHandle(old);Refuse(77,"C6 timing lock owner record torn");}
		HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(owner));
		if(process){const DWORD state=WaitForSingleObject(process,0);CloseHandle(process);
			if(state!=WAIT_OBJECT_0){CloseHandle(old);Refuse(77,"C6 timing lock already held");}}
		else if(GetLastError()!=ERROR_INVALID_PARAMETER){CloseHandle(old);Refuse(77,"C6 timing lock owner cannot be checked");}
		FILE_DISPOSITION_INFO disposition{TRUE};
		const bool removed=SetFileInformationByHandle(old,FileDispositionInfo,&disposition,sizeof(disposition))!=0;
		CloseHandle(old);
		if(!removed)Refuse(77,"C6 dead-owner lock cannot be reclaimed");
	}
	Refuse(77,"C6 timing lock owner record incomplete");
#else
	if(!path)path="/tmp/superslm-pkv-c6-timing.lock";
	for(int attempt=0;attempt<10;++attempt){
		const int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);
		if(fd>=0){const auto pid=std::to_string(getpid());const bool okay=write(fd,pid.data(),pid.size())==static_cast<ssize_t>(pid.size());close(fd);
			if(!okay){unlink(path);Refuse(77,"C6 timing lock owner write failed");}
			locked74_path=path;std::atexit([]{unlink(locked74_path.c_str());});return;}
		if(errno!=EEXIST)Refuse(77,"C6 timing lock create failed");
		std::ifstream old(path);std::string record;if(!(old>>record)){usleep(50000);continue;}
		char* end=nullptr;const long owner=std::strtol(record.c_str(),&end,10);
		if(owner<=0||*end)Refuse(77,"C6 timing lock owner record torn");
		if(kill(static_cast<pid_t>(owner),0)==0||errno!=ESRCH)Refuse(77,"C6 timing lock already held");
		if(unlink(path)!=0&&errno!=ENOENT)Refuse(77,"C6 dead-owner lock cannot be reclaimed");
	}
	Refuse(77,"C6 timing lock owner record incomplete");
#endif
}
void LockConstruction(){const char* path=std::getenv("PKV_DEBUNK_74_TEST_LOCK_PATH");
	if(!path||!*path)Refuse(77,"C6 lock construction path missing");Lock74(path);PKV_CHECK(true);}
void Print74Hooks(){
	std::vector<std::string> hooks;
	if(std::getenv("SUPERSLM_PAGED_KV_74_ADOPT_MUTANT"))hooks.emplace_back("adopt=1");
	if(std::getenv("SUPERSLM_PAGED_KV_74_RESET_MUTANT"))hooks.emplace_back("reset=1");
	const size_t aliases=hooks.size();
	constexpr const char* prefix="SUPERSLM_PAGED_KV_74_";
	constexpr size_t prefix_size=21;
#if defined(_WIN32)
	LPCH block=GetEnvironmentStringsA();
	if(!block)hooks.emplace_back("environment_read_failed");
	else {for(const char* entry=block;*entry;entry+=std::strlen(entry)+1)
		if(std::strncmp(entry,prefix,prefix_size)==0)hooks.emplace_back(entry);
		FreeEnvironmentStringsA(block);}
#else
	for(char** entry=environ;*entry;++entry)
		if(std::strncmp(*entry,prefix,prefix_size)==0)hooks.emplace_back(*entry);
#endif
	if(hooks.empty())return;
	std::sort(hooks.begin()+aliases,hooks.end());
	std::printf("7.4 hook_state=");
	for(size_t i=0;i<hooks.size();++i)std::printf("%s%s",i?";":"",hooks[i].c_str());
	std::printf(" publication=quarantined\n");
}
void HookConstruction(){Print74Hooks();PKV_CHECK(true);}

double Median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size()/2]; }
double Corrected(double raw,double empty) {
#ifdef PKV_74_NO_SUBTRACTION_MUTANT
	return raw;
#else
	return raw-empty;
#endif
}
bool SubtractionWitness(double low,double large,double empty) {
	return Corrected(low,empty)==low-empty && Corrected(large,empty)==large-empty;
}
bool Vitality(double low, double large, double empty) {
	const double short_net=Corrected(low,empty), long_net=Corrected(large,empty);
	return short_net>0 && long_net>0 && long_net/short_net>=8.0 && long_net/short_net<=12.0 && SubtractionWitness(low,large,empty);
}
Verdict Grade(std::vector<double> v, double* lo=nullptr, double* hi=nullptr) {
	if (v.size()!=kPairs || std::any_of(v.begin(), v.end(), [](double x){return !std::isfinite(x)||x<=0;})) return Verdict::NoResult;
	std::sort(v.begin(), v.end()); if(lo)*lo=v[85]; if(hi)*hi=v[119];
	if(v[119]<=2.0)return Verdict::Pass; if(v[85]>2.0)return Verdict::Fail; return Verdict::NoResult;
}
void Constructions() {
	std::vector<double> v(kPairs,1.0); PKV_CHECK(Grade(v)==Verdict::Pass);
	std::fill(v.begin(),v.end(),8.0); PKV_CHECK(Grade(v)==Verdict::Fail);
	std::fill(v.begin(),v.end(),4.0); PKV_CHECK(Grade(v)==Verdict::Fail);
	std::fill(v.begin(),v.end(),2.0); PKV_CHECK(Grade(v)==Verdict::Pass);
	std::fill(v.begin(),v.end(),std::nextafter(2.0,std::numeric_limits<double>::infinity())); PKV_CHECK(Grade(v)==Verdict::Fail);
	std::fill(v.begin(),v.end(),1.9); PKV_CHECK(Grade(v)==Verdict::Pass);
	std::fill(v.begin(),v.end(),2.1); PKV_CHECK(Grade(v)==Verdict::Fail);
	v.assign(kPairs,2.1);std::fill(v.begin(),v.begin()+120,2.0); PKV_CHECK(Grade(v)==Verdict::Pass);
	v.assign(kPairs,2.1);std::fill(v.begin(),v.begin()+119,2.0); PKV_CHECK(Grade(v)==Verdict::NoResult);
	v.assign(kPairs,2.1);std::fill(v.begin(),v.begin()+85,2.0); PKV_CHECK(Grade(v)==Verdict::Fail);
	v.assign(kPairs,2.1);std::fill(v.begin(),v.begin()+86,2.0); PKV_CHECK(Grade(v)==Verdict::NoResult);
	// Distinct ratios from a reproducible uniform population: sorting and both
	// decision ranks matter even though acquisition order is randomized.
	for (const auto& [start,expected,lower,upper] : {
		std::tuple<int,Verdict,double,double>{81,Verdict::Pass,1.66,2.0},
		std::tuple<int,Verdict,double,double>{115,Verdict::NoResult,2.0,2.34},
		std::tuple<int,Verdict,double,double>{116,Verdict::Fail,2.01,2.35}}) {
		v.resize(kPairs);
		for(int i=0;i<kPairs;++i) v[i]=double(start+i)/100.0;
		std::shuffle(v.begin(),v.end(),std::mt19937(7409+start));
		double lo=0,hi=0;
		PKV_CHECK(Grade(v,&lo,&hi)==expected && std::abs(lo-lower)<1e-12 && std::abs(hi-upper)<1e-12);
	}
	v.assign(kPairs-1,1.0); PKV_CHECK(Grade(v)==Verdict::NoResult);
	PKV_CHECK(Vitality(120,1020,20));
	PKV_CHECK(Vitality(120,1020,20) && !Vitality(120,2000,20) && !Vitality(10,-80,20));
	PKV_CHECK(!Vitality(120,810,20) && Vitality(120,820,20));
	PKV_CHECK(Vitality(120,1220,20) && !Vitality(120,1230,20));
	for(double cost:{20.0,40.0,80.0,200.0}) {
		PKV_CHECK(SubtractionWitness(100+cost,1000+cost,cost));
		PKV_CHECK(Vitality(100+cost,1000+cost,cost));
	}
}
[[noreturn]] void Refuse(int code,const char* why) {
	std::fprintf(stderr,"7.4 INFRASTRUCTURE %d: %s\n",code,why);
	if(State().failures){
		std::printf("[C6] 7.4/C6 RED (%d checks, %d failed)\n",State().checks,State().failures);
		std::printf("1 cells, 1 red; %d checks, %d failures\n",State().checks,State().failures);
		std::fflush(stdout);
		std::exit(1);
	}
	std::exit(code);
}

#if PKV_74_X86
struct Pin {
	bool ok=false;
#if defined(_WIN32)
	DWORD_PTR old=0;
	Pin(){DWORD_PTR p=0,s=0;if(GetProcessAffinityMask(GetCurrentProcess(),&p,&s)&&p){old=SetThreadAffinityMask(GetCurrentThread(),p&(~p+1));ok=old!=0;}}
	~Pin(){if(old)SetThreadAffinityMask(GetCurrentThread(),old);}
#else
	cpu_set_t old{};
	Pin(){if(pthread_getaffinity_np(pthread_self(),sizeof old,&old))return;for(int c=0;c<CPU_SETSIZE;++c)if(CPU_ISSET(c,&old)){cpu_set_t one;CPU_ZERO(&one);CPU_SET(c,&one);ok=pthread_setaffinity_np(pthread_self(),sizeof one,&one)==0;return;}}
	~Pin(){if(ok)pthread_setaffinity_np(pthread_self(),sizeof old,&old);}
#endif
};
bool TscInvariant(){
#ifdef PKV_74_FORCE_NO_TSC
	return false;
#endif
#if defined(_WIN32)
	int r[4]{};__cpuid(r,0x80000000);if(static_cast<unsigned>(r[0])<0x80000007u)return false;__cpuid(r,0x80000007);return (r[3]&(1<<8))!=0;
#else
	unsigned a,b,c,d;if(__get_cpuid_max(0x80000000,nullptr)<0x80000007u)return false;__get_cpuid(0x80000007,&a,&b,&c,&d);return (d&(1u<<8))!=0;
#endif
}
bool use_rdtscp=false;
bool RdtscpAvailable(){
#if defined(_WIN32)
	int r[4]{};__cpuid(r,0x80000000);if(static_cast<unsigned>(r[0])<0x80000001u)return false;__cpuid(r,0x80000001);return (r[3]&(1<<27))!=0;
#else
	unsigned a,b,c,d;if(__get_cpuid_max(0x80000000,nullptr)<0x80000001u)return false;__get_cpuid(0x80000001,&a,&b,&c,&d);return (d&(1u<<27))!=0;
#endif
}
uint64_t Tick(){_mm_lfence();unsigned aux=0;auto t=use_rdtscp?__rdtscp(&aux):__rdtsc();_mm_lfence();return t;}
uint64_t Adds(int n){volatile uint64_t x=0;for(int i=0;i<n;++i)x=x+static_cast<uint64_t>(i+1);return x;}
struct Clock {
	double empty=0, resolution=0;bool okay=false;std::vector<double> raw_empty;
	Clock(){std::vector<double> e,a,b;for(int i=0;i<21;++i){auto x=Tick(),y=Tick();e.push_back(double(y-x));
		x=Tick();(void)Adds(i%2?1000:100);y=Tick();(i%2?b:a).push_back(double(y-x));
		x=Tick();(void)Adds(i%2?100:1000);y=Tick();(i%2?a:b).push_back(double(y-x));}
		raw_empty=e;empty=Median(e);resolution=*std::min_element(e.begin(),e.end());okay=Vitality(Median(a),Median(b),empty);
		std::printf("7.4t: resolution %.0f ticks empty %.0f 100 %.0f 1000 %.0f ratio %.3f %s\n",resolution,empty,Median(a),Median(b),(Median(a)>empty)?(Median(b)-empty)/(Median(a)-empty):0,okay?"PASS":"INFRASTRUCTURE");}
};
void TimerLive(){Pin pin;PKV_CHECK(pin.ok);if(!pin.ok)return;Clock clock;PKV_CHECK(clock.okay);}
void Delay(uint64_t ticks){auto start=Tick();while(Tick()-start<ticks)_mm_pause();}
struct CallTrace {int pair;const char* holder;const char* verb;double raw;double corrected;};
struct Side {
	const Fixture& fx;std::unique_ptr<Rig> rig;sslm_prefix prefix=nullptr;sslm_seq seq=nullptr;bool ok=false;
	Side(const Fixture& f,bool budget):fx(f){if(!f.ok)return;auto own=budget?f.R(512):f.CapPages();rig=std::make_unique<Rig>(f,static_cast<uint32_t>(f.R(1000)+own+4));
		prefix=rig->Prefix(BudgetPrefix(f,rig->Pool(),PrefixTokens(1000)));sslm_seq s=nullptr;
		auto st=budget?sslm_seq_create_budgeted(f.model,rig->Pool(),512,&s):sslm_seq_create(f.model,rig->Pool(),&s);seq=rig->Seq(s);ok=st==SSLM_OK&&prefix&&seq;}
	bool Call(double empty,bool mutant_adopt,bool mutant_reset,double* adopt,double* reset,
	          uint64_t bias=0,std::vector<CallTrace>* trace=nullptr,int pair=-1,const char* holder=""){if(!ok)return false;
		auto a=Tick();auto sa=sslm_seq_adopt_prefix(seq,prefix);auto ad_done=Tick();
		if(mutant_adopt)Delay(3*(ad_done-a));if(bias)Delay(bias);auto b=Tick();
		auto sr=sslm_seq_reset(seq);auto re_done=Tick();
		if(mutant_reset)Delay(7*(re_done-b));auto c=Tick();
		ok=sa==SSLM_OK&&sr==SSLM_OK;*adopt=Corrected(double(b-a),empty);*reset=Corrected(double(c-b),empty);
		if(trace){trace->push_back({pair,holder,"adopt",double(b-a),*adopt});trace->push_back({pair,holder,"reset",double(c-b),*reset});}
		return ok;}
};
bool Same74Geometry(const superslm::SslmModelConfig& small_config,
                    const superslm::SslmModelConfig& large_config,
                    const Geometry& small_claim,const Geometry& large_claim) {
	return small_config.num_hidden_layers==large_config.num_hidden_layers&&
	       small_config.num_key_value_heads==large_config.num_key_value_heads&&
	       small_config.head_dim==large_config.head_dim&&
	       small_config.hidden_size==large_config.hidden_size&&
	       small_config.kv_block_size==large_config.kv_block_size&&
	       small_config.num_hidden_layers==small_claim.layers&&large_config.num_hidden_layers==large_claim.layers&&
	       small_config.num_key_value_heads==small_claim.kv_heads&&large_config.num_key_value_heads==large_claim.kv_heads&&
	       small_config.head_dim==small_claim.head_dim&&large_config.head_dim==large_claim.head_dim;
}
void Run74(const Fixture& low,const Fixture& large){
	if(!low.ok||!large.ok)Refuse(71,"artifact missing or unable to load");
	superslm::SslmModelView low_view,large_view;std::string error;
	if(superslm::SslmModel::Load(low.bytes.p,low.size,low_view,&error)!=superslm::SslmModelStatus::Ok||
	   superslm::SslmModel::Load(large.bytes.p,large.size,large_view,&error)!=superslm::SslmModelStatus::Ok)
		Refuse(71,"artifact config cannot be parsed");
	const auto& small_config=low_view.config;const auto& large_config=large_view.config;
	if(!Same74Geometry(small_config,large_config,low.geo,large.geo))
		Refuse(71,"geometry mismatch");
	if(sslm_kv_block_size(low.model)!=size_t{small_config.context_cap}*low.BytesPerToken()||
	   sslm_kv_block_size(large.model)!=size_t{large_config.context_cap}*large.BytesPerToken())Refuse(71,"block size mismatch");
	if(low.geo.context_cap!=4096||large.geo.context_cap!=32768)Refuse(72,"wrong cap");
	if(!TscInvariant())Refuse(73,"invariant TSC absent");Pin pin;if(!pin.ok||std::getenv("SUPERSLM_PAGED_KV_74_FORCE_PIN_FAILURE"))Refuse(73,"cannot pin timing thread");Clock clock;
	if(!clock.okay&&RdtscpAvailable()){use_rdtscp=true;clock=Clock();}
	if(std::getenv("SUPERSLM_PAGED_KV_74_FORCE_VITALITY_FAILURE"))clock.okay=false;
	if(!clock.okay)std::printf("7.4t: INFRASTRUCTURE 76 diagnostic timer vitality failed\n");
	const bool extra_ad=std::getenv("SUPERSLM_PAGED_KV_74_ADOPT_MUTANT")!=nullptr;
	const bool extra_re=std::getenv("SUPERSLM_PAGED_KV_74_RESET_MUTANT")!=nullptr;
	Print74Hooks();
	const char* order_path=std::getenv("SUPERSLM_PAGED_KV_74_ORDER_TRACE");
	const char* call_path=std::getenv("SUPERSLM_PAGED_KV_74_CALL_TRACE");
	const char* bias_value=std::getenv("SUPERSLM_PAGED_KV_74_ORDER_BIAS_TICKS");
	const uint64_t bias=bias_value?std::strtoull(bias_value,nullptr,10):0;
	std::ofstream order;
	if(order_path){order.open(order_path,std::ios::trunc);if(!order)Refuse(71,"cannot open order trace");order<<"holder,pair,first,bias_ticks\n";}
	std::vector<CallTrace> calls;
	std::printf("7.4 artifact hashes: cap4096=%s cap32768=%s\n",low.sha.c_str(),large.sha.c_str());bool unresolved=false;
	for(bool budget:{true,false}){Side a(low,budget),b(large,budget);if(!a.ok||!b.ok)Refuse(71,"holder creation failed");
		std::vector<double> base_reset;
		for(int i=0;i<10;++i){double x=0,y=0;if(!a.Call(clock.empty,0,0,&x,&y)||!b.Call(clock.empty,0,0,&x,&y))Refuse(71,"warmup failed");base_reset.push_back(y);}
		std::vector<double> ra,rb,aa,ab,adopt_ratios,reset_ratios;std::mt19937 rng(7404+static_cast<unsigned>(budget));
		for(int i=0;i<kPairs;++i){double xad=0,xre=0,yad=0,yre=0;
			const bool small_first=(rng()&1)!=0;const char* holder=budget?"budget":"whole_reserve";
			if(order)order<<holder<<','<<i<<','<<(small_first?"small":"large")<<','<<bias<<'\n';
			auto first=[&](uint64_t call_bias){return a.Call(clock.empty,0,0,&xad,&xre,call_bias,call_path?&calls:nullptr,i,holder);};
			auto second=[&](uint64_t call_bias){return b.Call(clock.empty,extra_ad,extra_re,&yad,&yre,call_bias,call_path?&calls:nullptr,i,holder);};
			if(!(small_first?(first(bias)&&second(0)):(second(bias)&&first(0))))Refuse(71,"timed verb failed");
			aa.push_back(xad);ab.push_back(yad);ra.push_back(xre);rb.push_back(yre);adopt_ratios.push_back(yad/xad);reset_ratios.push_back(yre/xre);}
		if(extra_re)std::printf("7.4 hook reset base_ticks=%.3f hooked_ticks=%.3f\n",Median(base_reset),Median(rb));
		const char* control=std::getenv("SUPERSLM_PAGED_KV_74_FLOOR_CONTROL_TICKS");
		const double floor_aa=control?std::strtod(control,nullptr):Median(aa);
		const double floor_ab=control?std::strtod(control,nullptr):Median(ab);
		const double floor_ra=control?std::strtod(control,nullptr):Median(ra);
		const double floor_rb=control?std::strtod(control,nullptr):Median(rb);
		if(std::getenv("SUPERSLM_PAGED_KV_74_FORCE_FLOOR")||floor_aa<100||floor_ab<100||floor_ra<100||floor_rb<100)Refuse(74,"corrected median under 100 ticks");
		for(auto& entry:{std::pair{"adopt",adopt_ratios},std::pair{"reset",reset_ratios}}){double lo=0,hi=0;auto verdict=Grade(entry.second,&lo,&hi);
			std::printf("7.4 %s %s r(86)=%.3f r(120)=%.3f bound=2.0 %s\n",budget?"budget":"whole_reserve",entry.first,lo,hi,verdict==Verdict::Pass?"PASS":verdict==Verdict::Fail?"FAIL":"NO RESULT");
			PKV_CHECK_MSG(verdict!=Verdict::Fail,"7.4 %s %s: ratio above 2.0",budget?"budget":"whole_reserve",entry.first);unresolved|=verdict==Verdict::NoResult;}
	}
	if(call_path){std::ofstream out(call_path,std::ios::trunc);if(!out)Refuse(71,"cannot open call trace");
		out<<"{\"raw_empty_ticks\":[";for(size_t i=0;i<clock.raw_empty.size();++i){if(i)out<<',';out<<clock.raw_empty[i];}
		out<<"],\"calls\":[";for(size_t i=0;i<calls.size();++i){if(i)out<<',';const auto& c=calls[i];
			out<<"{\"pair\":"<<c.pair<<",\"holder\":\""<<c.holder<<"\",\"verb\":\""<<c.verb<<"\",\"raw_ticks\":"<<c.raw<<",\"corrected_ticks\":"<<c.corrected<<'}';}out<<"]}\n";}
	if(unresolved||std::getenv("SUPERSLM_PAGED_KV_74_FORCE_UNRESOLVED"))Refuse(75,"sign interval did not resolve the bound");
}
#else
void Run74(const Fixture&,const Fixture&){Refuse(73,"x86 invariant TSC required");}
void TimerLive(){Refuse(73,"x86 invariant TSC required");}
#endif
// Paged-KV plan rev 16.24 (D-SLM8447, D-SLM8298): 7.4 pins both sides to one B, 64 on the box and 16 on the twin, and
// reads the pinned B back. The pin goes on a private mapping of each artifact, so no shared fixture (the other C6
// cells read the rule's B) is pinned, and the pin precedes every pool on that mapping. The pin's SSLM_OK is required
// once the T0 stub is gone (SUPERSLM_PKV_R9_STUB_PIN OFF); the read-back B is required always.
bool PinnedCopy(const Fixture& src,int64_t B,Fixture* out){
	if(!src.ok)return false;
	out->stem=src.stem;out->geo=src.geo;out->vocab=src.vocab;out->sha=src.sha;out->size=src.size;
	out->bytes.Reset(src.size);std::memcpy(out->bytes.p,src.bytes.p,src.size);
	const sslm_status st=sslm_model_map(out->bytes.p,out->size,&out->model);
	PKV_CHECK_MSG(st==SSLM_OK,"7.4: the private mapping of %s: %d",src.stem.c_str(),static_cast<int>(st));
	if(st!=SSLM_OK)return false;
	const sslm_status pin=KvPagesTestPinPagePositions(out->model,B);
#if !defined(PKV_R9_STUB_PIN)
	PKV_CHECK_MSG(pin==SSLM_OK,"7.4: the pin of %s to B = %lld is refused (%d)",src.stem.c_str(),static_cast<long long>(B),static_cast<int>(pin));
#else
	(void)pin;
#endif
	out->pinned=sslm_kv_page_positions(out->model);
	PKV_CHECK_MSG(out->pinned==B,"7.4: %s runs at B = %lld after its pin, %lld owed",src.stem.c_str(),static_cast<long long>(out->pinned),static_cast<long long>(B));
	out->ok=out->pinned==B;
	return out->ok;
}
void RunPinned74(const Fixture& low,const Fixture& large,int64_t B){
	Fixture a,b;
	if(!PinnedCopy(low,B,&a)||!PinnedCopy(large,B,&b))Refuse(71,"the 7.4 pin did not take");
	Run74(a,b);
}
void Box(){Lock74();const char* large=std::getenv("SUPERSLM_PAGED_KV_74_CAP32768_FILE");RunPinned74(GetArtifact("qwen2.5-0.5b-instruct-cap4096-aex.sslm",Geometry{24,2,64,4096}),
  GetArtifact(large?large:"qwen2.5-0.5b-instruct-cap32768-aex.sslm",Geometry{24,2,64,32768}),64);}
void Fixtures(){Lock74();const char* low=std::getenv("SUPERSLM_PAGED_KV_74_FIXTURE_LOW");const char* large=std::getenv("SUPERSLM_PAGED_KV_74_FIXTURE_LARGE");
	RunPinned74(GetFixture(low?low:"pkv_def"),GetFixture(large?large:"pkv_32k"),16);}
PKV_CELL("7.4/C6","C6",Box);
PKV_CELL("7.4/C6:fixtures","C6",Fixtures);
PKV_CELL("7.4t/C6:constructions","C6",Constructions);
PKV_CELL("7.4t/C6:lock","C6",LockConstruction);
PKV_CELL("7.4t/C6:hooks","C6",HookConstruction);
PKV_CELL("7.4t/C6:live","C6",TimerLive);
}
