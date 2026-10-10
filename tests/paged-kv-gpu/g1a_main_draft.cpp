// Paged-KV plan (rev 16.2) step G1, group g1a: DRAFT runner for the GPU red suite (modelled on
// tests/paged-kv/pkv_main.cpp and tests/t2791-gpu-prefill-read-red-suite). Not wired into any
// CMakeLists (G1 edits no shared file); the per-step executables and their source lists are in
// build/g-cloud/g1a-notes.md.
//
// Usage: superslm_pkv_gpu_<step> [--pkv-fixtures=DIR] [--pkv-reference=DIR] [--model05=PATH]
//                                [--model15=PATH] [--gpu-ref=DIR] [--record-gpu-ref] [CELL ...]
// Run once per certified GPU with SSLM_GPU_ADAPTER_INDEX set (0, 1) and SSLM_GPU_ENABLE_DEBUG_LAYER=1.
// With no CELL every registered cell runs; otherwise cells whose id starts with one of the given
// prefixes ("6.5b", "1.7/G5"); an argument starting with '=' names one cell exactly. An argument
// that selects no registered cell is named on stderr and fails the run. Exit status 0 only when at
// least one cell ran, every argument selected a cell, and every check passed.

#include "g1a_helpers.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
bool Matches(const char* id, const char* arg) {
	return arg[0] == '=' ? std::strcmp(id, arg + 1) == 0 : std::strncmp(id, arg, std::strlen(arg)) == 0;
}
bool Option(const char* arg, const char* name, std::string* out) {
	const size_t n = std::strlen(name);
	if (std::strncmp(arg, name, n) != 0) return false;
	*out = arg + n;
	return true;
}
}  // namespace

int main(int argc, char** argv) {
	g1a::Options& o = g1a::Opt();
	std::vector<const char*> sel;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (Option(a, "--pkv-fixtures=", &o.fixture_dir) || Option(a, "--pkv-reference=", &o.reference_dir) ||
		    Option(a, "--model05=", &o.model05) || Option(a, "--model15=", &o.model15) ||
		    Option(a, "--gpu-ref=", &o.gpu_ref_dir))
			continue;
		if (std::strcmp(a, "--record-gpu-ref") == 0) {
			o.record_gpu_ref = true;
			continue;
		}
		sel.push_back(a);
	}
	int unmatched = 0;
	for (const char* a : sel) {
		bool any = false;
		for (const g1a::CellEntry& c : g1a::Registry()) any = any || Matches(c.id, a);
		if (!any) {
			std::fprintf(stderr, "argument '%s' matches no registered cell\n", a);
			++unmatched;
		}
	}
	int ran = 0, failed_cells = 0;
	for (const g1a::CellEntry& c : g1a::Registry()) {
		bool selected = sel.empty();
		for (size_t i = 0; i < sel.size() && !selected; ++i) selected = Matches(c.id, sel[i]);
		if (!selected) continue;
		g1a::RunState& s = g1a::State();
		s.cell = c.id;
		s.cell_failures = 0;
		const int before = s.checks;
		std::printf("[%s] %s ...\n", c.step, c.id);
		std::fflush(stdout);
		c.fn();
		++ran;
		if (s.cell_failures) ++failed_cells;
		std::printf("[%s] %s %s (%d checks, %d failed)\n", c.step, c.id, s.cell_failures ? "RED" : "green",
		            s.checks - before, s.cell_failures);
		std::fflush(stdout);
	}
	std::printf("%d cells, %d red; %d checks, %d failures\n", ran, failed_cells, g1a::State().checks,
	            g1a::State().failures);
	if (unmatched) std::printf("%d argument(s) matched no registered cell (named on stderr)\n", unmatched);
	return ran > 0 && unmatched == 0 && g1a::State().failures == 0 ? 0 : 1;
}
