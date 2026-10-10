// Paged-KV plan (rev 16.2) step G1: the GPU red suite's process runner. One command runs the whole suite
// at a step, one process per job, on the GPU the environment selects (SSLM_GPU_ADAPTER_INDEX):
//   - the in-process runner of the step (pkvg_g2 .. pkvg_g5; pkvg_main.cpp), which carries every
//     g1a/g1b/g1c cell of the default engine at or before the step;
//   - the executables of the other engine configurations (pkvg_alloc_g3, pkvg_bench_g3,
//     pkvg_firsttouch_g3) and the cells that own their main() (g1d: per-cell engine configurations and a
//     fresh process per stranded leg; g1e: per-cell executables, and 9.8's second leg, which is the
//     v1.12.0 reader built from the other tree).
// It replaces g1d's draft job runner (g1d_main_draft.cpp), whose job table and rules it keeps: selection
// by job-id prefix or '=' exact id, a selector that selects nothing fails the run, a job whose artifact
// was not given is RED (a skip fails the run), and every job runs with SSLM_GPU_ENABLE_DEBUG_LAYER=1.
//
// Usage: pkvg_suite --bin=DIR [--step=G2|G3|G4|G5] [--list] [--no-debug-layer] [artifact flags] [JOB ...]
//   --bin=DIR           where build_paged_kv_gpu.bat (or _mingw.sh) put the executables
//   --step=GN           run the jobs whose bracket is at or before GN, and the in-process runner pkvg_gN
//                       (default G5: everything)
//   --pkv-fixtures=DIR  tools/gen_paged_kv_fixture.py's output (pkv_def.sslm, pkv_qk.sslm, ...)
//   --pkv-reference=DIR tests/paged-kv/reference (default: relative to the working directory, the repo root)
//   --gpu-ref=DIR       6.5a's pre-G2 GPU records (gpu_pre_g2_<fx>_adapter<N>.ref)
//   --model05=PATH --model15=PATH   the real Qwen2.5-0.5B / 1.5B artifacts (g1a 4.8, g1b 11.4)
//   --qwen3=PATH --r15=PATH --g5=PATH  g1d's artifacts (te425's --qwen3, --r15, --g5)
//   --adapter=PATH --adapter-other-base=PATH  g1c's LoRA adapters (3.6d adapter_map, adapter_other_base)
//   --out98=DIR         9.8's exchange directory (SUPERSLM_PKV_GPU_98_OUT)
//   --old-bin=DIR       where the v1.12.0 g1e_old_reader_9_8.exe was built (9.8 leg 2)
//   --no-debug-layer    do not set SSLM_GPU_ENABLE_DEBUG_LAYER=1 (software-GPU smoke runs only; a run
//                       without it is not an acceptance run, and 3.6 fails by design)
// Exit 0 only when at least one job ran, every selector matched and every job exited 0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Job {
	const char* id;    // "pkvg/g3", "3.6f/first", "9.8/leg2"
	int bracket;       // the step at which it can first link (2..5)
	int only_at;       // nonzero: the in-process runner of exactly that step (pkvg_gN)
	const char* exe;   // under --bin ("old:" prefix: under --old-bin)
	const char* args;  // {key} expands to the flag's value; a "--x={key}" word whose value is empty is dropped
	const char* need;  // the artifact keys whose absence makes the job RED before it runs ("" = none)
};

// Common flags of the in-process runners.
#define PKVG_COMMON "--pkv-fixtures={fx} --pkv-reference={ref} --gpu-ref={gpuref} --model05={m05} --model15={m15}"
#define PKVG_G1C "--adapter={adapter} --adapter-other-base={adapter_other}"

