// Fuzz the C ABI's sequence / prefix / KV-page-pool state machine. The model (base fixture + two
// schemas + damped-greedy constants) is embedded and mapped once; each input is a script of operations
// over three sequence slots, three prefix slots and two saved-blob slots. Handles are tracked by the
// harness (a released slot is nulled), so every call is one a well-behaved caller could make; blobs
// handed to the restore verbs are arbitrary bytes.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "embed.h"
#include "harness_common.h"
#include "superslm/sslm_abi.h"

MAYHEM_EMBED(g_model_full, MODEL_FULL_PATH)

namespace {

constexpr int kSeqSlots = 3;
constexpr int kPrefixSlots = 3;
constexpr int kBlobSlots = 2;
constexpr int kMaxOps = 48;
constexpr int32_t kCap = 16;

struct Model {
	sslm_model model = nullptr;
	Model() {
		const size_t n = static_cast<size_t>(g_model_full_end - g_model_full);
		if (sslm_model_map(g_model_full, n, &model) != SSLM_OK) model = nullptr;
	}
};

struct Reader {
	const uint8_t* p;
	size_t n;
	size_t i = 0;
	bool Has() const { return i < n; }
	uint8_t U8() { return i < n ? p[i++] : 0; }
};

struct Ctx {
	sslm_model model;
	sslm_kv_pool pool = nullptr;
	sslm_workspace ws = nullptr;
	sslm_seq seq[kSeqSlots] = {};
	sslm_prefix prefix[kPrefixSlots] = {};
	std::vector<uint8_t> blob[kBlobSlots];
	sslm_schema schemas[2] = {};
};

sslm_decode_params MakeParams(Reader& r) {
	sslm_decode_params p{};
	p.struct_size = sizeof(p);
	p.layer_budget = 1 + r.U8() % 3;
	const uint8_t flags = r.U8();
	if (flags & 1) {
		p.mode = SSLM_DECODE_MODE_DAMPED_GREEDY;
		p.alpha_q15 = static_cast<int32_t>(r.U8()) << (flags >> 4 & 7);
		p.anti_lm_max_order = r.U8() % 5;
		p.top_k = r.U8() % 34;
		p.q_ln2 = 493;
		p.q_b = 964;
		p.q_c = 487361;
	}
	if (flags & 2) p.struct_size += (flags & 4) ? 8 : -4;
	return p;
}

int32_t Tok(uint8_t b) { return static_cast<int32_t>(b % 36) - 2; }

void SaveSeq(Ctx& c, sslm_seq s, std::vector<uint8_t>& out) {
	size_t n = 0;
	if (sslm_seq_save(s, nullptr, &n) != SSLM_BUFFER_TOO_SMALL || n == 0 || n > (8u << 20)) return;
	std::vector<uint8_t> b(n);
	if (sslm_seq_save(s, b.data(), &n) == SSLM_OK) {
		b.resize(n);
		out = std::move(b);
	}
}

void SavePrefix(sslm_prefix pf, std::vector<uint8_t>& out) {
	size_t n = 0;
	if (sslm_prefix_save(pf, nullptr, &n) != SSLM_BUFFER_TOO_SMALL || n == 0 || n > (8u << 20)) return;
	std::vector<uint8_t> b(n);
	if (sslm_prefix_save(pf, b.data(), &n) == SSLM_OK) {
		b.resize(n);
		out = std::move(b);
	}
}

void Step(Ctx& c, Reader& r) {
	const uint8_t op = r.U8() % 24;
	const int si = r.U8() % kSeqSlots;
	const int pi = r.U8() % kPrefixSlots;
	const int bi = r.U8() % kBlobSlots;
	sslm_workspace ws = (r.U8() & 7) ? c.ws : nullptr;
	switch (op) {
	case 0:
		if (!c.seq[si]) (void)sslm_seq_create(c.model, &c.pool, &c.seq[si]);
		break;
	case 1:
		if (!c.seq[si]) {
			const int32_t budget = static_cast<int32_t>(r.U8() % (kCap + 3)) - 1;
			(void)sslm_seq_create_budgeted(c.model, &c.pool, budget, &c.seq[si]);
		}
		break;
	case 2:
		if (c.seq[si] && sslm_seq_release(c.seq[si]) == SSLM_OK) c.seq[si] = nullptr;
		break;
	case 3:
		if (c.seq[si]) (void)sslm_seq_reset(c.seq[si]);
		break;
	case 4: {
		if (!c.seq[si]) break;
		int32_t toks[kCap + 2];
		const int count = r.U8() % (kCap + 2);
		for (int k = 0; k < count; ++k) toks[k] = Tok(r.U8());
		const int32_t budget = 1 + r.U8() % 10;
		const sslm_span_kind kind = (r.U8() & 1) ? SSLM_SPAN_SCHEMA_CONTENT : SSLM_SPAN_PROMPT;
		int32_t consumed = 0;
		(void)sslm_prefill(c.model, c.seq[si], toks, count, budget, kind, ws, &consumed);
		break;
	}
	case 5: {
		sslm_decode_params p = MakeParams(r);
		sslm_seq batch[2] = {c.seq[si], c.seq[(si + 1) % kSeqSlots]};
		int32_t out[2] = {};
		const int32_t n = ((r.U8() & 1) && batch[0] != batch[1] && batch[1]) ? 2 : 1;
		if (batch[0]) (void)sslm_decode_step_v2(c.model, batch, n, &p, ws, out);
		break;
	}
	case 6:
		if (c.seq[si]) SaveSeq(c, c.seq[si], c.blob[bi]);
		break;
	case 7:
		if (!c.seq[si] && !c.blob[bi].empty())
			(void)sslm_seq_restore(c.model, &c.pool, c.blob[bi].data(), c.blob[bi].size(), &c.seq[si]);
		break;
	case 8: {
		if (c.seq[si]) break;
		const size_t len = r.n - r.i;
		(void)sslm_seq_restore(c.model, &c.pool, r.p + r.i, len, &c.seq[si]);
		r.i = r.n;
		break;
	}
	case 9:
		if (!c.prefix[pi]) {
			if (r.U8() & 1) (void)sslm_prefix_begin(c.model, &c.pool, &c.prefix[pi]);
			else (void)sslm_prefix_begin_budgeted(c.model, &c.pool, static_cast<int32_t>(r.U8() % (kCap + 3)) - 1,
			                                       &c.prefix[pi]);
		}
		break;
	case 10: {
		if (!c.prefix[pi]) break;
		int32_t toks[kCap + 2];
		const int count = r.U8() % (kCap + 2);
		for (int k = 0; k < count; ++k) toks[k] = Tok(r.U8());
		const int32_t budget = 1 + r.U8() % 10;
		const sslm_span_kind kind = (r.U8() & 1) ? SSLM_SPAN_SCHEMA_CONTENT : SSLM_SPAN_PROMPT;
		int32_t consumed = 0;
		(void)sslm_prefix_prefill(c.model, c.prefix[pi], toks, count, budget, kind, ws, &consumed);
		break;
	}
	case 11:
		if (c.prefix[pi]) (void)sslm_prefix_freeze(c.prefix[pi]);
		break;
	case 12:
		if (c.prefix[pi] && sslm_prefix_release(c.prefix[pi]) == SSLM_OK) c.prefix[pi] = nullptr;
		break;
	case 13:
		if (c.seq[si] && c.prefix[pi]) (void)sslm_seq_adopt_prefix(c.seq[si], c.prefix[pi]);
		break;
	case 14:
		if (c.prefix[pi]) SavePrefix(c.prefix[pi], c.blob[bi]);
		break;
	case 15: {
		const int dst = (pi + 1) % kPrefixSlots;
		if (c.prefix[dst] || c.blob[bi].empty()) break;
		std::vector<uint8_t> copy = c.blob[bi];
		FixSpx1Digest(copy);
		uint32_t shared = 0;
		(void)sslm_prefix_restore(c.model, &c.pool, copy.data(), copy.size(), c.prefix[pi], &c.prefix[dst], &shared);
		break;
	}
	case 16: {
		const int dst = (pi + 1) % kPrefixSlots;
		if (c.prefix[pi] && !c.prefix[dst])
			(void)sslm_prefix_begin_from(c.prefix[pi], static_cast<int32_t>(r.U8() % (kCap + 3)) - 1, &c.prefix[dst]);
		break;
	}
	case 17: {
		sslm_schema sc = c.schemas[r.U8() % 2];
		if (r.U8() & 1) sc = SSLM_SCHEMA_NONE;
		if (r.U8() & 1) {
			if (c.seq[si]) (void)sslm_seq_set_schema(c.seq[si], sc);
		} else if (c.prefix[pi]) {
			(void)sslm_prefix_set_schema(c.prefix[pi], sc);
		}
		break;
	}
	case 18: {
		if (!c.seq[si]) break;
		int32_t ids[8] = {};
		int32_t count = 0, accepting = 0;
		(void)sslm_seq_forced_tokens(c.model, c.seq[si], ids, 1 + r.U8() % 8, &count, &accepting);
		int32_t bound = 0;
		(void)sslm_seq_schema_bound(c.seq[si], &bound);
		break;
	}
	case 19: {
		if (!c.seq[si]) break;
		sslm_decode_params p = MakeParams(r);
		sslm_jump_options o{};
		o.struct_size = sizeof(o);
		int32_t draft[6];
		const uint8_t m = r.U8();
		o.mode = (m & 1) ? SSLM_JUMP_EXACT : SSLM_JUMP_CANONICAL;
		o.draft_count = (m >> 1) % 7;
		for (int k = 0; k < 6; ++k) draft[k] = Tok(r.U8());
		o.draft = (m & 0x80) ? nullptr : draft;
		const int32_t budget = 1 + r.U8() % 8;
		int32_t forced[8] = {};
		int32_t count = 0;
		(void)sslm_seq_jump_forward(c.model, c.seq[si], budget, (m & 0x40) ? nullptr : &p,
		                            (m & 0x20) ? nullptr : &o, ws, forced, 8, &count);
		break;
	}
	case 20: {
		if (!c.seq[si]) break;
		sslm_stats_out st{};
		(void)sslm_stats(c.model, c.seq[si], &st);
		sslm_seq_kv_stats_out ks{};
		ks.struct_size = sizeof(ks);
		(void)sslm_seq_kv_stats(c.seq[si], &ks);
		sslm_kv_pool_stats_out ps{};
		ps.struct_size = sizeof(ps);
		(void)sslm_kv_pool_stats(c.pool, &ps);
		break;
	}
	case 21: {
		if (c.seq[si] || c.blob[bi].empty()) break;
		uint32_t shared = 0;
		(void)sslm_seq_restore_shared(c.model, &c.pool, c.blob[bi].data(), c.blob[bi].size(), c.prefix[pi],
		                              &c.seq[si], &shared);
		break;
	}
	case 22: {
		const int dst = (pi + 1) % kPrefixSlots;
		if (c.prefix[dst]) break;
		std::vector<uint8_t> raw(r.p + r.i, r.p + r.n);
		r.i = r.n;
		FixSpx1Digest(raw);
		uint32_t shared = 0;
		(void)sslm_prefix_restore(c.model, &c.pool, raw.data(), raw.size(), nullptr, &c.prefix[dst], &shared);
		break;
	}
	default: {
		const int dst = (si + 1) % kSeqSlots;
		if (c.seq[si] && !c.seq[dst]) {
			std::vector<uint8_t> tmp;
			SaveSeq(c, c.seq[si], tmp);
			if (!tmp.empty()) (void)sslm_seq_restore(c.model, &c.pool, tmp.data(), tmp.size(), &c.seq[dst]);
		}
		break;
	}
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static Model m;
	if (!m.model || size < 4 || size > 4096) return -1;

	const int64_t pages_per_block = static_cast<int64_t>(sslm_kv_pages_for_budget(m.model, kCap));
	if (pages_per_block <= 0) return -1;
	const uint32_t page_count = static_cast<uint32_t>(pages_per_block * 6);
	const size_t pool_bytes =
	    static_cast<size_t>(page_count) * sslm_kv_page_size(m.model) + sslm_kv_page_pool_overhead_size(m.model, page_count);
	AlignedBuf pool_buf(pool_bytes);
	if (!pool_buf.p) return -1;

	Ctx c;
	c.model = m.model;
	if (sslm_kv_page_pool_create(m.model, pool_buf.p, pool_bytes, page_count, &c.pool) != SSLM_OK) return -1;

	sslm_config cfg{};
	cfg.max_batch = 2;
	cfg.max_chunk_budget = 10;
	cfg.max_layer_budget = 2;
	const size_t ws_bytes = sslm_workspace_size(m.model, &cfg);
	AlignedBuf ws_buf(ws_bytes);
	if (!ws_buf.p || sslm_workspace_create(m.model, &cfg, ws_buf.p, ws_bytes, &c.ws) != SSLM_OK) {
		sslm_kv_pool_destroy(c.pool);
		return -1;
	}
	(void)sslm_schema_lookup(m.model, "s_ab", &c.schemas[0]);
	(void)sslm_schema_lookup(m.model, "s_any", &c.schemas[1]);

	Reader r{data, size};
	for (int ops = 0; ops < kMaxOps && r.Has(); ++ops) Step(c, r);

	for (auto& s : c.seq)
		if (s) sslm_seq_release(s);
	for (auto& p : c.prefix)
		if (p) sslm_prefix_release(p);
	sslm_workspace_destroy(c.ws);
	sslm_kv_pool_destroy(c.pool);
	return 0;
}
