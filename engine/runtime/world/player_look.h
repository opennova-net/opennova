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
#pragma once

#include <cstdint>

namespace opennova::world {

// [orig: g_MouseSensitivity default 128; the 1..0x1FF clamp is the +/- ADJUST's
//  @ 0x49b19b-0x49b1b9 (player_look_mouse_scale); neither the profile apply
//  @0x55161e nor the per-frame read @0x4996dd clamps, so the range gates the
//  adjust alone]
constexpr int32_t kMouseSensitivityDefault = 128;
constexpr int32_t kMouseSensitivityMin = 1;
constexpr int32_t kMouseSensitivityMax = 0x1FF;
// Pitch clamps, BAM32. [orig: 0x38E38E00 / 0xC71C7200 @ 0x4e0d34/0x4e0d4e;
// prone up-limit 0x1C71C700 @ 0x4e0ffe]
constexpr int32_t kLookPitchMax = 0x38E38E00;         // +80 deg
constexpr int32_t kLookPitchMin = -0x38E38E00;        // -80 deg (= 0xC71C7200)
constexpr int32_t kLookPitchProneMax = 0x1C71C700;    // +40 deg while prone

struct PlayerLookSettings {
    int32_t sensitivity = kMouseSensitivityDefault; // [orig: g_MouseSensitivity]
    bool invert_y = false; // flipmouse [orig: g_MouseInvertY; default OFF = push-forward looks up]
};

// The flipmouse row (catalog 79, dispatch 9): the LIVE Y invert flips. The
// profile record keeps its word, so the next mission start's session copy
// (or the in-game options Accept) writes the record's back.
// [orig: Input_HandleActionBinding case 9 `xor g_MouseInvertY, 1` @0x49afbb]
void player_look_flip_mouse(PlayerLookSettings &s);

// The mousescale row (catalog 82, dispatch 17) on the LIVE sensitivity, the
// record untouched as above: a nonzero `param_value` (the console's
// `mousescale <n>` line) sets the word outright, else it steps by 16, down
// when the event carries the sign bit (`negative`: bit 31 of the queued
// binding id, Shift held on the keyboard fallback pass or the mouse dispatch),
// then clamps to 1..0x1FF. Returns the new word, which the embedder posts in
// the STRMISC_MOUSESCALE toast (hud::mouse_scale_toast).
// [orig: Input_HandleActionBinding case 17 @0x49b185..0x49b1b9 (`test esi,
//  esi` the param @0x49b185, `test ebp, ebp` the sign @0x49b189, +16
//  @0x49b193, -16 @0x49b198, >= 0x200 -> 0x1FF @0x49b19b..0x49b1a9, <= 0 -> 1
//  @0x49b1b0..0x49b1b4); the sign Input_DequeueEvent @0x497f7d (outFlags =
//  id >> 31) from Input_ProcessKeyboardEvents @0x49d452..0x49d483 and
//  Input_TryDispatchBindingByWeaponType @0x499180..0x4991a4]
int32_t player_look_mouse_scale(PlayerLookSettings &s, int32_t param_value, bool negative);

struct PlayerLookDelta { int32_t yaw = 0, pitch = 0; };
// Shared scaling before human-view or AbsorbPitch bounds are applied.
PlayerLookDelta player_look_delta(const PlayerLookSettings &, int32_t dx_px,
                                 int32_t dy_px, int32_t scoped_zoom);

// One frame of mouse pixels onto the look angles. `dx_px`/`dy_px` are cursor pixels
// in screen sense (+x right, +y down — the raw center-lock offset). `scoped_zoom` is
// the current zoom magnification when the scoped sensitivity reduction applies
// (> 1 divides), else 0 or 1. `prone` selects the +40 deg up-clamp.
void player_look_apply(int32_t &yaw_bam, int32_t &pitch_bam, const PlayerLookSettings &s,
                       int32_t dx_px, int32_t dy_px, int32_t scoped_zoom, bool prone);

// Held look/turn keys advance 0x1FFFFFF BAM per tick and clamp relative to
// body slope. [orig: Entity_ApplyFreeLookRotation @0x4ae090]
void player_look_keys(int32_t &yaw, int32_t &pitch, bool left, bool right,
    bool up, bool down, bool prone, int32_t body_pitch);

} // namespace opennova::world
