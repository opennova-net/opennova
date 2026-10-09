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
