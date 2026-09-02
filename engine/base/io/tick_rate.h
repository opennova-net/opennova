#pragma once

#include <cstdint>

namespace opennova::io {

// The logic clock, spelled once. Two DIFFERENT witnessed constants live here;
// they are not interchangeable:
//
// kTickHz: the real logic rate. Game_MainLoop banks wall-clock in 16 ms quanta
// and runs one logic tick per drained quantum, so the rate is exactly 62.5 Hz
// [orig: Game_MainLoop @0x52b630; current_tick @0x24c1968]. Use it wherever
// retail multiplies or divides by the double/float 62.5 (dbl_7C3B48 /
// flt_7C3B3C) or converts seconds to ticks in floating point.
inline constexpr double kTickHz = 62.5;
inline constexpr int32_t kTickMs = 16;

// kTicksPerSecondInt: the INTEGER divisor retail's integer arithmetic uses at
// every witnessed `* 62` / `/ 62` (the 0x84210843 magic multiply + sar 5),
// the def-file age parses and the end-of-round clock. Integer sites stay
// integer; do not "correct" one to the other.
inline constexpr int32_t kTicksPerSecondInt = 62;

} // namespace opennova::io
