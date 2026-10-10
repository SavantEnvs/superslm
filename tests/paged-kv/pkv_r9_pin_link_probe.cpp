// Paged-KV plan rev 16.21 cell 4.12, the link half of "absent from production": this unit references
// the test-only pin and is linked against the production `superslm` library only
// (superslm_pkv_r9_pin_link_probe in tests/paged-kv/paged_kv.cmake). The link must FAIL with an
// unresolved KvPagesTestPinPagePositions; tests/paged-kv/r9_pin_reach.py builds it and grades the
// failure. A link that succeeds means production carries the pin.

#include "superslm/sslm_abi.h"

#include <cstdint>
#include <cstdio>

extern "C" sslm_status KvPagesTestPinPagePositions(sslm_model model, int64_t page_positions);

int main() {
	const sslm_status st = KvPagesTestPinPagePositions(nullptr, 16);
	std::printf("pin linked against production: %d\n", static_cast<int>(st));
	return 0;
}
