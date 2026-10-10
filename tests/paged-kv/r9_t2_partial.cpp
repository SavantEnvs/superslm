// Paged-KV plan rev 16.21 §23.8 step T2, the fallback's removal: the partial last page (ceil(cap/B)
// pages, the slack never addressed), the u32 threshold and the block product on ceil, the state size
// at a non-dividing cap. Fixture: pkv_odd (D = 48, cap 4,100), unpinned: B = 256, 17 pages, 252
// positions of slack. On 2ebe5e3 its B is the cap (one page), so every cell is red there.
//
//   4.4/T2              legacy tokens and rows at 4.1's widths, cap - 1 and cap; budget reservations
//   4.10/T2:non-dividing  4,095 -> 64 (D 64), 4,097 -> 128, 4,100 -> 128 (D 64) and 256 (D 48),
//                       32,767 -> 64 (D 128), each with ceil(cap/B) pages
//   4.12/T2:cap4100     a pin of 16 at cap 4,100 is refused; B stays 256
//   2.9/T2              pinned B = 16 at cap 32,768: 2,097,152 blocks refused before the buffer check
//   2.9b/T2             unpinned thresholds: D = 64 at cap 32,768 (B = 128) and D = 128 (B = 64)
//   2.9c/T2             pkv_odd: 252,645,135 blocks passes the page check, 252,645,136 is refused
//   2.9d/T2             the block product's overflow at a non-dividing cap (model gap: the CFG1-only
//                       fixture does not map)
//   10.9/T2             admission at a non-dividing cap, the writes to the cap, save, adopt, state size
//   4.13/T2:cap4100     4.2 (prefix lengths) whole on pkv_odd at B = 256
//   6.7/T2:non-dividing contiguous option <-> page pool at cap 4,100, and a pin of 4 (needs T3's verb)
//   3.7/T2:odd-legacy-save  (routed by the conductor from the T-3190 strike) a pinned v1.11.0 legacy save
//                       at pkv_odd restores on the paged build, and the build's own legacy save of the
//                       same state is byte-equal to it

#include "pkv_r9.h"

