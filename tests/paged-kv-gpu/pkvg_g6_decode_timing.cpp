// Paged-KV plan (rev 16.2) step G6 and §10 R1: the GPU decode timing harness, paged against the legacy view.
//
// R1: "G6 measures paged vs legacy-view decode tok/s on the 0.5B cohort at width 1,712 on the RTX 2080 SUPER. Bar
// (planner default): no more than 5% slower, stated with resolving power. Over the bar -> a fold choosing a larger
// engine page (a multiple of kv_block_size) before G7." R2 ("table-buffer update adds a barrier per mapped page,
// at most once per 16 tokens in decode") is measured inside it. §8: the timing harness is commissioned at G6 (GPU);
// until its registry receipt is verified its readings are quarantined -- recorded, never acted on or headlined.
//
// It is the GPU twin of the CPU slice's in-process decode pairs (tools/paged_kv_pair.cpp pair79b, driven by
// tools/run_paged_kv_pairs.py): one process, one context, two arms decoding the same prompt, one token per call
// through the recommended one-call decode (SslmGpuSeqDecodeStepForG5Bridge, which waits for the token), each call
// timed alone with a steady clock:
//   arm A, paged:  the 0.5B cohort (g1e_cohort.h's construction): a page pool of exactly the cohort's pages, the
//                  world of 1,000, 10 personas by begin_from (each frozen as built), the world released, 50
//                  budget-512 sequences each adopting its persona. The timed sequence is --timed-seq (default 0,
//                  persona 0), decoding from 1,200 to the limit 1,712 -- the plan's width -- through its page table.
//   arm B, legacy: a dedicated GPU sequence (sslm_gpu_seq_create at the cap), which binds the one-page view (§4
//                  item 2), prefilled with the same 1,200 prompt tokens (the world's then persona 0's).
// The ready token is compared, then --warmup untimed tokens per arm, then --pairs timed pairs: each pair's order
// (A then B, or B then A) is drawn from mt19937(seed + repeat), and both arms' tokens must be equal at every pair
// (a mismatch is a red result, not a timing). warmup + pairs <= 512, the budget.
//
// Repeats. --repeats=R (default 5) fresh constructions (new pool, cohort and dedicated sequence), each graded
// alone and all pooled. Per pair the slowdown is 1 - legacy_ns / paged_ns (positive: paged slower). Grade (the CPU
// runner's grade(), generalised from n = 508): the median, and a distribution-free sign interval for it, the order
// statistics [n/2 - c, n/2 + c - 1] with c = ceil(2.26 * sqrt(n) / 2) (228 and 279 at n = 508, the CPU's ranks);
// half its width is the resolving power. effect: RESOLVED when the interval excludes 0, else NO RESULT. R1 bar:
// PASS when the interval's top is <= 0.05, FAIL when its bottom is > 0.05, else NO RESULT. The R1 verdict is the
// pooled one; each repeat's is printed beside it. A run with fewer than 508 pairs per repeat is a mechanism run:
// graded and printed, status MECHANISM_ONLY, never a verdict.
//
// Resumable logs. --out=DIR holds run.txt (the run's identity: this executable's and the model's SHA-256, the
// adapter selection, pairs, warmup, repeats, the timed sequence, the hook state and the seed, chosen once and kept),
// repeat_<k>.log for each completed repeat (written to repeat_<k>.partial, renamed only after its END line), and
// result.txt once every repeat is complete. A rerun with the same identity keeps every completed repeat and reruns
// the rest from a fresh construction (a repeat cannot resume mid-stream: its sequences' state dies with the
// process); a rerun whose identity differs is refused (exit 40) unless --fresh, which deletes DIR's records first.
//
// Quiet guard (the CPU timing runners' idle check, run_commissioning.py's BUSY_PROCESSES and the box lock, plus the
// GPU's own contenders): before each repeat the process list and D:\_ssu_build_lock are read; any offender refuses
// the repeat (exit 40, completed repeats kept for the resume). After each repeat they are read again, and a repeat
// that ended on a busy box is discarded the same way. --allow-busy records the offenders instead and stamps the
// result NOT_QUIET (never a verdict).
//
// Status of the result: MUTANT_CONSTRUCTION when the slow-arm hook is set (SUPERSLM_PKVG_G6_SLOW_ARM=A or B: that
// arm busy-waits 17.65% of its own call time after each timed call, the CPU 7.9 hook's figure, so a correct
// instrument must report FAIL for A -- the debunker's must-reject -- and PASS for B); MECHANISM_ONLY below 508
// pairs; NOT_QUIET under --allow-busy; STANDING only with --commissioned (passed by tools\run_paged_kv_g6_box.ps1
// only after instrument-commission.ps1 -Verify lists the receipt PKV-TIMING-G6-R1); otherwise QUARANTINED.
//
// Exit: 0 every repeat complete and the pooled bar PASS (or a complete mechanism / mutant / not-quiet run); 1
// complete with FAIL or NO RESULT, or a token mismatch; 40 infrastructure (arguments, artifact, refusal by the quiet
// guard, an identity mismatch, a construction that would not build).
//
// Usage: pkvg_g6_decode_timing.exe --model=PATH --out=DIR [--repeats=5] [--pairs=508] [--warmup=4] [--seed=N]
//                                  [--timed-seq=0] [--fresh] [--allow-busy] [--commissioned]
//        pkvg_g6_decode_timing.exe identity
// Run once per GPU (SSLM_GPU_ADAPTER_INDEX), on a quiet box, debug layer OFF (a timing run is not a debug-layer run).
#if defined(SUPERSLM_GPU_ALLOC_FAULT_INJECTION) || defined(SUPERSLM_GPU_TEST_SEAMS) || \
    defined(SUPERSLM_T2169_CHUNK_RECORDING_FAULT_INJECTION) || defined(SUPERSLM_ENABLE_GPU_BENCH_PRE_BATCHING) || \
    defined(SUPERSLM_ENABLE_GPU_API_FAILURE_INJECTION) || defined(SUPERSLM_ENABLE_BAD_ALLOC_INJECTION) || \
    defined(SUPERSLM_ENABLE_MATVEC_TEST_SEAMS) || defined(SUPERSLM_FORCE_SCALAR_MATMUL)
