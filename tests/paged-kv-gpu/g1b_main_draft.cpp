// Paged-KV plan (rev 16.2), step G1, group g1b: a DRAFT runner for this group's GPU cells, modelled on
// tests/paged-kv/pkv_main.cpp. The coordinator unifies the groups' drafts into the suite's one runner.
//
// Usage: <exe> --tiny=PATH [--m05=PATH] [--m15=PATH] [--shaders=DIR] [CELL_ID_PREFIX ...]
// With no selector every registered cell runs, except the cells whose id carries a ':' leg suffix
// ("11.4/G3:zerofill"): each of those leaves process state behind it (a terminal context) and runs only
// when named exactly with '=' ("=11.4/G3:zerofill"), in a process of its own. A selector that matches
// no registered cell fails the run (pkv_main.cpp's rule). Exit status 0 only when every check of every
// selected cell passed and at least one cell ran.
#include "g1b_helpers.h"

namespace {
bool IsLeg(const char* id) { return std::strchr(id, ':') != nullptr; }
bool Matches(const char* id, const std::string& arg) {
	if (!arg.empty() && arg[0] == '=') return arg.compare(1, std::string::npos, id) == 0;
	return !IsLeg(id) && std::strncmp(id, arg.c_str(), arg.size()) == 0;
}
}  // namespace

int main(int argc, char** argv) {
	const std::vector<std::string> sel = g1b::ParseArgs(argc, argv);
	int ran = 0, failed_cells = 0, unmatched = 0;
	for (const std::string& a : sel) {
		bool any = false;
		for (const g1b::CellEntry& c : g1b::Registry()) any = any || Matches(c.id, a);
		if (!any) {
			std::fprintf(stderr, "argument '%s' matches no registered cell\n", a.c_str());
			++unmatched;
		}
	}
	for (const g1b::CellEntry& c : g1b::Registry()) {
		bool selected = sel.empty() && !IsLeg(c.id);
		for (const std::string& a : sel) selected = selected || Matches(c.id, a);
		if (!selected) continue;
		g1b::RunState& s = g1b::State();
		s.cell = c.id;
		s.cell_failures = 0;
		const int before = s.checks;
		std::printf("[%s] %s ...\n", c.step, c.id);
		std::fflush(stdout);
		c.fn();
		++ran;
		if (s.cell_failures) ++failed_cells;
		std::printf("[%s] %s %s (%d checks, %d failed)\n", c.step, c.id, s.cell_failures ? "RED" : "green", s.checks - before,
		            s.cell_failures);
		std::fflush(stdout);
	}
	std::printf("%d cells, %d red; %d checks, %d failures\n", ran, failed_cells, g1b::State().checks, g1b::State().failures);
	if (unmatched) std::printf("%d argument(s) matched no registered cell (named on stderr)\n", unmatched);
	return ran > 0 && unmatched == 0 && g1b::State().failures == 0 ? 0 : 1;
}
