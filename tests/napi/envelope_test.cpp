#include <napi/envelope.h>
#include <novacrypto/crc32.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool roundtrip_one(const std::vector<uint8_t> &src) {
	std::vector<uint8_t> envelope(src.size() + 16, 0);
	size_t env_size = 0;
	const int enc = opennova::napi_envelope_encode(src.data(), src.size(),
			envelope.data(), envelope.size(), &env_size);
	if (enc != 0) {
		std::fprintf(stderr, "FAIL: encode returned %d for size %zu\n", enc, src.size());
		return false;
	}
	if (env_size != src.size() + 4) {
		std::fprintf(stderr, "FAIL: env_size=%zu expected %zu\n", env_size, src.size() + 4);
		return false;
	}
	std::vector<uint8_t> decoded(src.size(), 0);
	size_t dec_size = 0;
	const int dec = opennova::napi_envelope_decode(envelope.data(), env_size,
			decoded.data(), decoded.size(), &dec_size);
	if (dec != 0) {
		std::fprintf(stderr, "FAIL: decode returned %d for size %zu\n", dec, src.size());
		return false;
	}
	if (dec_size != src.size()) {
		std::fprintf(stderr, "FAIL: dec_size=%zu expected %zu\n", dec_size, src.size());
		return false;
	}
	if (decoded != src) {
		std::fprintf(stderr, "FAIL: roundtrip mismatch at size %zu\n", src.size());
		return false;
	}
	return true;
}

// Small (<32) payload — no scatter: header holds the CRC directly.
bool check_small_roundtrip() {
	const std::vector<uint8_t> src = {0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x23, 0x45, 0x67};
	return roundtrip_one(src);
}

// Large (>=32) payload — scatter path. The key invariant is that after
// encode the first 32 bytes have their low bits replaced by CRC bits,
// and decode restores them.
bool check_large_roundtrip() {
	std::vector<uint8_t> src(64);
	for (size_t i = 0; i < src.size(); ++i) {
		src[i] = static_cast<uint8_t>(i * 7u + 13u);
	}
	return roundtrip_one(src);
}

// 32-byte payload — exactly at the scatter threshold.
bool check_threshold_roundtrip() {
	std::vector<uint8_t> src(32, 0xA5);
	return roundtrip_one(src);
}

// After encode, the first 32 payload bytes should differ from the
// original in their low bits only (the CRC bits). Upper 7 bits are
// preserved verbatim.
bool check_scatter_only_touches_low_bits() {
	std::vector<uint8_t> src(48);
	for (size_t i = 0; i < src.size(); ++i) {
		src[i] = static_cast<uint8_t>(0x80u | (i & 0x7Eu)); // ensure high bit + avoid low-bit collisions
	}
	std::vector<uint8_t> envelope(src.size() + 16, 0);
	size_t env_size = 0;
	if (!expect(opennova::napi_envelope_encode(src.data(), src.size(),
			envelope.data(), envelope.size(), &env_size) == 0, "encode ok")) return false;
	for (size_t i = 0; i < 32; ++i) {
		const uint8_t encoded_hi = envelope[4 + i] & 0xFE;
		const uint8_t original_hi = src[i] & 0xFE;
		if (!expect(encoded_hi == original_hi, "high 7 bits preserved across first 32 bytes")) return false;
	}
	for (size_t i = 32; i < src.size(); ++i) {
		if (!expect(envelope[4 + i] == src[i], "bytes past the scatter threshold are verbatim")) return false;
	}
	return true;
}

// Tampering the envelope should cause decode to fail CRC check.
bool check_tamper_detection() {
	std::vector<uint8_t> src(48, 0x42);
	std::vector<uint8_t> envelope(src.size() + 16, 0);
	size_t env_size = 0;
	opennova::napi_envelope_encode(src.data(), src.size(), envelope.data(), envelope.size(), &env_size);
	envelope[4 + 35] ^= 0x10; // flip a high bit of a post-threshold byte (not CRC-carrying)
	std::vector<uint8_t> decoded(src.size(), 0);
	size_t dec_size = 0;
	const int rc = opennova::napi_envelope_decode(envelope.data(), env_size,
			decoded.data(), decoded.size(), &dec_size);
	if (!expect(rc != 0, "decode rejects tampered payload")) return false;
	return true;
}

// Variable-size-header mode must be rejected (first dword == 0).
bool check_variable_header_rejected() {
	uint8_t envelope[16] = {0}; // first dword is 0 -> variable mode -> reject
	envelope[9] = 0x08;
	uint8_t decoded[16];
	size_t dec_size = 0;
	const int rc = opennova::napi_envelope_decode(envelope, sizeof(envelope),
			decoded, sizeof(decoded), &dec_size);
	if (!expect(rc != 0, "variable-size-header mode rejected by 4-byte-only decoder")) return false;
	return true;
}

// Small payloads: the 4-byte header is literally the CRC of the payload.
bool check_small_header_equals_crc() {
	const std::vector<uint8_t> src = {0x01, 0x02, 0x03, 0x04, 0x05};
	std::vector<uint8_t> envelope(src.size() + 16, 0);
	size_t env_size = 0;
	opennova::napi_envelope_encode(src.data(), src.size(), envelope.data(), envelope.size(), &env_size);
	const uint32_t header = static_cast<uint32_t>(envelope[0]) |
			(static_cast<uint32_t>(envelope[1]) << 8) |
			(static_cast<uint32_t>(envelope[2]) << 16) |
			(static_cast<uint32_t>(envelope[3]) << 24);
	uint32_t crc = opennova::CRC32_INIT;
	for (uint8_t b : src) {
		crc = opennova::crc32_napi_update(crc, b);
	}
	if (!expect(header == crc, "small-payload envelope header equals payload CRC")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_small_roundtrip()) return 1;
	if (!check_large_roundtrip()) return 1;
	if (!check_threshold_roundtrip()) return 1;
	if (!check_scatter_only_touches_low_bits()) return 1;
	if (!check_tamper_detection()) return 1;
	if (!check_variable_header_rejected()) return 1;
	if (!check_small_header_equals_crc()) return 1;
	std::printf("OK: LSB-scatter CRC envelope roundtrips + CRC detection\n");
	return 0;
}
