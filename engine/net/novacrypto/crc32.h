#pragma once

#include <base/io/crc32_mpeg2.h>

#include <cstddef>
#include <cstdint>

namespace opennova {

// CRC-32/MPEG-2 variant used by the NAPI LSB-scatter envelope.
//
// Polynomial: 0x04C11DB7 (forward, MSB-first). Init: 0xFFFFFFFF. No final XOR.
// No input/output reflection.
//
// Witnessed in jodemo.exe at NapiPacket_EncodeWithCRC@0x5f7540 and
// NapiPacket_DecodeWithCRC@0x5f7670, which both run the loop:
//     crc = table[((crc >> 24) ^ byte) & 0xFF] ^ (crc << 8)
// with init 0xFFFFFFFF. The table is byte-exact from dword_7FD658
// in the jodemo binary (verified via get_bytes(0x7FD658, 1024)).
// Check value for "123456789" is 0x0376E6E7 — the standard
// CRC-32/MPEG-2 check constant.

constexpr uint32_t CRC32_INIT = io::kCrc32Mpeg2Init;

// Lifted from jodemo .rdata @ 0x7FD658; the literal lives at its shared home,
// io::kCrc32Mpeg2Table (base/io/crc32_mpeg2.h), which the vfs expansion
// version checksum walks too.
inline constexpr const uint32_t *CRC32_TABLE = io::kCrc32Mpeg2Table;

// Fold a single byte into the running CRC state.
constexpr uint32_t crc32_napi_update(uint32_t crc, uint8_t byte) {
	return io::crc32_mpeg2_update(crc, byte);
}

// Fold N bytes into the running CRC state. Suitable for chunked feeding.
inline uint32_t crc32_napi_update(uint32_t crc, const uint8_t *data, size_t len) {
	return io::crc32_mpeg2_update(crc, data, len);
}

// One-shot: compute the NAPI CRC-32 of a buffer.
inline uint32_t crc32_napi(const uint8_t *data, size_t len) {
	return crc32_napi_update(CRC32_INIT, data, len);
}

} // namespace opennova
