#include "packing.h"

namespace opennova {

// [orig: pack_words_to_bytes @ 0x403CD0, pack_words_to_10bit @ 0x403DD0]
// Ported from pack_words_to_bytes (0x403CD0).
// Reads low byte of each 16-bit value, packs 3 bytes per group.
std::vector<uint8_t> pack_words_to_bytes(const uint16_t* src, int count) {
    int groups = (count + 2) / 3;
    std::vector<uint8_t> out(3 * groups, 0);

    for (int i = 0; i < count; i++) {
        out[i] = static_cast<uint8_t>(src[i]);
    }
    return out;
}

// Ported from pack_words_to_10bit (0x403DD0).
// Packs 3 values per 32-bit DWORD: 10 + 11 + 11 bits.
std::vector<uint8_t> pack_words_to_10bit(const uint16_t* src, int count) {
    int groups = (count + 2) / 3;
    std::vector<uint8_t> out(4 * groups, 0);
    auto* dst = reinterpret_cast<uint32_t*>(out.data());

    for (int i = 0, g = 0; g < groups; g++, i += 3) {
        uint32_t packed = 0;
        if (i < count) packed = src[i] & 0x3FF;
        if (i + 1 < count) packed |= (static_cast<uint32_t>(src[i + 1]) & 0x7FF) << 10;
        if (i + 2 < count) packed |= static_cast<uint32_t>(src[i + 2]) << 21;
        dst[g] = packed;
    }
    return out;
}

// Ported from unpack_bytes_to_words (0x403E70).
std::vector<uint16_t> unpack_bytes_to_words(const uint8_t* src, int count) {
    std::vector<uint16_t> out(count);
    for (int i = 0; i < count; i++) {
        out[i] = src[i];
    }
    return out;
}

// Ported from unpack_10bit_to_words (0x403EF0).
std::vector<uint16_t> unpack_10bit_to_words(const uint32_t* src, int count) {
    std::vector<uint16_t> out(count);
    int groups = (count + 2) / 3;
    int idx = 0;

    for (int g = 0; g < groups && idx < count; g++) {
        uint32_t packed = src[g];
        if (idx < count) out[idx++] = packed & 0x3FF;
        if (idx < count) out[idx++] = (packed >> 10) & 0x3FF;
        if (idx < count) out[idx++] = (packed >> 21) & 0x3FF;
    }
    return out;
}

} // namespace opennova

