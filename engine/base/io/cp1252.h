#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Windows-1252, the code page of the retail text tables, fonts and wire strings
// (docs/interface/rtxt-strings-re.md "Text encoding"). The byte codec is what the
// format and glyph boundaries use; the UTF-8 halves serve every consumer that shows
// or edits retail text in a UTF-8 world (the editor's inspector, the Godot String
// wrapper): decode UTF-8 when the bytes are valid UTF-8 (ASCII and anything we wrote
// ourselves), cp1252 otherwise; encode back to cp1252 through the checked encoder,
// which names the characters cp1252 has no byte for (the editor refuses those, so an
// edit to a retail table keeps the game-readable encoding).

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

// The CRT `isspace` as the game runs it: WinMain's System_InitTimerAndLocale sets LC_ALL to
// ".ACP" and only LC_NUMERIC back to "C", so the CRT classes bytes by the host's ANSI code page
// (its ctype table is that page's C1_SPACE set), not the C locale. The parity target is cp1252,
// the code page of JO's audience: the six C-locale spaces (0x09..0x0D, 0x20) plus 0xA0, the
// no-break space. Every Windows single-byte ANSI page (874, 1250..1258) classes the same seven
// bytes; a double-byte or UTF-8-ACP host's set is not modeled: on a double-byte page the lead
// bytes differ, and the CRT refuses an ACP of 65000 / 65001, so ".ACP" fails there and the C
// locale's six stay (docs/net/novaworld-net-re.md D-NET-382).
// [orig: System_InitTimerAndLocale @0x762a00 — setlocale(LC_ALL, ".ACP") @0x762a6e,
//  setlocale(LC_NUMERIC, "C") @0x762a7a; isspace @0x76b964 -> _isspace_l @0x76b915 (the
//  locale's pctype & _SPACE); CRT_init_ctype @0x784a07 (the table from __crtGetStringTypeA
//  CT_CTYPE1 over the page's 256 bytes); ___get_qualified_locale's UTF-7 / UTF-8 refusals
//  @0x785592 / @0x78559e]
inline constexpr bool cp1252_isspace(std::uint8_t p_byte) noexcept {
	return p_byte == 0x20 || (p_byte >= 0x09 && p_byte <= 0x0D) || p_byte == 0xA0;
}

// The CRT `isdigit` under the same ".ACP" LC_CTYPE: cp1252's C1_DIGIT bytes, which are the ten
// ASCII digits plus the superscripts 0xB2, 0xB3 and 0xB9 (Windows' NLS classes U+00B2, U+00B3 and
// U+00B9 as digits; probed 2026-10-10 with MultiByteToWideChar(1252) + GetStringTypeW(CT_CTYPE1),
// and msvcrt's isdigit after setlocale(LC_ALL, ".ACP") on a cp1252 host gives the same set).
// Unlike the space set, the digit set differs between single-byte pages (1250 and 1251 have no
// superscript digits, 1253 lacks 0xB9, 874 adds the Thai digits 0xF0..0xF9); the pin to cp1252
// is D-NET-388's. A caller passing the byte sign-extended reads the same row: CRT_init_ctype
// copies the table's upper half below its base, so pctype[-128..-2] mirrors pctype[128..254]
// (and pctype[-1], EOF, is 0; 0xFF is no digit either way).
// [orig: _isdigit @0x76b86d -> _isdigit_l @0x76b81e (the locale's pctype & _DIGIT);
//  CRT_init_ctype @0x784a07 (the mirror memcpy @0x784c59, the EOF word @0x784bde)]
inline constexpr bool cp1252_isdigit(std::uint8_t p_byte) noexcept {
	return (p_byte >= '0' && p_byte <= '9') || p_byte == 0xB2 || p_byte == 0xB3 || p_byte == 0xB9;
}

// Strict UTF-8 well-formedness (no overlong forms, no surrogates, at most U+10FFFF).
inline bool is_valid_utf8(std::string_view s) noexcept {
	size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 0;
		if (c < 0x80) len = 1;
		else if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
		else return false;
		if (i + len > s.size()) return false;
		for (size_t k = 1; k < len; ++k)
			if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
		if (len == 3) {
			const char32_t cp = (char32_t(c & 0x0F) << 12) | (char32_t(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
			                    (static_cast<unsigned char>(s[i + 2]) & 0x3F);
			if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		}
		if (len == 4) {
			const char32_t cp = (char32_t(c & 0x07) << 18) | (char32_t(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
			                    (char32_t(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
			if (cp < 0x10000 || cp > 0x10FFFF) return false;
		}
		i += len;
	}
	return true;
}

inline void utf8_append(std::string &out, char32_t cp) {
	if (cp < 0x80) out.push_back(static_cast<char>(cp));
	else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

// cp1252 bytes as UTF-8.
inline std::string cp1252_to_utf8(std::string_view bytes) {
	std::string out;
	out.reserve(bytes.size());
	for (const char raw : bytes) utf8_append(out, cp1252_decode_byte(static_cast<std::uint8_t>(raw)));
	return out;
}

// UTF-8 as cp1252 bytes, checked: true with `out` the bytes when every character has a
// cp1252 byte; else false with `out` untouched and `unstorable` (when given) the characters
// that have none, each once, in the order they first appear (a text that is not UTF-8 at
// all: U+FFFD alone).
inline bool utf8_to_cp1252(std::string_view text, std::string &out, std::u32string *unstorable = nullptr) {
	if (!is_valid_utf8(text)) {
		if (unstorable) *unstorable = std::u32string(1, char32_t(0xFFFD));
		return false;
	}
	std::string encoded;
	encoded.reserve(text.size());
	bool fits = true;
	size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		char32_t cp = 0;
		size_t len = 1;
		if (c < 0x80) cp = c;
		else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
		else { len = 4; cp = c & 0x07; }
		for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
		i += len;
		std::uint8_t byte = 0;
		if (cp1252_encode_codepoint(cp, byte)) {
			encoded.push_back(static_cast<char>(byte));
			continue;
		}
		fits = false;
		if (!unstorable) return false;
		if (unstorable->find(cp) == std::u32string::npos) unstorable->push_back(cp);
	}
	if (!fits) return false;
	out = std::move(encoded);
	return true;
}

// Retail text for a UTF-8 consumer: valid UTF-8 passes through, anything else is cp1252.
inline std::string retail_text_to_utf8(std::string_view bytes) {
	return is_valid_utf8(bytes) ? std::string(bytes) : cp1252_to_utf8(bytes);
}

} // namespace opennova