namespace {

using namespace pkv;
using namespace pkv::r9;

const char* const kAllScenarios[] = {"lifecycle", "long_prefill", "shared_preamble", "persist",
                                     "saturating", "widths", "prefix_lengths"};

int64_t OddB() { return RuleB(48, 4100, 1); }  // 256

void RequireOddB(const Fixture& odd) {
	PKV_CHECK_MSG(sslm_kv_page_positions(odd.model) == 256, "pkv_odd runs at B = %lld; the rule gives 256 with 17 pages",
	              static_cast<long long>(sslm_kv_page_positions(odd.model)));
}

// 4.4 [T2]. Legacy: tokens and K/V rows equal the v1.11.0 reference in every scenario (4.1's widths,
// cap - 1 and cap are the `widths` scenario's), rows read through the peek at B = 256 rotated.
// Budget: the reservation R(budget) = min(ceil(budget/B) + 1, 17), graded by admission.
void Cell44() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	const int64_t B = OddB();
	const Lay y = LayoutOf(odd.geo, 1, B);
	PKV_CHECK_EQ(y.pages, 17);
	PKV_CHECK_EQ(y.slack, 252);
	for (const char* sc : kAllScenarios)
		ExpectMatchesReference("v1.11.0", odd, sc, RunScenario(odd, sc), Compare::kTokensAndRows);
	const RefRecord* saved = RefLookup(Reference("v1.11.0", odd), "persist", "saved");
	BlockPoolOf pool(odd.model, 1, y.block_bytes);
	PKV_CHECK_EQ(pool.status, SSLM_OK);
	sslm_seq s = nullptr;
	if (pool.pool) PKV_CHECK_EQ(sslm_seq_create(odd.model, &pool.pool, &s), SSLM_OK);
	if (s && saved) {
		PKV_CHECK_EQ(PrefillAll(odd.model, s, Stream(31, 100, odd.vocab), 64), SSLM_OK);
		bc::DecodeN(odd.model, s, 4);
		std::vector<uint8_t> rows;
		PKV_CHECK_MSG(SeqRows(pool.pool, s, odd.geo, 1, B, saved->context_length, Rot::kRotated, &rows) &&
		                  Sha(rows.data(), rows.size()) == saved->rows_sha,
		              "persist/saved: rows read from the pages at B = 256 rotated equal v1.11.0's");
		sslm_seq_release(s);
	}
	for (int32_t budget : {1, 255, 256, 257, 1000, 4100}) {
		const int64_t R = Reservation(budget, 4100, B);
		PagePoolOf p(odd.model, 17, y.page_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		if (!p.pool) continue;
		sslm_status refusal = SSLM_OK;
		const int n = CountCreates(odd.model, &p.pool, budget, &refusal);
		PKV_CHECK_MSG(n == 17 / R && refusal == SSLM_KV_POOL_EXHAUSTED,
		              "budget %d: R = %lld, so 17 pages admit %lld; admitted %d, refused with %s", budget,
		              static_cast<long long>(R), static_cast<long long>(17 / R), n, AbiStatusName(refusal));
	}
	// The fallback mutant: one 4,100-position page in the same bytes (4,352 positions) admits 1
	// budget-100 create against 8.
	PKV_CHECK(RuleB(48, 4100, 1, RuleMutant::kFallback) == 4100);
}

// 4.10 [T2]. Non-dividing caps take the rule's B with a partial last page.
void Cell410NonDividing() {
	struct Row { const Fixture& (*base)(); uint32_t D; int64_t cap, B, pages, slack; };
	auto d64 = []() -> const Fixture& { return D64Cap(4096); };
	auto def = []() -> const Fixture& { return GetFixture("pkv_def"); };
	auto qk = []() -> const Fixture& { return GetFixture("pkv_qk"); };
	const Row rows[] = {{d64, 64, 4095, 64, 64, 1}, {d64, 64, 4097, 128, 33, 127}, {d64, 64, 4100, 128, 33, 124},
	                    {def, 48, 4100, 256, 17, 252}, {qk, 128, 32767, 64, 512, 1}};
	for (const Row& r : rows) {
		const Fixture& base = r.base();
		if (!base.ok) continue;
		Variant v;
		v.cap = r.cap;
		v.strip_rope = true;
		Fixture fx;
		sslm_status ms = SSLM_OK;
		if (!MakeVariant(base, v, &fx, &ms)) {
			PKV_CHECK_MSG(false, "D %u cap %lld did not map (%s)", r.D, static_cast<long long>(r.cap), AbiStatusName(ms));
			continue;
		}
		PKV_CHECK_EQ(RuleB(r.D, static_cast<uint64_t>(r.cap), 1), r.B);
		const Lay y = LayoutOf(fx.geo, 1, r.B);
		PKV_CHECK_EQ(y.pages, r.pages);
		PKV_CHECK_EQ(y.slack, r.slack);
		PKV_CHECK_MSG(sslm_kv_page_positions(fx.model) == r.B, "D %u cap %lld: B = %lld, %lld owed", r.D,
		              static_cast<long long>(r.cap), static_cast<long long>(sslm_kv_page_positions(fx.model)),
		              static_cast<long long>(r.B));
		PKV_CHECK_MSG(sslm_kv_block_size(fx.model) == y.block_bytes, "D %u cap %lld: block %zu, %lld pages of %zu is %zu",
		              r.D, static_cast<long long>(r.cap), sslm_kv_block_size(fx.model), static_cast<long long>(r.pages),
		              y.page_bytes, y.block_bytes);
		PKV_CHECK_MSG(RuleB(r.D, static_cast<uint64_t>(r.cap), 1, RuleMutant::kFallback) != r.B,
		              "separation: the fallback gives the rule's B at D %u cap %lld", r.D, static_cast<long long>(r.cap));
	}
}

// 4.12 [T2]. A pin of 16 at cap 4,100 is refused, and the model's B stays 256.
void Cell412Cap4100() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	Fixture o;
	Variant v;
	v.kv_block_size = 16;
	if (!MakeVariant(odd, v, &o)) return;
	PKV_CHECK_EQ(KvPagesTestPinPagePositions(o.model, 16), SSLM_INVALID_ARGUMENT);
	PKV_CHECK_EQ(sslm_kv_page_positions(o.model), 256);
	// a pin of 4, which divides 4,100, takes (6.7's third direction)
	Fixture o4;
	if (MakeVariant(odd, v, &o4)) {
		PKV_CHECK_EQ(KvPagesTestPinPagePositions(o4.model, 4), SSLM_OK);
		PKV_CHECK_EQ(sslm_kv_page_positions(o4.model), 4);
	}
}

