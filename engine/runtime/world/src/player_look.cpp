#include "world/player_look.h"

#include <io/bam.h>

namespace opennova::world {

void player_look_apply(int32_t &yaw_bam, int32_t &pitch_bam, const PlayerLookSettings &s,
                       int32_t dx_px, int32_t dy_px, int32_t scoped_zoom, bool prone) {
    // Y sense: the raw center-lock delta is +down; the default (flipmouse OFF) negates
    // it so pushing the mouse forward looks up. [orig: @ 0x4996cf — negate when the
    // setting dword is 0]
    const int32_t dy = s.invert_y ? dy_px : -dy_px;

    int32_t setting = s.sensitivity;
    if (setting < kMouseSensitivityMin) setting = kMouseSensitivityMin;
    if (setting > kMouseSensitivityMax) setting = kMouseSensitivityMax;
    int64_t sens = static_cast<int64_t>(setting) << 11; // [orig: @ 0x4996dd]
    // The scoped reduction: base sens divided by the CURRENT zoom magnification.
    // [orig: @ 0x499714 — sens = base / Player_GetClampedWeaponElevation()]
    if (scoped_zoom > 1) sens /= scoped_zoom;

    // Per-axis scale with the witnessed +0x8000 rounding. [orig: @ 0x49972e/0x499744]
    const int32_t sdx = static_cast<int32_t>((static_cast<int64_t>(dx_px) * sens + 0x8000) >> 16);
    const int32_t sdy = static_cast<int32_t>((static_cast<int64_t>(dy) * sens + 0x8000) >> 16);

    // Yaw wraps, no clamp; mouse-right turns heading NEGATIVE (engine heading BAM).
    // [orig: case 166 @ 0x4e109d — sub [entity+0x10], scaled << 16]
    yaw_bam = io::bam_sub(yaw_bam, static_cast<int32_t>(static_cast<uint32_t>(sdx) << 16));

    // Pitch accumulates and clamps; the UP limit drops to +40 deg while prone.
    // [orig: case 164 @ 0x4e0fed-0x4e100d + the clamp @ 0x4e0d39-0x4e0d5c]
    pitch_bam = io::bam_add(pitch_bam, static_cast<int32_t>(static_cast<uint32_t>(sdy) << 16));
    const int32_t up = prone ? kLookPitchProneMax : kLookPitchMax;
    if (pitch_bam > up) pitch_bam = up;
    if (pitch_bam < kLookPitchMin) pitch_bam = kLookPitchMin;
}

} // namespace opennova::world