const Job kJobs[] = {
    // In-process runners: every g1a/g1b/g1c cell of the default engine at or before the step.
    {"pkvg/g2", 2, 2, "pkvg_g2.exe", PKVG_COMMON, "fx"},
    {"pkvg/g3", 3, 3, "pkvg_g3.exe", PKVG_COMMON " " PKVG_G1C, "fx"},
    {"pkvg/g4", 4, 4, "pkvg_g4.exe", PKVG_COMMON " " PKVG_G1C, "fx"},
    {"pkvg/g5", 5, 5, "pkvg_g5.exe", PKVG_COMMON " " PKVG_G1C, "fx"},
    // Allocation-fault engine: 11.4/G3, and its zero-fill leg alone in a fresh process (terminal context).
    {"11.4/G3", 3, 0, "pkvg_alloc_g3.exe", PKVG_COMMON " 11.4/G3", "fx"},
    {"11.4/G3:zerofill", 3, 0, "pkvg_alloc_g3.exe", "--pkv-fixtures={fx} =11.4/G3:zerofill", "fx"},
    // review-g23 F2: the pooled release that meets a refcount underflow still finishes.
    {"11.3/G3:release", 3, 0, "pkvg_alloc_g3.exe", "--pkv-fixtures={fx} =11.3/G3:release", "fx"},
    // Bench engine: 3.6d's bench prefill and its count==0 precedence.
    {"3.6d/G3:bench", 3, 0, "pkvg_bench_g3.exe", "--pkv-fixtures={fx}", "fx"},
    // Allocation-fault engine + te425 hooks: 3.6d's first touch, alone in a fresh process.
    {"3.6d/G3:first_touch", 3, 0, "pkvg_firsttouch_g3.exe", "--pkv-fixtures={fx} =3.6d/G3:first_touch", "fx"},
    // g1d (its job table, unchanged): 3.6e (allocation-fault build; the G3-only binary links at G3).
    {"3.6e/g3", 3, 0, "g1d_cell_3_6e_g3.exe", "--part=g3 --model={r15}", "r15"},
    {"3.6e/g4", 4, 0, "g1d_cell_3_6e.exe", "--part=g4 --model={r15}", "r15"},
    // 3.6f (allocation-fault build + te425 hooks), each run its own process.
    {"3.6f/first", 3, 0, "g1d_cell_3_6f.exe", "--run=first --model={r15}", "r15"},
    {"3.6f/stranded", 3, 0, "g1d_cell_3_6f.exe", "--run=stranded --model={r15}", "r15"},
    {"3.6f/waitnoarm", 3, 0, "g1d_cell_3_6f.exe", "--run=waitnoarm --model={r15}", "r15"},
    // 3.6g recovered (T2169 build), stranded and per-context (allocation-fault build + te425 hooks).
    {"3.6g/recovered", 3, 0, "g1d_cell_3_6g_recovered.exe", "--model={qwen3}", "qwen3"},
    {"3.6g/stranded.signal", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.signal --model={r15}", "r15"},
    {"3.6g/stranded.setevent", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.setevent --model={r15}", "r15"},
    {"3.6g/percontext", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.percontext --model={qwen3}", "qwen3"},
    // §4.4's nine stranded pins (G3's gate), each alone in a fresh process.
    {"pin/P.setevent1.oom", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent1.oom --model={qwen3}", "qwen3"},
    {"pin/P.setevent1.fail", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent1.fail --model={qwen3}", "qwen3"},
    {"pin/P.setevent2.fail", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent2.fail --model={qwen3}", "qwen3"},
    {"pin/D.setevent1.fail", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=D.setevent1.fail --model={qwen3}", "qwen3"},
    {"pin/R.signal2.oom", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=R.signal2.oom --model={qwen3}", "qwen3"},
    {"pin/S.setevent1.fail", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=S.setevent1.fail --model={g5}", "g5"},
    {"pin/C.signal1.oom", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=C.signal1.oom --model={qwen3}", "qwen3"},
    {"pin/M.signal1.oom", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=M.signal1.oom --model={qwen3}", "qwen3"},
    {"pin/L.setevent1.oom", 3, 0, "g1d_cell_3_6g_stranded.exe", "--leg=L.setevent1.oom --model={r15}", "r15"},
    // 3.12 (default build). The G4-only binary links at G4; the full binary needs G5's restore-into-pool.
    {"3.12/g4-set", 4, 0, "g1d_cell_3_12_g4.exe", "--run=g4-set --model={r15}", "r15"},
    {"3.12/g4-cleared", 4, 0, "g1d_cell_3_12_g4.exe", "--run=g4-cleared --model={r15}", "r15"},
    {"3.12/g5-set", 5, 0, "g1d_cell_3_12.exe", "--run=g5-set --model={r15}", "r15"},
    {"3.12/g5-cleared", 5, 0, "g1d_cell_3_12.exe", "--run=g5-cleared --model={r15}", "r15"},
    {"3.12/g5-clear", 5, 0, "g1d_cell_3_12.exe", "--run=g5-clear --model={r15}", "r15"},
    // g1e: one executable per cell (default build).
    {"9.6", 5, 0, "g1e_cell_9_6.exe", "--fixture-dir={fx}", "fx"},
    {"9.12", 5, 0, "g1e_cell_9_12.exe", "--fixture-dir={fx}", "fx"},
    {"9.13", 5, 0, "g1e_cell_9_13.exe", "--fixture-dir={fx}", "fx"},
    {"8.17", 5, 0, "g1e_cell_8_17.exe", "--fixture-dir={fx}", "fx"},
    // 9.8: leg 1 writes the pooled SLM6 and dedicated SLM5 files into --out98; leg 2 is the v1.12.0
    // reader (built from the other tree, --old-bin) refusing SLM6 with 7. Leg 2 runs after leg 1.
    {"9.8/leg1", 5, 0, "g1e_cell_9_8.exe", "--fixture-dir={fx}", "fx out98"},
    {"9.8/leg2", 5, 0, "old:g1e_old_reader_9_8.exe", "--model={fx}/pkv_def.sslm --in={out98}", "fx out98 oldbin"},
};

bool Matches(const char* id, const std::string& arg) {
	return arg[0] == '=' ? arg.compare(1, std::string::npos, id) == 0 : std::strncmp(id, arg.c_str(), arg.size()) == 0;
}

std::vector<std::string> Words(const std::string& s) {
	std::vector<std::string> w;
	size_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && s[i] == ' ') ++i;
		size_t j = i;
		while (j < s.size() && s[j] != ' ') ++j;
		if (j > i) w.push_back(s.substr(i, j - i));
		i = j;
	}
	return w;
}

// Expands {key}; a word that names an empty value is dropped (the cell then reports the artifact it lacks).
std::string Expand(const std::string& tmpl, const std::map<std::string, std::string>& v) {
	std::string out;
	for (const std::string& w : Words(tmpl)) {
		std::string e;
		bool empty_value = false;
		for (size_t i = 0; i < w.size(); ++i) {
			if (w[i] == '{') {
				const size_t close = w.find('}', i);
				const std::string key = w.substr(i + 1, close - i - 1);
				const auto it = v.find(key);
				const std::string val = it == v.end() ? std::string() : it->second;
				if (val.empty()) empty_value = true;
				e += val;
				i = close;
			} else {
				e += w[i];
			}
		}
		if (empty_value) continue;
		out += (out.empty() ? "" : " ") + std::string("\"") + e + "\"";
	}
	return out;
}

bool FileExists(const std::string& p) {
	if (FILE* f = std::fopen(p.c_str(), "rb")) {
		std::fclose(f);
		return true;
	}
	return false;
}

void SetEnv(const char* k, const std::string& v) {
#if defined(_WIN32)
	_putenv_s(k, v.c_str());
#else
	setenv(k, v.c_str(), 1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
	std::map<std::string, std::string> v;
	v["ref"] = "tests/paged-kv/reference";
	std::string bin = ".", step_s = "G5";
	bool list = false, debug_layer = true;
	std::vector<std::string> sel;
	const std::pair<const char*, const char*> kFlags[] = {
	    {"--pkv-fixtures=", "fx"},  {"--pkv-reference=", "ref"}, {"--gpu-ref=", "gpuref"},
	    {"--model05=", "m05"},      {"--model15=", "m15"},       {"--qwen3=", "qwen3"},
	    {"--r15=", "r15"},          {"--g5=", "g5"},             {"--adapter=", "adapter"},
	    {"--adapter-other-base=", "adapter_other"},              {"--out98=", "out98"},
	    {"--old-bin=", "oldbin"}};
	int bad = 0;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		bool taken = false;
		for (const auto& f : kFlags) {
			const size_t n = std::strlen(f.first);
			if (a.compare(0, n, f.first) == 0) {
				v[f.second] = a.substr(n);
				taken = true;
				break;
			}
		}
		if (taken) continue;
		if (a.rfind("--bin=", 0) == 0) bin = a.substr(6);
		else if (a.rfind("--step=", 0) == 0) step_s = a.substr(7);
		else if (a == "--list") list = true;
		else if (a == "--no-debug-layer") debug_layer = false;
		else if (a.rfind("--", 0) == 0) {
			std::fprintf(stderr, "unknown flag %s\n", a.c_str());
			++bad;
		} else sel.push_back(a);
	}
	const int step = (step_s.size() == 2 && (step_s[0] == 'G' || step_s[0] == 'g')) ? step_s[1] - '0' : 0;
	if (step < 2 || step > 5) {
		std::fprintf(stderr, "--step must be G2, G3, G4 or G5\n");
		return 2;
	}
	if (v.count("fx") == 0) {
		if (const char* d = std::getenv("SUPERSLM_PAGED_KV_FIXTURE_DIR")) v["fx"] = d;
	}
	if (!v["out98"].empty()) SetEnv("SUPERSLM_PKV_GPU_98_OUT", v["out98"]);
	if (debug_layer) SetEnv("SSLM_GPU_ENABLE_DEBUG_LAYER", "1");
#if defined(_WIN32)
	const char sep = '\\';
#else
	const char sep = '/';
#endif

	int unmatched = 0;
	for (const std::string& a : sel) {
		bool any = false;
		for (const Job& j : kJobs) any = any || Matches(j.id, a);
		if (!any) {
			std::fprintf(stderr, "argument '%s' matches no job\n", a.c_str());
			++unmatched;
		}
	}
	int ran = 0, failed = 0;
	for (const Job& j : kJobs) {
		if (j.bracket > step || (j.only_at && j.only_at != step)) continue;
		bool selected = sel.empty();
		for (size_t i = 0; i < sel.size() && !selected; ++i) selected = Matches(j.id, sel[i]);
		if (!selected) continue;
		const bool old = std::strncmp(j.exe, "old:", 4) == 0;
		const std::string dir = old ? v["oldbin"] : bin;
		const std::string exe = dir + sep + (old ? j.exe + 4 : j.exe);
		const std::string cmd = "\"" + exe + "\" " + Expand(j.args, v);
		if (list) {
			std::printf("[G%d] %s: %s\n", j.bracket, j.id, cmd.c_str());
			continue;
		}
		++ran;
		std::string missing;
		for (const std::string& k : Words(j.need))
			if (v[k].empty()) missing += (missing.empty() ? "" : ", ") + k;
		if (!missing.empty()) {
			std::printf("[G%d] %s RED (not given: %s; a skip fails the run)\n", j.bracket, j.id, missing.c_str());
			++failed;
			continue;
		}
		if (!FileExists(exe)) {
			std::printf("[G%d] %s RED (%s not built: red by link or compile at this step, see the build log)\n", j.bracket,
			            j.id, exe.c_str());
			++failed;
			continue;
		}
#if defined(_WIN32)
		const std::string full = "\"" + cmd + "\"";  // cmd /c strips one pair of outer quotes
#else
		const std::string full = cmd;
#endif
		std::printf("[G%d] %s ...\n  %s\n", j.bracket, j.id, cmd.c_str());
		std::fflush(stdout);
		const int rc = std::system(full.c_str());
		if (rc != 0) ++failed;
		std::printf("[G%d] %s %s (exit %d)\n", j.bracket, j.id, rc == 0 ? "green" : "RED", rc);
		std::fflush(stdout);
	}
	if (list) return bad ? 2 : 0;
	std::printf("pkvg_suite at G%d: %d jobs, %d red%s\n", step, ran, failed,
	            debug_layer ? "" : " (debug layer off: not an acceptance run)");
	if (unmatched) std::printf("%d argument(s) matched no job (named on stderr)\n", unmatched);
	return ran > 0 && unmatched == 0 && failed == 0 && bad == 0 ? 0 : 1;
}
