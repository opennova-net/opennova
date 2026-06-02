#include <novacrypto/crc32.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Classic CRC-32/MPEG-2 check value: crc32("123456789") == 0x0376E6E7.
// Witnessed by computing with the lifted dword_7FD658 table + init 0xFFFFFFFF.
bool check_standard_vector() {
	const char *str = "123456789";
	const uint32_t got = opennova::crc32_napi(reinterpret_cast<const uint8_t *>(str), std::strlen(str));
	if (!expect(got == 0x0376E6E7u, "crc32(\"123456789\") must be 0x0376E6E7 (CRC-32/MPEG-2 check value)")) return false;
	return true;
}

// Empty input yields the init value. Zero-length input takes no table steps.
bool check_empty_input() {
	const uint32_t got = opennova::crc32_napi(nullptr, 0);
	if (!expect(got == opennova::CRC32_INIT, "crc32 of empty input is the init value 0xFFFFFFFF")) return false;
	return true;
}

// Table row anchor: table[1] is the polynomial value 0x04C11DB7. This
// catches any row accidentally nudged during a copy.
bool check_table_anchors() {
	if (!expect(opennova::CRC32_TABLE[0] == 0x00000000u, "table[0] must be 0")) return false;
	if (!expect(opennova::CRC32_TABLE[1] == 0x04C11DB7u, "table[1] must be polynomial 0x04C11DB7")) return false;
	if (!expect(opennova::CRC32_TABLE[255] == 0xB1F740B4u, "table[255] must match jodemo dword_7FD658+1020")) return false;
	return true;
}

// Streaming update: folding in byte-by-byte must equal folding in the buffer.
bool check_streaming_equals_oneshot() {
	const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
	const uint32_t oneshot = opennova::crc32_napi(data, sizeof(data));
	uint32_t streamed = opennova::CRC32_INIT;
	for (uint8_t b : data) {
		streamed = opennova::crc32_napi_update(streamed, b);
	}
	if (!expect(streamed == oneshot, "streamed update must equal one-shot crc32")) return false;
	return true;
}

// Single-byte case: crc32({0}) = table[0xFF] ^ 0xFFFFFF00 (since init is
// 0xFFFFFFFF, top byte is 0xFF, XOR with input byte 0 -> index 0xFF).
bool check_single_byte_zero() {
	const uint8_t data[] = {0x00};
	const uint32_t got = opennova::crc32_napi(data, 1);
	const uint32_t expected = opennova::CRC32_TABLE[0xFF] ^ 0xFFFFFF00u;
	if (!expect(got == expected, "crc32({0}) must equal table[0xFF] ^ 0xFFFFFF00")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_standard_vector()) return 1;
	if (!check_empty_input()) return 1;
	if (!check_table_anchors()) return 1;
	if (!check_streaming_equals_oneshot()) return 1;
	if (!check_single_byte_zero()) return 1;
	std::printf("OK: crc32 matches CRC-32/MPEG-2 + jodemo dword_7FD658 lifts\n");
	return 0;
}