#error The G6 decode timing harness must be built in the plain configuration: no seam or injection definition.
#endif
#include "g1e_cohort.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace g1e;
using namespace g1e::cohort;

namespace {

constexpr const char* kIdentity = "PKVG_G6_DECODE_TIMING_V1 defs=none";
constexpr int kGradedPairs = 508;
using clock_type = std::chrono::steady_clock;
namespace fs = std::filesystem;

[[noreturn]] void Infra(const std::string& why) {
	std::printf("INFRASTRUCTURE: %s\n", why.c_str());
	std::fflush(stdout);
	std::exit(40);
}

std::string FileSha(const std::string& path) {
	std::vector<uint8_t> b;
	if (!ReadFileBytes(path, &b)) return "missing";
	return Sha256Hex(b.data(), b.size());
}
std::string SelfPath() {
#if defined(_WIN32)
	char p[4096]{};
	const DWORD n = GetModuleFileNameA(nullptr, p, sizeof p);
	return n && n < sizeof p ? std::string(p, n) : std::string();
#else
	return fs::read_symlink("/proc/self/exe").string();
#endif
}
std::string SelfName() {
	const std::string p = SelfPath();
	const size_t cut = p.find_last_of("/\\");
	return cut == std::string::npos ? p : p.substr(cut + 1);
}

// ---- the quiet guard -----------------------------------------------------------------------------

std::string Lower(std::string s) {
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
// The CPU runners' offenders (run_commissioning.py BUSY_PROCESSES, run_paged_kv_c6_box.ps1 Get-IdleReport) and the
// GPU's own: another suite, harness or engine process would share the device.
std::vector<std::string> Offenders() {
	std::vector<std::string> out;
	if (fs::exists("D:\\_ssu_build_lock")) out.push_back("D:\\_ssu_build_lock present");
#if defined(_WIN32)
	static const char* const kBusy[] = {"unrealeditor", "unrealeditor-cmd", "cl", "link", "dotnet", "unrealbuildtool", "ninja",
	                                    "cmake", "msbuild", "cc1plus", "smoke_adapter_driver"};
	static const char* const kBusyPrefix[] = {"superslm_", "pkvg_", "g1d_cell_", "g1e_cell_"};
	const std::string self = Lower(SelfName());
	const DWORD self_pid = GetCurrentProcessId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		out.push_back("process list unreadable");
		return out;
	}
	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof pe;
	for (BOOL more = Process32First(snap, &pe); more; more = Process32Next(snap, &pe)) {
		if (pe.th32ProcessID == self_pid) continue;
		std::string name = Lower(pe.szExeFile);
		const std::string full = name;
		if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
		bool busy = false;
		for (const char* b : kBusy) busy = busy || name == b;
		for (const char* b : kBusyPrefix) busy = busy || name.rfind(b, 0) == 0;
		if (busy) out.push_back("process " + full + " (pid " + std::to_string(pe.th32ProcessID) + ")");
	}
	CloseHandle(snap);
	(void)self;
#endif
	return out;
}
std::string Join(const std::vector<std::string>& v) {
	std::string s;
	for (const std::string& x : v) s += (s.empty() ? "" : "; ") + x;
	return s.empty() ? "none" : s;
}

// ---- grading -------------------------------------------------------------------------------------

struct Grade {
	size_t n = 0;
	double median = 0, lo = 0, hi = 0;
	bool interval = false;
	std::string effect = "NO RESULT", bar = "NO RESULT";
};
Grade GradeOf(std::vector<double> v) {
	Grade g;
	g.n = v.size();
	if (v.empty()) return g;
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	g.median = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
	const long c = static_cast<long>(std::ceil(2.26 * std::sqrt(static_cast<double>(n)) / 2));
	const long lo = static_cast<long>(n / 2) - c, hi = static_cast<long>(n / 2) + c - 1;
	if (lo >= 0 && hi < static_cast<long>(n) && lo <= hi) {
		g.interval = true;
		g.lo = v[static_cast<size_t>(lo)];
		g.hi = v[static_cast<size_t>(hi)];
		g.effect = (g.lo > 0 || g.hi < 0) ? "RESOLVED" : "NO RESULT";
		g.bar = g.hi <= 0.05 ? "PASS" : g.lo > 0.05 ? "FAIL" : "NO RESULT";
	}
	return g;
}
double Median(std::vector<double> v) {
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// ---- the arms ------------------------------------------------------------------------------------

struct Pair {
	char first = 'A';
	double paged_ns = 0, legacy_ns = 0;
	int32_t paged_tok = -1, legacy_tok = -1;
};

// One timed whole token on `s`; the slow-arm hook busy-waits 17.65% of the call's own time (CPU 7.9's figure).
std::pair<double, int32_t> TimedToken(const Env& e, GSeq& s, bool slow) {
	GS st = GS::SSLM_OK;
	const auto t0 = clock_type::now();
	const int32_t tok = Token(e, s, &st);
	if (slow) {
		const double own = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
		const auto until = clock_type::now() + std::chrono::nanoseconds(static_cast<int64_t>(own * 0.1765));
		while (clock_type::now() < until) {
		}
	}
	const double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
	if (st != GS::SSLM_OK || tok < 0) Infra(std::string("a timed decode returned ") + StatusName(st) + ", token " + std::to_string(tok));
	return {ns, tok};
}

struct Options {
	std::string model, out;
	int repeats = 5, pairs = kGradedPairs, warmup = 4, timed_seq = 0;
	uint64_t seed = 0;
	bool seed_given = false, fresh = false, allow_busy = false, commissioned = false;
};

// One repeat: a fresh construction of both arms, the ready token, the warm-up, the pairs. Returns the pairs; a token
// mismatch returns them with `mismatch` set at the first differing pair.
std::vector<Pair> RunRepeat(const Env& e, const Options& o, uint32_t seed, const std::string& hook, int* mismatch) {
	*mismatch = -1;
	const Geo& g = e.geo;
	// arm A: the cohort, in its exact pool (§3.4's arithmetic at this geometry)
	Rig rig(e, static_cast<uint32_t>(CohortPages(g)));
	if (!rig.pool) Infra("the paged arm's pool did not create");
	const GWorld w = BuildWorld(rig);
	if (!w.ok) Infra("the paged arm's world and personas did not build");
	std::vector<GSeq> cohort;
	for (int i = 0; i < kSeqs; ++i) {
		GSeq s = rig.Budgeted(kBudget);
		if (!s.h) Infra("cohort create " + std::to_string(i) + " was refused");
		if (Adopt(rig, s, w.personas[static_cast<size_t>(PersonaOf(i))], kOrigin) != GS::SSLM_OK)
			Infra("cohort adopt " + std::to_string(i) + " was refused");
		cohort.push_back(s);
	}
	GSeq& paged = cohort[static_cast<size_t>(o.timed_seq)];
	// arm B: a dedicated sequence (one-page view) holding the same prompt
	SslmGpuSequenceHandle* h = nullptr;
	if (sslm_gpu_seq_create(e.ctx, e.model(), g.cap, &h) != GS::SSLM_OK || !h) Infra("the legacy arm's sslm_gpu_seq_create failed");
	GSeq legacy;
	legacy.h = rig.Seq(h);
	std::vector<int32_t> prompt = WorldTokens();
	const std::vector<int32_t> persona = PersonaTokens(PersonaOf(o.timed_seq));
	prompt.insert(prompt.end(), persona.begin(), persona.end());
	const GS ps = Prefill(e, legacy, prompt);
	if (ps != GS::SSLM_OK) Infra(std::string("the legacy arm's prefill: ") + StatusName(ps));
	std::vector<Pair> pairs;
	const bool slow_a = hook == "A", slow_b = hook == "B";
	const int32_t ra = TimedToken(e, paged, false).second, rb = TimedToken(e, legacy, false).second;
	if (ra != rb) {
		*mismatch = -2;
		return pairs;
	}
	for (int i = 0; i < o.warmup; ++i)
		if (TimedToken(e, paged, false).second != TimedToken(e, legacy, false).second) {
			*mismatch = -3;
			return pairs;
		}
	std::mt19937 random(seed);
	for (int i = 0; i < o.pairs; ++i) {
		Pair p;
		std::pair<double, int32_t> x, y;
		if ((random() & 1u) != 0) {
			p.first = 'A';
			x = TimedToken(e, paged, slow_a);
			y = TimedToken(e, legacy, slow_b);
		} else {
			p.first = 'B';
			y = TimedToken(e, legacy, slow_b);
			x = TimedToken(e, paged, slow_a);
		}
		p.paged_ns = x.first;
		p.paged_tok = x.second;
		p.legacy_ns = y.first;
		p.legacy_tok = y.second;
		pairs.push_back(p);
		if (x.second != y.second) {
			*mismatch = i;
			return pairs;
		}
	}
	return pairs;  // the rig releases the dedicated sequence, the cohort, the personas and the pool
}

// ---- the run's records ---------------------------------------------------------------------------

std::vector<std::string> ReadLines(const fs::path& p) {
	std::vector<std::string> out;
	std::ifstream in(p);
	std::string l;
	while (std::getline(in, l)) {
		if (!l.empty() && l.back() == '\r') l.pop_back();
		out.push_back(l);
	}
	return out;
}
void WriteAtomic(const fs::path& p, const std::string& text) {
	const fs::path tmp = p.string() + ".partial";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out) Infra("cannot write " + tmp.string());
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);
	if (ec) Infra("cannot rename " + tmp.string() + " to " + p.string() + ": " + ec.message());
}

