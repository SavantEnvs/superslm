// Writes a saved-sequence blob (SSB5), a budgeted one (SSB6) and a saved-prefix blob (SPX1) from the
// embedded-model flow, as seeds for fuzz_seq_ops' raw-restore ops. Not built by build.sh; see
// mayhem/gen_seeds.py. Usage: gen_blob_seeds <model.sslm> <outdir>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "harness_common.h"
#include "superslm/sslm_abi.h"

static std::vector<uint8_t> ReadAll(const char* path) {
	std::vector<uint8_t> v;
	if (FILE* f = std::fopen(path, "rb")) {
		uint8_t buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
		std::fclose(f);
	}
	return v;
}

static void WriteAll(const std::string& path, const std::vector<uint8_t>& v) {
	if (FILE* f = std::fopen(path.c_str(), "wb")) {
		std::fwrite(v.data(), 1, v.size(), f);
		std::fclose(f);
	}
}

int main(int argc, char** argv) {
	if (argc != 3) return 2;
	std::vector<uint8_t> model_bytes = ReadAll(argv[1]);
	sslm_model model = nullptr;
	if (sslm_model_map(model_bytes.data(), model_bytes.size(), &model) != SSLM_OK) return 1;
	const uint32_t pages = static_cast<uint32_t>(sslm_kv_pages_for_budget(model, 16) * 6);
	const size_t pool_bytes = pages * sslm_kv_page_size(model) + sslm_kv_page_pool_overhead_size(model, pages);
	AlignedBuf pool_buf(pool_bytes);
	sslm_kv_pool pool = nullptr;
	if (sslm_kv_page_pool_create(model, pool_buf.p, pool_bytes, pages, &pool) != SSLM_OK) return 1;
	sslm_config cfg{2, 10, 2, 0};
	const size_t ws_bytes = sslm_workspace_size(model, &cfg);
	AlignedBuf ws_buf(ws_bytes);
	sslm_workspace ws = nullptr;
	if (sslm_workspace_create(model, &cfg, ws_buf.p, ws_bytes, &ws) != SSLM_OK) return 1;

	const int32_t toks[4] = {1, 2, 3, 4};
	int32_t consumed = 0;
	std::string dir = argv[2];
	for (int budgeted = 0; budgeted < 2; ++budgeted) {
		sslm_seq seq = nullptr;
		if ((budgeted ? sslm_seq_create_budgeted(model, &pool, 8, &seq) : sslm_seq_create(model, &pool, &seq)) != SSLM_OK) return 1;
		if (sslm_prefill(model, seq, toks, 4, 4, SSLM_SPAN_PROMPT, ws, &consumed) != SSLM_OK) return 1;
		sslm_decode_params p{};
		p.struct_size = sizeof(p);
		p.layer_budget = 2;
		int32_t out = 0;
		(void)sslm_decode_step_v2(model, &seq, 1, &p, ws, &out);
		size_t n = 0;
		(void)sslm_seq_save(seq, nullptr, &n);
		std::vector<uint8_t> blob(n);
		if (sslm_seq_save(seq, blob.data(), &n) != SSLM_OK) return 1;
		WriteAll(dir + (budgeted ? "/seq_ssb6.blob" : "/seq_ssb5.blob"), blob);
		sslm_seq_release(seq);
	}
	sslm_prefix pf = nullptr;
	if (sslm_prefix_begin(model, &pool, &pf) != SSLM_OK) return 1;
	if (sslm_prefix_prefill(model, pf, toks, 4, 4, SSLM_SPAN_PROMPT, ws, &consumed) != SSLM_OK) return 1;
	if (sslm_prefix_freeze(pf) != SSLM_OK) return 1;
	size_t n = 0;
	(void)sslm_prefix_save(pf, nullptr, &n);
	std::vector<uint8_t> blob(n);
	if (sslm_prefix_save(pf, blob.data(), &n) != SSLM_OK) return 1;
	WriteAll(dir + "/prefix_spx1.blob", blob);
	sslm_prefix_release(pf);
	sslm_workspace_destroy(ws);
	sslm_kv_pool_destroy(pool);
	sslm_model_unmap(model);
	return 0;
}
