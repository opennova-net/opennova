#pragma once

// The retail CRT float-to-int conversion (_ftol2_sse), shared by the formats
// and the runtime: the items.def parser's scaled properties and the WAC
// compiler's literals and VM power folds call it on an x87 value.
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

} // namespace opennova::io
