#pragma once

#include <cstdint>
#include <vector>

namespace opennova {

// Ported from pack/unpack functions at 0x403CD0-0x403EF0.
// Used by TPM1 mesh file I/O for index compression.

// Pack 16-bit values into 8-bit bytes (3 per group). For max_index <= 256.
std::vector<uint8_t> pack_words_to_bytes(const uint16_t* src, int count);

// Pack 16-bit values into 10-bit fields (3 per 32-bit DWORD). For max_index <= 1024.
std::vector<uint8_t> pack_words_to_10bit(const uint16_t* src, int count);

// Unpack 8-bit packed bytes back to 16-bit words.
std::vector<uint16_t> unpack_bytes_to_words(const uint8_t* src, int count);

// Unpack 10-bit packed DWORDs back to 16-bit words.
std::vector<uint16_t> unpack_10bit_to_words(const uint32_t* src, int count);

} // namespace opennova

