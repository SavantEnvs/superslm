// Paged-KV plan rev 16.21 §23.8, step T0: the R9 red suite's surface and shared harness.
//
// What R9 changes (plan §3.1, §21, §23.8): T1 derives the page size B from (head_dim, cap, element
// width) and rotates each run's slot by the logical page; T2 removes the one-page fallback (a
// partial last page at a cap B does not divide); T3 adds the contiguous pool option. At 2ebe5e3 B is
// CFG1's kv_block_size when it divides the cap and the cap otherwise, nothing rotates, and the
// option does not exist, so every cell here is red on 2ebe5e3 by construction of what it asserts.
//
// This file carries:
//   - the R9 surface, declared as the plan states it (§3.6's table, §3.1's pin). The test author's
//     readings where the plan leaves a field open are named at each declaration; the builder keeps
//     them or changes this file and the cells together, never the assertions;
//   - §3.1's rule and the address formula, written here from the plan's text, with the mutants the
//     cells name, so each cell's separation of each named mutant is executed at the cell's own
//     construction (the mutant's value substituted for the engine's observable);
//   - fixtures: the generated ones (tools/gen_paged_kv_fixture.py, tests/paged-kv/r9_gen_fixtures.py)
//     and variants made from them by patching CFG1 (cap, kv_precision, kv_block_size) and, for the
//     geometry-only cells, stripping the RoPE table (which sslm_model_map does not need: ROP1 is
//     optional at load and BuildEngineCache reads only WGT1 and the composition constants);
//   - an independent twin for fixtures that have no committed reference: the engine's flat layer
//     loops over the test's own one-page buffer (pkv_engine_helpers.h, B = cap, table {0}), which
//     reach neither the page module, the ABI holders nor the derived B.
//
// Every verdict reads only statuses, admission counts against pools the test sizes from its own
// formulas, tokens, blob bytes, and raw pool bytes through the peek seams addressed by the formula
// written here (plan §7's grading rule). sslm_kv_pool_stats and sslm_seq_kv_stats are recorded as
// diagnostics only, except where a cell's subject is the stats verb itself.

#ifndef SUPERSLM_TESTS_PKV_R9_H
#define SUPERSLM_TESTS_PKV_R9_H

#include "pkv_budget_c_helpers.h"
#include "pkv_common.h"
#include "pkv_engine_helpers.h"
#include "pkv_legacy_a_helpers.h"

#include "superslm/artifact.h"
#include "superslm/sha256.h"

#include "sslm_cfg1_hostile_fixtures.h"
#include "sslm_fixtures.h"
#include "sslm_sil1_hostile_fixtures.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {

// §3.6 / §21.2: the bytes a contiguous pool of `block_count` blocks needs: blocks of `cap` positions,
// `cap * L*2*H_kv*D*elem` bytes each, plus the page module's overhead for `block_count` pages
// (8 * block_count + 32). 0 on a null model, block_count 0, or when the product or the sum overflows.

// §3.6 / §21.2: a pool whose page is `cap` positions. Refusals in sslm_kv_pool_create's order: null
// out, null model or buffer, block_count 0 -> SSLM_INVALID_ARGUMENT; an overflowing size ->
// SSLM_INVALID_ARGUMENT; buf_size below the size -> SSLM_BUFFER_TOO_SMALL; a misaligned buffer ->
// SSLM_MISALIGNED_BUFFER; heap failure -> SSLM_ALLOCATION_FAILED.
size_t sslm_kv_pool_contiguous_size(sslm_model model, uint32_t block_count);
sslm_status sslm_kv_pool_create_contiguous(sslm_model model, void* buf, size_t buf_size, uint32_t block_count,
                                           sslm_kv_pool* out);

// §3.1: the test-only pin, compiled only with SUPERSLM_ENABLE_KV_PAGES_TEST_SEAMS. The plan names it
// `KvPagesTestPinPagePositions(model, int64_t B)` and says what it refuses; the test author's
// reading of the rest: C linkage (so cell 4.12's symbol scan names one symbol), and an sslm_status
// result, SSLM_OK when the pin took and SSLM_INVALID_ARGUMENT for every refusal (a B that neither
// divides the cap nor is the cap, B <= 0, B > cap, a pin after a pool exists on the model).
sslm_status KvPagesTestPinPagePositions(sslm_model model, int64_t page_positions);

