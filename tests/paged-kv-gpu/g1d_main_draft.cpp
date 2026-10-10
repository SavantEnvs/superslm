// Paged KV GPU slice, plan step G1 -- DRAFT runner for group g1d's cells (3.6e, 3.6f, 3.6g, 3.12 and Sec4.4's nine
// stranded pins). Modelled on tests/paged-kv/pkv_main.cpp (selection by id prefix, '=' for an exact id, an
// argument that selects nothing fails the run, exit 0 only when every selected job passed) and on t2791's
// run_red_suite.bat (one executable per cell, artifact flags passed through, a skip fails the run).
//
// Why a process runner and not a registry in one binary: the cells need three engine builds (the
// allocation-fault build, the T2169 chunk-recording build, the default build), and the stranded legs need a
// fresh process each (a stranded ledger is terminal for its process; Sec4.4, te425's `isolate`). So every job
// below is one process, started by this runner; g1d_build_draft.bat builds the executables.
//
// Usage: g1d_main_draft --bin=DIR --qwen3=PATH --r15=PATH --g5=PATH [ID_PREFIX ...] [--list] [--step=G3|G4|G5]
//   --qwen3  qwen3-0.6b.sslm (te425's --qwen3)
//   --r15    qwen2.5-1.5b-instruct.sslm (te425's --r15; square, so the internal-layer legs run on it)
//   --g5     the G5 schema fixture (te425's --g5; carries "shopkeeper_intent_extraction")
//   --step   run only the jobs whose bracket is that step (G3: the G3 parts; G4: adds the G4 parts; G5: all)
// Each job runs with SSLM_GPU_ENABLE_DEBUG_LAYER=1 in its environment.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Job {
	const char* id;    // the cell id and run, e.g. "3.6f/first"
	const char* step;  // the bracket that turns it green
	const char* exe;   // under --bin
	const char* args;  // the run's own arguments
	const char* art;   // which artifact flag becomes --model ("qwen3", "r15", "g5")
};

const Job kJobs[] = {
    // 3.6e (allocation-fault build). The G3-only binary links at G3; the full binary needs G4's upload.
    {"3.6e/g3", "G3", "g1d_cell_3_6e_g3.exe", "--part=g3", "r15"},
    {"3.6e/g4", "G4", "g1d_cell_3_6e.exe", "--part=g4", "r15"},
    // 3.6f (allocation-fault build + te425 hooks). Each run its own process.
    {"3.6f/first", "G3", "g1d_cell_3_6f.exe", "--run=first", "r15"},
    {"3.6f/stranded", "G3", "g1d_cell_3_6f.exe", "--run=stranded", "r15"},
    {"3.6f/waitnoarm", "G3", "g1d_cell_3_6f.exe", "--run=waitnoarm", "r15"},
    // 3.6g recovered (T2169 build), stranded and per-context (allocation-fault build + te425 hooks).
    {"3.6g/recovered", "G3", "g1d_cell_3_6g_recovered.exe", "", "qwen3"},
    {"3.6g/stranded.signal", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.signal", "r15"},
    {"3.6g/stranded.setevent", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.setevent", "r15"},
    {"3.6g/percontext", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=3.6g.percontext", "qwen3"},
    // Sec4.4's nine stranded pins (G3's gate), each alone in a fresh process.
    {"pin/P.setevent1.oom", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent1.oom", "qwen3"},
    {"pin/P.setevent1.fail", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent1.fail", "qwen3"},
    {"pin/P.setevent2.fail", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=P.setevent2.fail", "qwen3"},
    {"pin/D.setevent1.fail", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=D.setevent1.fail", "qwen3"},
    {"pin/R.signal2.oom", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=R.signal2.oom", "qwen3"},
    {"pin/S.setevent1.fail", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=S.setevent1.fail", "g5"},
    {"pin/C.signal1.oom", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=C.signal1.oom", "qwen3"},
    {"pin/M.signal1.oom", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=M.signal1.oom", "qwen3"},
    {"pin/L.setevent1.oom", "G3", "g1d_cell_3_6g_stranded.exe", "--leg=L.setevent1.oom", "r15"},
    // 3.12 (default build). The G4-only binary links at G4; the full binary needs G5's restore-into-pool.
    {"3.12/g4-set", "G4", "g1d_cell_3_12_g4.exe", "--run=g4-set", "r15"},
    {"3.12/g4-cleared", "G4", "g1d_cell_3_12_g4.exe", "--run=g4-cleared", "r15"},
    {"3.12/g5-set", "G5", "g1d_cell_3_12.exe", "--run=g5-set", "r15"},
    {"3.12/g5-cleared", "G5", "g1d_cell_3_12.exe", "--run=g5-cleared", "r15"},
    {"3.12/g5-clear", "G5", "g1d_cell_3_12.exe", "--run=g5-clear", "r15"},
};

bool Matches(const char* id, const std::string& arg) {
	return arg[0] == '=' ? arg.compare(1, std::string::npos, id) == 0 : std::strncmp(id, arg.c_str(), arg.size()) == 0;
}
int StepRank(const std::string& s) { return s == "G3" ? 3 : s == "G4" ? 4 : s == "G5" ? 5 : 99; }

}  // namespace

int main(int argc, char** argv) {
	std::string bin = ".", qwen3, r15, g5, step;
	bool list = false;
	std::vector<std::string> sel;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a.rfind("--bin=", 0) == 0) bin = a.substr(6);
		else if (a.rfind("--qwen3=", 0) == 0) qwen3 = a.substr(8);
		else if (a.rfind("--r15=", 0) == 0) r15 = a.substr(6);
		else if (a.rfind("--g5=", 0) == 0) g5 = a.substr(5);
		else if (a.rfind("--step=", 0) == 0) step = a.substr(7);
		else if (a == "--list") list = true;
		else sel.push_back(a);
	}
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
		bool selected = sel.empty();
		for (size_t i = 0; i < sel.size() && !selected; ++i) selected = Matches(j.id, sel[i]);
		if (!selected) continue;
		if (!step.empty() && StepRank(j.step) > StepRank(step)) continue;
		const std::string& model = std::strcmp(j.art, "qwen3") == 0 ? qwen3 : std::strcmp(j.art, "r15") == 0 ? r15 : g5;
		const std::string cmd = "\"" + bin + "\\" + j.exe + "\" " + j.args + " \"--model=" + model + "\"";
		if (list) {
			std::printf("[%s] %s: %s\n", j.step, j.id, cmd.c_str());
			continue;
		}
		if (model.empty()) {
			std::printf("[%s] %s RED (artifact --%s not given: a skip fails the run)\n", j.step, j.id, j.art);
			++ran;
			++failed;
			continue;
		}
#if defined(_WIN32)
		_putenv("SSLM_GPU_ENABLE_DEBUG_LAYER=1");
		const std::string full = "\"" + cmd + "\"";  // cmd /c strips one pair of outer quotes
#else
		const std::string full = cmd;
#endif
		std::printf("[%s] %s ...\n  %s\n", j.step, j.id, cmd.c_str());
		std::fflush(stdout);
		const int rc = std::system(full.c_str());
		++ran;
		if (rc != 0) ++failed;
		std::printf("[%s] %s %s (exit %d)\n", j.step, j.id, rc == 0 ? "green" : "RED", rc);
	}
	if (list) return 0;
	std::printf("%d jobs, %d red\n", ran, failed);
	if (unmatched) std::printf("%d argument(s) matched no job (named on stderr)\n", unmatched);
	return ran > 0 && unmatched == 0 && failed == 0 ? 0 : 1;
}
