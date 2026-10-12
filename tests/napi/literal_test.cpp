#include <net/napi/literal.h>

#include <cstdio>
#include <string_view>

namespace {

bool expect_literal(std::string_view input, uint32_t expected,
		size_t expected_consumed) {
	uint32_t value = 0xDEADBEEFu;
	size_t consumed = 99;
	if (!opennova::napi_parse_literal_value(input, value, &consumed)) {
		std::fprintf(stderr, "FAIL: literal '%.*s' did not parse\n",
				int(input.size()), input.data());
		return false;
	}
	if (value != expected || consumed != expected_consumed) {
		std::fprintf(stderr,
				"FAIL: literal '%.*s' -> value=%u consumed=%zu; expected %u/%zu\n",
				int(input.size()), input.data(), value, consumed,
				expected, expected_consumed);
		return false;
	}
	return true;
}

bool expect_rejected(std::string_view input) {
	uint32_t value = 0x12345678u;
	size_t consumed = 77;
	if (opennova::napi_parse_literal_value(input, value, &consumed)) {
		std::fprintf(stderr, "FAIL: literal '%.*s' unexpectedly parsed\n",
				int(input.size()), input.data());
		return false;
	}
	return value == 0x12345678u && consumed == 77;
}

// The byte after a run is tested with the CRT isalpha under ".ACP", pinned to cp1252, on the
// sign-extended byte, in the prefix forms of the hex, octal and binary arms and in the
// decimal arm (D-NET-391, D-NET-478): a cp1252 high-byte letter fails the literal as an
// ASCII one does, whatever the process locale; 0xFF (its -1 the EOF row) and the
// multiplication / division signs 0xD7 / 0xF7 end the run as any non-letter does.
// [orig: NapiScript_ParseHexValue @0x62d610 (the _isalpha call @0x62d6fc);
//  NapiScript_ParseOctalInteger @0x62d7b0 (@0x62d85f); NapiScript_ParseBinaryLiteral
//  @0x62d900 (@0x62d9b1); NapiScript_ParseDecimalIntegerB @0x62da20 (@0x62da97)]
bool check_letter_class() {
	// Rejected: e acute after a decimal run, A grave after a '$' hex run, S caron after a
	// 0x hex run, sharp s after an octal run, Y diaeresis after a '%' binary run, the
	// feminine ordinal and micro after a decimal run.
	const bool letters = expect_rejected("12\xE9") && expect_rejected("$1F\xC0") &&
			expect_rejected("0x1F\x8A") && expect_rejected("017\xDF") &&
			expect_rejected("%101\x9F") && expect_rejected("7\xAA") && expect_rejected("7\xB5");
	const bool non_letters = expect_literal("12\xFF", 12, 2) && expect_literal("12\xD7", 12, 2) &&
			expect_literal("0x1F\xFF", 31, 4) && expect_literal("017\xF7", 15, 3) &&
			expect_literal("%101\xFF", 5, 4);
	return letters && non_letters;
}

} // namespace

int main() {
	if (!check_letter_class()) return 1;
	if (!expect_literal("'x' tail", uint32_t('x'), 3)) return 1;
	if (!expect_literal("$2A", 42, 3)) return 1;
	if (!expect_literal("0x2Ah", 42, 4)) return 1;
	if (!expect_literal("$2Ah", 42, 3)) return 1;
	if (!expect_literal("2Ah", 42, 3)) return 1;
	if (!expect_literal("017", 15, 3)) return 1;
	if (!expect_literal("17o", 15, 3)) return 1;
	if (!expect_literal("0o", 0, 2)) return 1;
	// D-NET-466: a leading zero with no octal digit after it and an o / O reads 0 over the two
	// bytes whatever follows; a lone zero fails the octal arm and the decimal arm reads it.
	// [orig: NapiScript_ParseOctalInteger @0x62d7b0 - the o counted @0x62d875, the flag
	//  @0x62d878, the zero stored @0x62d89b]
	if (!expect_literal("0o5", 0, 2)) return 1;
	if (!expect_literal("0o.", 0, 2)) return 1;
	if (!expect_literal("0O17", 0, 2)) return 1;
	if (!expect_literal("0ox", 0, 2)) return 1;
	if (!expect_literal("0", 0, 1)) return 1;
	if (!expect_literal("0 ", 0, 1)) return 1;
	if (!expect_literal("017o5", 15, 4)) return 1;
	if (!expect_rejected("0.")) return 1;
	if (!expect_rejected("0q")) return 1;
	if (!expect_rejected("o")) return 1;
	if (!expect_literal("%101010", 42, 7)) return 1;
	if (!expect_literal("101010b", 42, 7)) return 1;
	if (!expect_literal("42", 42, 2)) return 1;
	if (!expect_literal("42d", 42, 3)) return 1;
	if (!expect_literal("08", 8, 2)) return 1;
	// D-NET-391: the decimal value is the CRT atol of the run, saturating at INT32_MAX.
	// [orig: NapiScript_ParseDecimalIntegerB @0x62da20 - _atol called @0x62dabc]
	if (!expect_literal("2147483647", 0x7FFFFFFFu, 10)) return 1;
	if (!expect_literal("2147483648", 0x7FFFFFFFu, 10)) return 1;
	if (!expect_literal("4294967295", 0x7FFFFFFFu, 10)) return 1;
	if (!expect_literal("4294967296d", 0x7FFFFFFFu, 11)) return 1;
	// D-NET-391: every arm's digit test is the CRT isdigit under the ".ACP" locale, pinned
	// to cp1252 (D-NET-388), so the superscripts 0xB2 / 0xB3 / 0xB9 are digits. The decimal
	// run takes them and atol stops at them; the hex run folds one as a negative digit
	// (16 * value + the sign-extended byte less '0'); a leading-zero octal or a '%' binary
	// run followed by one fails its arm, as a '%' run followed by any digit does.
	// [orig: _isdigit @0x76b86d - NapiScript_ParseHexValue @0x62d694 / @0x62d73e, the fold
	//  @0x62d74a..0x62d754; NapiScript_ParseOctalInteger @0x62d84f;
	//  NapiScript_ParseBinaryLiteral @0x62d9a1; NapiScript_ParseDecimalIntegerB @0x62da61 /
	//  @0x62da87]
	if (!expect_literal("12\xB2", 12, 3)) return 1;
	if (!expect_literal("\xB9", 0, 1)) return 1;
	if (!expect_literal("4\xB3" "5d", 4, 4)) return 1;
	if (!expect_literal("0x1\xB2", 0xFFFFFF92u, 4)) return 1;
	if (!expect_literal("\xB2h", 0xFFFFFF82u, 2)) return 1;
	if (!expect_literal("$\xB9" "0", 0xFFFFF890u, 3)) return 1;
	if (!expect_literal("07\xB2", 7, 3)) return 1;
	if (!expect_literal("%1b", 1, 3)) return 1;
	if (!expect_literal("%10 ", 2, 3)) return 1;
	if (!expect_rejected("%12")) return 1;
	if (!expect_rejected("%1\xB3")) return 1;
	if (!expect_rejected("-1")) return 1;
	if (!expect_rejected("+1")) return 1;
	if (!expect_rejected("1.0")) return 1;
	if (!expect_rejected("$")) return 1;
	if (!expect_rejected("2A")) return 1;
	const char signed_character[] = {'\'', static_cast<char>(0x80), '\'', '\0'};
	if (!expect_literal(std::string_view(signed_character, 3), 0xFFFFFF80u, 3)) return 1;
	std::printf("OK: retail NAPI literal radix dispatch\n");
	return 0;
}
