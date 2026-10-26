// Fuzz the byte-BPE tokenizer (Encode / Decode) against the real Qwen2.5 tokenizer artifact, which is
// embedded at build time and parsed once. Input: byte 0 selects the mode, the rest is text or token ids.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "embed.h"
#include "superslm/artifact.h"
#include "superslm/tokenizer.h"

MAYHEM_EMBED(g_tok_artifact, TOK_ARTIFACT_PATH)

namespace {

struct Fixture {
	superslm::SslmArtifact art;
	superslm::TokenizerView tok;
	bool ok = false;
	Fixture() {
		superslm::SslmError err;
		const size_t n = static_cast<size_t>(g_tok_artifact_end - g_tok_artifact);
		if (superslm::SslmArtifact::OpenFromMemory(g_tok_artifact, n, art, &err) != superslm::SslmStatus::Ok) return;
		std::string terr;
		ok = superslm::TokenizerView::Open(art, tok, &terr);
	}
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static Fixture fx;
	if (!fx.ok || size < 1 || size > (1u << 16)) return -1;
	const uint8_t mode = data[0] & 3;
	const uint8_t* body = data + 1;
	const size_t blen = size - 1;
	const int32_t vocab = fx.tok.VocabSize();

	if (mode == 0 || mode == 1) {
		std::vector<int32_t> ids = fx.tok.Encode(std::string_view(reinterpret_cast<const char*>(body), blen));
		for (int32_t id : ids)
			if (id < 0 || id >= vocab) return 0;
		(void)fx.tok.Decode(ids);
	} else {
		std::vector<int32_t> ids;
		for (size_t i = 0; i + 3 <= blen; i += 3) {
			const uint32_t v = body[i] | (body[i + 1] << 8) | (static_cast<uint32_t>(body[i + 2]) << 16);
			ids.push_back(static_cast<int32_t>(v % static_cast<uint32_t>(vocab)));
		}
		(void)fx.tok.Decode(ids);
	}
	return 0;
}
