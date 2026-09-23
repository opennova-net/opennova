#pragma once

// The retail float-to-int conversions, shared by the formats and the runtime:
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

namespace opennova::io {

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
