#include <net/napi/literal.h>

#include <cctype>

namespace opennova {
namespace {

// The process locale's classes, where retail's decimal arm calls the CRT _isdigit /
// _isalpha under the game's ".ACP" LC_CTYPE [orig: NapiScript_ParseDecimalIntegerB
// @0x62da61 / @0x62da97] (docs/net/novaworld-net-re.md D-NET-391, open).
bool is_alpha(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool is_digit(char c) {
	return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool finish_radix(std::string_view input, size_t cursor, bool suffix_required,
		char suffix_lower, size_t &matched) {
	if (cursor < input.size() && input[cursor] == '.') return false;
	if (cursor < input.size() &&
			(input[cursor] == suffix_lower ||
			 input[cursor] == static_cast<char>(suffix_lower - ('a' - 'A')))) {
		matched = cursor + 1;
		return true;
	}
	if (suffix_required || (cursor < input.size() && is_alpha(input[cursor])))
		return false;
	matched = cursor;
	return true;
}

bool parse_hex(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty()) return false;
	size_t cursor = 0;
	bool suffix_required = true;
	bool prefixed = false;
	if (input[0] == '$') {
		cursor = 1;
		suffix_required = false;
		prefixed = true;
	} else if (input.size() >= 2 && input[0] == '0' &&
			(input[1] == 'x' || input[1] == 'X')) {
		cursor = 2;
		suffix_required = false;
		prefixed = true;
	}
	const size_t first_digit = cursor;
	uint32_t parsed = 0;
	while (cursor < input.size()) {
		const int digit = hex_digit(input[cursor]);
		if (digit < 0) break;
		parsed = parsed * 16u + static_cast<uint32_t>(digit);
		++cursor;
	}
	if (cursor == first_digit) return false;
	// In the prefixed forms retail recognizes a following h/H as a valid
	// terminator but leaves it outside the consumed cursor. Only the bare form
	// consumes its required suffix. [orig: @0x62d6e5..0x62d710]
	if (prefixed && cursor < input.size() &&
			(input[cursor] == 'h' || input[cursor] == 'H')) {
		consumed = cursor;
		value = parsed;
		return true;
	}
	if (!finish_radix(input, cursor, suffix_required, 'h', consumed)) return false;
	value = parsed;
	return true;
}

bool parse_octal(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty()) return false;
	size_t cursor = input[0] == '0' ? 1 : 0;
	const bool suffix_required = cursor == 0;
	const size_t first_digit = cursor;
	uint32_t parsed = 0;
	while (cursor < input.size() && input[cursor] >= '0' && input[cursor] <= '7') {
		parsed = parsed * 8u + static_cast<uint32_t>(input[cursor] - '0');
		++cursor;
	}
	if (cursor == first_digit) {
		// A lone leading zero is still a valid zero; an invalid following
		// decimal digit makes this arm fail so decimal gets its turn.
		if (!suffix_required && (cursor == input.size() ||
				(cursor + 1 == input.size() &&
				 (input[cursor] == 'o' || input[cursor] == 'O')))) {
			value = 0;
			consumed = cursor == input.size() ? 1 : 2;
			return true;
		}
		return false;
	}
	if (!suffix_required && cursor < input.size() && is_digit(input[cursor]))
		return false;
	if (!finish_radix(input, cursor, suffix_required, 'o', consumed)) return false;
	value = parsed;
	return true;
}

bool parse_binary(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty()) return false;
	size_t cursor = input[0] == '%' ? 1 : 0;
	const bool suffix_required = cursor == 0;
	const size_t first_digit = cursor;
	uint32_t parsed = 0;
	while (cursor < input.size() && (input[cursor] == '0' || input[cursor] == '1')) {
		parsed = parsed * 2u + static_cast<uint32_t>(input[cursor] - '0');
		++cursor;
	}
	if (cursor == first_digit) return false;
	if (!finish_radix(input, cursor, suffix_required, 'b', consumed)) return false;
	value = parsed;
	return true;
}

bool parse_decimal(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty() || !is_digit(input[0])) return false;
	size_t cursor = 0;
	uint32_t parsed = 0;
	while (cursor < input.size() && is_digit(input[cursor])) {
		parsed = parsed * 10u + static_cast<uint32_t>(input[cursor] - '0');
		++cursor;
		if (cursor < input.size() && input[cursor] == '.') return false;
	}
	if (cursor < input.size() && (input[cursor] == 'd' || input[cursor] == 'D'))
		++cursor;
	else if (cursor < input.size() && is_alpha(input[cursor]))
		return false;
	value = parsed;
	consumed = cursor;
	return true;
}

} // namespace

bool napi_parse_literal_value(std::string_view input, uint32_t &value,
		size_t *consumed) {
	uint32_t parsed = 0;
	size_t matched = 0;
	if (input.size() >= 3 && input[0] == '\'' && input[1] != '\0' &&
			input[2] == '\'') {
		parsed = static_cast<uint32_t>(static_cast<int32_t>(
				static_cast<int8_t>(static_cast<unsigned char>(input[1]))));
		matched = 3;
	} else if (!parse_hex(input, parsed, matched) &&
			!parse_octal(input, parsed, matched) &&
			!parse_binary(input, parsed, matched) &&
			!parse_decimal(input, parsed, matched)) {
		return false;
	}
	value = parsed;
	if (consumed != nullptr) *consumed = matched;
	return true;
}

} // namespace opennova
