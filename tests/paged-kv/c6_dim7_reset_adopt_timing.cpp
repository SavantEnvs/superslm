// C6 7.4: paired calls at two caps on one model; fixed timer vitality precedes grading.
#include "pkv_budget_c_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

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

namespace {
using namespace pkv;
using namespace pkv::bc;
constexpr int kPairs = 205;
enum class Verdict { Pass, Fail, NoResult };

double Median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size()/2]; }
bool Vitality(double low, double large, double empty) {
	return low > empty && large > empty && (large-empty)/(low-empty) >= 9.0 && (large-empty)/(low-empty) <= 11.0;
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
	PKV_CHECK(Vitality(120,1020,20));
	for(double cost:{20.0,40.0,80.0,200.0}) {
		PKV_CHECK(!Vitality(100+cost,1000+cost,0));
		PKV_CHECK(Vitality(100+cost,1000+cost,cost));
	}
}
[[noreturn]] void Refuse(int code,const char* why) { std::fprintf(stderr,"7.4 INFRASTRUCTURE %d: %s\n",code,why); std::exit(code); }

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
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
uint64_t Adds(int n){volatile uint64_t x=0;for(int i=0;i<n;++i)x=x+static_cast<uint64_t>(i+1);return x;}
struct Clock {
	double empty=0, resolution=0;bool okay=false;
	Clock(){std::vector<double> e,a,b;for(int i=0;i<21;++i){auto x=Tick(),y=Tick();e.push_back(double(y-x));
		x=Tick();(void)Adds(i%2?1000:100);y=Tick();(i%2?b:a).push_back(double(y-x));
		x=Tick();(void)Adds(i%2?100:1000);y=Tick();(i%2?a:b).push_back(double(y-x));}
		empty=Median(e);resolution=*std::min_element(e.begin(),e.end());okay=Vitality(Median(a),Median(b),empty);
		std::printf("7.4t: resolution %.0f ticks empty %.0f 100 %.0f 1000 %.0f ratio %.3f %s\n",resolution,empty,Median(a),Median(b),(Median(a)>empty)?(Median(b)-empty)/(Median(a)-empty):0,okay?"PASS":"INFRASTRUCTURE");}
};
void Delay(uint64_t ticks){auto start=Tick();while(Tick()-start<ticks)_mm_pause();}
struct Side {
	const Fixture& fx;std::unique_ptr<Rig> rig;sslm_prefix prefix=nullptr;sslm_seq seq=nullptr;bool ok=false;
	Side(const Fixture& f,bool budget):fx(f){if(!f.ok)return;auto own=budget?f.R(512):f.CapPages();rig=std::make_unique<Rig>(f,static_cast<uint32_t>(f.R(1000)+own+4));
		prefix=rig->Prefix(BudgetPrefix(f,rig->Pool(),PrefixTokens(1000)));sslm_seq s=nullptr;
		auto st=budget?sslm_seq_create_budgeted(f.model,rig->Pool(),512,&s):sslm_seq_create(f.model,rig->Pool(),&s);seq=rig->Seq(s);ok=st==SSLM_OK&&prefix&&seq;}
	bool Call(double empty,double extra_adopt,double extra_reset,double* adopt,double* reset){if(!ok)return false;auto a=Tick();auto sa=sslm_seq_adopt_prefix(seq,prefix);if(extra_adopt>0)Delay(static_cast<uint64_t>(extra_adopt));auto b=Tick();
		auto sr=sslm_seq_reset(seq);if(extra_reset>0)Delay(static_cast<uint64_t>(extra_reset));auto c=Tick();ok=sa==SSLM_OK&&sr==SSLM_OK;*adopt=double(b-a)-empty;*reset=double(c-b)-empty;return ok;}
};
void Run74(const Fixture& low,const Fixture& large){
	if(!low.ok||!large.ok)Refuse(71,"artifact missing or unable to load");
	if(low.geo.context_cap!=4096||large.geo.context_cap!=32768)Refuse(72,"wrong cap");
	if(low.geo.layers!=large.geo.layers||low.geo.kv_heads!=large.geo.kv_heads||low.geo.head_dim!=large.geo.head_dim||low.B()!=large.B()||
	   sslm_kv_page_size(low.model)!=sslm_kv_page_size(large.model))Refuse(71,"geometry mismatch");
	if(!TscInvariant())Refuse(73,"invariant TSC absent");Pin pin;if(!pin.ok)Refuse(73,"cannot pin timing thread");Clock clock;
	if(!clock.okay&&RdtscpAvailable()){use_rdtscp=true;clock=Clock();}
	if(!clock.okay)Refuse(76,"fixed-L timer vitality failed with every supported fence");
	std::printf("7.4 artifact hashes: cap4096=%s cap32768=%s\n",low.sha.c_str(),large.sha.c_str());bool unresolved=false;
	for(bool budget:{true,false}){Side a(low,budget),b(large,budget);if(!a.ok||!b.ok)Refuse(71,"holder creation failed");
		for(int i=0;i<10;++i){double x=0,y=0;if(!a.Call(clock.empty,0,0,&x,&y)||!b.Call(clock.empty,0,0,&x,&y))Refuse(71,"warmup failed");}
		double ad0=0,re0=0;if(!a.Call(clock.empty,0,0,&ad0,&re0))Refuse(71,"control probe failed");
		double extra_ad=std::getenv("SUPERSLM_PAGED_KV_74_ADOPT_MUTANT")?std::max(0.0,3*ad0):0;
		double extra_re=std::getenv("SUPERSLM_PAGED_KV_74_RESET_MUTANT")?std::max(0.0,7*re0):0;
		std::vector<double> ra,rb,aa,ab,adopt_ratios,reset_ratios;std::mt19937 rng(7404+static_cast<unsigned>(budget));
		for(int i=0;i<kPairs;++i){double xad=0,xre=0,yad=0,yre=0;
			auto first=[&]{return a.Call(clock.empty,0,0,&xad,&xre);};auto second=[&]{return b.Call(clock.empty,extra_ad,extra_re,&yad,&yre);};
			if(!((rng()&1)?(first()&&second()):(second()&&first())))Refuse(71,"timed verb failed");
			aa.push_back(xad);ab.push_back(yad);ra.push_back(xre);rb.push_back(yre);adopt_ratios.push_back(yad/xad);reset_ratios.push_back(yre/xre);}
		if(Median(aa)<100||Median(ab)<100||Median(ra)<100||Median(rb)<100)Refuse(74,"corrected median under 100 ticks");
		for(auto& entry:{std::pair{"adopt",adopt_ratios},std::pair{"reset",reset_ratios}}){double lo=0,hi=0;auto verdict=Grade(entry.second,&lo,&hi);
			std::printf("7.4 %s %s r(86)=%.3f r(120)=%.3f bound=2.0 %s\n",budget?"budget":"whole_reserve",entry.first,lo,hi,verdict==Verdict::Pass?"PASS":verdict==Verdict::Fail?"FAIL":"NO RESULT");
			PKV_CHECK_MSG(verdict!=Verdict::Fail,"7.4 %s %s: ratio above 2.0",budget?"budget":"whole_reserve",entry.first);unresolved|=verdict==Verdict::NoResult;}
	}
	if(unresolved)Refuse(75,"sign interval did not resolve the bound");
}
#else
void Run74(const Fixture&,const Fixture&){Refuse(73,"x86 invariant TSC required");}
#endif
void Box(){const char* large=std::getenv("SUPERSLM_PAGED_KV_74_CAP32768_FILE");Run74(GetArtifact("qwen2.5-0.5b-instruct-cap4096-aex.sslm",Geometry{24,2,64,4096}),
  GetArtifact(large?large:"qwen2.5-0.5b-instruct-cap32768-aex.sslm",Geometry{24,2,64,32768}));}
void Fixtures(){Run74(GetFixture("pkv_def"),GetFixture("pkv_32k"));}
PKV_CELL("7.4/C6","C6",Box);
PKV_CELL("7.4/C6:fixtures","C6",Fixtures);
PKV_CELL("7.4t/C6:constructions","C6",Constructions);
}
