// The mouse-look pipeline: integer pixel deltas -> entity Yaw/Pitch (BAM32), the
// faithful port of the engine's per-frame mouse pump + axis-binding apply.
//
// [orig: Input_ProcessMouseAxisBindings @ 0x499680 — deltas are center-lock cursor
//  PIXELS per frame (Input_PumpAndCenterCursor @ 0x7616e0 pins the cursor at 320,240
//  and reads the offset); sensitivity = setting << 11 (profile +0x590, default 128
//  @ PlayerProfile_InitDefaults 0x54bbc0, clamped [1, 0x1FF] by the 'mousescale'
//  adjust @ 0x49b18b); the scoped reduction divides by the current zoom
//  (Player_GetClampedWeaponElevation @ 0x4dc6b0 = slot zoom clamped to the def's
//  scope_max_mag) when the weapon or vehicle gun is scoped and the binocular view is
//  down @ 0x499706-0x499714; scaled = (px * sens + 0x8000) >> 16 per axis
//  @ 0x49972e/0x499744; Y negated unless the flipmouse setting @ 0x4996cf.
//  Apply: yaw -= scaledX << 16 (case 166 @ 0x4e109d); pitch += scaledY << 16
//  (case 164 @ 0x4e0fed) clamped to +-80 deg, with the UP limit +40 deg while
//  prone (MoveOrder & 0x100) @ 0x4e0ff7.]
#ifndef OPENNOVA_WORLD_PLAYER_LOOK_H
#define OPENNOVA_WORLD_PLAYER_LOOK_H

#include <cstdint>

namespace opennova::world {

// [orig: dword_24D207C default 128; clamp 1..0x1FF @ 0x49b19b-0x49b1b9]
constexpr int32_t kMouseSensitivityDefault = 128;
constexpr int32_t kMouseSensitivityMin = 1;
constexpr int32_t kMouseSensitivityMax = 0x1FF;
// Pitch clamps, BAM32. [orig: 0x38E38E00 / 0xC71C7200 @ 0x4e0d34/0x4e0d4e;
// prone up-limit 0x1C71C700 @ 0x4e0ffe]
constexpr int32_t kLookPitchMax = 0x38E38E00;         // +80 deg
constexpr int32_t kLookPitchMin = -0x38E38E00;        // -80 deg (= 0xC71C7200)
constexpr int32_t kLookPitchProneMax = 0x1C71C700;    // +40 deg while prone

struct PlayerLookSettings {
    int32_t sensitivity = kMouseSensitivityDefault; // [orig: dword_24D207C]
    bool invert_y = false; // flipmouse [orig: dword_24D2078; default OFF = push-forward looks up]
};

// One frame of mouse pixels onto the look angles. `dx_px`/`dy_px` are cursor pixels
// in screen sense (+x right, +y down — the raw center-lock offset). `scoped_zoom` is
// the current zoom magnification when the scoped sensitivity reduction applies
// (> 1 divides), else 0 or 1. `prone` selects the +40 deg up-clamp.
void player_look_apply(int32_t &yaw_bam, int32_t &pitch_bam, const PlayerLookSettings &s,
                       int32_t dx_px, int32_t dy_px, int32_t scoped_zoom, bool prone);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_PLAYER_LOOK_H
