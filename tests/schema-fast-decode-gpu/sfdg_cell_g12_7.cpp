// Schema fast-decode plan rev 1.4.4 §11.2, cell G12.7 (slice G1-J; rev 1.4.1, review L9, audit G-2): canonical
// failure. S-GCHUNKFAULT fires at sub-chunk 0 and at sub-chunk 1 of a canonical call, with
// SSLM_GPU_ALLOCATION_FAILED and with SSLM_DEVICE_LOST, on a dedicated and on a pooled sequence. The verb returns
// that status and commits nothing: everything in its entry snapshot equals the entry -- context length, walk, the
// five counters, the hidden state, layer index and ready flag (the blob holds them all, and the dedicated host
// mirror), the pooled holder's mapped pages, and sslm_gpu_seq_read_prefill_final_hidden, which is valid at entry
// because the call starts after a prefill bridge (rev 1.4.2, strike N5; XG30). The call is LONGKEY's start span at
// chunk budget 8: two sub-chunks, so a fault at sub-chunk 1 comes after sub-chunk 0 committed four rows on the
// device, which the exit must not keep (XG19). After the failure, an off decode equals GPU off's, and a canonical
// call equals a fresh one's. Box only. Run: sfdg_cell_g12_7.exe [--fixture-dir=DIR]
#include "sfdg_helpers.h"

using namespace sfdg;

namespace {

SslmGpuKvPoolHandle* g_pool = nullptr;

GSeq NewSeq(GRig& rig, bool pooled) {
	if (!pooled) return rig.New(kLongKey);
	GSeq s;
	const GS st = sslm_gpu_seq_create_budgeted(rig.ctx(), rig.model(), g_pool, 200, &s.h);
	CHECK_MSG(st == GS::SSLM_OK && s.h, "[G12.7] budgeted create: %s", Name(st));
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
		const std::vector<int32_t> prompt = c.Tokenize(kPromptPotionText);
		const GS ps = sslm_gpu_kv_pool_create(rig.ctx(), rig.model(), 64, &g_pool);
		CHECK_MSG(ps == GS::SSLM_OK && g_pool, "[G12.7] pool: %s", Name(ps));
		for (const bool pooled : {false, true}) {
			if (pooled && !g_pool) continue;
			for (const GS fault : {GS::SSLM_GPU_ALLOCATION_FAILED, GS::SSLM_DEVICE_LOST}) {
				for (const int32_t at : {0, 1}) {
					char arm[96];
					std::snprintf(arm, sizeof arm, "%s, %s at sub-chunk %d", pooled ? "pooled" : "dedicated", Name(fault), at);
					GSeq s = NewSeq(rig, pooled), off = NewSeq(rig, pooled), fresh = NewSeq(rig, pooled);
					if (!s.h || !off.h || !fresh.h) continue;
					for (GSeq* q : {&s, &off, &fresh}) rig.Prompt(*q, prompt);
					CHECK_MSG(QueryGpu(rig.ctx(), s.h, 0).count >= 8, "[G12.7] precondition: LONGKEY's start chain >= 8");
					const SeqObs before = Observe(rig, s.h);
					const SnapRead snap = ReadSnapshot(rig.ctx(), s.h);
					CHECK_MSG(snap.st == GS::SSLM_OK, "[G12.7 %s] precondition: a valid snapshot at entry (%s)", arm,
					          Name(snap.st));
					CHECK_MSG(sslm_gpu_test_only_arm_chunk_fault(at, fault) == GS::SSLM_OK, "[G12.7] arm");
					const JumpOut j = Jump(rig, s, -1, 8);
					sslm_gpu_test_only_arm_chunk_fault(-1, GS::SSLM_OK);
					CHECK_MSG(j.st == fault && j.count == 0, "[G12.7 %s] the verb returned %s with %d forced", arm, Name(j.st),
					          j.count);
					const SeqObs after = Observe(rig, s.h);
					CHECK_MSG(after == before, "[G12.7 %s] the entry state was not restored: context %lld -> %lld, walk %u -> "
					          "%u, ready %d -> %d, layer %u -> %u, mapped %u -> %u, blob %s", arm,
					          static_cast<long long>(before.ctx), static_cast<long long>(after.ctx), before.walk, after.walk,
					          before.ready, after.ready, before.layer, after.layer, before.mapped, after.mapped,
					          after.blob == before.blob ? "equal" : "differs");
					CHECK_MSG(ReadSnapshot(rig.ctx(), s.h) == snap, "[G12.7 %s] the prefill snapshot differs from the entry's "
					          "(%s)", arm, Name(ReadSnapshot(rig.ctx(), s.h).st));
					// An off decode with the same token equals GPU off's (XG19), and a canonical call a fresh one's.
					GSeq s2 = s;
					GS st = GS::SSLM_OK, st_off = GS::SSLM_OK;
					const int32_t t = Token(rig.env, s2, &st), t_off = Token(rig.env, off, &st_off);
					CHECK_MSG(st == st_off && t == t_off && rig.Blob(s.h) == rig.Blob(off.h),
					          "[G12.7 %s] the decode after: %s %d; GPU off %s %d", arm, Name(st), t, Name(st_off), t_off);
					GSeq s3 = NewSeq(rig, pooled);
					if (s3.h) {
						rig.Prompt(s3, prompt);
						CHECK_MSG(sslm_gpu_test_only_arm_chunk_fault(at, fault) == GS::SSLM_OK, "[G12.7] arm");
						(void)Jump(rig, s3, -1, 8);
						sslm_gpu_test_only_arm_chunk_fault(-1, GS::SSLM_OK);
						const JumpOut a = Jump(rig, s3, -1, 8), b = Jump(rig, fresh, -1, 8);
						CHECK_MSG(a.st == GS::SSLM_OK && a.forced == b.forced && Observe(rig, s3.h) == Observe(rig, fresh.h),
						          "[G12.7 %s] the canonical call after the failure differs from a fresh one's", arm);
					}
					rig.ReleaseSeqs();
				}
			}
		}
		rig.ReleaseSeqs();
		if (g_pool) sslm_gpu_kv_pool_destroy(rig.ctx(), g_pool);
	}
	return FinishSuite("sfdg_cell_g12_7");
}
