// Paged-KV plan (rev 16.2), G1, group g1c: DRAFT runner for the GPU red suite's cells 3.6-3.6d,
// modelled on tests/paged-kv/pkv_main.cpp. The coordinator unifies the groups' drafts into one
// runner; until then each g1c executable links this file with its own cell translation units:
//
//   g1c_default.exe   g1c_cell_3_6.cpp g1c_cell_3_6b.cpp g1c_cell_3_6c.cpp g1c_cell_3_6d.cpp
//                     g1c_debug_layer.cpp g1c_main_draft.cpp           (the default GPU build)
//   g1c_bench.exe     g1c_cell_3_6d_bench.cpp g1c_debug_layer.cpp g1c_main_draft.cpp
//                     (the bench configuration build: engine and cells compiled with
//                      SUPERSLM_ENABLE_GPU_BENCH_PRE_BATCHING, as build.bat:184 compiles its one tool)
//   g1c_firsttouch.exe g1c_cell_3_6d_first_touch.cpp g1c_main_draft.cpp, plus
//                     tests/te425-gpu-alloc-status-red-suite/te425_cells.cpp compiled with
//                     /Dmain=te425_cells_main_unused (for te425::InstallHooks, its hooks and its operator
//                     new family) and te425_getdevice.cpp (/I src\gpu; te425_cells.obj references it), engine and cells compiled with SUPERSLM_GPU_ALLOC_FAULT_INJECTION and
//                     SUPERSLM_GPU_TEST_SEAMS. One cell; run it alone, in a fresh process.
//
// Usage: g1c_<build>.exe [--model=PATH] [--adapter=PATH] [--adapter-other-base=PATH] [--schema=NAME]
//                        [--pool-pages=N] [--allow-skip] [CELL_ID_PREFIX ... | =CELL_ID ...]
// Exit 0 only when every check of every selected cell passed, nothing was skipped (unless
// --allow-skip), at least one cell ran, and every selector matched a registered cell.
#include "g1c_helpers.h"

#include <cstdio>
#include <cstring>

namespace {
bool Matches(const char* id, const std::string& arg) {
	return arg[0] == '=' ? std::strcmp(id, arg.c_str() + 1) == 0 : std::strncmp(id, arg.c_str(), arg.size()) == 0;
}
}  // namespace

int main(int argc, char** argv) {
	g1c::ParseOptions(argc, argv);
	const g1c::Options& o = g1c::Opt();
	int unmatched = 0;
	for (const std::string& sel : o.selectors) {
		bool any = false;
		for (const g1c::CellEntry& c : g1c::Registry()) any = any || Matches(c.id, sel);
		if (!any) {
			std::fprintf(stderr, "argument '%s' matches no registered cell\n", sel.c_str());
			++unmatched;
		}
	}
	int ran = 0, red = 0;
	for (const g1c::CellEntry& c : g1c::Registry()) {
		bool selected = o.selectors.empty();
		for (const std::string& sel : o.selectors) selected = selected || Matches(c.id, sel);
		if (!selected) continue;
		g1c::RunState& s = g1c::State();
		s.cell = c.id;
		s.cell_failures = 0;
		const int before = s.checks, skips_before = s.skips;
		std::printf("[%s] %s ...\n", c.step, c.id);
		std::fflush(stdout);
		c.fn();
		++ran;
		const bool skipped = s.skips > skips_before;
		if (s.cell_failures || (skipped && !o.allow_skip)) ++red;
		std::printf("[%s] %s %s (%d checks, %d failed%s)\n", c.step, c.id,
		            s.cell_failures ? "RED" : (skipped ? "SKIPPED" : "green"), s.checks - before, s.cell_failures,
		            skipped ? ", skipped" : "");
		std::fflush(stdout);
	}
	const g1c::RunState& s = g1c::State();
	std::printf("g1c: %d cells, %d red; %d checks, %d failures, %d skips%s\n", ran, red, s.checks, s.failures, s.skips,
	            s.skips && o.allow_skip ? " (allowed by --allow-skip: not an acceptance run)" : "");
	const bool pass = ran > 0 && unmatched == 0 && s.failures == 0 && (s.skips == 0 || o.allow_skip);
	return pass ? 0 : 1;
}
