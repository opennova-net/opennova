// The mouse-look pipeline: the cursor offset -> the main frame's pump
// (MouseAxisPump: four times the offset spread over the frame's updates, each
// summed with the previous share) -> the axis words -> entity Yaw/Pitch (BAM32),
// the faithful port of the engine's per-frame mouse pump + axis-binding apply.
//
// [orig: Input_ProcessMouseAxisBindings @ 0x499680: the deltas it scales are the
//  pump's per-update sums (g_MouseState.deltaX / deltaY as Game_ProcessMainFrame
//  rewrites them, MouseAxisPump below), over the center-lock cursor offset
//  (Input_PumpAndCenterCursor @ 0x7616D0 pins the cursor at 320,240 and reads the
//  offset); sensitivity = setting << 11 (profile +0x590, default 128
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

// One update's mouse axis words onto the look angles. `dx_px`/`dy_px` are in screen
// sense (+x right, +y down): the pump's sum for the update. `scoped_zoom` is
// the current zoom magnification when the scoped sensitivity reduction applies
// (> 1 divides), else 0 or 1. `prone` selects the +40 deg up-clamp.
void player_look_apply(int32_t &yaw_bam, int32_t &pitch_bam, const PlayerLookSettings &s,
                       int32_t dx_px, int32_t dy_px, int32_t scoped_zoom, bool prone);

// The main frame's mouse pump, between the cursor and the axis bindings: the
// drain's first update reads the cursor's offset since its last centering,
// takes four times it and spreads that over the updates the drain runs (the
// share (n / 2 + 4 * offset) / n on each, the remainder 4 * offset - share *
// (n - 1) on the last), and every update hands the axis bindings its share
// plus the previous update's, a two-tap sum: one cursor pixel feeds the axis
// scale eight times over two updates. A menu screen up at the first update
// zeroes the frame's shares. Retail's words are process words that only the
// main frame writes, never reset; ours live with the session (Session::
// mouse_pump_), so a new mission's first update starts from zero where retail's
// would carry the last frame's shares. Every stock mission end passes a menu
// screen whose first update zeroes them, a disconnect's exit being the one path
// without one.
// [orig: Game_ProcessMainFrame @0x5263F0 -- the first update's read (the
//  g_MainLoopFirstUpdate test @0x52640E, the Input_PumpAndCenterCursor call
//  @0x526458), the menu's zeroes @0x52642F..0x52644D, the share and the
//  remainder @0x52645D..0x5264A9 (the divisor g_MainLoopUpdatesDue; x's share
//  @0x526481, its remainder @0x526491), the last update's remainder
//  @0x5264B3..0x5264D4 (dword_24E0E80), the share @0x5264DB, the two-tap sum
//  into g_MouseState.deltaX / deltaY @0x5264E6..0x526508, which
//  Input_ProcessMouseAxisBindings @0x499680 scales (the read @0x4996BE;
//  player_look_delta); Input_PumpAndCenterCursor @0x7616D0]
struct MouseAxisPump {
	int32_t share[2] = {};     // g_MouseUpdateShareX / Y: this update's
	int32_t remainder[2] = {}; // g_MouseLastShareX / Y: the last update's
	int32_t previous[2] = {};  // g_MousePrevShareX / Y: the two-tap's other half
	// One update's axis input (x, y) into `axis`; `cursor` is read on the
	// first update only. `updates_due` >= 1.
	void step(bool first_update, bool last_update, int32_t updates_due, bool menu_open,
			const int32_t (&cursor)[2], int32_t (&axis)[2]);
};

// Held look/turn keys advance 0x1FFFFFF BAM per tick and clamp relative to
// body slope. [orig: Entity_ApplyFreeLookRotation @0x4ae090]
void player_look_keys(int32_t &yaw, int32_t &pitch, bool left, bool right,
    bool up, bool down, bool prone, int32_t body_pitch);

} // namespace opennova::world