struct RepeatRecord {
	bool complete = false;
	std::vector<double> slowdown, paged_ns, legacy_ns;
	std::string quiet_after;
};
// A completed repeat log: its header names this run's identity digest and the repeat's seed; one PAIR line per pair,
// in order, tokens equal; an END line with the pair count.
RepeatRecord ReadRepeat(const fs::path& p, const std::string& run_sha, uint32_t seed, int pairs) {
	RepeatRecord r;
	const std::vector<std::string> lines = ReadLines(p);
	if (lines.empty() || lines[0] != "REPEAT run=" + run_sha + " seed=" + std::to_string(seed)) return r;
	int count = 0;
	for (size_t i = 1; i < lines.size(); ++i) {
		std::stringstream ss(lines[i]);
		std::string tag;
		ss >> tag;
		if (tag == "PAIR") {
			int idx = -1;
			std::string first;
			double a = 0, b = 0;
			int32_t ta = -1, tb = -1;
			ss >> idx >> first >> a >> b >> ta >> tb;
			if (!ss || idx != count || ta != tb || !(a > 0) || !(b > 0) || !std::isfinite(a) || !std::isfinite(b)) return RepeatRecord{};
			r.paged_ns.push_back(a);
			r.legacy_ns.push_back(b);
			r.slowdown.push_back(1.0 - b / a);
			++count;
		} else if (tag == "END") {
			int n = -1;
			std::string field;
			ss >> field;
			if (field.rfind("pairs=", 0) == 0) n = std::atoi(field.c_str() + 6);
			std::getline(ss, r.quiet_after);
			r.complete = n == pairs && count == pairs && i + 1 == lines.size();
		} else {
			return RepeatRecord{};
		}
	}
	if (!r.complete) return RepeatRecord{};
	return r;
}