// G19's block-index peek (src/sslm_abi.cpp, every library build): n raw bytes of block `block_index`.
sslm_status sslm_g5_test_only_peek_kv_block_bytes(sslm_kv_pool pool, uint32_t block_index, uint8_t* out_buf, size_t n);

}  // extern "C"

namespace pkv {
namespace r9 {

// ---- §3.1's rule, written here -------------------------------------------------------------------

// The mutants the cells name (4.10, 4.4, 10.9, 2.9b, 2.9c). kShipped is 2ebe5e3's rule: CFG1's
// kv_block_size when it divides the cap, else the cap.
enum class RuleMutant {
	kNone,
	kNoCapTerm,     // the run target is 8 KiB everywhere (4.10)
	kNoFloor,       // no B >= 16 floor (4.10)
	kNoElem,        // B * D >= S, the element width dropped (4.10)
	kU32Product,    // the rule's loop product formed in 32 bits (4.10)
	kNoPow2,        // the smallest B >= 16 with B * D * elem >= S, not a power of two (4.10)
	kReadsCfg1,     // B = CFG1 kv_block_size where it is at most the cap (4.10, 4.11)
	kNoClamp,       // no B = cap below one rule page (4.10)
	kClampAt2B,     // the clamp fires at 2B > cap (4.10's boundary row)
	kFallback,      // one page at a cap B does not divide (4.4, 4.10, 10.9)
	kShipped,       // 2ebe5e3: kv_block_size if it divides the cap, else the cap
};

inline const char* RuleMutantName(RuleMutant m) {
	switch (m) {
		case RuleMutant::kNone: return "the rule";
		case RuleMutant::kNoCapTerm: return "no cap term";
		case RuleMutant::kNoFloor: return "no floor";
		case RuleMutant::kNoElem: return "elem dropped";
		case RuleMutant::kU32Product: return "32-bit product";
		case RuleMutant::kNoPow2: return "no power-of-two rounding";
		case RuleMutant::kReadsCfg1: return "reads CFG1 kv_block_size";
		case RuleMutant::kNoClamp: return "no clamp below one page";
		case RuleMutant::kClampAt2B: return "clamp at 2B > cap";
		case RuleMutant::kFallback: return "one-page fallback";
		case RuleMutant::kShipped: return "2ebe5e3's rule";
	}
	return "?";
}

// §3.1: S = 4 KiB when D <= 64 and cap <= 4,096, else 8 KiB; B = the smallest power of two with
// B >= 16 and B * D * elem >= S; B = cap when B > cap. `kvbs` is CFG1's kv_block_size, read only by
// the mutants that read it. Returns 0 where a mutant's arithmetic wraps to nothing.
inline int64_t RuleB(uint64_t D, uint64_t cap, uint64_t elem, RuleMutant m = RuleMutant::kNone, uint64_t kvbs = 16) {
	if (m == RuleMutant::kShipped)
		return static_cast<int64_t>((kvbs >= 1 && kvbs <= cap && cap % kvbs == 0) ? kvbs : cap);
	if (m == RuleMutant::kReadsCfg1) return static_cast<int64_t>(kvbs >= 1 && kvbs <= cap ? kvbs : cap);
	const uint64_t S = (m != RuleMutant::kNoCapTerm && D <= 64 && cap <= 4096) ? 4096 : 8192;
	const uint64_t e = m == RuleMutant::kNoElem ? 1 : elem;
	const uint64_t floor_b = m == RuleMutant::kNoFloor ? 1 : 16;
	uint64_t B = 0;
	if (m == RuleMutant::kNoPow2) {
		B = std::max<uint64_t>(floor_b, (S + D * e - 1) / (D * e));
	} else if (m == RuleMutant::kU32Product) {
		uint32_t b = static_cast<uint32_t>(floor_b);
		while (b != 0 && static_cast<uint32_t>(b * static_cast<uint32_t>(D) * static_cast<uint32_t>(e)) < S) b *= 2;
		B = b;
	} else {
		B = floor_b;
		while (B * D * e < S) B *= 2;
	}
	if (m == RuleMutant::kClampAt2B) {
		if (2 * B > cap) B = cap;
	} else if (m != RuleMutant::kNoClamp && B > cap) {
		B = cap;
	}
	if (m == RuleMutant::kFallback && B != 0 && cap % B != 0) B = cap;
	return static_cast<int64_t>(B);
}

inline int64_t CeilDiv(int64_t a, int64_t b) { return b > 0 ? (a + b - 1) / b : 0; }
// Pages of a whole-cap holder (§3.1): ceil(cap / B). The floor mutant (10.9, 4.10) counts floor.
inline int64_t CapPages(int64_t cap, int64_t B, bool floor_mutant = false) {
	return B <= 0 ? 0 : floor_mutant ? cap / B : CeilDiv(cap, B);
}
inline int64_t Slack(int64_t cap, int64_t B) { return CapPages(cap, B) * B - cap; }
// §3.4: R(budget) = min(ceil(budget / B) + 1, ceil(cap / B)).
inline int64_t Reservation(int64_t budget, int64_t cap, int64_t B) {
	return std::min<int64_t>(CeilDiv(budget, B) + 1, CapPages(cap, B));
}

// The page geometry the test derives for a model: element width from CFG1, B by the rule (or a pin).
// Rev 16.22 (D-SLM8427) separates two quantities. The flat block, `block_bytes`, is 1.11.0's
// L*cap*H_kv*D*2*elem at every cap: sslm_kv_block_size and the legacy save's K/V section. A legacy
// holder's ceil(cap/B) pages hold `slack` positions more, and sslm_kv_pool_overhead_size carries them:
// Overhead(N) = 8N + 32 + N * slack * q. So N blocks need N * pool_block_bytes + 8N + 32 bytes.
struct Lay {
	int64_t cap = 0, B = 0, pages = 0, slack = 0;
	uint32_t runs = 0;
	size_t elem = 1, bytes_per_position = 0, page_bytes = 0, block_bytes = 0, pool_block_bytes = 0,
	       contiguous_block_bytes = 0;
	size_t Overhead(uint64_t n) const { return static_cast<size_t>(8 * n + 32 + n * slack * bytes_per_position); }
	size_t PoolBytes(uint64_t n) const { return static_cast<size_t>(n * block_bytes) + Overhead(n); }
};

inline Lay LayoutOf(const Geometry& g, size_t elem, int64_t B) {
	Lay y;
	y.cap = g.context_cap;
	y.B = B;
	y.pages = CapPages(g.context_cap, B);
	y.slack = Slack(g.context_cap, B);
	y.runs = g.layers * 2u * g.kv_heads;
	y.elem = elem;
	y.bytes_per_position = size_t{g.layers} * 2u * g.kv_heads * g.head_dim * elem;
	y.page_bytes = y.bytes_per_position * static_cast<size_t>(B);
	y.block_bytes = y.bytes_per_position * static_cast<size_t>(g.context_cap);  // 1.11.0's block (D-SLM8427)
	y.pool_block_bytes = y.page_bytes * static_cast<size_t>(y.pages);          // ceil(cap/B) pages
	y.contiguous_block_bytes = y.block_bytes;
	return y;
}

// ---- the address formula, written here (§3.1) ----------------------------------------------------

// Slot of run r = (l*2 + half)*H_kv + h on logical page j: (r + j) mod runs, and the mutants 6.6a
// names: (r + (j & 63)) mod runs, (r + j) & (runs - 1), (r + j) mod pages, and a slot keyed on the
// physical page id; kNone is 2ebe5e3's unrotated slot r (7.9-MR-OPT's layout).
enum class Rot { kRotated, kNone, kJAnd63, kAndMask, kModPages, kPhysical };

inline const char* RotName(Rot m) {
	switch (m) {
		case Rot::kRotated: return "(r + j) mod runs";
		case Rot::kNone: return "unrotated (slot r)";
		case Rot::kJAnd63: return "(r + (j & 63)) mod runs";
		case Rot::kAndMask: return "(r + j) & (runs - 1)";
		case Rot::kModPages: return "(r + j) mod pages";
		case Rot::kPhysical: return "slot keyed on the physical page id";
	}
	return "?";
}

inline uint64_t Slot(uint64_t r, uint64_t j, uint64_t runs, Rot m, uint64_t physical = 0, uint64_t pages = 1) {
	switch (m) {
		case Rot::kRotated: return (r + j) % runs;
		case Rot::kNone: return r;
		case Rot::kJAnd63: return (r + (j & 63)) % runs;
		case Rot::kAndMask: return (r + j) & (runs - 1);
		case Rot::kModPages: return (r + j) % (pages ? pages : 1);
		case Rot::kPhysical: return (r + physical) % runs;
	}
	return r;
}

// Byte offset of (l, half, h, pos) inside its page, at page size B.
inline size_t InPage(const Geometry& g, size_t elem, int64_t B, uint32_t l, uint32_t half, uint32_t h, int64_t pos,
                     Rot m = Rot::kRotated, uint64_t physical = 0, uint64_t pages = 1) {
	const uint64_t runs = uint64_t{g.layers} * 2u * g.kv_heads;
	const uint64_t r = (uint64_t{l} * 2u + half) * g.kv_heads + h;
	const uint64_t j = static_cast<uint64_t>(pos / B);
	const uint64_t slot = Slot(r, j, runs, m, physical, pages);
	return static_cast<size_t>((slot * static_cast<uint64_t>(B) + static_cast<uint64_t>(pos % B)) * g.head_dim * elem);
}

// The rows [0, L) of a holder whose mapped table is `table`, read from the pool's raw pages through
// the page-index peek at page size B under address formula `m`, in pkv_scenarios.h's position-major
// order (so a digest compares with a reference record's rows_sha or a twin's). False on a seam
// refusal or a table that does not cover L.
inline bool RowsFromTable(sslm_kv_pool pool, const std::vector<uint32_t>& table, const Geometry& g, size_t elem,
                          int64_t B, int64_t L, Rot m, std::vector<uint8_t>* rows) {
	rows->clear();
	if (B <= 0 || static_cast<int64_t>(table.size()) * B < L) return false;
	const Lay y = LayoutOf(g, elem, B);
	const size_t rb = size_t{g.head_dim} * elem;
	std::map<uint32_t, std::vector<uint8_t>> pages;
	for (int64_t p = 0; p < L; ++p) {
		const uint32_t page = table[static_cast<size_t>(p / B)];
		auto it = pages.find(page);
		if (it == pages.end()) {
			it = pages.emplace(page, legacy_a::PeekPage(pool, page, y.page_bytes)).first;
			if (it->second.empty()) return false;
		}
		for (uint32_t l = 0; l < g.layers; ++l)
			for (uint32_t half = 0; half < 2; ++half)
				for (uint32_t h = 0; h < g.kv_heads; ++h) {
					const size_t at = InPage(g, elem, B, l, half, h, p, m, page, static_cast<uint64_t>(y.pages));
					if (at + rb > it->second.size()) return false;
					rows->insert(rows->end(), it->second.begin() + static_cast<std::ptrdiff_t>(at),
					             it->second.begin() + static_cast<std::ptrdiff_t>(at + rb));
				}
	}
	return true;
}

inline bool SeqRows(sslm_kv_pool pool, sslm_seq s, const Geometry& g, size_t elem, int64_t B, int64_t L, Rot m,
                    std::vector<uint8_t>* rows) {
	std::vector<uint32_t> table;
	if (!legacy_a::SeqTable(s, &table)) return false;
	return RowsFromTable(pool, table, g, elem, B, L, m, rows);
}

// ---- fixtures --------------------------------------------------------------------------------

// CFG1 field offsets inside the Config section (model.cpp's ParseConfig).
constexpr size_t kCfgHeadDim = 24, kCfgCap = 36, kCfgKvPrecision = 44, kCfgKvBlockSize = 48;

inline bool MapInto(std::vector<uint8_t> b, const std::string& stem, const Geometry& geo, Fixture* out,
                    sslm_status* map_status = nullptr) {
	out->stem = stem;
	out->geo = geo;
	out->size = b.size();
	out->bytes.Reset(b.size());
	std::memcpy(out->bytes.p, b.data(), b.size());
	out->sha = Sha(b.data(), b.size());
	const sslm_status st = sslm_model_map(out->bytes.p, out->size, &out->model);
	if (map_status) *map_status = st;
	out->ok = st == SSLM_OK;
	return out->ok;
}

// One of r9_gen_fixtures.py's files (or a generated one), by stem, from SUPERSLM_PAGED_KV_FIXTURE_DIR,
// loaded once. A missing file fails the cell.
inline const Fixture& Get(const char* stem, Geometry geo) {
	static std::map<std::string, std::unique_ptr<Fixture>> cache;
	auto it = cache.find(stem);
	if (it != cache.end()) {
		PKV_CHECK_MSG(it->second->ok, "fixture %s did not load (see its first failure)", stem);
		return *it->second;
	}
	auto f = std::make_unique<Fixture>();
	const char* dir = std::getenv("SUPERSLM_PAGED_KV_FIXTURE_DIR");
	PKV_CHECK_MSG(dir && *dir, "SUPERSLM_PAGED_KV_FIXTURE_DIR is not set; run tests/paged-kv/r9_gen_fixtures.py DIR");
	std::vector<uint8_t> raw;
	if (dir && *dir && ReadAll(std::string(dir) + "/" + stem + ".sslm", &raw) && !raw.empty()) {
		sslm_status st = SSLM_OK;
		PKV_CHECK_MSG(MapInto(std::move(raw), stem, geo, f.get(), &st), "sslm_model_map(%s) == %s", stem,
		              AbiStatusName(st));
	} else {
		PKV_CHECK_MSG(false, "fixture %s/%s.sslm not found; run tests/paged-kv/r9_gen_fixtures.py",
		              dir ? dir : "(unset)", stem);
	}
	const Fixture& ref = *f;
	cache[stem] = std::move(f);
	return ref;
}

// The R9 fixtures (r9_gen_fixtures.py's ROWS; two layers and two KV heads unless named).
inline const Fixture& D2Cap8() { return Get("r9_d2_c8", Geometry{2, 2, 2, 8}); }
inline const Fixture& D64Cap(int cap) {
	const std::string stem = "r9_d64_c" + std::to_string(cap);
	return Get(stem.c_str(), Geometry{2, 2, 64, cap});
}
inline const Fixture& D512() { return Get("r9_d512_c64", Geometry{2, 2, 512, 64}); }
inline const Fixture& D1024() { return Get("r9_d1024_c64", Geometry{2, 2, 1024, 64}); }
inline const Fixture& L3() { return Get("r9_l3_c4096", Geometry{3, 2, 48, 4096}); }
inline const Fixture& Q8() { return Get("r9_q8_c64", Geometry{36, 8, 128, 64}); }

// A copy of `base` with CFG1 fields patched (any field < 0 is kept) and, when `strip_rope`, the RoPE
// table dropped and the artifact re-laid-out; the integrity hash is recomputed and the header flags
// kept. The weights and every other section are byte-for-byte the base's.
struct Variant {
	int64_t cap = -1;
	int64_t kv_block_size = -1;
	int64_t kv_precision = -1;  // 0 Int8, 1 Int16
	bool strip_rope = false;
};

inline bool VariantBytes(const Fixture& base, const Variant& v, std::vector<uint8_t>* out) {
	if (!base.ok) return false;
	const uint8_t* p = base.bytes.p;
	const size_t n = base.size;
	uint32_t count = 0, flags = 0;
	std::memcpy(&count, p + 12, 4);
	std::memcpy(&flags, p + 16, 4);
	std::vector<superslm_test::FixtureSection> sections;
	bool patched = false;
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t* row = p + superslm::kHeaderBytes + size_t{i} * superslm::kSectionDescBytes;
		uint32_t type = 0, dtype = 0, align = 0;
		uint64_t off = 0, size = 0;
		std::memcpy(&type, row, 4);
		std::memcpy(&dtype, row + 4, 4);
		std::memcpy(&off, row + 8, 8);
		std::memcpy(&size, row + 16, 8);
		std::memcpy(&align, row + 32, 4);
		if (off + size > n) return false;
		if (v.strip_rope && type == static_cast<uint32_t>(superslm::SslmSectionType::RopeTables)) continue;
		superslm_test::FixtureSection s;
		s.type = type;
		s.dtype = dtype;
		s.alignment = align;
		s.data.assign(p + off, p + off + size);
		if (type == static_cast<uint32_t>(superslm::SslmSectionType::Config) && s.data.size() >= kCfgKvBlockSize + 4) {
			auto put = [&](size_t at, int64_t value) {
				if (value < 0) return;
				const uint32_t u = static_cast<uint32_t>(value);
				std::memcpy(s.data.data() + at, &u, 4);
			};
			put(kCfgCap, v.cap);
			put(kCfgKvPrecision, v.kv_precision);
			put(kCfgKvBlockSize, v.kv_block_size);
			patched = true;
		}
		sections.push_back(std::move(s));
	}
	if (!patched) return false;
	*out = superslm_test::BuildArtifact(sections).bytes;
	superslm_test::PutU32(*out, 16, flags);
	superslm_test::RecomputeIntegrityHash(*out);
	return true;
}

// Maps the variant into `out` (geometry: the base's, with the patched cap). False, with the map
// status in *st, when the variant does not map.
inline bool MakeVariant(const Fixture& base, const Variant& v, Fixture* out, sslm_status* st = nullptr) {
	std::vector<uint8_t> b;
	if (!VariantBytes(base, v, &b)) {
		if (st) *st = SSLM_INVALID_ARGUMENT;
		return false;
	}
	Geometry g = base.geo;
	if (v.cap >= 0) g.context_cap = v.cap;
	std::string stem = base.stem;
	if (v.cap >= 0) stem += "_cap" + std::to_string(v.cap);
	if (v.kv_block_size >= 0) stem += "_kvbs" + std::to_string(v.kv_block_size);
	if (v.kv_precision >= 0) stem += "_prec" + std::to_string(v.kv_precision);
	if (v.strip_rope) stem += "_norope";
	return MapInto(std::move(b), stem, g, out, st);
}

// A CFG1-and-SigmoidLut-only artifact, the shape cells 2.9d, 4.10's hostile head_dim and 4.10's
// generality cell specify ("CFG1 and the sigmoid table only, the sections OpenFromMemory requires").
inline std::vector<uint8_t> Cfg1OnlyArtifact(const superslm_test::Cfg1Spec& spec) {
	return superslm_test::BuildArtifact({superslm_test::MakeSection(superslm::SslmSectionType::Config,
	                                                                superslm::SslmDtype::Raw,
	                                                                superslm_test::BuildCfg1(spec)),
	                                     superslm_test::MakeSigmoidLutSection()})
	    .bytes;
}

// The SslmModel::Load status of a byte image (the loader the cells cite), and its message.
inline superslm::SslmModelStatus LoadStatus(const std::vector<uint8_t>& b, std::string* err) {
	superslm::SslmModelView view;
	return superslm::SslmModel::Load(b.data(), b.size(), view, err);
}

// ---- pools and admission -----------------------------------------------------------------------

// A page pool of `pages` pages whose buffer is sized from the TEST's page bytes (plus the engine's
// overhead, which no verdict reads). A build whose page is larger than the rule's refuses the create
// with SSLM_BUFFER_TOO_SMALL, which the cell sees as a status.
struct PagePoolOf {
	AlignedBuf mem;
	sslm_kv_pool pool = nullptr;
	sslm_status status = SSLM_INVALID_ARGUMENT;
	PagePoolOf(sslm_model model, uint32_t pages, size_t page_bytes, uint8_t fill = 0) {
		mem.Reset(page_bytes * pages + sslm_kv_page_pool_overhead_size(model, pages), fill);
		status = sslm_kv_page_pool_create(model, mem.p, mem.n, pages, &pool);
	}
	~PagePoolOf() {
		if (pool) sslm_kv_pool_destroy(pool);
	}
};

// A legacy block pool of `blocks` blocks in exactly the bytes the test computes for it:
// N * block + Overhead(N) (rev 16.22: the pages ceil(cap/B) * page_bytes per block, plus 8N + 32).
struct BlockPoolOf {
	AlignedBuf mem;
	sslm_kv_pool pool = nullptr;
	sslm_status status = SSLM_INVALID_ARGUMENT;
	BlockPoolOf(sslm_model model, uint32_t blocks, const Lay& y, uint8_t fill = 0) {
		mem.Reset(y.PoolBytes(blocks), fill);
		status = sslm_kv_pool_create(model, mem.p, mem.n, blocks, &pool);
	}
	~BlockPoolOf() {
		if (pool) sslm_kv_pool_destroy(pool);
	}
};

// The test's contiguous-pool size (§21.2): block_count * cap * bytes_per_position + 8 * block_count
// + 32, in 128 bits so the cells can name an overflowing count. *fits is false past SIZE_MAX.
inline uint64_t ContiguousSize(const Lay& y, uint64_t block_count, bool* fits) {
	const uint64_t per = static_cast<uint64_t>(y.contiguous_block_bytes);
	// 128-bit by halves: (hi, lo) = block_count * (per + 8) + 32.
	const uint64_t m = per + 8;
	const uint64_t a_lo = (block_count & 0xFFFFFFFFull) * m;
	const uint64_t hi_part = (block_count >> 32) * m;  // contributes << 32
	const uint64_t lo_add = hi_part << 32;
	uint64_t lo = a_lo + lo_add;
	bool overflow = (hi_part >> 32) != 0 || lo < a_lo;
	const uint64_t lo2 = lo + 32;
	overflow = overflow || lo2 < lo;
	*fits = !overflow;
	return lo2;
}

struct ContigPoolOf {
	AlignedBuf mem;
	sslm_kv_pool pool = nullptr;
	sslm_status status = SSLM_INVALID_ARGUMENT;
	ContigPoolOf(sslm_model model, uint32_t blocks, size_t bytes, uint8_t fill = 0) {
		mem.Reset(bytes, fill);
		status = sslm_kv_pool_create_contiguous(model, mem.p, mem.n, blocks, &pool);
	}
	~ContigPoolOf() {
		if (pool) sslm_kv_pool_destroy(pool);
	}
};

// Creates until a refusal and counts the admissions: legacy creates when budget == 0, budget creates
// of `budget` otherwise. Every made handle is released. The refusal is reported in *refusal.
inline int CountCreates(sslm_model model, sslm_kv_pool* pool, int32_t budget, sslm_status* refusal) {
	std::vector<sslm_seq> made;
	sslm_status st = SSLM_OK;
	for (int guard = 0; guard < 1 << 20; ++guard) {
		sslm_seq s = nullptr;
		st = budget == 0 ? sslm_seq_create(model, pool, &s) : sslm_seq_create_budgeted(model, pool, budget, &s);
		if (st != SSLM_OK) break;
		made.push_back(s);
	}
	if (refusal) *refusal = st;
	for (auto it = made.rbegin(); it != made.rend(); ++it) sslm_seq_release(*it);
	return static_cast<int>(made.size());
}

// ---- tokens --------------------------------------------------------------------------------------

inline int32_t Next(sslm_model model, sslm_seq s, sslm_status* st = nullptr) { return NextToken(model, s, st); }

// Prefill `tokens` in chunks, then the ready token, then (when `decode1`) one more step: the widths
// scenario's shape (pkv_engine_helpers.h RunWidthUncached), through the ABI.
struct AbiWidth {
	sslm_status prefill = SSLM_INVALID_ARGUMENT;
	int32_t ready = -1, decode1 = -1;
	sslm_status decode_status = SSLM_OK;
};

inline AbiWidth RunAbiWidth(sslm_model model, sslm_seq s, int64_t W, int32_t vocab, bool decode1) {
	AbiWidth r;
	r.prefill = PrefillAll(model, s, Stream(pkv_engine::kWidthsSeed, static_cast<int32_t>(W), vocab), pkv_engine::kChunk);
	if (r.prefill != SSLM_OK) return r;
	r.ready = Next(model, s, &r.decode_status);
	if (decode1 && r.ready >= 0) r.decode1 = Next(model, s, &r.decode_status);
	return r;
}

// ---- the flat twin -------------------------------------------------------------------------------

// pkv_engine_helpers.h's EngineFixture, built for any fixture (GetEngineFixture serves the four
// generated stems only). The twin runs the flat one-page view: B = cap, table {0}.
inline std::unique_ptr<pkv_engine::EngineFixture> EngineOf(const Fixture& fx) {
	auto e = std::make_unique<pkv_engine::EngineFixture>();
	e->fx = &fx;
	if (!fx.ok) return e;
	std::string err;
	bool ok = superslm::SslmModel::Load(fx.bytes.p, fx.size, e->view, &err) == superslm::SslmModelStatus::Ok;
	PKV_CHECK_MSG(ok, "SslmModel::Load(%s): %s", fx.stem.c_str(), err.c_str());
	ok = ok && e->view.config.kv_precision == superslm::SslmKvPrecision::Int8;
	if (ok) {
		const uint32_t n = e->view.config.num_hidden_layers;
		e->backings.resize(n);
		e->layers.resize(n);
		for (uint32_t l = 0; l < n && ok; ++l) {
			std::string merr;
			ok = superslm_marshal::MarshalLayer(e->view, l, e->view.config.num_attention_heads,
			                                    e->view.config.num_key_value_heads, e->backings[l], e->layers[l], &merr);
			PKV_CHECK_MSG(ok, "MarshalLayer(%s, %u): %s", fx.stem.c_str(), l, merr.c_str());
		}
	}
	if (ok) {
		const superslm::SslmTensorView* embed_w = e->view.weights.Tensor("embed");
		const superslm::SslmTensorView* gain_w = e->view.weights.Tensor("final_norm.gain");
		const superslm::SslmTensorView* head_w =
		    e->view.config.tie_word_embeddings ? embed_w : e->view.weights.Tensor("lm_head");
		bool sc_ok = true;
		e->embed_sc = superslm_marshal::ReadCarriedScale(e->view.composition_constants, "embed", &sc_ok);
		e->final_sc = superslm_marshal::ReadCarriedScale(e->view.composition_constants, "final_norm", &sc_ok);
		ok = embed_w && gain_w && head_w && sc_ok;
		if (ok) {
			e->final_gain = superslm_marshal::WidenGainToInt32(*gain_w);
			e->embed = reinterpret_cast<const int8_t*>(embed_w->data);
			e->head = reinterpret_cast<const int8_t*>(head_w->data);
		}
	}
	e->ok = ok;
	return e;
}

// The flat twin's widths run at width W: tokens ready and decode1, and the canonical rows' digests.
inline pkv_engine::WidthRun FlatTwin(const pkv_engine::EngineFixture& e, int64_t W) {
	return pkv_engine::RunWidthUncached(e, e.Cap(), pkv_engine::Drive::kChunked, W);
}

// ---- engine-level rotation (6.6a) -----------------------------------------------------------------

// KvPageView gains `runs` at T1 (§3.2 item 1). The cells set it where the shipped struct has it, by
// detection, so the suite compiles on 2ebe5e3's struct (no field: the engine cannot rotate, and the
// cell's address assertions fail for exactly that reason) and on the R9 struct unchanged.
template <class V, class = void>
struct HasRuns : std::false_type {};
template <class V>
struct HasRuns<V, std::void_t<decltype(std::declval<V&>().runs)>> : std::true_type {};

template <class V>
inline void SetRuns(V& view, uint32_t runs) {
	if constexpr (HasRuns<V>::value) view.runs = runs;
	(void)view;
	(void)runs;
}

}  // namespace r9
}  // namespace pkv

#endif  // SUPERSLM_TESTS_PKV_R9_H
