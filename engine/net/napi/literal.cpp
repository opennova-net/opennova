#include <net/napi/literal.h>

#include <base/io/cp1252.h> // cp1252_isalpha_sign_extended, cp1252_isdigit
#include <base/io/crt_ftol.h> // io::retail_atol_n

namespace opennova {
namespace {

// The CRT _isalpha under the game's ".ACP" LC_CTYPE, pinned to cp1252 (D-NET-478), on the
// sign-extended byte: the test of the byte after the run in the prefix forms of the hex,
// octal and binary arms and in the decimal arm, where a letter other than the arm's own
// h / o / b / d fails the literal, so a cp1252 high-byte letter does too and 0xFF, the EOF
// row, does not (docs/net/novaworld-net-re.md D-NET-391).
// [orig: NapiScript_ParseHexValue @0x62d610 (the _isalpha call @0x62d6fc);
//  NapiScript_ParseOctalInteger @0x62d7b0 (@0x62d85f); NapiScript_ParseBinaryLiteral
//  @0x62d900 (@0x62d9b1); NapiScript_ParseDecimalIntegerB @0x62da20 (@0x62da97) — each
//  _isalpha @0x76b703 of a movsx'd byte]
bool is_alpha(char c) {
	return cp1252_isalpha_sign_extended(c);
}

// The CRT _isdigit under the same LC_CTYPE, pinned to cp1252 (D-NET-388): the ten ASCII digits
// and the superscripts 0xB2, 0xB3, 0xB9. Every arm hands it the sign-extended byte, which reads
// the byte's own row (the table's mirror; 0xFF is no digit either way) (D-NET-391).
// [orig: _isdigit @0x76b86d -> _isdigit_l @0x76b81e (pctype & 4); NapiScript_ParseHexValue
//  @0x62d610 (the calls @0x62d694, @0x62d73e); NapiScript_ParseOctalInteger @0x62d7b0
//  (@0x62d84f); NapiScript_ParseBinaryLiteral @0x62d900 (@0x62d9a1);
//  NapiScript_ParseDecimalIntegerB @0x62da20 (@0x62da61, @0x62da87)]
bool is_digit(char c) {
	return cp1252_isdigit(static_cast<uint8_t>(c));
}

// The hex run's letters, compared as signed chars, so no high byte is one.
// [orig: NapiScript_ParseHexValue @0x62d610 — @0x62d6a2..0x62d6b0]
bool is_hex_letter(char c) {
	return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
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

// The run is a digit or an ASCII hex letter. Each run byte folds into the value in 32 bits, a
// digit as 16 * (value - 3) + byte (16 * value + byte - '0'), an upper-case letter as
// 16 * value + byte - 55 and a lower-case one as 16 * value + byte - 87, the byte sign-extended,
// so a cp1252 superscript digit folds as its signed byte less '0' (0xB2 as -126).
// [orig: NapiScript_ParseHexValue @0x62d610 — the run @0x62d690..0x62d6c6; the fold
//  @0x62d730..0x62d788: _isdigit @0x62d73e and @0x62d74a..0x62d754, isupper @0x62d759 and
//  @0x62d765..0x62d76e, else @0x62d772..0x62d77b]
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
	while (cursor < input.size() && (is_digit(input[cursor]) || is_hex_letter(input[cursor]))) {
		const char c = input[cursor];
		const int32_t byte = static_cast<int8_t>(static_cast<uint8_t>(c));
		const int32_t offset = is_digit(c) ? '0' : (c >= 'A' && c <= 'F') ? 55 : 87;
		parsed = parsed * 16u + static_cast<uint32_t>(byte - offset);
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
	// After a leading zero, a digit past the 0..7 run fails the arm.
	// [orig: NapiScript_ParseOctalInteger @0x62d7b0 — _isdigit @0x62d84f, the fail @0x62d859]
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
	// After a '%', any digit past the 0 / 1 run fails the arm ("%12", "%1\xB3").
	// [orig: NapiScript_ParseBinaryLiteral @0x62d900 — _isdigit @0x62d9a1, the fail @0x62d9ab]
	if (!suffix_required && cursor < input.size() && is_digit(input[cursor]))
		return false;
	if (!finish_radix(input, cursor, suffix_required, 'b', consumed)) return false;
	value = parsed;
	return true;
}

// The run is gated and extended by the digit class (a superscript extends it), and its value
// is the CRT atol of the run, which stops at a superscript and saturates at the int32 range
// (io::retail_atol_n, D-NET-384), so "12\xB2" reads 12 over three bytes and 4294967295 reads
// 0x7FFFFFFF.
// [orig: NapiScript_ParseDecimalIntegerB @0x62da20 — the run @0x62da61..0x62da91, _atol
//  @0x76ab0a called @0x62dabc, the store @0x62dac5]
bool parse_decimal(std::string_view input, uint32_t &value, size_t &consumed) {
	if (input.empty() || !is_digit(input[0])) return false;
	size_t cursor = 0;
	while (cursor < input.size() && is_digit(input[cursor])) {
		++cursor;
		if (cursor < input.size() && input[cursor] == '.') return false;
	}
	const size_t run = cursor;
	if (cursor < input.size() && (input[cursor] == 'd' || input[cursor] == 'D'))
		++cursor;
	else if (cursor < input.size() && is_alpha(input[cursor]))
		return false;
	value = static_cast<uint32_t>(io::retail_atol_n(input.data(), run));
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
