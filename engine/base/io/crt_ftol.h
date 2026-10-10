#pragma once

// The retail number conversions, shared by the formats and the runtime: the
// CRT's integer readers (retail_strtol, retail_atol, retail_atoi64 below), its
// atof (retail_atof) and its float-to-int conversions:
// the CRT's _ftol2_sse (the items.def parser's scaled properties, the WAC
// compiler's literals and VM power folds) and the inline x87 `fistp qword`
// a parser compiles for itself (the items.def scale).
//
// _ftol2_sse branches on the CRT's SSE2 flag, which the startup initializer
// sets from CPUID on every SSE2 processor [orig: sub_7887AF @0x7887B4 (the
// `mov dword_334A444, eax` store)], so the shipped game always takes the
// cvttsd2si leg: the x87 value is rounded to a double, then truncated toward
// zero into 32 bits; a NaN, an infinity or a truncation outside the int32
// range yields the integer indefinite 0x80000000. The x87 64-bit fistp leg
// behind the flag is dead on that hardware.
// [orig: _ftol2_sse @0x76BC00 (the flag test), @0x76BC15 (cvttsd2si)]
#include <base/io/cp1252.h>

#include <charconv>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>

namespace opennova::io {

namespace detail {

// A decimal spelled [-]digits[.digits][e[+-]digits] to the nearest double,
// whatever the process locale: the game pins LC_NUMERIC to "C" after its
// `setlocale(LC_ALL, ".ACP")` [orig: System_InitTimerAndLocale @0x762A6E, the
// LC_NUMERIC "C" call @0x762A7A], and our embedder owns the locale, so the
// point must not follow it. from_chars reads no locale; where the library has
// no floating from_chars, or the value is out of range (where from_chars
// stores nothing and strtod gives the infinity or the zero), strtod reads the
// number with the point respelled as the C library's current one.
inline double decimal_to_double(const std::string &number) {
#if defined(__cpp_lib_to_chars)
	double value = 0.0;
	const std::from_chars_result r = std::from_chars(
			number.data(), number.data() + number.size(), value, std::chars_format::general);
	if (r.ec == std::errc()) return value;
#endif
	std::string spelled = number;
	const std::lconv *conv = std::localeconv();
	const char *point = conv != nullptr ? conv->decimal_point : nullptr;
	const size_t dot = spelled.find('.');
	if (dot != std::string::npos && point != nullptr && point[0] != '\0' && std::strcmp(point, ".") != 0)
		spelled.replace(dot, 1, point);
	return std::strtod(spelled.c_str(), nullptr);
}

// The leading white space the CRT's integer readers skip. strtoxl and strtoxq test each
// byte, zero-extended, against the thread locale's ctype table (pctype & _SPACE, or
// _isctype_l on a double-byte page), and WinMain sets that locale to ".ACP" with only
// LC_NUMERIC put back to "C", so the set is the host ANSI page's C1_SPACE bytes: on cp1252,
// the pinned target (docs/net/novaworld-net-re.md D-NET-382), the six C-locale spaces and
// 0xA0, the no-break space (D-NET-384). A sign-extended caller would read the same row
// (cp1252_isspace's note).
// [orig: strtoxl @0x76B0AE, the skip @0x76B11C..0x76B153; CRT_strtoxq @0x777947, the skip
//  @0x7779B4..0x7779EE; System_InitTimerAndLocale @0x762A00, setlocale(LC_ALL, ".ACP")
//  @0x762A6E, setlocale(LC_NUMERIC, "C") @0x762A7A]
inline const char *skip_crt_space(const char *p, const char *end) {
	while (p < end && cp1252_isspace(static_cast<std::uint8_t>(*p))) ++p;
	return p;
}

// strtoxl (32 bits) or strtoxq (64 bits) for a signed result with no end pointer, over
// [s, s + len): the white space above, an optional sign, then for radix 0 the C prefix rule
// (0x hexadecimal, a leading 0 octal, else decimal) and for radix 16 an optional 0x / 0X
// (skipped even when no digit follows it), then the digits below the radix. A value past the
// type's range saturates to its MAX, or its MIN under a minus (the walk stops at the overflow
// when there is no end pointer); no digit reads 0. The digit and letter tests read the
// locale's table too (pctype & _DIGIT, then & (_UPPER | _LOWER | _ALPHA)), but the value is
// the SIGN-EXTENDED byte less '0' (or less 'A' - 10), so a cp1252 superscript digit or a
// high-byte letter yields a value at or past every radix and stops the walk like any other
// byte: only 0-9, a-z and A-Z ever count. `radix` is 0 or 2..36.
// [orig: strtoxl @0x76B0AE — the sign @0x76B155..0x76B167, the radix prefixes
//  @0x76B185..0x76B1CA, the digit and letter classes @0x76B1D9..0x76B20A, the overflow test
//  @0x76B20C..0x76B223, the saturation @0x76B24D..0x76B295, the negation @0x76B2A1..0x76B2A7;
//  CRT_strtoxq @0x777947, the same walk on 64 bits @0x7779F0..0x777BBC]
template <typename Int>
Int crt_strtox_signed(const char *s, size_t len, unsigned radix) {
	using UInt = std::make_unsigned_t<Int>;
	if (s == nullptr) return 0;
	const char *const end = s + len;
	const auto at = [end](const char *q) -> unsigned {
		return q < end ? static_cast<unsigned char>(*q) : 0u;
	};
	const char *p = skip_crt_space(s, end);
	bool negative = false;
	if (at(p) == '-') {
		negative = true;
		++p;
	} else if (at(p) == '+') {
		++p;
	}
	const bool x_follows = p < end && (at(p + 1) == 'x' || at(p + 1) == 'X');
	if (radix == 0) radix = at(p) != '0' ? 10u : x_follows ? 16u : 8u;
	if (radix == 16 && at(p) == '0' && x_follows) p += 2;
	const UInt limit = static_cast<UInt>(~UInt{0} / radix);
	const UInt last = static_cast<UInt>(~UInt{0} % radix);
	UInt number = 0;
	bool digits = false;
	bool overflow = false;
	for (;; ++p) {
		const unsigned c = at(p);
		unsigned digit = 0;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
		else break;
		if (digit >= radix) break;
		digits = true;
		if (number > limit || (number == limit && digit > last)) {
			overflow = true;
			break;
		}
		number = static_cast<UInt>(number * radix + digit);
	}
	if (!digits) return 0;
	constexpr UInt kMax = static_cast<UInt>(std::numeric_limits<Int>::max());
	if (negative) {
		if (overflow || number > kMax) return std::numeric_limits<Int>::min();
		return static_cast<Int>(-static_cast<Int>(number));
	}
	return overflow || number > kMax ? std::numeric_limits<Int>::max() : static_cast<Int>(number);
}

} // namespace detail

// The CRT's atof as the game links it [orig: _atof @0x76B6A1 -> _atof_l ->
// _fltin2]: leading white space, an optional sign, decimal digits with at most
// one '.', then an optional exponent marked e, E, d or D. The longest such
// prefix is the value, and a string with no digit reads 0.0. There is no hex,
// infinity or NaN spelling (later CRTs' strtod reads those, so the prefix is cut
// here before it converts). Over the `len` bytes at `s`, which need no NUL.
// The conversion rounds correctly, where the game's (`__strgtold12_l` then
// `_ld12tod`) need not in the last place (D-ITEMDEF-11). The white space here is
// still the C locale's six: the game's `_atof_l` skips by the locale's table like
// strtoxl, so 0xA0 too (docs/net/novaworld-net-re.md D-NET-389, open).
inline double retail_atof_n(const char *s, size_t len) {
	if (s == nullptr) return 0.0;
	const char *const end = s + len;
	while (s < end && (*s == ' ' || (*s >= '\t' && *s <= '\r'))) ++s;
	std::string number;
	const char *p = s;
	if (p < end && (*p == '+' || *p == '-')) {
		if (*p == '-') number.push_back('-');
		++p;
	}
	size_t digits = 0;
	while (p < end && *p >= '0' && *p <= '9') {
		number.push_back(*p++);
		++digits;
	}
	if (p < end && *p == '.') {
		number.push_back(*p++);
		while (p < end && *p >= '0' && *p <= '9') {
			number.push_back(*p++);
			++digits;
		}
	}
	if (digits == 0) return 0.0;
	if (p < end && (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D')) {
		const char *q = p + 1;
		std::string exponent = "e";
		if (q < end && (*q == '+' || *q == '-')) exponent.push_back(*q++);
		if (q < end && *q >= '0' && *q <= '9') {
			while (q < end && *q >= '0' && *q <= '9') exponent.push_back(*q++);
			number += exponent;
		}
	}
	return detail::decimal_to_double(number);
}

inline double retail_atof(const char *s) {
	return s != nullptr ? retail_atof_n(s, std::strlen(s)) : 0.0;
}

// The CRT's strtol as the game links it, with no end pointer [orig: strtol @0x76B2D9 ->
// strtoxl @0x76B0AE], on a 32-bit long on every host: the locale's leading white space (the
// six C-locale spaces and cp1252's 0xA0, detail::skip_crt_space), an optional sign, the
// radix's digits (0 or 2..36; detail::crt_strtox_signed); a value past the int32 range
// saturates to INT32_MAX or INT32_MIN, where an LP64 strtol would read 64 bits and a
// narrowing cast wrap. Over the `len` bytes at `s`, which need no NUL.
inline int32_t retail_strtol_n(const char *s, size_t len, unsigned radix) {
	return detail::crt_strtox_signed<int32_t>(s, len, radix);
}

inline int32_t retail_strtol(const char *s, unsigned radix) {
	return s != nullptr ? retail_strtol_n(s, std::strlen(s), radix) : 0;
}

// The CRT's atol as the game links it [orig: _atol @0x76AB0A = strtol(s, NULL, 10)
// @0x76AB12]: retail_strtol at radix 10, so leading white space by the locale (0xA0
// included), an optional sign, decimal digits, saturating at the int32 range.
// Over the `len` bytes at `s`, which need no NUL.
inline int32_t retail_atol_n(const char *s, size_t len) {
	return retail_strtol_n(s, len, 10);
}

inline int32_t retail_atol(const char *s) {
	return s != nullptr ? retail_atol_n(s, std::strlen(s)) : 0;
}

// The CRT's _atoi64 as the game links it [orig: _atoi64 @0x76AB20 = _strtoi64(s, NULL, 10)
// @0x76AB28 -> CRT_strtoxq @0x777947]: atol's walk on 64 bits, saturating at the int64
// range. Over the `len` bytes at `s`, which need no NUL.
inline int64_t retail_atoi64_n(const char *s, size_t len) {
	return detail::crt_strtox_signed<int64_t>(s, len, 10);
}

inline int64_t retail_atoi64(const char *s) {
	return s != nullptr ? retail_atoi64_n(s, std::strlen(s)) : 0;
}

inline int32_t retail_ftol_sse2(double value) {
	if (!(value > -2147483649.0 && value < 2147483648.0)) return INT32_MIN;
	return static_cast<int32_t>(value);
}

// The inline conversion: the x87 control word switched to round toward zero,
// `fistp qword`, and only the low dword kept, so a value past int32 wraps. A
// NaN, an infinity or a value outside int64 stores the integer indefinite
// 0x8000000000000000, whose low dword is 0.
// [orig: ItemDef_ParseProperty @0x49EB00 (RC=truncate @0x49F710, `fistp
// qword` @0x49F728, the low dword read @0x49F72C)]
inline int32_t retail_fistp_truncate_low_dword(double value) {
	if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) return 0;
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(value)));
}

} // namespace opennova::io
