// Pins base/io/sha256.h: the FIPS 180-4 / NIST vectors one-shot and streamed in odd-sized
// chunks, the padding boundaries around the 56-byte length slot, the reset finish() leaves,
// the lower-case hex spelling, HMAC-SHA256 against RFC 4231, and constant_time_equal.
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

static std::string repeat(char c, size_t n) { return std::string(n, c); }

// RFC 4231 section 4: cases 1-4, 6 and 7 (case 5 truncates the output to 128 bits). Case 6 and 7
// carry a 131-byte key, the hash-the-key-first path.
static int test_hmac_rfc4231() {
	const std::string key1 = repeat('\x0b', 20);
	TEST_EXPECT(sha256_hex(hmac_sha256(key1, "Hi There")) ==
	            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	TEST_EXPECT(sha256_hex(hmac_sha256("Jefe", "what do ya want for nothing?")) ==
	            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	TEST_EXPECT(sha256_hex(hmac_sha256(repeat('\xaa', 20), repeat('\xdd', 50))) ==
	            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
	std::string key4;
	for (int i = 1; i <= 25; ++i) key4.push_back(char(i));
	TEST_EXPECT(sha256_hex(hmac_sha256(key4, repeat('\xcd', 50))) ==
	            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
	const std::string key131 = repeat('\xaa', 131);
	TEST_EXPECT(sha256_hex(hmac_sha256(key131, "Test Using Larger Than Block-Size Key - Hash Key First")) ==
	            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
	TEST_EXPECT(sha256_hex(hmac_sha256(key131,
	                                   "This is a test using a larger than block-size key and a larger "
	                                   "than block-size data. The key needs to be hashed before being "
	                                   "used by the HMAC algorithm.")) ==
	            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
	return 0;
}

// A 64-byte key is used as is (no hash, no pad); 65 bytes is hashed, so the two differ even
// though the 65-byte key starts with the 64-byte one.
static int test_hmac_block_size_key() {
	const std::string key64 = repeat('k', 64);
	const std::string key65 = repeat('k', 65);
	TEST_EXPECT(hmac_sha256(key64, "m") != hmac_sha256(key65, "m"));
	TEST_EXPECT(hmac_sha256("", "m") == hmac_sha256(std::string(1, '\0'), "m"));
	return 0;
}

static int test_constant_time_equal() {
	TEST_EXPECT(constant_time_equal("", ""));
	TEST_EXPECT(constant_time_equal("abc", "abc"));
	TEST_EXPECT(!constant_time_equal("abc", "abd"));
	TEST_EXPECT(!constant_time_equal("abc", "ab"));
	TEST_EXPECT(!constant_time_equal("", "a"));
	TEST_EXPECT(!constant_time_equal(std::string("a\0b", 3), std::string("a\0c", 3)));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_hmac_rfc4231();
	failures += test_hmac_block_size_key();
	failures += test_constant_time_equal();
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
