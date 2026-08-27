#pragma once

// The engine's channel volume law: a 0..255 volume byte (member/clamp
// volumes, the emitter fire volume) onto the decibel scale a mixer's fader
// takes. 0 is HARD silent; otherwise linear = volume / 255 floored at 1e-4
// (-80 dB) and converted with 20 * log10.
// [orig: the full-volume emitter fire path passes 255 @0x528e20; the
//  distance curve SoundBank_CalcDistanceVolPan @0x75ca20 feeds the same byte]

#include <cmath>
#include <cstdint>

namespace opennova::audio {

inline constexpr int32_t kVolumeByteMax = 255;
inline constexpr double kVolumeSilentDb = -80.0;

// dB for a 0..255 channel volume; 0 -> hard silent. The 20/ln(10) factor is
// spelled the way the presenting layer's linear_to_db spells it so the two
// agree to the last bit.
inline double volume_db_from_byte(int32_t volume) {
    if (volume <= 0) return kVolumeSilentDb;
    double linear = static_cast<double>(volume) / static_cast<double>(kVolumeByteMax);
    if (linear < 0.0001) linear = 0.0001;
    if (linear > 1.0) linear = 1.0;
    return std::log(linear) * 8.6858896380650365530225783783321;
}

} // namespace opennova::audio
