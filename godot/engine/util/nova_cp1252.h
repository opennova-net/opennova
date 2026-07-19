#pragma once

#include <cstdint>

namespace opennova {

// Unicode codepoints for Windows-1252 bytes 0x80..0x9F. Zero marks one of
// the five undefined byte positions, which OpenNova preserves as its raw C1
// codepoint so an untouched retail string still round-trips losslessly.
inline constexpr char32_t kCp1252SpecialCodepoints[32] = {
	0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
	0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

inline constexpr char32_t cp1252_decode_byte(std::uint8_t p_byte) noexcept {
	if (p_byte < 0x80 || p_byte > 0x9F) {
		return static_cast<char32_t>(p_byte);
	}
	const char32_t mapped = kCp1252SpecialCodepoints[p_byte - 0x80];
	return mapped != 0 ? mapped : static_cast<char32_t>(p_byte);
}

inline constexpr bool cp1252_encode_codepoint(char32_t p_codepoint, std::uint8_t &r_byte) noexcept {
	if (p_codepoint < 0x80 || (p_codepoint >= 0xA0 && p_codepoint <= 0xFF)) {
		r_byte = static_cast<std::uint8_t>(p_codepoint);
		return true;
	}
	if (p_codepoint >= 0x80 && p_codepoint <= 0x9F &&
	    kCp1252SpecialCodepoints[p_codepoint - 0x80] == 0) {
		r_byte = static_cast<std::uint8_t>(p_codepoint);
		return true;
	}
	for (std::uint8_t i = 0; i < 32; ++i) {
		if (kCp1252SpecialCodepoints[i] == p_codepoint) {
			r_byte = static_cast<std::uint8_t>(0x80 + i);
			return true;
		}
	}
	return false;
}

} // namespace opennova
