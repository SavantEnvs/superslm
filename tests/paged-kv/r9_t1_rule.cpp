// Paged-KV plan rev 16.21 §23.8 step T1, the page-size rule as a pure function and the pin's reach:
//
//   4.10/T1:grid          sslm_kv_page_positions over D {2, 48, 64, 128, 512, 1,024} x caps {8, 4,095,
//                         4,096, 4,097, 4,100, 8,192, 32,767, 32,768, 40,960} against the test's rule
//   4.10/T1:below-page    D = 2, cap 8: B = 8, one page per holder, and what holds there
//   4.10/T1:boundary      D = 64, caps 63, 64, 65, 127: the page condition's boundary row (L-7, R-1)
//   4.10/T1:extremes      the floor, the element width, the cap term's boundary, a non-power-of-two D
//   4.10/T1:hostile-d     D = 2^20 and 2^31 on CFG1-only fixtures (model gap: they do not map)
//   4.10/T1:generality    every §17.1 model row's (D, cap, elem)
//   4.11/T1               CFG1 kv_block_size in {1, 16, 32, 4,096, 0xFFFFFFFF} gives the rule's B; 0 refused
//   4.12/T1:refusals      the pin's refusals and hostile pins, the model left unchanged
//   4.12/T1:pins          a pin that takes: 16, 1 (the hand-built B = 1 geometry), the cap
//
// Each named mutant's separation is executed at the cell's own construction: the mutant's value
// (r9::RuleB with the mutant) is required to differ from the asserted value at the row the plan
// names, so a mutant the construction cannot see fails the cell rather than passing silently.
// 2ebe5e3 itself is r9::RuleMutant::kShipped (CFG1's kv_block_size, else the cap), so a red run on
// 2ebe5e3 is that mutant's kill against a real build.

#include "pkv_r9.h"

