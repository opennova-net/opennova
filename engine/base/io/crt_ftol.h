#pragma once

// The retail number conversions, shared by the formats and the runtime: the
// CRT's atof (retail_atof below) and its float-to-int conversions:
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
#include <charconv>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

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

} // namespace detail

// The CRT's atof as the game links it [orig: _atof @0x76B6A1 -> _atof_l ->
// _fltin2]: leading white space, an optional sign, decimal digits with at most
// one '.', then an optional exponent marked e, E, d or D. The longest such
// prefix is the value, and a string with no digit reads 0.0. There is no hex,
// infinity or NaN spelling (later CRTs' strtod reads those, so the prefix is cut
// here before it converts). Over the `len` bytes at `s`, which need no NUL.
// The conversion rounds correctly, where the game's (`__strgtold12_l` then
// `_ld12tod`) need not in the last place (D-ITEMDEF-11).
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

// The CRT's atol as the game links it [orig: _atol @0x76AB0A = strtol(s,
// NULL, 10)], on a 32-bit long on every host: leading white space, an optional
// sign, decimal digits; a value past the int32 range saturates to INT32_MAX or
// INT32_MIN, where an LP64 strtol would read 64 bits and a narrowing cast wrap.
// Over the `len` bytes at `s`, which need no NUL.
inline int32_t retail_atol_n(const char *s, size_t len) {
	if (s == nullptr) return 0;
	const char *p = s;
	const char *const end = s + len;
	while (p < end && (*p == ' ' || (*p >= '\t' && *p <= '\r'))) ++p;
	bool negative = false;
	if (p < end && (*p == '+' || *p == '-')) negative = *p++ == '-';
	constexpr int64_t kLimit = int64_t{1} << 31; // |INT32_MIN|
	int64_t magnitude = 0;
	for (; p < end && *p >= '0' && *p <= '9'; ++p) {
		magnitude = magnitude * 10 + (*p - '0');
		if (magnitude > kLimit) magnitude = kLimit + 1; // saturated; stays put
	}
	if (negative) return magnitude >= kLimit ? INT32_MIN : static_cast<int32_t>(-magnitude);
	return magnitude >= kLimit ? INT32_MAX : static_cast<int32_t>(magnitude);
}

inline int32_t retail_atol(const char *s) {
	return s != nullptr ? retail_atol_n(s, std::strlen(s)) : 0;
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
