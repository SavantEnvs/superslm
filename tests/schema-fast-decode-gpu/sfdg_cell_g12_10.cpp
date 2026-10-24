// Schema fast-decode plan rev 1.4.4 §11.2, cell G12.10 (slice G1-J; engine code review B1): a host allocation
// failure inside the verb's pass restores the entry state. G12.7 covers a failure the pass returns as a status; this
// cell covers one it throws. S-GJUMPALLOC throws std::bad_alloc on the host at one of three sites of a canonical
// call, after the entry snapshot is taken:
//   site 0  before the chunk pre-scan, with the prefill snapshot's flag already cleared;
//   site 1  after the pooled holder's pages are mapped through the admitted rows;
//   site 2  after the pass's sub-chunks committed on the device, before the walk and the ready flag move.
// The verb must return SSLM_GPU_ALLOCATION_FAILED with nothing admitted and everything in its entry snapshot as at
// entry: context length, walk, ready flag, layer, the pooled holder's mapped page count, the blob, and
// sslm_gpu_seq_read_prefill_final_hidden (valid at entry, because the call starts after a prefill bridge). After it,
// a canonical call equals a fresh sequence's. Dedicated and pooled; LONGKEY's start span at chunk budget 8. The
// prompt is sized so that the 8 rows cross a page boundary, so site 1 leaves a page mapped that the exit must
// unmap. Before the fix the throw reached the API boundary, which returns the same status without the restore.
// Box only. Run: sfdg_cell_g12_10.exe [--fixture-dir=DIR]
#include "sfdg_helpers.h"

using namespace sfdg;

namespace {

SslmGpuKvPoolHandle* g_pool = nullptr;

GSeq NewSeq(GRig& rig, bool pooled) {
	if (!pooled) return rig.New(kLongKey);
	GSeq s;
	const GS st = sslm_gpu_seq_create_budgeted(rig.ctx(), rig.model(), g_pool, 200, &s.h);
	CHECK_MSG(st == GS::SSLM_OK && s.h, "[G12.10] budgeted create: %s", Name(st));
	if (!s.h) return s;
	rig.seqs.push_back(s.h);
	SslmGpuSeqSetSchemaForG5Bridge(rig.ctx(), s.h, rig.Index(kLongKey));
	return s;
}

}  // namespace

int main(int argc, char** argv) {
	GRig rig;
	if (rig.Open(argc, argv, /*load_ref=*/false)) {
		CRig c(rig.raw);
		const std::vector<int32_t> base = c.Tokenize(kPromptPotionText);
		// A prompt ending two positions before a page boundary: 8 admitted rows map one more page.
		const int64_t B = rig.env.geo.B > 0 ? rig.env.geo.B : 16;
		const std::vector<int32_t> prompt = PromptOfLength(base, static_cast<size_t>(3 * B - 2));
		const GS ps = sslm_gpu_kv_pool_create(rig.ctx(), rig.model(), 64, &g_pool);
		CHECK_MSG(ps == GS::SSLM_OK && g_pool, "[G12.10] pool: %s", Name(ps));
		for (const bool pooled : {false, true}) {
			if (pooled && !g_pool) continue;
			for (const int32_t site : {0, 1, 2}) {
				char arm[64];
				std::snprintf(arm, sizeof arm, "%s, site %d", pooled ? "pooled" : "dedicated", site);
				GSeq s = NewSeq(rig, pooled), fresh = NewSeq(rig, pooled), twin = NewSeq(rig, pooled);
				if (!s.h || !fresh.h || !twin.h) continue;
				for (GSeq* q : {&s, &fresh, &twin}) rig.Prompt(*q, prompt);
				CHECK_MSG(QueryGpu(rig.ctx(), s.h, 0).count >= 8, "[G12.10] precondition: LONGKEY's start chain >= 8");
				if (pooled) {
					// The precondition for site 1: a successful call maps at least one page.
					const SeqObs t0 = Observe(rig, twin.h);
					const JumpOut tj = Jump(rig, twin, -1, 8);
					CHECK_MSG(tj.st == GS::SSLM_OK && Observe(rig, twin.h).mapped > t0.mapped,
					          "[G12.10 %s] precondition: the call maps a page (%s, mapped %u -> %u)", arm, Name(tj.st),
					          t0.mapped, Observe(rig, twin.h).mapped);
				}
				const SeqObs before = Observe(rig, s.h);
				const SnapRead snap = ReadSnapshot(rig.ctx(), s.h);
				CHECK_MSG(snap.st == GS::SSLM_OK, "[G12.10 %s] precondition: a valid snapshot at entry (%s)", arm,
				          Name(snap.st));
				CHECK_MSG(sslm_gpu_test_only_arm_jump_host_bad_alloc(site) == GS::SSLM_OK, "[G12.10] arm");
				const JumpOut j = Jump(rig, s, -1, 8);
				sslm_gpu_test_only_arm_jump_host_bad_alloc(-1);
				CHECK_MSG(j.st == GS::SSLM_GPU_ALLOCATION_FAILED && j.count == 0,
				          "[G12.10 %s] the verb returned %s with %d forced", arm, Name(j.st), j.count);
				const SeqObs after = Observe(rig, s.h);
				CHECK_MSG(after == before, "[G12.10 %s] the entry state was not restored: context %lld -> %lld, walk %u -> "
				          "%u, ready %d -> %d, layer %u -> %u, mapped %u -> %u, blob %s", arm,
				          static_cast<long long>(before.ctx), static_cast<long long>(after.ctx), before.walk, after.walk,
				          before.ready, after.ready, before.layer, after.layer, before.mapped, after.mapped,
				          after.blob == before.blob ? "equal" : "differs");
				const SnapRead snap_after = ReadSnapshot(rig.ctx(), s.h);
				CHECK_MSG(snap_after == snap, "[G12.10 %s] the prefill snapshot differs from the entry's (%s)", arm,
				          Name(snap_after.st));
				// The sequence decodes as if the call had not been made: a canonical call equals a fresh one's.
				const JumpOut a = Jump(rig, s, -1, 8), b = Jump(rig, fresh, -1, 8);
				CHECK_MSG(a.st == GS::SSLM_OK && b.st == GS::SSLM_OK && a.forced == b.forced &&
				              Observe(rig, s.h) == Observe(rig, fresh.h),
				          "[G12.10 %s] the canonical call after the failure differs from a fresh one's (%s, %s)", arm,
				          Name(a.st), Name(b.st));
				rig.ReleaseSeqs();
			}
		}
		rig.ReleaseSeqs();
		if (g_pool) sslm_gpu_kv_pool_destroy(rig.ctx(), g_pool);
	}
	return Finish("sfdg_cell_g12_10");
}