namespace {

using namespace pkv;
using namespace pkv::r9;

// A fixture variant at `cap` (RoPE stripped: these cells read geometry only), mapped fresh.
struct At {
	Fixture fx;
	sslm_status map = SSLM_INVALID_ARGUMENT;
	At(const Fixture& base, int64_t cap, int64_t kvbs = -1, int64_t prec = -1) {
		Variant v;
		v.cap = cap;
		v.kv_block_size = kvbs;
		v.kv_precision = prec;
		v.strip_rope = true;
		MakeVariant(base, v, &fx, &map);
	}
};

// Where a cell's assertion is `value == expected`, the named mutant must give something else there.
void Separates(RuleMutant m, uint64_t D, uint64_t cap, uint64_t elem, int64_t expected, const char* row) {
	const int64_t got = RuleB(D, cap, elem, m);
	PKV_CHECK_MSG(got != expected,
	              "separation: the mutant '%s' gives B = %lld at %s, the asserted value; the cell cannot kill it",
	              RuleMutantName(m), static_cast<long long>(got), row);
}

struct DBase {
	uint32_t D;
	const Fixture& (*get)();
};

const Fixture& GetDef() { return GetFixture("pkv_def"); }
const Fixture& GetQk() { return GetFixture("pkv_qk"); }
const Fixture& GetD64() { return D64Cap(4096); }

const DBase kBases[] = {{2, D2Cap8}, {48, GetDef}, {64, GetD64}, {128, GetQk}, {512, D512}, {1024, D1024}};
const int64_t kGridCaps[] = {8, 4095, 4096, 4097, 4100, 8192, 32767, 32768, 40960};

// 4.10 [T1]. The grid: every value equals the test's own evaluation of §3.1's rule.
void Cell410Grid() {
	for (const DBase& b : kBases) {
		const Fixture& base = b.get();
		if (!base.ok) continue;
		for (int64_t cap : kGridCaps) {
			At a(base, cap);
			PKV_CHECK_MSG(a.fx.ok, "D = %u at cap %lld: the variant did not map (%s)", b.D, static_cast<long long>(cap),
			              AbiStatusName(a.map));
			if (!a.fx.ok) continue;
			const int64_t want = RuleB(b.D, static_cast<uint64_t>(cap), 1);
			const int64_t got = sslm_kv_page_positions(a.fx.model);
			// kills: every rule mutant at some grid row (each is asserted separately below and in
			// :extremes); 2ebe5e3's rule reads kv_block_size 16 here
			PKV_CHECK_MSG(got == want, "D = %u, cap %lld: sslm_kv_page_positions = %lld, the rule gives %lld", b.D,
			              static_cast<long long>(cap), static_cast<long long>(got), static_cast<long long>(want));
		}
	}
	// The grid's own reach: 2ebe5e3's rule (kv_block_size 16, else the cap) differs at D = 64, cap 4,096.
	Separates(RuleMutant::kShipped, 64, 4096, 1, 64, "D = 64, cap 4,096");
	Separates(RuleMutant::kReadsCfg1, 48, 4096, 1, 128, "D = 48, cap 4,096");
}

// 4.10 [T1]. Below one page: D = 2, cap 8 -> B = 8, one page per holder (D-SLM8412). The fixture's
// CFG1 kv_block_size is patched to 4, a divisor of 8, so 2ebe5e3's rule (which reads it) gives
// B = 4 and two pages; the rule ignores it (4.11).
void Cell410BelowPage() {
	const Fixture& base = D2Cap8();
	if (!base.ok) return;
	Fixture fx;
	Variant v;
	v.kv_block_size = 4;
	PKV_CHECK_MSG(MakeVariant(base, v, &fx), "r9_d2_c8 with kv_block_size 4 maps");
	if (!fx.ok) return;
	const int64_t cap = 8, B = RuleB(2, 8, 1);
	PKV_CHECK_EQ(B, 8);
	const Lay y = LayoutOf(fx.geo, 1, B);
	// The first bullet's two assertions: the no-clamp mutant keeps B = 2,048 and a 2,048-position page.
	PKV_CHECK_EQ(sslm_kv_page_positions(fx.model), 8);
	PKV_CHECK_EQ(sslm_kv_block_size(fx.model), y.bytes_per_position * 8);  // one 8-position page
	Separates(RuleMutant::kNoClamp, 2, 8, 1, 8, "D = 2, cap 8 (B)");
	PKV_CHECK_MSG(LayoutOf(fx.geo, 1, RuleB(2, 8, 1, RuleMutant::kNoClamp)).bytes_per_position *
	                      static_cast<size_t>(RuleB(2, 8, 1, RuleMutant::kNoClamp)) !=
	                  y.bytes_per_position * 8,
	              "separation: the no-clamp mutant's page bytes equal one 8-position page");
	// The four after them (they hold under the no-clamp mutant too, as the plan says):
	for (int32_t b = 1; b <= 8; ++b) PKV_CHECK_EQ(sslm_kv_pages_for_budget(fx.model, b), 1);
	const uint32_t N = 3;
	for (int32_t budget = 0; budget <= 8; ++budget) {
		PagePoolOf pool(fx.model, N, y.page_bytes);
		PKV_CHECK_EQ(pool.status, SSLM_OK);
		if (!pool.pool) continue;
		sslm_status refusal = SSLM_OK;
		const int n = CountCreates(fx.model, &pool.pool, budget, &refusal);
		PKV_CHECK_MSG(n == static_cast<int>(N) && refusal == SSLM_KV_POOL_EXHAUSTED,
		              "a pool of %u pages admits %d %s creates (budget %d), refused with %s; %u owed", N, n,
		              budget ? "budget" : "legacy", budget, AbiStatusName(refusal), N);
	}
	// A share adopt of a frozen 5-token prefix maps 0 shared pages and copies one page.
	{
		PagePoolOf pool(fx.model, 4, y.page_bytes);
		PKV_CHECK_EQ(pool.status, SSLM_OK);
		if (pool.pool) {
			sslm_prefix px = nullptr;
			sslm_seq s = nullptr;
			PKV_CHECK_EQ(sslm_prefix_begin_budgeted(fx.model, &pool.pool, 5, &px), SSLM_OK);
			if (px) PKV_CHECK_EQ(PrefixPrefillAll(fx.model, px, Stream(51, 5, fx.vocab), 5), SSLM_OK);
			if (px) PKV_CHECK_EQ(sslm_prefix_freeze(px), SSLM_OK);
			PKV_CHECK_EQ(sslm_seq_create_budgeted(fx.model, &pool.pool, 1, &s), SSLM_OK);
			if (px && s) {
				PKV_CHECK_EQ(sslm_seq_adopt_prefix(s, px), SSLM_OK);
				uint32_t pp = 0, sp = 0, pm = 0, sm = 0;
				PKV_CHECK_EQ(sslm_pkv_test_only_prefix_table_entry(px, 0, &pp, &pm), SSLM_OK);
				PKV_CHECK_EQ(sslm_pkv_test_only_seq_table_entry(s, 0, &sp, &sm), SSLM_OK);
				PKV_CHECK_EQ(sm, 1);
				PKV_CHECK_MSG(sp != pp, "the adopter maps the prefix's page %u: a share at one page", pp);
				// The fill probe: two holders of one page each leave exactly 2 of 4 pages.
				sslm_status refusal = SSLM_OK;
				PKV_CHECK_EQ(CountCreates(fx.model, &pool.pool, 1, &refusal), 2);
				sslm_seq_kv_stats_out st{};
				st.struct_size = sizeof st;
				if (sslm_seq_kv_stats(s, &st) == SSLM_OK)
					std::printf("  diagnostic: adopter shared_pages=%u mapped_private_pages=%u\n", st.shared_pages,
					            st.mapped_private_pages);
			}
			if (s) sslm_seq_release(s);
			if (px) sslm_prefix_release(px);
		}
	}
	// Tokens and K/V rows equal the flat twin at widths 1, 7 and 8 (8 = the cap: ready token only).
	auto e = EngineOf(fx);
	if (!e->ok) return;
	for (int64_t W : {int64_t{1}, int64_t{7}, cap}) {
		const pkv_engine::WidthRun twin = FlatTwin(*e, W);
		PKV_CHECK_MSG(twin.fail.empty(), "flat twin at W = %lld: %s", static_cast<long long>(W), twin.fail.c_str());
		PagePoolOf pool(fx.model, 1, y.page_bytes);
		PKV_CHECK_EQ(pool.status, SSLM_OK);
		if (!pool.pool) continue;
		sslm_seq s = nullptr;
		PKV_CHECK_EQ(sslm_seq_create(fx.model, &pool.pool, &s), SSLM_OK);
		if (!s) continue;
		const AbiWidth w = RunAbiWidth(fx.model, s, W, fx.vocab, W < cap);
		PKV_CHECK_EQ(w.prefill, SSLM_OK);
		PKV_CHECK_EQ(w.ready, twin.ready);
		if (W < cap) PKV_CHECK_EQ(w.decode1, twin.decode1);
		std::vector<uint8_t> rows;
		const int64_t L = W < cap ? W + 1 : W;
		PKV_CHECK_MSG(SeqRows(pool.pool, s, fx.geo, 1, B, L, Rot::kRotated, &rows) &&
		                  Sha(rows.data(), rows.size()) == (W < cap ? twin.rows_decode1 : twin.rows_prefill),
		              "W = %lld: the rows read from the page at B = 8 equal the flat twin's", static_cast<long long>(W));
		sslm_seq_release(s);
	}
}

// 4.10 [T1]. The page condition's boundary (T-3178 G-4, R-1, D-SLM8412): D = 64, Int8, unpinned.
// Each fixture keeps the converter's kv_block_size 16, so 2ebe5e3 gives 63, 16, 65 and 127.
void Cell410Boundary() {
	struct Row { int cap; int64_t B, pages, slack, budget1_pages; };
	const Row rows[] = {{63, 63, 1, 0, 1}, {64, 64, 1, 0, 1}, {65, 64, 2, 63, 2}, {127, 64, 2, 1, 2}};
	for (const Row& r : rows) {
		const Fixture& fx = D64Cap(r.cap);
		if (!fx.ok) continue;
		const int64_t B = RuleB(64, static_cast<uint64_t>(r.cap), 1);
		PKV_CHECK_EQ(B, r.B);
		const Lay y = LayoutOf(fx.geo, 1, B);
		PKV_CHECK_EQ(y.pages, r.pages);
		PKV_CHECK_EQ(y.slack, r.slack);
		PKV_CHECK_MSG(sslm_kv_page_positions(fx.model) == B, "cap %d: B = %lld, the rule gives %lld", r.cap,
		              static_cast<long long>(sslm_kv_page_positions(fx.model)), static_cast<long long>(B));
		// The block stays 1.11.0's; pages and slack show in the overhead (rev 16.22, D-SLM8427).
		PKV_CHECK_MSG(sslm_kv_block_size(fx.model) == y.block_bytes, "cap %d: sslm_kv_block_size = %zu, 1.11.0's %zu owed",
		              r.cap, sslm_kv_block_size(fx.model), y.block_bytes);
		PKV_CHECK_MSG(sslm_kv_pool_overhead_size(fx.model, 1) == y.Overhead(1),
		              "cap %d: sslm_kv_pool_overhead_size(1) = %zu; 40 + %lld slack positions of %zu bytes is %zu", r.cap,
		              sslm_kv_pool_overhead_size(fx.model, 1), static_cast<long long>(y.slack), y.bytes_per_position,
		              y.Overhead(1));
		// A budget-1 holder reserves min(ceil(1/B) + 1, pages): graded by admission in a 4-page pool.
		{
			PagePoolOf pool(fx.model, 4, y.page_bytes);
			PKV_CHECK_EQ(pool.status, SSLM_OK);
			if (pool.pool) {
				sslm_status refusal = SSLM_OK;
				const int n = CountCreates(fx.model, &pool.pool, 1, &refusal);
				PKV_CHECK_MSG(n == 4 / r.budget1_pages, "cap %d: a 4-page pool admits %d budget-1 creates, %lld owed",
				              r.cap, n, static_cast<long long>(4 / r.budget1_pages));
			}
		}
		// A whole-cap holder writes to the cap and is refused at it.
		{
			BlockPoolOf pool(fx.model, 1, y);
			PKV_CHECK_EQ(pool.status, SSLM_OK);
			sslm_seq s = nullptr;
			if (pool.pool) PKV_CHECK_EQ(sslm_seq_create(fx.model, &pool.pool, &s), SSLM_OK);
			if (s) {
				PKV_CHECK_EQ(PrefillAll(fx.model, s, Stream(41, r.cap, fx.vocab), 16), SSLM_OK);
				sslm_status st = SSLM_OK;
				PKV_CHECK(Next(fx.model, s, &st) >= 0);
				PKV_CHECK_EQ(Next(fx.model, s, &st), -1);
				PKV_CHECK_EQ(st, SSLM_CONTEXT_CAP_EXCEEDED);
				// kills: a page count rounded down (cap 127 holds one page; the write to 64 has none)
				std::vector<uint8_t> rows;
				PKV_CHECK_MSG(SeqRows(pool.pool, s, fx.geo, 1, B, r.cap, Rot::kRotated, &rows),
				              "cap %d: the table covers all %d written positions at B = %lld", r.cap, r.cap,
				              static_cast<long long>(B));
				sslm_seq_release(s);
			}
		}
		// The adopt at the boundary: cap 64 copies the one page; cap 65 shares one full page.
		if (r.cap == 64 || r.cap == 65) {
			const int32_t len = r.cap == 64 ? 5 : 64;
			PagePoolOf pool(fx.model, 6, y.page_bytes);
			PKV_CHECK_EQ(pool.status, SSLM_OK);
			if (!pool.pool) continue;
			sslm_prefix px = nullptr;
			sslm_seq s = nullptr;
			PKV_CHECK_EQ(sslm_prefix_begin_budgeted(fx.model, &pool.pool, len, &px), SSLM_OK);
			if (px) PKV_CHECK_EQ(PrefixPrefillAll(fx.model, px, Stream(51, len, fx.vocab), 16), SSLM_OK);
			if (px) PKV_CHECK_EQ(sslm_prefix_freeze(px), SSLM_OK);
			PKV_CHECK_EQ(sslm_seq_create_budgeted(fx.model, &pool.pool, 1, &s), SSLM_OK);
			if (px && s) {
				PKV_CHECK_EQ(sslm_seq_adopt_prefix(s, px), SSLM_OK);
				uint32_t pp = 0, sp = 0, pm = 0, sm = 0;
				PKV_CHECK_EQ(sslm_pkv_test_only_prefix_table_entry(px, 0, &pp, &pm), SSLM_OK);
				PKV_CHECK_EQ(sslm_pkv_test_only_seq_table_entry(s, 0, &sp, &sm), SSLM_OK);
				if (r.cap == 64) {
					// claim (3) does not hold at exactly one page: 0 shared pages, the whole block copied
					PKV_CHECK_MSG(sp != pp, "cap 64: the adopter maps the prefix's page (a share at one page)");
				} else {
					// cap 65: the 64-token prefix's one full page is shared; nothing is copied
					PKV_CHECK_MSG(sp == pp, "cap 65: the adopter's page 0 is %u, the prefix's is %u (not shared)", sp, pp);
					PKV_CHECK_EQ(sm, 1);  // the shared page only: no tail page was copied
				}
			}
			if (s) sslm_seq_release(s);
			if (px) sslm_prefix_release(px);
		}
	}
	// Mutants: a clamp at 2B > cap gives cap 65 B = 65 and one page; floor(cap/B) gives cap 127 one page.
	Separates(RuleMutant::kClampAt2B, 64, 65, 1, 64, "cap 65");
	PKV_CHECK_MSG(CapPages(127, 64, true) != 2, "separation: the floor page count gives 2 pages at cap 127");
}

// 4.10 [T1]. The floor, the element width, the cap term's boundary and a non-power-of-two D.
void Cell410Extremes() {
	struct Row { const Fixture& (*base)(); uint32_t D; int64_t cap; int64_t prec; int64_t want; RuleMutant kills; };
	const Row rows[] = {
	    {D1024, 1024, 4096, -1, 16, RuleMutant::kNoFloor},   // the floor
	    {D1024, 1024, 32768, -1, 16, RuleMutant::kNoFloor},
	    {D512, 512, 4096, -1, 16, RuleMutant::kNone},        // 16 with or without the floor: a grid row
	    {GetD64, 64, 4096, 1, 32, RuleMutant::kNoElem},      // Int16 (elem 2)
	    {GetD64, 64, 4096, -1, 64, RuleMutant::kNoCapTerm},  // the cap term
	    {GetD64, 64, 4097, -1, 128, RuleMutant::kNone},
	    {GetD64, 64, 8192, -1, 128, RuleMutant::kNone},
	    {GetDef, 48, 4096, -1, 128, RuleMutant::kNoPow2},    // a non-power-of-two D
	};
	for (const Row& r : rows) {
		const Fixture& base = r.base();
		if (!base.ok) continue;
		At a(base, r.cap, -1, r.prec);
		PKV_CHECK_MSG(a.fx.ok, "D = %u, cap %lld, kv_precision %lld: the variant did not map (%s)", r.D,
		              static_cast<long long>(r.cap), static_cast<long long>(r.prec), AbiStatusName(a.map));
		if (!a.fx.ok) continue;
		const uint64_t elem = r.prec == 1 ? 2 : 1;
		PKV_CHECK_EQ(RuleB(r.D, static_cast<uint64_t>(r.cap), elem), r.want);
		const int64_t got = sslm_kv_page_positions(a.fx.model);
		PKV_CHECK_MSG(got == r.want, "D = %u, cap %lld, elem %llu: B = %lld, %lld owed", r.D,
		              static_cast<long long>(r.cap), static_cast<unsigned long long>(elem), static_cast<long long>(got),
		              static_cast<long long>(r.want));
		if (r.kills != RuleMutant::kNone) Separates(r.kills, r.D, static_cast<uint64_t>(r.cap), elem, r.want, "its row");
		if (r.cap == 4097)
			PKV_CHECK_MSG(sslm_kv_pool_overhead_size(a.fx.model, 1) == LayoutOf(a.fx.geo, 1, 128).Overhead(1),
			              "cap 4,097: 33 pages of 128 positions, so the overhead carries 127 slack positions");
	}
	// The 32-bit product wraps at D = 2^31 (the hostile row, model gap below); its separation:
	Separates(RuleMutant::kU32Product, uint64_t{1} << 31, 4096, 1, 16, "D = 2^31");
}

// 4.10 [T1]. A hostile head_dim, D = 2^20 and 2^31 (both even, so the loader's CFG1 checks admit
// them), on the CFG1-and-sigmoid-only fixture the plan specifies. The value asserted is 16.
// MODEL GAP (routed to the planner, record §4): sslm_kv_page_positions takes an sslm_model, and
// sslm_model_map runs BuildEngineCache, which marshals every layer's WGT1 weights; a CFG1-only
// artifact therefore cannot be mapped, and no seam evaluates the rule on a loaded view. The cell
// records the loader's status and the map status, and fails while the gap stands.
void Cell410HostileD() {
	for (uint64_t D : {uint64_t{1} << 20, uint64_t{1} << 31}) {
		superslm_test::Cfg1Spec spec;
		spec.hidden_size = 2;
		spec.num_hidden_layers = 2;
		spec.num_attention_heads = 1;
		spec.num_key_value_heads = 1;
		spec.head_dim = static_cast<uint32_t>(D);
		spec.intermediate_size = 2;
		spec.vocab_size = 256;
		spec.context_cap = 4096;
		spec.kv_precision = 0;
		spec.kv_block_size = 16;
		const std::vector<uint8_t> b = Cfg1OnlyArtifact(spec);
		std::string err;
		const superslm::SslmModelStatus ls = LoadStatus(b, &err);
		Fixture fx;
		sslm_status ms = SSLM_OK;
		MapInto(b, "cfg1only_d" + std::to_string(D), Geometry{2, 1, static_cast<uint32_t>(D), 4096}, &fx, &ms);
		std::printf("  D = %llu: SslmModel::Load = %s (%s); sslm_model_map = %s\n", static_cast<unsigned long long>(D),
		            superslm::SslmModelStatusName(ls), err.c_str(), AbiStatusName(ms));
		PKV_CHECK_MSG(fx.ok, "D = %llu: the CFG1-only fixture does not map (%s), so B cannot be read (model gap)",
		              static_cast<unsigned long long>(D), AbiStatusName(ms));
		if (fx.ok) PKV_CHECK_EQ(sslm_kv_page_positions(fx.model), 16);
		PKV_CHECK_EQ(RuleB(D, 4096, 1), 16);
	}
}

// 4.10 [T1]. Generality: the rule's B for every §17.1 model row, at its game cap 4,096 and its
// max_position_embeddings cap. The rule reads (D, cap, elem) only; each row is carried by a fixture
// with that D, cap and element width (layers and KV heads do not enter the rule). The plan specifies
// CFG1-only fixtures, which do not map (the gap of :hostile-d), so the rows run on mapped fixtures.
void Cell410Generality() {
	struct Row { const char* model; const Fixture& (*base)(); uint32_t D; int64_t cap; int64_t want; };
	const Row rows[] = {
	    {"Qwen2.5-0.5B", GetD64, 64, 4096, 64},         {"Qwen2.5-0.5B", GetD64, 64, 32768, 128},
	    {"Qwen2.5-1.5B", GetQk, 128, 4096, 64},         {"Qwen2.5-1.5B", GetQk, 128, 32768, 64},
	    {"Qwen2.5-3B", GetQk, 128, 32768, 64},          {"Qwen2.5-7B", GetQk, 128, 32768, 64},
	    {"Qwen3-0.6B/1.7B/4B/8B", GetQk, 128, 40960, 64}, {"Qwen3-0.6B/1.7B/4B/8B", GetQk, 128, 4096, 64},
	};
	for (const Row& r : rows) {
		const Fixture& base = r.base();
		if (!base.ok) continue;
		At a(base, r.cap);
		if (!a.fx.ok) {
			PKV_CHECK_MSG(false, "%s row (D %u, cap %lld) did not map", r.model, r.D, static_cast<long long>(r.cap));
			continue;
		}
		PKV_CHECK_EQ(RuleB(r.D, static_cast<uint64_t>(r.cap), 1), r.want);
		PKV_CHECK_MSG(sslm_kv_page_positions(a.fx.model) == r.want, "%s (D %u, cap %lld): B = %lld, %lld owed", r.model,
		              r.D, static_cast<long long>(r.cap), static_cast<long long>(sslm_kv_page_positions(a.fx.model)),
		              static_cast<long long>(r.want));
	}
}

// 4.11 [T1]. Header independence: kv_block_size in {1, 16, 32, 4,096, 0xFFFFFFFF} gives the rule's B
// at each (D, cap) of the grid, and 0 is still refused at load.
void Cell411() {
	const uint32_t kvbs[] = {1, 16, 32, 4096, 0xFFFFFFFFu};
	for (const DBase& b : kBases) {
		const Fixture& base = b.get();
		if (!base.ok) continue;
		for (int64_t cap : kGridCaps) {
			const int64_t want = RuleB(b.D, static_cast<uint64_t>(cap), 1);
			for (uint32_t k : kvbs) {
				At a(base, cap, k);
				if (!a.fx.ok) {
					PKV_CHECK_MSG(false, "D %u cap %lld kv_block_size %u did not map (%s)", b.D,
					              static_cast<long long>(cap), k, AbiStatusName(a.map));
					continue;
				}
				const int64_t got = sslm_kv_page_positions(a.fx.model);
				if (got != want)
					PKV_CHECK_MSG(false, "D %u, cap %lld, kv_block_size %u: B = %lld, the rule gives %lld", b.D,
					              static_cast<long long>(cap), k, static_cast<long long>(got),
					              static_cast<long long>(want));
				else
					PKV_CHECK(true);
			}
		}
		At zero(base, 4096, 0);
		PKV_CHECK_MSG(!zero.fx.ok && zero.map == SSLM_ARTIFACT_REJECTED,
		              "D %u: kv_block_size 0 maps with %s; the loader must refuse it", b.D, AbiStatusName(zero.map));
	}
	Separates(RuleMutant::kReadsCfg1, 64, 4096, 1, 64, "D = 64, cap 4,096, kv_block_size 32");
}

// 4.12 [T1]. The pin's refusals in the test library, each leaving the model's B unchanged.
void Cell412Refusals() {
	const Fixture& def = GetFixture("pkv_def");
	if (!def.ok) return;
	const int64_t rule = RuleB(48, 4096, 1);  // 128
	// A pin after a pool exists is refused.
	{
		At a(def, 4096);
		if (a.fx.ok) {
			LegacyPool pool(a.fx.model, 1);
			PKV_CHECK_EQ(pool.status, SSLM_OK);
			PKV_CHECK_EQ(KvPagesTestPinPagePositions(a.fx.model, 16), SSLM_INVALID_ARGUMENT);
			PKV_CHECK_EQ(sslm_kv_page_positions(a.fx.model), rule);
		}
	}
	// A B that neither divides the cap nor is the cap (the cap-4,100 case is 4.12/T2:cap4100).
	{
		At a(def, 4096);
		if (a.fx.ok) {
			PKV_CHECK_EQ(KvPagesTestPinPagePositions(a.fx.model, 48), SSLM_INVALID_ARGUMENT);
			PKV_CHECK_EQ(sslm_kv_page_positions(a.fx.model), rule);
		}
	}
	// Hostile pins (G9): 0, -1, cap + 1, 2^31; the model is left unchanged.
	for (int64_t pin : {int64_t{0}, int64_t{-1}, int64_t{4097}, int64_t{1} << 31}) {
		At a(def, 4096);
		if (!a.fx.ok) continue;
		PKV_CHECK_MSG(KvPagesTestPinPagePositions(a.fx.model, pin) == SSLM_INVALID_ARGUMENT, "pin %lld is not refused",
		              static_cast<long long>(pin));
		PKV_CHECK_MSG(sslm_kv_page_positions(a.fx.model) == rule, "after the refused pin %lld, B = %lld, not the rule's %lld",
		              static_cast<long long>(pin), static_cast<long long>(sslm_kv_page_positions(a.fx.model)),
		              static_cast<long long>(rule));
	}
}

// 4.12 [T1]. A pin that takes: 16 (every generated-fixture cell's B), 1 (the hand-built fixtures'
// B = 1 geometry, G36; see the model gap in the record: the 21 hand-built specs are engine-level
// fixtures with no sslm_model, so the cell pins pkv_def to 1 as cell 4.9 stands in for them), and
// the cap. Without the pin the model reports the rule's B (128), not the pinned value.
void Cell412Pins() {
	const Fixture& def = GetFixture("pkv_def");
	if (!def.ok) return;
	for (int64_t pin : {int64_t{16}, int64_t{1}, int64_t{4096}, int64_t{4}}) {
		At a(def, 4096, 32);  // kv_block_size 32, so 2ebe5e3's own reading cannot supply the pinned value
		if (!a.fx.ok) continue;
		PKV_CHECK_MSG(sslm_kv_page_positions(a.fx.model) == 128, "unpinned: B = %lld, the rule's is 128",
		              static_cast<long long>(sslm_kv_page_positions(a.fx.model)));
		PKV_CHECK_MSG(KvPagesTestPinPagePositions(a.fx.model, pin) == SSLM_OK, "the pin to %lld is refused",
		              static_cast<long long>(pin));
		PKV_CHECK_MSG(sslm_kv_page_positions(a.fx.model) == pin, "after the pin to %lld, B = %lld",
		              static_cast<long long>(pin), static_cast<long long>(sslm_kv_page_positions(a.fx.model)));
		const Lay y = LayoutOf(a.fx.geo, 1, pin);
		PKV_CHECK_EQ(sslm_kv_page_size(a.fx.model), y.page_bytes);
	}
}

PKV_CELL("4.10/T1:grid", "T1", Cell410Grid);
PKV_CELL("4.10/T1:below-page", "T1", Cell410BelowPage);
PKV_CELL("4.10/T1:boundary", "T1", Cell410Boundary);
PKV_CELL("4.10/T1:extremes", "T1", Cell410Extremes);
PKV_CELL("4.10/T1:hostile-d", "T1", Cell410HostileD);
PKV_CELL("4.10/T1:generality", "T1", Cell410Generality);
PKV_CELL("4.11/T1", "T1", Cell411);
PKV_CELL("4.12/T1:refusals", "T1", Cell412Refusals);
PKV_CELL("4.12/T1:pins", "T1", Cell412Pins);

}  // namespace
