#include <net/napi/literal.h>

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>

#include <cctype>

namespace opennova {
namespace {

// The process locale's letter class, where the prefix forms of the hex, octal and binary arms
// and the decimal arm call the CRT _isalpha on the byte after the run under the game's ".ACP"
// LC_CTYPE [orig: NapiScript_ParseHexValue @0x62d6fc; NapiScript_ParseOctalInteger @0x62d85f;
// NapiScript_ParseBinaryLiteral @0x62d9b1; NapiScript_ParseDecimalIntegerB @0x62da97]; the
// suffix forms test h / o / b directly [orig: @0x62d6e7, @0x62d835, @0x62d98e]
// (docs/net/novaworld-net-re.md D-NET-391, open for the letter class).
bool is_alpha(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

// The CRT _isdigit every arm calls on the sign-extended byte under the ".ACP" LC_CTYPE: the
// ten ASCII digits and cp1252's superscripts 0xB2 / 0xB3 / 0xB9 (D-NET-388's pin; the
// sign-extended byte reads the same row). [orig: _isdigit @0x76b86d, called @0x62d694 /
// @0x62d73e (hex), @0x62d84f (octal), @0x62d9a1 (binary), @0x62da61 / @0x62da87 (decimal)]
bool is_digit(char c) {
	return cp1252_isdigit(static_cast<uint8_t>(c));
}

// The sign-extended byte, as the arms' folds read it (movsx).
int32_t signed_byte(char c) {
	return static_cast<int32_t>(static_cast<int8_t>(static_cast<unsigned char>(c)));
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
	// The run takes a CRT digit (a cp1252 superscript included) or a letter A..F / a..f, and
	// folds each as 16 * value + the sign-extended byte less '0', so a superscript folds as a
	// negative digit; a letter folds less 55 (upper case) or 87 (lower case).
	// [orig: NapiScript_ParseHexValue @0x62d610 - the run @0x62d690..0x62d6c6 (isdigit
	//  @0x62d694, A..F @0x62d6a2, a..f @0x62d6aa); the fold @0x62d730..0x62d77d (isdigit
	//  @0x62d73e, (value - 3) * 16 + byte @0x62d74a..0x62d754, isupper @0x62d759)]
	const size_t first_digit = cursor;
	uint32_t parsed = 0;
	while (cursor < input.size()) {
		const char c = input[cursor];
		if (is_digit(c))
			parsed = parsed * 16u + static_cast<uint32_t>(signed_byte(c) - '0');
		else if (c >= 'A' && c <= 'F')
			parsed = parsed * 16u + static_cast<uint32_t>(c - 55);
		else if (c >= 'a' && c <= 'f')
			parsed = parsed * 16u + static_cast<uint32_t>(c - 87);
		else
			break;
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
	// After a leading zero's run, a CRT digit (8, 9 or a cp1252 superscript) fails the arm.
	// [orig: NapiScript_ParseOctalInteger @0x62d7b0 - isdigit @0x62d84f, the fail @0x62d859]
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
	// After a '%' run, a CRT digit (2..9 or a cp1252 superscript) fails the arm.
	// [orig: NapiScript_ParseBinaryLiteral @0x62d900 - isdigit @0x62d9a1, the fail @0x62d9ab]
	if (!suffix_required && cursor < input.size() && is_digit(input[cursor]))
		return false;
	if (!finish_radix(input, cursor, suffix_required, 'b', consumed)) return false;
	value = parsed;
	return true;
}

// The run is CRT digits (a cp1252 superscript included) and its value the CRT atol of it,
// which stops at a superscript and saturates at INT32_MAX.
// [orig: NapiScript_ParseDecimalIntegerB @0x62da20 - isdigit @0x62da61 / @0x62da87, the '.'
//  fail @0x62da81; _atol @0x76ab0a called @0x62dabc, stored @0x62dac5]
bool parse_decimal(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty() || !is_digit(input[0])) return false;
	size_t cursor = 0;
	while (cursor < input.size() && is_digit(input[cursor])) {
		++cursor;
		if (cursor < input.size() && input[cursor] == '.') return false;
	}
	const uint32_t parsed = static_cast<uint32_t>(io::retail_atol_n(input.data(), cursor));
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
