// The WAC language's words (runtime/wac/wac_lexis.h): the keywords the compiler reads as statements,
// declarations and logic [orig: Script_Compile @0x4F31F0], the comment start [orig: Script_Compile
// @0x4F54BA..0x4F54D9] and the operand prefixes [orig: WacScript_ResolveParameter @0x4F2920, the prefix
// legs @0x4F2B84..0x4F2CE9], checked against what the compiler itself reads.

#include <cstdio>
#include <string>
#include <string_view>

#include <base/io/strutil.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_lexis.h>

using namespace opennova;
using namespace opennova::wac;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static bool is_keyword(std::string_view word) {
	for (const char *keyword : kWacKeywords)
		if (strutil::iequals(word, keyword)) return true;
	return false;
}

static void comments() {
	CHECK(wac_comment_starts(';', 'x'));
	CHECK(wac_comment_starts('/', '/'));
	CHECK(!wac_comment_starts('/', 'x'));
	CHECK(!wac_comment_starts('a', '/'));
	// The compiler skips both forms to the line's end.
	const Program plain = compile_source("V1 = 2\r\n", {});
	const Program commented = compile_source("; a note\r\nV1 = 2 // another\r\n", {});
	CHECK(plain.ok() && commented.ok());
	CHECK(plain.code == commented.code);
}

static void prefixes() {
	CHECK(std::string(wac_operand_prefix(ParamType::Group)) == "G_");
	CHECK(std::string(wac_operand_prefix(ParamType::Ssn)) == "SSN_");
	CHECK(std::string(wac_operand_prefix(ParamType::TextToken)) == "TT_");
	CHECK(std::string(wac_operand_prefix(ParamType::Ammo)) == "AMMO_");
	CHECK(std::string(wac_operand_prefix(ParamType::Number)).empty());
	CHECK(sizeof(kWacOperandPrefixes) / sizeof(kWacOperandPrefixes[0]) == 8);
}

// The hash the compiler matches a token on: a word's bytes folded above 0x60, the first four padded
// with ';' and hashed as signed chars [orig: Script_Compile @0x4F3412..0x4F34C9], ELSEIF renamed, and
// END's family [orig: the switch @0x4F461E..0x4F4634].
static void token_hash() {
	CHECK(wac_word_byte('a') == 'A' && wac_word_byte('A') == 'A' && wac_word_byte('{') == '[' &&
	      wac_word_byte(0x85) == 0x65 && wac_word_byte('0') == '0');
	CHECK(wac_hash_bytes('E', 'N', 'D', ';') == wac_hash4('E', 'N', 'D', ';'));
	// A byte past 0x7F after the first is sign-extended into the bytes before it.
	CHECK(wac_hash_bytes('A', 0x80, ';', ';') == 0x40803B3Bu && wac_hash4('A', char(0x80), ';', ';') == 0x41803B3Bu);
	CHECK(wac_word_hash("end") == wac_hash4('E', 'N', 'D', ';') && wac_word_hash("") == wac_hash4(';', ';', ';', ';'));
	CHECK(wac_word_hash("ENTERS") == wac_word_hash("enter") && wac_word_hash("ELSEX") == wac_word_hash("ELSE"));
	CHECK(wac_word_hash("ElseIf") == wac_hash4('E', 'L', 'S', 'I') && wac_word_hash("ELSE") == wac_hash4('E', 'L', 'S', 'E'));
	CHECK(wac_token_hash("ELSEIF", wac_hash4('E', 'L', 'S', 'E')) == wac_hash4('E', 'L', 'S', 'I') &&
	      wac_token_hash("ELSE", wac_hash4('E', 'L', 'S', 'E')) == wac_hash4('E', 'L', 'S', 'E'));
	for (const char *end : {"END", "endif", "ENDDO", "EndLoop", "ENDP"}) CHECK(wac_is_end_hash(wac_word_hash(end)));
	for (const char *other : {"ENDS", "EN", "ENTER", "ELSE"}) CHECK(!wac_is_end_hash(wac_word_hash(other)));
	// Every keyword's hash is its own.
	for (size_t i = 0; i < sizeof(kWacKeywords) / sizeof(kWacKeywords[0]); ++i)
		for (size_t j = i + 1; j < sizeof(kWacKeywords) / sizeof(kWacKeywords[0]); ++j)
			CHECK(wac_word_hash(kWacKeywords[i]) != wac_word_hash(kWacKeywords[j]));
}

static void keywords() {
	CHECK(sizeof(kWacKeywords) / sizeof(kWacKeywords[0]) == 18);
	// Every word the compiler reads as a keyword is one of the table's, and the table's words (but
	// RUN, which would load a file) are each read so.
	const std::string source =
			"VAR myvar\r\n"
			"CHEAT mycheat\r\n"
			"IF NOT V1 == 2 AND V2 == 3 OR V3 == 1 THEN\r\n"
			"V4 = 1\r\n"
			"ELSEIF V5 == 1 THEN\r\n"
			"V4 = 2\r\n"
			"ELSE\r\n"
			"V4 = 3\r\n"
			"END\r\n"
			"IF V1 == 1 ENTER\r\nV4 = 4\r\nEND\r\n"
			"IF V1 == 1 LEAVE\r\nV4 = 5\r\nEND\r\n"
			"DOSEQ\r\nV4 = 6\r\nNEXT\r\nV4 = 7\r\nEND\r\n"
			"DORND\r\nV4 = 8\r\nNEXT\r\nV4 = 9\r\nEND\r\n"
			"GLOOP humans\r\nV4 = 10\r\nEND\r\n"
			"PLOOP\r\nV4 = 11\r\nEND\r\n";
	const Program program = compile_source(source, {});
	bool seen[sizeof(kWacKeywords) / sizeof(kWacKeywords[0])] = {};
	for (const WordUse &use : program.word_uses) {
		if (use.kind != WordUse::Kind::Keyword) continue;
		const std::string_view word = std::string_view(source).substr(use.offset, use.length);
		CHECK(is_keyword(word));
		for (size_t i = 0; i < sizeof(kWacKeywords) / sizeof(kWacKeywords[0]); ++i)
			if (strutil::iequals(word, kWacKeywords[i])) seen[i] = true;
	}
	for (size_t i = 0; i < sizeof(kWacKeywords) / sizeof(kWacKeywords[0]); ++i) {
		if (std::string_view(kWacKeywords[i]) == "RUN") continue;
		if (!seen[i]) std::printf("not read as a keyword: %s\n", kWacKeywords[i]);
		CHECK(seen[i]);
	}
}

int main() {
	comments();
	prefixes();
	keywords();
	token_hash();
	if (failures) {
		std::printf("wac_lexis: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("wac_lexis: ok\n");
	return 0;
}
