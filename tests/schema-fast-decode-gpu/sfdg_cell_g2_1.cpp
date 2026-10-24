// Schema fast-decode plan rev 1.4.4 §11.2, cell G2.1 (slice G1-F; rev 1.4.1, review L6): a base view without a
// tokenizer maps on the GPU with no forced-next table. The query returns SSLM_GPU_FORCED_TABLE_ABSENT (the GPU
// enum has no artifact refusal), and schema-bound decode still works. G1-J adds the verb's arm (the same status)
// to this cell.
//
// The construction: F-SFD's own view with has_tokenizer cleared, which is what the GPU map reads to decide
// (an artifact without TOK1 loads with has_tokenizer false). Box only. Run: sfdg_cell_g2_1.exe [--fixture-dir=DIR]
#include "sfdg_helpers.h"

using namespace sfdg;

int main(int argc, char** argv) {
	GRig rig;
	if (rig.Open(argc, argv, false)) {
		superslm::SslmModelView view{};
		std::string err;
		const bool loaded = superslm::SslmModel::Load(rig.raw.data(), rig.raw.size(), view, &err) ==
		                    superslm::SslmModelStatus::Ok;
		CHECK_MSG(loaded && view.has_tokenizer, "[G2.1] F-SFD's second view: %s", err.c_str());
		view.has_tokenizer = false;
		SslmGpuModelHandle* m = OutSentinel<SslmGpuModelHandle>();
		const GS ms = sslm_gpu_model_map(rig.ctx(), &view, GpuResidencyConfig{}, &m);
		CHECK_MSG(ms == GS::SSLM_OK && Real(m), "[G2.1] a view without a tokenizer must map: %s", Name(ms));
		if (ms == GS::SSLM_OK && Real(m)) {
			{
				GSeq s = rig.New(kPotion, m);
				const GQuery q = QueryGpu(rig.ctx(), s.h);
				CHECK_MSG(q.st == kForcedTableAbsent && q.count == -9 && q.accepting == -9,
				          "[G2.1] the query without a table: %s, count %d, accepting %d (want SSLM_GPU_FORCED_TABLE_ABSENT, "
				          "nothing written)", Name(q.st), q.count, q.accepting);
				// Decode works: the prompt, then greedy tokens, all legal under the schema.
				CRig c(rig.raw);
				const std::vector<int32_t> prompt = c.Tokenize(kPromptPotionText);
				const GS ps = rig.Prompt(s, prompt);
				CHECK_MSG(ps == GS::SSLM_OK, "[G2.1] prompt prefill on the table-less model: %s", Name(ps));
				GS last = GS::SSLM_OK;
				const std::vector<int32_t> out = DecodeN(rig.env, s, 8, &last);
				CHECK_MSG(last == GS::SSLM_OK && !out.empty(), "[G2.1] decode on the table-less model: %s, %zu tokens",
				          Name(last), out.size());
				// The same tokens as the table-carrying model: the table changes no decode.
				GSeq t = rig.New(kPotion);
				rig.Prompt(t, prompt);
				const std::vector<int32_t> want = DecodeN(rig.env, t, 8);
				CHECK_MSG(out == want, "[G2.1] decode differs with and without the table: %s vs %s", Str(out).c_str(),
				          Str(want).c_str());
				sslm_gpu_seq_release(rig.ctx(), s.h);
				rig.seqs.erase(std::remove(rig.seqs.begin(), rig.seqs.end(), s.h), rig.seqs.end());
			}
			// The table-carrying model answers the same query with OK.
			GSeq u = rig.New(kPotion);
			const GQuery q2 = QueryGpu(rig.ctx(), u.h);
			CHECK_MSG(q2.st == GS::SSLM_OK && q2.count >= 0, "[G2.1] F-SFD with its tokenizer: %s, count %d", Name(q2.st), q2.count);
			CHECK_MSG(sslm_gpu_model_unmap(rig.ctx(), m) == GS::SSLM_OK, "[G2.1] unmap");
		}
	}
	return FinishSuite("sfdg_cell_g2_1");
}
