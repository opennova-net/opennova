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
	if (!expect_literal("4294967295", 0xFFFFFFFFu, 10)) return 1;
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
