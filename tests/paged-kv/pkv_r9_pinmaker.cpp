// T-3189 (the conductor's routed cell under §3.7, from the T-3190 strike): mints the pinned v1.11.0
// legacy blob at pkv_odd (cap 4,100), reference/pins/v1.11.0_pkv_odd_persist_saved.zrl.
//
// The blob is the `persist` scenario's "saved" stage (prefill 100, decode 4; L = 103), run on the
// build this tool is linked against. It is written only when its SHA-256 equals the blob= digest of
// the "persist saved" record in reference/v1.11.0_pkv_odd.ref, which v1.11.0's own ABI recorded
// (tools/build_paged_kv_reference.ps1). So the pin's bytes are v1.11.0's bytes, verified, whatever
// build mints them; a build whose legacy blob differs from v1.11.0's cannot mint it. Minted once on
// 2ebe5e3 (B = cap at pkv_odd: one page, v1.11.0's block) and committed; never rerun by the suite.
//
// Usage: superslm_pkv_r9_pinmaker OUT_PIN_PATH   (SUPERSLM_PAGED_KV_FIXTURE_DIR must be set)

#include "pkv_common.h"

int main(int argc, char** argv) {
	using namespace pkv;
	if (argc != 2) {
		std::fprintf(stderr, "usage: superslm_pkv_r9_pinmaker OUT_PIN_PATH\n");
		return 2;
	}
	const Fixture& fx = GetFixture("pkv_odd");
	if (!fx.ok) return 3;
	Driver d;
	d.model = fx.model;
	d.geo = fx.geo;
	std::vector<Pinned> pins;
	size_t n = 0;
	const Scenario* sc = Scenarios(&n);
	for (size_t i = 0; i < n; ++i)
		if (std::strcmp(sc[i].name, "persist") == 0) sc[i].run(d, &pins);
	if (!d.fail.empty() || pins.empty() || pins[0].stage != "saved") {
		std::fprintf(stderr, "persist scenario did not produce its saved blob: %s\n", d.fail.c_str());
		return 4;
	}
	const std::vector<uint8_t>& blob = pins[0].blob;
	const RefRecord* rec = RefLookup(Reference("v1.11.0", fx), "persist", "saved");
	const std::string got = Sha(blob.data(), blob.size());
	if (!rec || got != rec->blob_sha) {
		std::fprintf(stderr, "refused: this build's blob sha256 %s is not v1.11.0's recorded %s\n", got.c_str(),
		             rec ? rec->blob_sha.c_str() : "(no record)");
		return 5;
	}
	const std::vector<uint8_t> enc = ZrlEncode(blob);
	std::FILE* f = std::fopen(argv[1], "wb");
	if (!f || std::fwrite(enc.data(), 1, enc.size(), f) != enc.size() || std::fclose(f) != 0) return 6;
	std::printf("wrote %s: %zu bytes decoded (%zu encoded), sha256 %s = v1.11.0_pkv_odd.ref 'persist saved'\n", argv[1],
	            blob.size(), enc.size(), got.c_str());
	return 0;
}