// A create with a buffer smaller than any real requirement: SSLM_BUFFER_TOO_SMALL when the page
// count passes, SSLM_INVALID_ARGUMENT when it is refused first. Nothing allocated either way.
sslm_status TinyCreate(sslm_model model, uint32_t blocks) {
	AlignedBuf tiny(4096);
	sslm_kv_pool pool = nullptr;
	const sslm_status st = sslm_kv_pool_create(model, tiny.p, tiny.n, blocks, &pool);
	if (pool) sslm_kv_pool_destroy(pool);
	return st;
}

// 2.9 [T2]. Pinned to 16 at cap 32,768: 2,097,152 blocks exceed UINT32_MAX pages -> refused first.
void Cell29() {
	const Fixture& base = GetFixture("pkv_32k");
	if (!base.ok) return;
	Fixture fx;
	Variant v;
	v.kv_block_size = 16;
	if (!MakeVariant(base, v, &fx)) return;
	PKV_CHECK_EQ(KvPagesTestPinPagePositions(fx.model, 16), SSLM_OK);
	PKV_CHECK_EQ(sslm_kv_page_positions(fx.model), 16);
	PKV_CHECK_EQ(TinyCreate(fx.model, 2097151), SSLM_BUFFER_TOO_SMALL);
	PKV_CHECK_EQ(TinyCreate(fx.model, 2097152), SSLM_INVALID_ARGUMENT);
}

// 2.9b [T2]. The unpinned u32 thresholds: floor(UINT32_MAX / ceil(cap/B)) at the derived B.
void Cell29b() {
	struct Row { const char* what; const Fixture& (*base)(); uint32_t D; int64_t B; uint32_t threshold; };
	auto d64 = []() -> const Fixture& { return D64Cap(4096); };
	auto qk = []() -> const Fixture& { return GetFixture("pkv_qk"); };
	const Row rows[] = {{"D = 64 at cap 32,768 (the 0.5B's geometry)", d64, 64, 128, 16777215u},
	                    {"D = 128 at cap 32,768", qk, 128, 64, 8388607u}};
	for (const Row& r : rows) {
		Variant v;
		v.cap = 32768;
		v.strip_rope = true;
		Fixture fx;
		if (!r.base().ok || !MakeVariant(r.base(), v, &fx)) continue;
		PKV_CHECK_EQ(RuleB(r.D, 32768, 1), r.B);
		PKV_CHECK_EQ(static_cast<uint64_t>(UINT32_MAX) / CapPages(32768, r.B), r.threshold);
		PKV_CHECK_MSG(TinyCreate(fx.model, r.threshold) == SSLM_BUFFER_TOO_SMALL,
		              "%s: %u blocks must pass the page-count check (B = 16's threshold refuses them)", r.what, r.threshold);
		PKV_CHECK_MSG(TinyCreate(fx.model, r.threshold + 1) == SSLM_INVALID_ARGUMENT,
		              "%s: %u blocks must be refused before the buffer check", r.what, r.threshold + 1);
		// mutant: a threshold computed from B = 16 refuses at the threshold
		PKV_CHECK(static_cast<uint64_t>(UINT32_MAX) / CapPages(32768, 16) < r.threshold);
	}
}

