// Fuzz the .sslm container loader and the tokenizer-section parser. The integrity hash is
// recomputed so mutated bytes reach the structural validation behind it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "harness_common.h"
#include "superslm/tokenizer.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < superslm::kHeaderBytes || size > (1u << 22)) return -1;
	std::vector<uint8_t> bytes(data, data + size);
	FixIntegrityHash(bytes);

	superslm::SslmArtifact art;
	superslm::SslmError err;
	if (superslm::SslmArtifact::OpenFromMemory(bytes.data(), bytes.size(), art, &err) !=
	    superslm::SslmStatus::Ok)
		return -1;

	superslm::TokenizerView tok;
	std::string terr;
	if (!superslm::TokenizerView::Open(art, tok, &terr)) return 0;

	const int32_t vocab = tok.VocabSize();
	std::vector<int32_t> ids = tok.Encode("Hello, wörld! 日本語 \xF0\x9F\x99\x82 12345\n\t  x");
	std::vector<int32_t> probe;
	for (int32_t i = 0; i < 16 && i < vocab; ++i) probe.push_back(i * 7 % vocab);
	(void)tok.Decode(probe);
	for (int32_t& id : ids)
		if (id < 0 || id >= vocab) return 0;
	(void)tok.Decode(ids);
	return 0;
}
