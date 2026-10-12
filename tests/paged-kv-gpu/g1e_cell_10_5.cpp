// Paged-KV plan (rev 16.2) §7 cell 10.5 [G6]: "GPU twin of 10.4 on both certified GPUs." 10.4 (rev 2; rev 16.2):
// "Create a page pool of exactly 1,842 pages at 0.5B and cap 4096, the §5 figure. The order is: world, then 10
// personas via begin_from, then release the world handle, then 50 budget-512 sequences, each running to its budget.
// Every create, begin_from and adopt succeeds. The executed peak is 1,842. [...] The suite checks 203 as its own
// leg, in the all-open order [...]. A pool of 1,841 pages refuses exactly one create, the 50th, with
// SSLM_KV_POOL_EXHAUSTED. Contrast run: a v1.9.0 block pool in the same bytes admits floor(bytes / block) = 7
// sequences. Nothing in the verdict reads the pool's own stats." Verdict provenance (§7): GPU statuses; the same
// formulas. Group g1e (one cell per executable), written at G1 (§9's G1 row), bracketed G6.
//
// Legs, in order (the CPU twin is tests/paged-kv/c6_dim10_cohort.cpp Cell104On and Peak104):
//   (a) 1,842 pages: the cohort in §5's order, each sequence running to its budget (513 tokens, then
//       SSLM_GPU_KV_BUDGET_EXCEEDED) before the next create: every create, begin_from and adopt is admitted.
//       Kills: any draw beyond §3.4's (an adopt that draws, a begin_from that copies the world, a decode that maps
//       outside the reservation, freeze keeping a spare page).
//   (b) 1,841 pages, admission only (no decode: the verdict is the creates'): exactly one create is refused, the
//       50th, with SSLM_GPU_KV_POOL_EXHAUSTED. Kills: a reservation one page short anywhere in the cohort.
//   (c) the peak, all ten personas open (every begin_from before any persona is filled): a pool of 203 admits
//       them, and after the construction (personas filled and frozen, the world released) the GPU fill probe's
//       one-state form (§8) shows exactly 203 - 192 = 11 pages free; a pool of 202 refuses the tenth begin_from,
//       exhausted. Kills: a begin_from drawing more or less than R(200), freeze not returning a persona's spare
//       reserve page, the world's release keeping its unshared tail page or freeing a page the personas share.
//   (d) the contrast. The GPU has no block pool: a v1.12.0-style GPU sequence (sslm_gpu_seq_create) owns a
//       dedicated buffer of the whole cap. Its block is read from the GPU itself -- the workspace_size field of
//       the dedicated sequence's own SLM5 save -- and asserted equal to §3.1's formula (ceil(cap/B) pages of
//       page_bytes); then floor(1,842 pages' bytes / block) = 7. (Reading taken: §7's "v1.9.0 block pool" has
//       no GPU twin, so the contrast's operand is the GPU's own dedicated block size.)
// Nothing reads sslm_gpu_kv_pool_stats.
//
// Legs (g1e_cohort.h): --leg=fixture (pkv_def; runs on the software GPU) and --leg=box (the real 0.5B; box-only).
// Run once per certified GPU (SSLM_GPU_ADAPTER_INDEX=0, then 1) with SSLM_GPU_ENABLE_DEBUG_LAYER=1.
//
// Run: g1e_cell_10_5.exe --leg=fixture --fixture-dir=DIR     |     g1e_cell_10_5.exe --leg=box --model05=PATH
#include "g1e_cohort.h"

using namespace g1e;
using namespace g1e::cohort;