// 2.9c [T2]. pkv_odd: 17 pages per block; 17 * 252,645,135 = UINT32_MAX exactly.
void Cell29c() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	PKV_CHECK_EQ(17ull * 252645135ull, static_cast<unsigned long long>(UINT32_MAX));
	PKV_CHECK_EQ(TinyCreate(odd.model, 252645135u), SSLM_BUFFER_TOO_SMALL);
	PKV_CHECK_EQ(TinyCreate(odd.model, 252645136u), SSLM_INVALID_ARGUMENT);
	// mutant: floor(cap/B) = 16 pages per block puts the threshold at 268,435,455
	PKV_CHECK(static_cast<uint64_t>(UINT32_MAX) / CapPages(4100, 256, true) == 268435455u);
}

// 2.9d [T2]. The block product's overflow at a non-dividing cap, on the plan's CFG1-only fixture:
// L 2,090,000, H_kv 1,024, D 1,048,576, Int8, cap 4,100 (B = 16, 257 pages; q = L*2*H_kv*D per
// position; cap*q and 16*q fit 64 bits, 257*16*q does not).
// MODEL GAP (record §4): the fixture loads (SslmModel::Load) but cannot map (BuildEngineCache marshals
// WGT1 weights it does not carry), so the ABI verbs it asserts on cannot be reached. Per the cell's
// own rule the refusal is recorded with its source; it is neither a check the fixture is specified to
// meet nor a bound on the overflow window, so it goes to the planner.
void Cell29d() {
	superslm_test::Cfg1Spec spec;
	spec.hidden_size = 1u << 30;
	spec.num_hidden_layers = 2090000;
	spec.num_attention_heads = 1024;
	spec.num_key_value_heads = 1024;
	spec.head_dim = 1048576;
	spec.intermediate_size = 2;
	spec.vocab_size = 256;
	spec.context_cap = 4100;
	spec.kv_precision = 0;
	spec.kv_block_size = 16;
	const uint64_t q = 2090000ull * 2 * 1024 * 1048576;
	PKV_CHECK_EQ(q, 4488240824320000ull);
	PKV_CHECK_EQ(RuleB(1048576, 4100, 1), 16);
	PKV_CHECK(q <= UINT64_MAX / 4100 && q <= UINT64_MAX / 16 && q > UINT64_MAX / (257ull * 16));
	const std::vector<uint8_t> b = Cfg1OnlyArtifact(spec);
	std::string err;
	const superslm::SslmModelStatus ls = LoadStatus(b, &err);
	Fixture fx;
	sslm_status ms = SSLM_OK;
	MapInto(b, "cfg1only_29d", Geometry{2090000, 1024, 1048576, 4100}, &fx, &ms);
	std::printf("  2.9d fixture: SslmModel::Load = %s (%s); sslm_model_map = %s\n", superslm::SslmModelStatusName(ls),
	            err.c_str(), AbiStatusName(ms));
	PKV_CHECK_MSG(ls == superslm::SslmModelStatus::Ok, "the loader refuses the 2.9d fixture: %s (%s)",
	              superslm::SslmModelStatusName(ls), err.c_str());
	PKV_CHECK_MSG(fx.ok, "the 2.9d fixture does not map (%s): BuildEngineCache needs WGT1 (model gap)", AbiStatusName(ms));
	if (!fx.ok) return;
	PKV_CHECK_EQ(sslm_kv_block_size(fx.model), SIZE_MAX);
	PKV_CHECK_EQ(TinyCreate(fx.model, 1), SSLM_INVALID_ARGUMENT);
	PKV_CHECK_EQ(sslm_kv_page_size(fx.model), static_cast<size_t>(16 * q));
}

