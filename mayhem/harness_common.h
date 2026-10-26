// Shared helpers for the mayhem fuzz harnesses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "superslm/artifact.h"
#include "superslm/sha256.h"

// The loader rejects any artifact whose SHA-256 (over the file with the hash field zeroed) does not
// match, so raw mutations would never get past the check. Recompute it so the fuzzer reaches the parsers.
inline void FixIntegrityHash(std::vector<uint8_t>& bytes) {
	if (bytes.size() < superslm::kHeaderBytes) return;
	std::memset(bytes.data() + superslm::kIntegrityHashOffset, 0, superslm::kIntegrityHashBytes);
	uint8_t digest[32];
	superslm::Sha256Hash(bytes.data(), bytes.size(), digest);
	std::memcpy(bytes.data() + superslm::kIntegrityHashOffset, digest, superslm::kIntegrityHashBytes);
}

// SPX1 prefix blobs carry a digest at offset 28 over the blob with that field zeroed.
inline void FixSpx1Digest(std::vector<uint8_t>& blob) {
	if (blob.size() < 60 || std::memcmp(blob.data(), "SPX1", 4) != 0) return;
	std::memset(blob.data() + 28, 0, 32);
	uint8_t digest[32];
	superslm::Sha256Hash(blob.data(), blob.size(), digest);
	std::memcpy(blob.data() + 28, digest, 32);
}

struct AlignedBuf {
	void* p = nullptr;
	size_t n = 0;
	explicit AlignedBuf(size_t bytes, size_t align = 64) : n(bytes) {
		p = ::operator new(bytes ? bytes : 1, std::align_val_t(align), std::nothrow);
		if (p) std::memset(p, 0, bytes);
	}
	~AlignedBuf() {
		if (p) ::operator delete(p, std::align_val_t(64));
	}
	AlignedBuf(const AlignedBuf&) = delete;
	AlignedBuf& operator=(const AlignedBuf&) = delete;
};