std::string Fmt(double v) {
	char b[64];
	std::snprintf(b, sizeof b, "%.5f", v);
	return b;
}
std::string GradeLine(const Grade& g) {
	return "n=" + std::to_string(g.n) + " median_slowdown=" + Fmt(g.median) +
	       (g.interval ? " interval=[" + Fmt(g.lo) + "," + Fmt(g.hi) + "] half_width=" + Fmt((g.hi - g.lo) / 2) : " interval=none") +
	       " effect=" + g.effect + " bar=" + g.bar;
}

Options Parse(int argc, char** argv) {
	Options o;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto val = [&](const char* f, std::string* out) {
			const size_t n = std::strlen(f);
			if (a.compare(0, n, f) != 0) return false;
			*out = a.substr(n);
			return true;
		};
		std::string v;
		try {
			if (val("--model=", &o.model) || val("--out=", &o.out)) continue;
			if (val("--repeats=", &v)) o.repeats = std::stoi(v);
			else if (val("--pairs=", &v)) o.pairs = std::stoi(v);
			else if (val("--warmup=", &v)) o.warmup = std::stoi(v);
			else if (val("--timed-seq=", &v)) o.timed_seq = std::stoi(v);
			else if (val("--seed=", &v)) {
				o.seed = std::stoull(v);
				o.seed_given = true;
			} else if (a == "--fresh") o.fresh = true;
			else if (a == "--allow-busy") o.allow_busy = true;
			else if (a == "--commissioned") o.commissioned = true;
			else Infra("unknown argument " + a);
		} catch (const std::exception&) {
			Infra("invalid value in " + a);
		}
	}
	if (o.model.empty() || o.out.empty()) Infra("--model=PATH and --out=DIR are required");
	if (o.repeats < 1 || o.repeats > 100) Infra("--repeats must be 1..100");
	if (o.pairs < 1 || o.warmup < 0 || o.pairs + o.warmup > kBudget) Infra("--pairs >= 1, --warmup >= 0, pairs + warmup <= 512");
	if (o.timed_seq < 0 || o.timed_seq >= kSeqs) Infra("--timed-seq must be 0..49");
	if (o.seed > 0xffffffffull) Infra("--seed must fit 32 bits");
	return o;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc == 2 && std::string(argv[1]) == "identity") {
		std::printf("%s\n", kIdentity);
		return 0;
	}
	Options o = Parse(argc, argv);
	std::printf("%s\n", kIdentity);
	const fs::path out(o.out);
	std::error_code ec;
	fs::create_directories(out, ec);
	if (!fs::is_directory(out)) Infra("cannot create " + o.out);
	if (o.fresh)
		for (const auto& entry : fs::directory_iterator(out)) {
			const std::string n = entry.path().filename().string();
			if (n == "run.txt" || n == "result.txt" || n.rfind("repeat_", 0) == 0) fs::remove(entry.path(), ec);
		}

	const char* hook_env = std::getenv("SUPERSLM_PKVG_G6_SLOW_ARM");
	const std::string hook = hook_env ? hook_env : "";
	if (!hook.empty() && hook != "A" && hook != "B") Infra("SUPERSLM_PKVG_G6_SLOW_ARM must be A or B");
	const char* adapter_env = std::getenv("SSLM_GPU_ADAPTER_INDEX");
	const char* debug_env = std::getenv("SSLM_GPU_ENABLE_DEBUG_LAYER");
	if (debug_env && std::string(debug_env) == "1")
		std::printf("NOTE: SSLM_GPU_ENABLE_DEBUG_LAYER=1 is set: the debug layer costs time; the identity records it\n");

	// The identity; the seed is chosen once and kept by run.txt.
	std::vector<std::pair<std::string, std::string>> id = {
	    {"identity", kIdentity},
	    {"exe_sha256", FileSha(SelfPath())},
	    {"model", Stem(o.model)},
	    {"model_sha256", FileSha(o.model)},
	    {"adapter_index", adapter_env ? adapter_env : "default"},
	    {"debug_layer", debug_env ? debug_env : "unset"},
	    {"repeats", std::to_string(o.repeats)},
	    {"pairs", std::to_string(o.pairs)},
	    {"warmup", std::to_string(o.warmup)},
	    {"timed_seq", std::to_string(o.timed_seq)},
	    {"hook", hook.empty() ? "none" : hook},
	    {"allow_busy", o.allow_busy ? "1" : "0"},
	};
	if (id[3].second == "missing") Infra("cannot read the model " + o.model);
	const fs::path run_txt = out / "run.txt";
	uint64_t seed = o.seed;
	if (fs::exists(run_txt)) {
		std::map<std::string, std::string> have;
		for (const std::string& l : ReadLines(run_txt)) {
			const size_t eq = l.find('=');
			if (eq != std::string::npos) have[l.substr(0, eq)] = l.substr(eq + 1);
		}
		for (const auto& kv : id)
			if (have[kv.first] != kv.second)
				Infra("this run's " + kv.first + " (" + kv.second + ") differs from " + run_txt.string() + "'s (" + have[kv.first] +
				      "): pass --fresh to discard those records, or another --out");
		const uint64_t kept = std::stoull(have.count("seed") ? have["seed"] : "0");
		if (o.seed_given && kept != o.seed) Infra("--seed differs from the kept seed " + std::to_string(kept) + " (pass --fresh)");
		seed = kept;
		std::printf("resuming %s (seed %llu)\n", o.out.c_str(), static_cast<unsigned long long>(seed));
	} else {
		if (!o.seed_given) seed = std::random_device{}() & 0xffffffffu;
		std::string text;
		for (const auto& kv : id) text += kv.first + "=" + kv.second + "\n";
		text += "seed=" + std::to_string(seed) + "\n";
		WriteAtomic(run_txt, text);
	}
	std::vector<uint8_t> run_bytes;
	ReadFileBytes(run_txt.string(), &run_bytes);
	const std::string run_sha = Sha256Hex(run_bytes.data(), run_bytes.size());

	// Which repeats are already complete.
	std::vector<RepeatRecord> rec(static_cast<size_t>(o.repeats));
	int todo = 0;
	for (int k = 0; k < o.repeats; ++k) {
		const fs::path p = out / ("repeat_" + std::to_string(k) + ".log");
		const uint32_t rs = static_cast<uint32_t>(seed + static_cast<uint64_t>(k));
		if (fs::exists(p)) {
			rec[static_cast<size_t>(k)] = ReadRepeat(p, run_sha, rs, o.pairs);
			if (!rec[static_cast<size_t>(k)].complete) {
				std::printf("repeat %d: %s is not a complete record of this run; rerunning it\n", k, p.string().c_str());
				fs::remove(p, ec);
			} else {
				std::printf("repeat %d: kept (%s)\n", k, p.string().c_str());
			}
		}
		if (!rec[static_cast<size_t>(k)].complete) ++todo;
	}

	if (todo > 0) {
		// Refuse before the device is touched when the box is busy already (each repeat checks again).
		const std::vector<std::string> busy = Offenders();
		if (!busy.empty() && !o.allow_busy) Infra("refused: the box is not quiet: " + Join(busy));
		Env e;
		if (!e.Open(o.model)) Infra("the model did not open on the GPU");
		std::printf("model %s: B = %lld, cap %lld, cohort pool %lld pages\n", o.model.c_str(), static_cast<long long>(e.geo.B),
		            static_cast<long long>(e.geo.cap), static_cast<long long>(CohortPages(e.geo)));
		for (int k = 0; k < o.repeats; ++k) {
			if (rec[static_cast<size_t>(k)].complete) continue;
			const std::vector<std::string> before = Offenders();
			if (!before.empty() && !o.allow_busy) Infra("repeat " + std::to_string(k) + " refused: the box is not quiet: " + Join(before));
			const uint32_t rs = static_cast<uint32_t>(seed + static_cast<uint64_t>(k));
			std::printf("repeat %d: seed %u, quiet before: %s\n", k, rs, Join(before).c_str());
			std::fflush(stdout);
			const auto t0 = clock_type::now();
			int mismatch = -1;
			const std::vector<Pair> pairs = RunRepeat(e, o, rs, hook, &mismatch);
			if (GFailures) Infra("a check failed while building the arms (see FAIL above)");
			if (mismatch != -1) {
				std::printf("RED: repeat %d: the arms' tokens differ at %s\n", k,
				            mismatch == -2 ? "the ready token" : mismatch == -3 ? "the warm-up" : ("pair " + std::to_string(mismatch)).c_str());
				return 1;
			}
			const std::vector<std::string> after = Offenders();
			if (!after.empty() && !o.allow_busy)
				Infra("repeat " + std::to_string(k) + " discarded: the box was not quiet at its end: " + Join(after));
			std::string text = "REPEAT run=" + run_sha + " seed=" + std::to_string(rs) + "\n";
			for (size_t i = 0; i < pairs.size(); ++i) {
				char b[200];
				std::snprintf(b, sizeof b, "PAIR %zu %c %.1f %.1f %d %d\n", i, pairs[i].first, pairs[i].paged_ns, pairs[i].legacy_ns,
				              pairs[i].paged_tok, pairs[i].legacy_tok);
				text += b;
			}
			text += "END pairs=" + std::to_string(pairs.size()) + " quiet_before=" + Join(before) + " quiet_after=" + Join(after) + "\n";
			WriteAtomic(out / ("repeat_" + std::to_string(k) + ".log"), text);
			rec[static_cast<size_t>(k)] = ReadRepeat(out / ("repeat_" + std::to_string(k) + ".log"), run_sha, rs, o.pairs);
			if (!rec[static_cast<size_t>(k)].complete) Infra("repeat " + std::to_string(k) + "'s record does not read back");
			std::printf("repeat %d: %zu pairs in %.0f s\n", k, pairs.size(),
			            std::chrono::duration<double>(clock_type::now() - t0).count());
			std::fflush(stdout);
		}
	}

	// Grade: each repeat, then all pooled.
	const bool mechanism = o.pairs < kGradedPairs;
	const std::string status = !hook.empty() ? "MUTANT_CONSTRUCTION"
	                           : mechanism     ? "MECHANISM_ONLY"
	                           : o.allow_busy  ? "NOT_QUIET"
	                           : o.commissioned ? "STANDING"
	                                            : "QUARANTINED";
	std::vector<double> all, all_paged, all_legacy, medians;
	std::string report;
	for (int k = 0; k < o.repeats; ++k) {
		const RepeatRecord& r = rec[static_cast<size_t>(k)];
		const Grade g = GradeOf(r.slowdown);
		medians.push_back(g.median);
		all.insert(all.end(), r.slowdown.begin(), r.slowdown.end());
		all_paged.insert(all_paged.end(), r.paged_ns.begin(), r.paged_ns.end());
		all_legacy.insert(all_legacy.end(), r.legacy_ns.begin(), r.legacy_ns.end());
		report += "G6 R1 repeat " + std::to_string(k) + ": " + GradeLine(g) + " paged_tok_s=" + Fmt(1e9 / Median(r.paged_ns)) +
		          " legacy_tok_s=" + Fmt(1e9 / Median(r.legacy_ns)) + "\n";
	}
	const Grade pooled = GradeOf(all);
	report += "G6 R1 pooled: " + GradeLine(pooled) + " paged_tok_s=" + Fmt(1e9 / Median(all_paged)) +
	          " legacy_tok_s=" + Fmt(1e9 / Median(all_legacy)) + " median_of_repeat_medians=" + Fmt(Median(medians)) +
	          " status=" + status + "\n";
	report += "G6 R1 verdict: " + (mechanism || !hook.empty() || o.allow_busy ? std::string("none (") + status + ")" : pooled.bar) +
	          " (bar: paged no more than 5% slower than the legacy view; " + status + ")\n";
	std::printf("%s", report.c_str());
	std::string result;
	for (const auto& kv : id) result += kv.first + "=" + kv.second + "\n";
	result += "seed=" + std::to_string(seed) + "\nrun_sha256=" + run_sha + "\n" + report;
	WriteAtomic(out / "result.txt", result);
	if (mechanism || !hook.empty() || o.allow_busy) return 0;
	return pooled.bar == "PASS" ? 0 : 1;
}
