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
#include <cstdint>
#include <cstdlib>
#include <string>

namespace opennova::io {

// The CRT's atof as the game links it [orig: _atof @0x76B6A1 -> _atof_l ->
// _fltin2]: leading white space, an optional sign, decimal digits with at most
// one '.', then an optional exponent marked e, E, d or D. The longest such
// prefix is the value, and a string with no digit reads 0.0. There is no hex,
// infinity or NaN spelling (later CRTs' strtod reads those, so the prefix is cut
// here before strtod converts it).
inline double retail_atof(const char *s) {
	if (s == nullptr) return 0.0;
	while (*s == ' ' || (*s >= '\t' && *s <= '\r')) ++s;
	std::string number;
	const char *p = s;
	if (*p == '+' || *p == '-') number.push_back(*p++);
	size_t digits = 0;
	while (*p >= '0' && *p <= '9') {
		number.push_back(*p++);
		++digits;
	}
	if (*p == '.') {
		number.push_back(*p++);
		while (*p >= '0' && *p <= '9') {
			number.push_back(*p++);
			++digits;
		}
	}
	if (digits == 0) return 0.0;
	if (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D') {
		const char *q = p + 1;
		std::string exponent = "e";
		if (*q == '+' || *q == '-') exponent.push_back(*q++);
		if (*q >= '0' && *q <= '9') {
			while (*q >= '0' && *q <= '9') exponent.push_back(*q++);
			number += exponent;
		}
	}
	return std::strtod(number.c_str(), nullptr);
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
