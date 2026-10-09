#pragma once

// The WAC language's words as the retail compiler reads them (compiler.h): the statement, declaration
// and logic words, where a comment starts, and the operand prefixes that name a slot type's table.
// The compiler's own walk uses the comment rule and the prefixes from here; a tool that reads a script
// the way the compiler does (the OpenNova Editor's script assist) takes all three.

#include <formats/wac/param_type.h>

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

} // namespace opennova::wac
