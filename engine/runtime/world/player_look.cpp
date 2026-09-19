#include <runtime/world/player_look.h>

#include <base/io/bam.h>

namespace opennova::world {

PlayerLookDelta player_look_delta(const PlayerLookSettings &s,
        int32_t dx_px, int32_t dy_px, int32_t scoped_zoom) {
    // Y sense: the raw center-lock delta is +down; the default (flipmouse OFF) negates
    // it so pushing the mouse forward looks up. [orig: @ 0x4996cf — negate when the
    // setting dword is 0]
    const int32_t dy = s.invert_y ? dy_px : -dy_px;

    // The setting dword is read RAW here — no clamp on the per-frame path. The
    // [1, 0x1FF] range belongs to the 'mousescale' +/- adjust that WRITES the
    // setting [orig: @0x49b19b-0x49b1b9]; the profile apply copies it unclamped
    // [orig: apply_session_settings_to_globals @0x55161e]. (x2048 == the
    // witnessed `shl 11` @0x4996dd, spelled as a multiply so a negative setting
    // is defined arithmetic here too.)
    int64_t sens = static_cast<int64_t>(s.sensitivity) * 2048; // [orig: @ 0x4996dd]
    // The scoped reduction: base sens divided by the CURRENT zoom magnification.
    // [orig: @ 0x499714 — sens = base / Player_GetClampedWeaponElevation()]
    if (scoped_zoom > 1) sens /= scoped_zoom;

    // Per-axis scale with the witnessed +0x8000 rounding. [orig: @ 0x49972e/0x499744]
    const int32_t sdx = static_cast<int32_t>((static_cast<int64_t>(dx_px) * sens + 0x8000) >> 16);
    const int32_t sdy = static_cast<int32_t>((static_cast<int64_t>(dy) * sens + 0x8000) >> 16);

    return {io::bam_sub(0, int32_t(uint32_t(sdx) << 16)), int32_t(uint32_t(sdy) << 16)};
}

void player_look_apply(int32_t &yaw_bam, int32_t &pitch_bam, const PlayerLookSettings &s,
                       int32_t dx_px, int32_t dy_px, int32_t scoped_zoom, bool prone) {
    const auto delta = player_look_delta(s, dx_px, dy_px, scoped_zoom);
    // Yaw wraps, no clamp; mouse-right turns heading NEGATIVE (engine heading BAM).
    // [orig: case 166 @ 0x4e109d — sub [entity+0x10], scaled << 16]
    yaw_bam = io::bam_add(yaw_bam, delta.yaw);

    // Pitch accumulates and clamps; the UP limit drops to +40 deg while prone.
    // [orig: case 164 @ 0x4e0fed-0x4e100d + the clamp @ 0x4e0d39-0x4e0d5c]
    pitch_bam = io::bam_add(pitch_bam, delta.pitch);
    const int32_t up = prone ? kLookPitchProneMax : kLookPitchMax;
    if (pitch_bam > up) pitch_bam = up;
    if (pitch_bam < kLookPitchMin) pitch_bam = kLookPitchMin;
}

void player_look_keys(int32_t &yaw, int32_t &pitch, bool left, bool right,
    bool up, bool down, bool prone, int32_t body_pitch) {
    if (left) yaw = io::bam_add(yaw, 0x1FFFFFF);
    if (right) yaw = io::bam_sub(yaw, 0x1FFFFFF);
    if (up) pitch = io::bam_add(pitch, 0x1FFFFFF);
    if (down) pitch = io::bam_sub(pitch, 0x1FFFFFF);
    const int32_t limit = prone ? kLookPitchProneMax : kLookPitchMax;
    if (io::bam_sub(pitch, body_pitch) > limit) pitch = io::bam_add(body_pitch, limit);
    if (io::bam_sub(pitch, body_pitch) < -limit) pitch = io::bam_sub(body_pitch, limit);
}

} // namespace opennova::world