// 10.9 [T2]. Admission at a non-dividing cap (D-SLM8396).
void Cell109() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	const int64_t B = OddB();
	const Lay y = LayoutOf(odd.geo, 1, B);
	{  // a pool of 17 pages admits one legacy create and refuses a second
		PagePoolOf p(odd.model, 17, y.page_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		sslm_status refusal = SSLM_OK;
		if (p.pool) PKV_CHECK_EQ(CountCreates(odd.model, &p.pool, 0, &refusal), 1);
		if (p.pool) PKV_CHECK_EQ(refusal, SSLM_KV_POOL_EXHAUSTED);
	}
	{  // eight budget-100 creates (R(100) = 2), the ninth refused
		PKV_CHECK_EQ(Reservation(100, 4100, B), 2);
		PagePoolOf p(odd.model, 17, y.page_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		sslm_status refusal = SSLM_OK;
		if (p.pool) PKV_CHECK_EQ(CountCreates(odd.model, &p.pool, 100, &refusal), 8);
		if (p.pool) PKV_CHECK_EQ(refusal, SSLM_KV_POOL_EXHAUSTED);
	}
	// a legacy holder writes positions 0 to 4,099 and refuses 4,100; a save at width 4,099 restores
	{
		BlockPoolOf p(odd.model, 2, y.block_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		sslm_seq s = nullptr;
		if (p.pool) PKV_CHECK_EQ(sslm_seq_create(odd.model, &p.pool, &s), SSLM_OK);
		if (s) {
			PKV_CHECK_EQ(PrefillAll(odd.model, s, Stream(41, 4099, odd.vocab), 64), SSLM_OK);
			const std::vector<uint8_t> blob = bc::Save(s);
			sslm_seq r = nullptr;
			PKV_CHECK_EQ(sslm_seq_restore(odd.model, &p.pool, blob.data(), blob.size(), &r), SSLM_OK);
			if (r) {
				std::vector<uint8_t> rows, brows;
				PKV_CHECK_MSG(SeqRows(p.pool, r, odd.geo, 1, B, 4099, Rot::kRotated, &rows) &&
				                  BlobRows(blob, odd, HiddenSize(odd), 4099, &brows) && rows == brows,
				              "the save at width 4,099 restores with rows equal (read at B = 256)");
				sslm_seq_release(r);
			}
			// The state size: the fixed part, the residual and the history, plus 17 pages. The fixed
			// part and the residual are the same at pkv_def (same hidden size), so the cell grades the
			// difference against the test's own block and history terms rather than reading either.
			const Fixture& def = GetFixture("pkv_def");
			const size_t odd_size = sslm_seq_state_size(odd.model), def_size = sslm_seq_state_size(def.model);
			const size_t odd_tail = 4 * 4100 + 17 * y.page_bytes;
			const size_t def_tail = 4 * 4096 + LayoutOf(def.geo, 1, RuleB(48, 4096, 1)).block_bytes;
			PKV_CHECK_MSG(odd_size - odd_tail == def_size - def_tail,
			              "sslm_seq_state_size at cap 4,100 is not the fixed part, residual and history plus 17 pages "
			              "(%zu - %zu vs pkv_def's %zu - %zu)", odd_size, odd_tail, def_size, def_tail);
			PKV_CHECK(blob.size() <= odd_size);
			// the last row, then the cap
			PKV_CHECK_EQ(PrefillAll(odd.model, s, Stream(42, 1, odd.vocab), 1), SSLM_OK);
			sslm_status st = SSLM_OK;
			PKV_CHECK(Next(odd.model, s, &st) >= 0);  // the ready token at L = 4,100 writes no row
			PKV_CHECK_EQ(Next(odd.model, s, &st), -1);
			PKV_CHECK_EQ(st, SSLM_CONTEXT_CAP_EXCEEDED);
			std::vector<uint8_t> rows;
			// kills: the floor mutant (16 pages) has no page for positions 4,096 to 4,099
			PKV_CHECK_MSG(SeqRows(p.pool, s, odd.geo, 1, B, 4100, Rot::kRotated, &rows),
			              "the table maps 17 pages: positions 4,096 to 4,099 are on the partial last page");
			sslm_seq_release(s);
		}
	}
	// a share adopt at origin 4,000 maps 15 full pages and copies one
	{
		PagePoolOf p(odd.model, 40, y.page_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		if (p.pool) {
			sslm_prefix px = bc::BudgetPrefix(odd, &p.pool, Stream(51, 4000, odd.vocab));
			sslm_seq s = nullptr;
			PKV_CHECK_EQ(sslm_seq_create_budgeted(odd.model, &p.pool, 64, &s), SSLM_OK);
			if (px && s) {
				PKV_CHECK_EQ(sslm_seq_adopt_prefix(s, px), SSLM_OK);
				uint32_t shared_ok = 0, sm = 0;
				for (uint32_t i = 0; i < 16; ++i) {
					uint32_t a = 0, b = 0;
					sslm_pkv_test_only_seq_table_entry(s, i, &a, &sm);
					sslm_pkv_test_only_prefix_table_entry(px, i, &b, nullptr);
					shared_ok += (a == b) ? 1 : 0;
				}
				PKV_CHECK_MSG(sm == 16, "after the adopt the holder maps %u pages, 16 owed (15 shared + 1 copied)", sm);
				uint32_t last_s = 0, last_p = 0;
				sslm_pkv_test_only_seq_table_entry(s, 15, &last_s, nullptr);
				sslm_pkv_test_only_prefix_table_entry(px, 15, &last_p, nullptr);
				PKV_CHECK_MSG(shared_ok == 15 && last_s != last_p, "the first 15 pages are shared (%u) and page 15 is a copy",
				              shared_ok);
			}
			if (s) sslm_seq_release(s);
			if (px) sslm_prefix_release(px);
		}
	}
	// must-rejects, separated at this construction
	PKV_CHECK_MSG(17 / Reservation(100, 4100, 4100) == 17 && (4352 / 4100) == 1,
	              "separation: the fallback's one 4,100-position page in 17 * 256 = 4,352 positions holds 1 holder");
	PKV_CHECK(CapPages(4100, 256, true) == 16);
}

// 4.13 [T2]. The cap-4,100 member: 4.2 (prefix lengths) whole on pkv_odd at B = 256 with a partial
// last page; 1,024 is aligned at 256.
void Cell413Cap4100() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	ExpectMatchesReference("v1.11.0", odd, "prefix_lengths", RunScenario(odd, "prefix_lengths"), Compare::kTokensAndRows);
	PKV_CHECK_EQ(1024 % OddB(), 0);
}

// 6.7 [T2]. At a non-dividing cap (T-3172 G-9): a blob saved in the contiguous option restores into
// the B = 256 page pool and one saved in the page pool restores into the option, at widths 1, 255,
// 256, 4,099 and 4,100; a third direction pins B = 4. Tokens and rows [0, L') equal, and the restoring
// legacy holder reserves ceil(cap/B) pages. Needs T3's contiguous verb (record §4).
void Cell67NonDividing() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	const int64_t B = OddB();
	const Lay y = LayoutOf(odd.geo, 1, B);
	bool fits = false;
	const uint64_t csize = ContiguousSize(y, 2, &fits);
	for (int64_t W : {int64_t{1}, int64_t{255}, int64_t{256}, int64_t{4099}, int64_t{4100}}) {
		ContigPoolOf cp(odd.model, 2, static_cast<size_t>(csize));
		BlockPoolOf pp(odd.model, 2, y.block_bytes);
		PKV_CHECK_MSG(cp.status == SSLM_OK, "W %lld: the contiguous pool: %s", static_cast<long long>(W), AbiStatusName(cp.status));
		PKV_CHECK_EQ(pp.status, SSLM_OK);
		if (!cp.pool || !pp.pool) continue;
		for (int dir = 0; dir < 2; ++dir) {
			sslm_kv_pool* src = dir == 0 ? &cp.pool : &pp.pool;
			sslm_kv_pool* dst = dir == 0 ? &pp.pool : &cp.pool;
			const int64_t dstB = dir == 0 ? B : 4100;
			sslm_seq s = nullptr, r = nullptr;
			PKV_CHECK_EQ(sslm_seq_create(odd.model, src, &s), SSLM_OK);
			if (!s) continue;
			PKV_CHECK_EQ(PrefillAll(odd.model, s, Stream(41, static_cast<int32_t>(W), odd.vocab), 64), SSLM_OK);
			const std::vector<uint8_t> blob = bc::Save(s);
			PKV_CHECK_EQ(sslm_seq_restore(odd.model, dst, blob.data(), blob.size(), &r), SSLM_OK);
			if (r) {
				std::vector<uint8_t> rows, brows;
				// kills: a restore that writes floor(L/B) * B rows loses the tail (fails at 4,099)
				PKV_CHECK_MSG(SeqRows(*dst, r, odd.geo, 1, dstB, W, Rot::kRotated, &rows) &&
				                  BlobRows(blob, odd, HiddenSize(odd), W, &brows) && rows == brows,
				              "W %lld, %s: restored rows equal the blob's", static_cast<long long>(W),
				              dir == 0 ? "option -> page pool" : "page pool -> option");
				if (W < 4100) PKV_CHECK(bc::DecodeN(odd.model, r, 2) == bc::DecodeN(odd.model, s, 2));
				sslm_seq_release(r);
			}
			sslm_seq_release(s);
		}
	}
	// the restoring legacy holder reserves ceil(cap/B) = 17 pages: a 17-page page pool takes exactly one
	{
		PagePoolOf p(odd.model, 17, y.page_bytes);
		PKV_CHECK_EQ(p.status, SSLM_OK);
		BlockPoolOf src(odd.model, 1, y.block_bytes);
		sslm_seq s = nullptr, r = nullptr;
		if (src.pool && sslm_seq_create(odd.model, &src.pool, &s) == SSLM_OK) {
			PrefillAll(odd.model, s, Stream(41, 300, odd.vocab), 64);
			const std::vector<uint8_t> blob = bc::Save(s);
			PKV_CHECK_EQ(sslm_seq_restore(odd.model, &p.pool, blob.data(), blob.size(), &r), SSLM_OK);
			sslm_seq extra = nullptr;
			PKV_CHECK_EQ(sslm_seq_create_budgeted(odd.model, &p.pool, 1, &extra), SSLM_KV_POOL_EXHAUSTED);
			if (extra) sslm_seq_release(extra);
			if (r) sslm_seq_release(r);
			sslm_seq_release(s);
		}
	}
	// the third direction: pinned B = 4 (divides 4,100) -> derived 256
	{
		Fixture o4;
		Variant v;
		v.kv_block_size = 16;
		if (MakeVariant(odd, v, &o4)) {
			PKV_CHECK_EQ(KvPagesTestPinPagePositions(o4.model, 4), SSLM_OK);
			const Lay y4 = LayoutOf(o4.geo, 1, 4);
			BlockPoolOf src(o4.model, 1, y4.block_bytes);
			BlockPoolOf dst(odd.model, 1, y.block_bytes);
			sslm_seq s = nullptr, r = nullptr;
			if (src.pool && sslm_seq_create(o4.model, &src.pool, &s) == SSLM_OK) {
				PKV_CHECK_EQ(PrefillAll(o4.model, s, Stream(41, 4099, o4.vocab), 64), SSLM_OK);
				const std::vector<uint8_t> blob = bc::Save(s);
				// the twin's model hash differs from pkv_odd's (kv_block_size patched), so the blob restores
				// on the pinned twin's identity: map a second copy of the twin unpinned
				Fixture o4u;
				if (MakeVariant(odd, v, &o4u)) {
					BlockPoolOf d2(o4u.model, 1, y.block_bytes);
					PKV_CHECK_EQ(sslm_seq_restore(o4u.model, &d2.pool, blob.data(), blob.size(), &r), SSLM_OK);
					if (r) {
						std::vector<uint8_t> rows, brows;
						PKV_CHECK_MSG(SeqRows(d2.pool, r, o4u.geo, 1, B, 4099, Rot::kRotated, &rows) &&
						                  BlobRows(blob, o4, HiddenSize(o4), 4099, &brows) && rows == brows,
						              "pinned 4 -> derived 256 at width 4,099: rows equal");
						sslm_seq_release(r);
					}
				}
				sslm_seq_release(s);
			}
		}
	}
}

// 3.7 [T2], routed by the conductor (T-3190 strike, FRACTURE): §3.7's writer promise ("a legacy
// holder's blob is byte-equal to the blob 1.9.0 would write for the same state") and its reader
// promise ("readers accept 1.9.0's SSB5"), at a cap the page size does not divide. The pin is
// v1.11.0's own legacy blob of persist/saved at pkv_odd (v1.11.0 and 1.9.0 write the same SSB5;
// tests/paged-kv/pkv_r9_pinmaker.cpp minted it only on a SHA-256 match with v1.11.0's recorded
// digest). The paged build must restore it, decode on as the reference does, and save that state, and
// a fresh holder brought to the same state, as bytes equal to the pin.
// The cell's subject is a paged legacy holder at the partial last page, so it first requires
// pkv_odd to run at the rule's B = 256 (red on 2ebe5e3, where B is the cap). Its save assertions are
// green on 2ebe5e3, which writes v1.11.0's one-page block; they turn red on a T2 build whose legacy
// blob is sized by the 17-page block (the strike's +96,768 bytes).
void Cell37OddLegacySave() {
	const Fixture& odd = GetFixture("pkv_odd");
	if (!odd.ok) return;
	RequireOddB(odd);
	std::vector<uint8_t> enc, pin;
	const std::string path = std::string(PKV_REFERENCE_DIR) + "/pins/v1.11.0_pkv_odd_persist_saved.zrl";
	PKV_CHECK_MSG(ReadAll(path, &enc) && ZrlDecode(enc, &pin), "pin %s missing or malformed", path.c_str());
	const RefRecord* rec = RefLookup(Reference("v1.11.0", odd), "persist", "saved");
	const RefRecord* cont = RefLookup(Reference("v1.11.0", odd), "persist", "restored+continuation8");
	if (pin.empty() || !rec || !cont) return;
	PKV_CHECK_MSG(Sha(pin.data(), pin.size()) == rec->blob_sha, "the pin is not v1.11.0's recorded blob");
	PKV_CHECK(IsMagic(pin, "SSB5"));
	const Lay y = LayoutOf(odd.geo, 1, OddB());
	BlockPoolOf pool(odd.model, 2, y.block_bytes);
	PKV_CHECK_EQ(pool.status, SSLM_OK);
	if (!pool.pool) return;
	// The paged build restores the pinned blob, and its re-save of that state is the pin's bytes.
	sslm_seq r = nullptr;
	PKV_CHECK_MSG(sslm_seq_restore(odd.model, &pool.pool, pin.data(), pin.size(), &r) == SSLM_OK,
	              "the paged build refuses v1.11.0's legacy blob at cap 4,100 (%zu bytes)", pin.size());
	if (r) {
		const std::vector<uint8_t> resaved = bc::Save(r);
		PKV_CHECK_MSG(resaved == pin, "re-save of the restored state: %zu bytes, the pin is %zu; not byte-equal",
		              resaved.size(), pin.size());
		PKV_CHECK(bc::DecodeN(odd.model, r, 8) == cont->tokens);
		sslm_seq_release(r);
	}
	// A fresh legacy holder brought to the same state saves the pin's bytes.
	sslm_seq s = nullptr;
	PKV_CHECK_EQ(sslm_seq_create(odd.model, &pool.pool, &s), SSLM_OK);
	if (s) {
		PKV_CHECK_EQ(PrefillAll(odd.model, s, Stream(31, 100, odd.vocab), 64), SSLM_OK);
		bc::DecodeN(odd.model, s, 4);
		const std::vector<uint8_t> own = bc::Save(s);
		PKV_CHECK_MSG(own == pin, "the paged build's own legacy save: %zu bytes, v1.11.0's is %zu; not byte-equal",
		              own.size(), pin.size());
		sslm_seq_release(s);
	}
}

PKV_CELL("4.4/T2", "T2", Cell44);
PKV_CELL("4.10/T2:non-dividing", "T2", Cell410NonDividing);
PKV_CELL("4.12/T2:cap4100", "T2", Cell412Cap4100);
PKV_CELL("2.9/T2", "T2", Cell29);
PKV_CELL("2.9b/T2", "T2", Cell29b);
PKV_CELL("2.9c/T2", "T2", Cell29c);
PKV_CELL("2.9d/T2", "T2", Cell29d);
PKV_CELL("10.9/T2", "T2", Cell109);
PKV_CELL("4.13/T2:cap4100", "T2", Cell413Cap4100);
PKV_CELL("6.7/T2:non-dividing", "T2", Cell67NonDividing);
PKV_CELL("3.7/T2:odd-legacy-save", "T2", Cell37OddLegacySave);

}  // namespace
