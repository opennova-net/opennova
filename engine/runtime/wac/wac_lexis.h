#pragma once

// The WAC language's words as the retail compiler reads them (compiler.h): the statement, declaration
// and logic words, where a comment starts, the operand prefixes that name a slot type's table, and the
// hash the compiler matches a token on. The compiler's own walk uses the comment rule, the prefixes and
// the hash from here; a tool that reads a script the way the compiler does (the OpenNova Editor's
// script assist) takes them all.

#include <base/io/strutil.h>
#include <formats/wac/param_type.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova::wac {

// The words Script_Compile reads as statements, declarations and logic rather than as commands or
// operands: the block words, the VAR / CHEAT / RUN declarations, NOT, and the AND / OR the operator
// precedence table holds. The compiler matches each by its token hash, the token's first four bytes
// [orig: Script_Compile @0x4F31F0 -- the hash compares; Script_GetOperatorPrecedence @0x4EE540 for
// AND / OR], so a longer word sharing those four bytes reads as the same keyword (ENDIF is END's).
inline constexpr const char *kWacKeywords[] = {
	"IF", "THEN", "ELSE", "ELSEIF", "END", "ENTER", "LEAVE", "DOSEQ", "DORND",
	"NEXT", "GLOOP", "PLOOP", "VAR", "CHEAT", "RUN", "NOT", "AND", "OR",
};

// A comment starts at ';' or at "//" and runs to the line's CR.
// [orig: Script_Compile @0x4F54BA..0x4F54D9]
inline constexpr bool wac_comment_starts(char c, char next) {
	return c == ';' || (c == '/' && next == '/');
}

// The operator set: the bytes Script_Compile reads as an operator token, each alone or as one of the
// two-byte operators, and that end a word [orig: @0x7CE2E8, 20 bytes, copied into the compile frame
// @0x4F320F]. The compiler keeps its own copy in its frame, where a paren frame past the sixteenth
// overwrites it as retail's does; this is the set as the binary holds it.
inline constexpr char kWacOperatorSet[] = "{}()[]+-*/|&^%<>=!~";

// Whether `c` is in the operator set (never the terminator).
inline constexpr bool wac_in_operator_set(char c) {
	for (const char *op = kWacOperatorSet; *op != '\0'; ++op)
		if (*op == c) return true;
	return false;
}

// Whether the byte `c` ends a word the tokenizer reads: a blank or a control byte (<= ' '), ';', ','
// or an operator byte [orig: Script_Compile @0x4F3412..0x4F345A]. A '"' does not: it opens a string
// only where a token starts.
inline constexpr bool wac_token_ends(char c) {
	return static_cast<unsigned char>(c) <= ' ' || c == ';' || c == ',' || wac_in_operator_set(c);
}

// The operand prefixes, in the order the parameter resolver tests them: a token starting with one
// (the compiler has upper-cased it) names the prefix's slot type's table whatever slot it fills, the
// name being what follows the prefix [orig: WacScript_ResolveParameter @0x4F2920, the prefix legs
// @0x4F2B84..0x4F2CE9].
struct WacOperandPrefix {
	const char *prefix;
	ParamType type;
};
inline constexpr WacOperandPrefix kWacOperandPrefixes[] = {
	{ "G_", ParamType::Group },
	{ "FX_", ParamType::Fx },
	{ "FACE_", ParamType::Face },
	{ "SS_", ParamType::SoundSet },
	{ "TT_", ParamType::TextToken },
	{ "ANIM_", ParamType::Anim },
	{ "SSN_", ParamType::Ssn },
	{ "AMMO_", ParamType::Ammo },
};

// The prefix of a slot type ("" for a type with none).
inline const char *wac_operand_prefix(ParamType type) {
	for (const WacOperandPrefix &row : kWacOperandPrefixes)
		if (row.type == type) return row.prefix;
	return "";
}

// A word's byte as the tokenizer stores it: every byte above 0x60 folds down by 0x20 (a letter to
// its capital) [orig: Script_Compile @0x4F3412..0x4F345A].
inline constexpr uint8_t wac_word_byte(uint8_t c) {
	return c > 0x60 ? static_cast<uint8_t>(c - 0x20) : c;
}

// A hash spelled as its four bytes, big-endian: the compiler's keyword and operator constants
// ("END" is wac_hash4('E', 'N', 'D', ';')).
inline constexpr uint32_t wac_hash4(char a, char b, char c, char d) {
	return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
			(uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

// A token's hash over the first four bytes of the tokenizer's buffer, the token padded with ';':
// each byte read as a signed char, shifted in big-endian [orig: Script_Compile
// @0x4F3464..0x4F34C9].
inline constexpr uint32_t wac_hash_bytes(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
	uint32_t h = static_cast<uint32_t>(int32_t(int8_t(b0)));
	h = (h << 8) + static_cast<uint32_t>(int32_t(int8_t(b1)));
	h = (h << 8) + static_cast<uint32_t>(int32_t(int8_t(b2)));
	return (h << 8) + static_cast<uint32_t>(int32_t(int8_t(b3)));
}

// The hash the compiler keeps for a token (`token` as the tokenizer stores it, `bytes_hash` its
// wac_hash_bytes): ELSEIF alone is renamed ELSI, so ELSE keeps its own [orig: Script_Compile
// @0x4F3464..0x4F34C9].
inline uint32_t wac_token_hash(std::string_view token, uint32_t bytes_hash) {
	return strutil::iequals(token, "ELSEIF") ? wac_hash4('E', 'L', 'S', 'I') : bytes_hash;
}

// A source word's hash as the compiler takes it: its bytes as the tokenizer stores them
// (wac_word_byte), the first four padded with ';' and hashed (wac_hash_bytes), ELSEIF renamed
// (wac_token_hash). The compiler matches its keywords on this hash, so a longer word sharing a
// keyword's first four bytes reads as that keyword (ENTERS is ENTER).
inline uint32_t wac_word_hash(std::string_view word) {
	std::string stored(word);
	for (char &c : stored) c = static_cast<char>(wac_word_byte(static_cast<uint8_t>(c)));
	const auto at = [&stored](size_t i) {
		return i < stored.size() ? static_cast<uint8_t>(stored[i]) : uint8_t(';');
	};
	return wac_token_hash(stored, wac_hash_bytes(at(0), at(1), at(2), at(3)));
}

// Whether a token's hash is END's: END itself and every word starting ENDD, ENDI, ENDL or ENDP
// (ENDDO, ENDIF, ENDLOOP...) [orig: Script_Compile @0x4F4065 (END;), the switch
// @0x4F461E..0x4F4634 (cases 0/5/8/12 -> @0x4F463B)].
inline constexpr bool wac_is_end_hash(uint32_t h) {
	return h == wac_hash4('E', 'N', 'D', ';') || h == wac_hash4('E', 'N', 'D', 'D') ||
			h == wac_hash4('E', 'N', 'D', 'I') || h == wac_hash4('E', 'N', 'D', 'L') ||
			h == wac_hash4('E', 'N', 'D', 'P');
}

} // namespace opennova::wac
