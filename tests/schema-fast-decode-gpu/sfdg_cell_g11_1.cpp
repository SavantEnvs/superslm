// Schema fast-decode plan rev 1.4.4 §11.2, cell G11.1 (slice G1-J): GPU pending admission walks the pending token
// zero times, and jump-forward from a ready seed and after a dead end embeds nothing twice.
//   (a) A token in hand: the verb's walk state, context length and blob equal the probe's composition (the prompt
//       bridge for the pending token, which does not walk it, then the schema-content bridge for the chain). A verb
//       that walked the pending token again (XG2, XG8) leaves another state or admits nothing.
//   (b) A ready seed (after the prompt): the verb admits the start chain over the ready row, which it never
//       finishes or embeds again: context = prompt + chain, and the blob equals the schema-content bridge's.
//   (c) After a dead end (the -2 finish re-arms the ready flag): the verb with -1 admits nothing and changes
//       nothing, and the next decode gives -2 again at the same context length.
// Box only. Run: sfdg_cell_g11_1.exe [--fixture-dir=DIR]
#include "sfdg_helpers.h"

using namespace sfdg;

int main(int argc, char** argv) {
	GRig rig;
	if (rig.Open(argc, argv, /*load_ref=*/false)) {
		CRig c(rig.raw);
		const std::vector<int32_t> prompt = c.Tokenize(kPromptPotionText);
		for (const char* schema : {kPotion, kUtf8Enum}) {
			// (b) the ready seed.
			GSeq a = rig.New(schema), b = rig.New(schema);
			if (!a.h || !b.h) continue;
			rig.Prompt(a, prompt);
			rig.Prompt(b, prompt);
			const GQuery q0 = QueryGpu(rig.ctx(), a.h);
			const JumpOut j0 = Jump(rig, a, -1, 64);
			Compose(rig, b, -1, q0.ids);
			CHECK_MSG(j0.st == GS::SSLM_OK && j0.forced == q0.ids && CtxLen(a.h) == int64_t(prompt.size() + q0.ids.size()),
			          "[G11.1 (b)] %s: %s, forced %s (query %s), context %lld", schema, Name(j0.st), Str(j0.forced).c_str(),
			          Str(q0.ids).c_str(), static_cast<long long>(CtxLen(a.h)));
			CHECK_MSG(Observe(rig, a.h) == Observe(rig, b.h), "[G11.1 (b)] %s: the verb's sequence differs from the "
			          "schema-content bridge's", schema);
			// (a) a token in hand on both.
			GS st = GS::SSLM_OK;
			const int32_t t1 = Token(rig.env, a, &st);
			const int32_t t1b = Token(rig.env, b, &st);
			CHECK_MSG(t1 >= 0 && t1 == t1b, "[G11.1 (a)] %s: the choice %d and %d", schema, t1, t1b);
			if (t1 < 0) continue;
			const uint32_t walk_after_choice = SslmGpuSeqWalkStateForG5Bridge(a.h);
			const GQuery q1 = QueryGpu(rig.ctx(), a.h);
			CHECK_MSG(q1.count > 0, "[G11.1 (a)] %s: precondition: the choice %d leads to a forced state", schema, t1);
			if (q1.count <= 0) continue;
			const int64_t ctx0 = CtxLen(a.h);
			const JumpOut j1 = Jump(rig, a, t1, 64);
			Compose(rig, b, t1, q1.ids);
			CHECK_MSG(j1.st == GS::SSLM_OK && j1.forced == q1.ids, "[G11.1 (a)] %s: %s, forced %s, the chain from state "
			          "%u is %s", schema, Name(j1.st), Str(j1.forced).c_str(), walk_after_choice, Str(q1.ids).c_str());
			CHECK_MSG(CtxLen(a.h) == ctx0 + 1 + int64_t(q1.ids.size()), "[G11.1 (a)] %s: context %lld, want %lld", schema,
			          static_cast<long long>(CtxLen(a.h)), static_cast<long long>(ctx0 + 1 + int64_t(q1.ids.size())));
			const SeqObs oa = Observe(rig, a.h), ob = Observe(rig, b.h);
			CHECK_MSG(oa == ob, "[G11.1 (a)] %s: the verb's walk %u context %lld, the composition's walk %u context %lld "
			          "(blobs %s)", schema, oa.walk, static_cast<long long>(oa.ctx), ob.walk, static_cast<long long>(ob.ctx),
			          oa.blob == ob.blob ? "equal" : "differ");
		}
		// (c) after a dead end.
		GSeq d = rig.New(kPotion);
		if (d.h) {
			rig.Prompt(d, prompt);
			const GJfRun r = RunGpuJf(rig, d, 64);
			CHECK_MSG(r.dead_end, "[G11.1 (c)] the loop ended with %s and no dead end", Name(r.end));
			if (r.dead_end) {
				const SeqObs before = Observe(rig, d.h);
				CHECK_MSG(before.ready == 1, "[G11.1 (c)] the dead end left the ready flag %d", before.ready);
				const JumpOut j = Jump(rig, d, -1, 64);
				CHECK_MSG(j.st == GS::SSLM_OK && j.count == 0 && Observe(rig, d.h) == before,
				          "[G11.1 (c)] the verb after the dead end: %s, %d forced, sequence %s", Name(j.st), j.count,
				          Observe(rig, d.h) == before ? "unchanged" : "changed");
				GS st = GS::SSLM_OK;
				const int32_t t = Token(rig.env, d, &st);
				CHECK_MSG(st == GS::SSLM_OK && t == -2 && CtxLen(d.h) == before.ctx,
				          "[G11.1 (c)] the decode after: %s token %d, context %lld (was %lld)", Name(st), t,
				          static_cast<long long>(CtxLen(d.h)), static_cast<long long>(before.ctx));
			}
		}
		rig.ReleaseSeqs();
	}
	return FinishSuite("sfdg_cell_g11_1");
}
