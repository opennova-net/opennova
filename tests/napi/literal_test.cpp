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

} // namespace

int main() {
	if (!expect_literal("'x' tail", uint32_t('x'), 3)) return 1;
	if (!expect_literal("$2A", 42, 3)) return 1;
	if (!expect_literal("0x2Ah", 42, 4)) return 1;
	if (!expect_literal("$2Ah", 42, 3)) return 1;
	if (!expect_literal("2Ah", 42, 3)) return 1;
	if (!expect_literal("017", 15, 3)) return 1;
	if (!expect_literal("17o", 15, 3)) return 1;
	if (!expect_literal("0o", 0, 2)) return 1;
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
