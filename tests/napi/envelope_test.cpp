#include <net/napi/envelope.h>
#include <net/novacrypto/crc32.h>

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

void write_le32(uint8_t *p, uint32_t value) {
	p[0] = static_cast<uint8_t>(value);
	p[1] = static_cast<uint8_t>(value >> 8);
	p[2] = static_cast<uint8_t>(value >> 16);
	p[3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> make_variable_envelope(const std::vector<uint8_t> &src,
		size_t header_size) {
	std::vector<uint8_t> envelope(header_size + src.size(), 0);
	envelope[9] = static_cast<uint8_t>(header_size);
	std::memcpy(envelope.data() + header_size, src.data(), src.size());
	const uint32_t crc = opennova::crc32_napi(src.data(), src.size());
	if (src.size() < 32) {
		write_le32(envelope.data() + 4, crc);
		return envelope;
	}
	uint32_t carrier = 0;
	for (size_t i = 0; i < 32; ++i) {
		uint8_t &byte = envelope[header_size + i];
		carrier |= static_cast<uint32_t>(byte & 1u) << i;
		byte = static_cast<uint8_t>((byte & 0xFEu) | ((crc >> i) & 1u));
	}
	write_le32(envelope.data() + 4, carrier);
	return envelope;
}

bool check_variable_header_decode() {
	for (size_t payload_size : {size_t(8), size_t(48)}) {
		std::vector<uint8_t> src(payload_size);
		for (size_t i = 0; i < src.size(); ++i)
			src[i] = static_cast<uint8_t>(0x31u + i * 9u);
		const std::vector<uint8_t> envelope = make_variable_envelope(src, 12);
		std::vector<uint8_t> decoded(src.size());
		size_t dec_size = 0;
		if (!expect(opennova::napi_envelope_decode(envelope.data(), envelope.size(),
				decoded.data(), decoded.size(), &dec_size) == 0,
				"variable-size-header envelope decodes")) return false;
		if (!expect(dec_size == src.size() && decoded == src,
				"variable-size-header payload restored")) return false;
	}
	return true;
}

// A normal scatter carrier can legitimately be zero. Retail first attempts the
// extended interpretation and then falls back to the four-byte layout.
bool check_zero_carrier_fallback() {
	std::vector<uint8_t> src(48);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<uint8_t>((i * 6u + 2u) & 0xFEu);
	std::vector<uint8_t> envelope(src.size() + 4);
	size_t env_size = 0;
	if (!expect(opennova::napi_envelope_encode(src.data(), src.size(),
			envelope.data(), envelope.size(), &env_size) == 0,
			"zero-carrier envelope encodes")) return false;
	if (!expect(envelope[0] == 0 && envelope[1] == 0 &&
			envelope[2] == 0 && envelope[3] == 0,
			"test payload produces zero scatter carrier")) return false;
	std::vector<uint8_t> decoded(src.size());
	size_t dec_size = 0;
	if (!expect(opennova::napi_envelope_decode(envelope.data(), env_size,
			decoded.data(), decoded.size(), &dec_size) == 0,
			"zero scatter carrier takes four-byte fallback")) return false;
	return expect(dec_size == src.size() && decoded == src,
			"zero-carrier fallback restores payload");
}

// Retail does not take the normal-header fallback unless the candidate
// extended header size at +9 is greater than four.
bool check_zero_carrier_small_header_size_rejected() {
	std::vector<uint8_t> src(48, 2u);
	std::vector<uint8_t> envelope(src.size() + 4);
	size_t env_size = 0;
	if (!expect(opennova::napi_envelope_encode(src.data(), src.size(),
			envelope.data(), envelope.size(), &env_size) == 0,
			"small-header candidate encodes with zero carrier")) return false;
	// Scatter owns the low bit of this payload byte, so the encoded candidate
	// may be 2 or 3. Its high seven bits are the invariant that keeps it below
	// retail's >4 extended-header gate.
	if (!expect(envelope[9] < 4,
			"normal payload supplies an extended header candidate below four")) return false;
	std::vector<uint8_t> decoded(src.size());
	size_t dec_size = 0;
	return expect(opennova::napi_envelope_decode(envelope.data(), env_size,
			decoded.data(), decoded.size(), &dec_size) != 0,
			"zero carrier with candidate header size below four rejects");
}

// A zero carrier whose payload byte 5 (packet +9) reads exactly 4 is the
// ORDINARY layout again: retail takes the four-byte path with the zero
// carrier and decodes the packet [orig: NapiNP_UnpackPacket `cmp ebx,4; jz`
// @0x62ca6a]. The pre-2026-09-10 port rejected that packet outright.
bool check_zero_carrier_header_size_four_decodes() {
	std::vector<uint8_t> src(48);
	std::vector<uint8_t> envelope(src.size() + 4);
	size_t env_size = 0;
	bool found = false;
	// Even bytes keep the scatter carrier zero; byte 5 stays 4 or 5 depending
	// on CRC bit 5, so vary a byte outside the scatter window until the CRC
	// leaves the low bit clear.
	for (unsigned seed = 0; seed < 256 && !found; seed += 2) {
		for (size_t i = 0; i < src.size(); ++i)
			src[i] = static_cast<uint8_t>((i * 6u + 2u) & 0xFEu);
		src[5] = 4;
		src[40] = static_cast<uint8_t>(seed);
		if (opennova::napi_envelope_encode(src.data(), src.size(),
				envelope.data(), envelope.size(), &env_size) != 0)
			return expect(false, "header-size-four candidate encodes");
		found = envelope[0] == 0 && envelope[1] == 0 && envelope[2] == 0 &&
				envelope[3] == 0 && envelope[9] == 4;
	}
	if (!expect(found, "a zero-carrier packet with +9 == 4 exists")) return false;
	std::vector<uint8_t> decoded(src.size());
	size_t dec_size = 0;
	if (!expect(opennova::napi_envelope_decode(envelope.data(), env_size,
			decoded.data(), decoded.size(), &dec_size) == 0,
			"zero carrier with header size four decodes on the ordinary path")) return false;
	return expect(dec_size == src.size() && decoded == src,
			"header-size-four packet restores its payload");
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
	if (!check_variable_header_decode()) return 1;
	if (!check_zero_carrier_fallback()) return 1;
	if (!check_zero_carrier_small_header_size_rejected()) return 1;
	if (!check_zero_carrier_header_size_four_decodes()) return 1;
	if (!check_small_header_equals_crc()) return 1;
	std::printf("OK: LSB-scatter CRC envelope roundtrips + CRC detection\n");
	return 0;
}
