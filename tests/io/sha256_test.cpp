// Pins base/io/sha256.h: the FIPS 180-4 / NIST vectors one-shot and streamed in odd-sized
// chunks, the padding boundaries around the 56-byte length slot, the reset finish() leaves,
// and the lower-case hex spelling.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/sha256.h>

#include "common/test_expect.h"

using namespace opennova::io;

static const char kEmptyHex[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static const char kAbcHex[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static int test_nist_empty() {
	TEST_EXPECT(sha256_hex(sha256("")) == kEmptyHex);
	TEST_EXPECT(sha256_hex(sha256(nullptr, 0)) == kEmptyHex);
	Sha256 hash;
	TEST_EXPECT(sha256_hex(hash.finish()) == kEmptyHex);
	return 0;
}

static int test_nist_abc() {
	const Sha256Digest digest = sha256("abc");
	TEST_EXPECT(sha256_hex(digest) == kAbcHex);
	Sha256 hash;
	hash.update("a");
	hash.update("bc");
	TEST_EXPECT(hash.finish() == digest);
	return 0;
}

static int test_nist_two_block_448_bits() {
	const std::string message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	TEST_EXPECT(message.size() == 56);
	TEST_EXPECT(sha256_hex(sha256(message)) ==
	            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	return 0;
}

static int test_nist_million_a_streamed() {
	const std::vector<uint8_t> buffer(1000000, uint8_t('a'));
	static constexpr size_t kChunks[] = {1, 3, 7, 61, 63, 65, 127, 1001};
	Sha256 hash;
	size_t offset = 0;
	for (size_t turn = 0; offset < buffer.size(); ++turn) {
		size_t chunk = kChunks[turn % (sizeof(kChunks) / sizeof(kChunks[0]))];
		if (chunk > buffer.size() - offset) chunk = buffer.size() - offset;
		hash.update(buffer.data() + offset, chunk);
		offset += chunk;
	}
	const Sha256Digest streamed = hash.finish();
	TEST_EXPECT(sha256_hex(streamed) ==
	            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	TEST_EXPECT(streamed == sha256(buffer.data(), buffer.size()));
	return 0;
}

static int test_padding_boundaries() {
	static constexpr size_t kLengths[] = {55, 56, 57, 63, 64, 65, 119, 120, 128, 129};
	std::vector<uint8_t> pattern(129);
	for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = uint8_t(i & 0xFF);
	Sha256Digest previous{};
	bool have_previous = false;
	for (const size_t length : kLengths) {
		const Sha256Digest whole = sha256(pattern.data(), length);
		Sha256 hash;
		for (size_t i = 0; i < length; ++i) hash.update(pattern.data() + i, 1);
		if (hash.finish() != whole) {
			std::fprintf(stderr, "padding boundary: streamed length %zu differs\n", length);
			return 1;
		}
		if (have_previous && whole == previous) {
			std::fprintf(stderr, "padding boundary: length %zu repeats its predecessor\n", length);
			return 1;
		}
		previous = whole;
		have_previous = true;
	}
	return 0;
}

static int test_finish_resets() {
	const Sha256Digest abc = sha256("abc");
	Sha256 hash;
	hash.update("abc");
	TEST_EXPECT(hash.finish() == abc);
	TEST_EXPECT(sha256_hex(hash.finish()) == kEmptyHex);
	hash.update("abc");
	TEST_EXPECT(hash.finish() == abc);
	hash.update("xyz");
	hash.reset();
	hash.update("abc");
	TEST_EXPECT(hash.finish() == abc);
	return 0;
}

static int test_hex_spelling() {
	const Sha256Digest abc = sha256("abc");
	TEST_EXPECT(abc[0] == 0xBA);
	const std::string hex = sha256_hex(abc);
	TEST_EXPECT(hex.size() == 64);
	for (const char c : hex) TEST_EXPECT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
	TEST_EXPECT(hex.compare(0, 2, "ba") == 0);
	TEST_EXPECT(sha256_hex("abc", 3) == hex);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_nist_empty();
	failures += test_nist_abc();
	failures += test_nist_two_block_448_bits();
	failures += test_nist_million_a_streamed();
	failures += test_padding_boundaries();
	failures += test_finish_resets();
	failures += test_hex_spelling();
	if (failures == 0) std::printf("sha256_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