namespace {

// The GPU fill probe's one-state form (§8, "Precondition, and the one-state form"; the CPU's
// ProbeExactlyFreeOneState), for k >= 3, on the one state `s` holds: creates totalling k - 1 are admitted (at least
// k - 1 free); a 2-page create is refused for capacity (at most k free); one admitted create of r pages is released
// and creates totalling r + 1 are admitted (at least k were free). Every create is released again.
bool ProbeExactlyFreeOneState(const Env& e, ProbeState& s, int64_t k, const char* what) {
	CHECK_MSG(k >= 3, "%s: the one-state probe needs k >= 3 (k = %lld)", what, static_cast<long long>(k));
	if (k < 3) return false;
	s.DrainAll();
	std::vector<SslmGpuSequenceHandle*> made;
	Refusal first;
	const bool step1 = AdmitPages(e.geo, s, k - 1, &made, &first);
	CHECK_MSG(step1, "%s: one-state fill probe at k=%lld: creates totalling %lld pages were not all admitted (a %lld-page create: %s)",
	          what, static_cast<long long>(k), static_cast<long long>(k - 1), static_cast<long long>(first.pages),
	          StatusName(first.status));
	SslmGpuSequenceHandle* two = nullptr;
	const GS st2 = sslm_gpu_seq_create_budgeted(s.Ctx(), s.Model(), s.Pool(), static_cast<int32_t>(e.geo.B), &two);
	if (two) sslm_gpu_seq_release(s.Ctx(), two);
	const bool step2 = st2 == GS::SSLM_GPU_KV_POOL_EXHAUSTED;
	CHECK_MSG(step2, "%s: one-state fill probe at k=%lld: after %lld pages, a 2-page create returned %s, not exhausted", what,
	          static_cast<long long>(k), static_cast<long long>(k - 1), StatusName(st2));
	bool step3 = false;
	if (step1 && !made.empty()) {
		const int64_t r = ProbeSplit(e.geo, k - 1).back();
		sslm_gpu_seq_release(s.Ctx(), made.back());
		made.pop_back();
		step3 = AdmitPages(e.geo, s, r + 1, &made, &first);
		CHECK_MSG(step3, "%s: one-state fill probe at k=%lld: after releasing a %lld-page create, creates totalling %lld were not all "
		                 "admitted (%s)",
		          what, static_cast<long long>(k), static_cast<long long>(r), static_cast<long long>(r + 1), StatusName(first.status));
	}
	for (auto it = made.rbegin(); it != made.rend(); ++it) sslm_gpu_seq_release(s.Ctx(), *it);
	return step1 && step2 && step3;
}

void PeakLeg(const Env& e, const Figures& f, const std::string& stem) {
	const int64_t k = f.peak - ChainPages(e.geo, kPersonas);  // 203 - 192 = 11 at B = 16
	{
		Rig rig(e, static_cast<uint32_t>(f.peak));
		if (!rig.pool) return;
		int refused = -1;
		GS refusal = GS::SSLM_OK;
		const GWorld w = BuildWorld(rig, /*all_open=*/true, &refused, &refusal);
		// kills: a begin_from that draws more than R(200) or copies the world's pages (refused at 203)
		CHECK_MSG(refused == -1 && w.ok, "10.5 peak %s: a pool of %lld refused begin_from %d (%s)", stem.c_str(),
		          static_cast<long long>(f.peak), refused, StatusName(refusal));
		// kills: freeze not returning a persona's spare reserve page; the world's release keeping its unshared tail
		// page (fewer free), or freeing a page the personas share (more free)
		if (w.ok) ProbeExactlyFreeOneState(e, rig, k, ("10.5 peak " + stem).c_str());
	}
	{
		Rig rig(e, static_cast<uint32_t>(f.peak - 1));
		if (!rig.pool) return;
		int refused = -1;
		GS refusal = GS::SSLM_OK;
		(void)BuildWorld(rig, /*all_open=*/true, &refused, &refusal);
		// kills: a begin_from that draws less than R(200) (the tenth admitted one page below the peak)
		CHECK_MSG(refused == kPersonas - 1 && refusal == GS::SSLM_GPU_KV_POOL_EXHAUSTED,
		          "10.5 peak %s: a pool of %lld refused begin_from %d (%s); want the tenth (index 9), exhausted", stem.c_str(),
		          static_cast<long long>(f.peak - 1), refused, StatusName(refusal));
	}
}

void ContrastLeg(const Env& e, const Figures& f, const std::string& stem) {
	SslmGpuSequenceHandle* h = nullptr;
	const GS cs = sslm_gpu_seq_create(e.ctx, e.model(), e.geo.cap, &h);
	CHECK_MSG(cs == GS::SSLM_OK && h, "10.5 contrast %s: sslm_gpu_seq_create(cap %lld): %s", stem.c_str(),
	          static_cast<long long>(e.geo.cap), StatusName(cs));
	if (cs != GS::SSLM_OK || !h) return;
	GSeq s;
	s.h = h;
	const GS ps = Prefill(e, s, PrefixTokens(1));  // one token, so the save is of a live sequence
	CHECK_MSG(ps == GS::SSLM_OK, "10.5 contrast %s: one-token prefill of the dedicated sequence: %s", stem.c_str(), StatusName(ps));
	const std::vector<uint8_t> blob = ps == GS::SSLM_OK ? Save(e, h) : std::vector<uint8_t>{};
	sslm_gpu_seq_release(e.ctx, h);
	CHECK_MSG(IsMagic(blob, "SLM5"), "10.5 contrast %s: a dedicated GPU sequence must save 'SLM5'", stem.c_str());
	if (!IsMagic(blob, "SLM5")) return;
	const uint64_t block = Le64(blob, 48);  // GpuSeqBlobHeader.workspace_size: the dedicated K/V buffer
	const uint64_t formula = static_cast<uint64_t>(e.geo.CapPages()) * e.geo.PageBytes();
	// kills: a contrast computed from a block size the GPU does not allocate
	CHECK_MSG(block == formula, "10.5 contrast %s: the dedicated sequence's block is %llu bytes; §3.1's ceil(cap/B) * page_bytes is %llu",
	          stem.c_str(), static_cast<unsigned long long>(block), static_cast<unsigned long long>(formula));
	const uint64_t bytes = static_cast<uint64_t>(f.pool) * e.geo.PageBytes();
	const uint64_t blocks = block ? bytes / block : 0;
	CHECK_MSG(static_cast<int64_t>(blocks) == f.contrast, "10.5 contrast %s: %llu bytes hold %llu dedicated blocks; the plan says %lld",
	          stem.c_str(), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(blocks),
	          static_cast<long long>(f.contrast));
	std::printf("10.5 contrast %s: %lld pages = %llu bytes; a dedicated cap-%lld block = %llu bytes; %llu blocks fit, against %d "
	            "cohort sequences paged\n",
	            stem.c_str(), static_cast<long long>(f.pool), static_cast<unsigned long long>(bytes),
	            static_cast<long long>(e.geo.cap), static_cast<unsigned long long>(block), static_cast<unsigned long long>(blocks), kSeqs);
}

void Cell105(const Args& a) {
	const std::string path = ModelPath(a, "10.5");
	if (path.empty()) return;
	Env e;
	if (!e.Open(path)) return;
	const Figures* f = FiguresFor(e.geo, "10.5");
	if (!f) return;
	const std::string stem = Stem(path);
	{  // (a)
		const CohortRun r = RunCohort(e, static_cast<uint32_t>(f->pool), /*drive=*/true, /*keep_blobs=*/false, "10.5 (a)");
		CHECK_MSG(AdmissionAsExpected(r, -1), "10.5 (a) %s: a pool of %lld: built %d, %d creates refused (the first at %d, %s), "
		                                      "%d adopts refused",
		          stem.c_str(), static_cast<long long>(f->pool), r.built ? 1 : 0, r.creates_refused, r.first_refused,
		          StatusName(r.refusal), r.adopts_refused);
		size_t full = 0;
		for (size_t i = 0; i < r.tokens.size(); ++i)
			full += (r.tokens[i].size() == kTokens && r.end[i] == GS::SSLM_GPU_KV_BUDGET_EXCEEDED) ? 1 : 0;
		CHECK_MSG(r.tokens.size() == static_cast<size_t>(kSeqs) && full == r.tokens.size(),
		          "10.5 (a) %s: %zu of %zu sequences ran to their budget (513 tokens, then SSLM_GPU_KV_BUDGET_EXCEEDED); want 50",
		          stem.c_str(), full, r.tokens.size());
	}
	{  // (b)
		const CohortRun r = RunCohort(e, static_cast<uint32_t>(f->pool - 1), /*drive=*/false, /*keep_blobs=*/false, "10.5 (b)");
		CHECK_MSG(AdmissionAsExpected(r, kSeqs - 1),
		          "10.5 (b) %s: a pool of %lld refused %d creates, the first at index %d (%s), %d adopts refused; want exactly "
		          "the 50th (index 49), exhausted",
		          stem.c_str(), static_cast<long long>(f->pool - 1), r.creates_refused, r.first_refused, StatusName(r.refusal),
		          r.adopts_refused);
	}
	PeakLeg(e, *f, stem);      // (c)
	ContrastLeg(e, *f, stem);  // (d)
}

}  // namespace

int main(int argc, char** argv) {
	const Args a = ParseArgs(argc, argv);
	Cell105(a);
	return FinishSuite(a.leg == "box" ? "g1e_cell_10_5/box" : "g1e_cell_10_5/fixture");
}
