// Fuzz sslm_model_map and the inference path behind it: a mutated artifact (integrity hash
// recomputed) is mapped, then prefilled and decoded greedy and damped, saved and restored.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness_common.h"
#include "superslm/model.h"
#include "superslm/sslm_abi.h"

namespace {

constexpr size_t kMaxArtifact = 1u << 22;
constexpr size_t kMaxPoolBytes = 16u << 20;
constexpr size_t kMaxWorkspaceBytes = 64u << 20;

void Run(sslm_model model, const uint8_t* data, size_t size) {
	const size_t block = sslm_kv_block_size(model);
	if (block == 0) return;
	const size_t pool_bytes = block + sslm_kv_pool_overhead_size(model, 1);
	if (pool_bytes == 0 || pool_bytes > kMaxPoolBytes || pool_bytes < block) return;
	AlignedBuf pool_buf(pool_bytes);
	if (!pool_buf.p) return;
	sslm_kv_pool pool = nullptr;
	if (sslm_kv_pool_create(model, pool_buf.p, pool_bytes, 1, &pool) != SSLM_OK) return;

	sslm_config cfg{};
	cfg.max_batch = 1;
	cfg.max_chunk_budget = 8;
	cfg.max_layer_budget = 1;
	const size_t ws_bytes = sslm_workspace_size(model, &cfg);
	AlignedBuf ws_buf(ws_bytes <= kMaxWorkspaceBytes ? ws_bytes : 0);
	sslm_workspace ws = nullptr;
	if (ws_bytes != 0 && ws_bytes <= kMaxWorkspaceBytes && ws_buf.p)
		(void)sslm_workspace_create(model, &cfg, ws_buf.p, ws_bytes, &ws);

	sslm_seq seq = nullptr;
	if (sslm_seq_create(model, &pool, &seq) == SSLM_OK) {
		if (sslm_schema_count(model) > 0) {
			char name[64] = {};
			size_t n = sizeof(name) - 1;
			sslm_schema schema = nullptr;
			if (sslm_schema_name(model, 0, name, &n) == SSLM_OK && n < sizeof(name)) {
				name[n] = 0;
				if (sslm_schema_lookup(model, name, &schema) == SSLM_OK) (void)sslm_seq_set_schema(seq, schema);
			}
		}

		int32_t tokens[4];
		for (int i = 0; i < 4; ++i) tokens[i] = i < static_cast<int>(size) ? data[i] % 32 : i;
		int32_t consumed = 0;
		if (sslm_prefill(model, seq, tokens, 4, 4, SSLM_SPAN_PROMPT, ws, &consumed) == SSLM_OK) {
			sslm_decode_params greedy{};
			if (sslm_decode_params_init(model, SSLM_DECODE_MODE_GREEDY, 1, &greedy) == SSLM_OK) {
				for (int i = 0; i < 3; ++i) {
					int32_t out = 0;
					if (sslm_decode_step_v2(model, &seq, 1, &greedy, ws, &out) != SSLM_OK) break;
				}
			}
			sslm_decode_params damped{};
			if (sslm_decode_params_init(model, SSLM_DECODE_MODE_DAMPED_GREEDY, 1, &damped) == SSLM_OK) {
				for (int i = 0; i < 3; ++i) {
					int32_t out = 0;
					if (sslm_decode_step_v2(model, &seq, 1, &damped, ws, &out) != SSLM_OK) break;
				}
			}
		}

		size_t blob_n = 0;
		if (sslm_seq_save(seq, nullptr, &blob_n) == SSLM_BUFFER_TOO_SMALL && blob_n > 0 &&
		    blob_n <= kMaxPoolBytes) {
			std::vector<uint8_t> blob(blob_n);
			if (sslm_seq_save(seq, blob.data(), &blob_n) == SSLM_OK) {
				sslm_seq again = nullptr;
				sslm_seq_release(seq);
				seq = nullptr;
				if (sslm_seq_restore(model, &pool, blob.data(), blob_n, &again) == SSLM_OK)
					sslm_seq_release(again);
			}
		}
		if (seq) sslm_seq_release(seq);
	}

	if (size > 8) {
		std::string text(reinterpret_cast<const char*>(data), size < 64 ? size : 64);
		text.erase(std::find(text.begin(), text.end(), '\0'), text.end());
		int32_t ids[32];
		int32_t n = 32;
		(void)sslm_tokenize(model, text.c_str(), ids, &n);
	}

	if (ws) sslm_workspace_destroy(ws);
	sslm_kv_pool_destroy(pool);
}

// sslm_model_map takes minutes once a validly hashed Config claims millions of layers (no upper
// bound on num_hidden_layers is enforced), which would stall the fuzzer on one input; skip those.
bool PlausibleLayerCount(const std::vector<uint8_t>& bytes) {
	superslm::SslmArtifact art;
	if (superslm::SslmArtifact::OpenFromMemory(bytes.data(), bytes.size(), art, nullptr) != superslm::SslmStatus::Ok)
		return false;
	const superslm::SslmSectionView* cfg = art.Section(superslm::SslmSectionType::Config);
	if (!cfg) return true;
	superslm::SslmModelConfig parsed;
	if (superslm::ParseConfig(*cfg, parsed, nullptr) != superslm::SslmModelStatus::Ok) return true;
	return parsed.num_hidden_layers <= 256;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < superslm::kHeaderBytes || size > kMaxArtifact) return -1;
	std::vector<uint8_t> bytes(data, data + size);
	FixIntegrityHash(bytes);
	if (!PlausibleLayerCount(bytes)) return -1;
	sslm_model model = nullptr;
	if (sslm_model_map(bytes.data(), bytes.size(), &model) != SSLM_OK) return -1;
	Run(model, data, size);
	sslm_model_unmap(model);
	return 0;
}
