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

// The cursor's re-centre point, in the window's client pixels
// [orig: Input_PumpAndCenterCursor @0x7616D0 (SetCursorPos(client + (0x140, 0xF0)) @0x761706)].
inline constexpr int32_t kCursorCentreX = 0x140;
inline constexpr int32_t kCursorCentreY = 0xF0;

// The close of a pass-through screen (death.mnu's DEATH, stat.mnu's STAT): their
// activation raises the menu input pass-through, and only their deactivation, which
// the next screen's open dispatches, or the death screen's back clears it; the close
// navigates nowhere, so the flag holds through it. The update that closes the screen
// re-centres the cursor the menu left free, which writes the axis words as the
// cursor's client offset from the centre, then runs the pass-through input frame,
// whose axis read takes that offset whole (no share, no two-tap, no 4x): one
// update's axis input of (x - 0x140, y - 0xF0), which the look rows' head admits or
// drops as any axis input. `client_x` / `client_y` are the cursor's client-area
// position at the close; the next update's head pump re-centres again and
// overwrites the words, so only that one update reads it.
// [orig: UI_TeardownScene @0x54E650 (the UI_CloseMenuScreen call @0x54E659, the
//  g_MenuInputPassThrough test @0x54E686, the Input_ProcessFrame call @0x54E695;
//  its caller under a menu screen Game_ProcessMainFrame @0x5263F0, the call
//  @0x52667B); UI_CloseMenuScreen @0x54E5E0 (UIScene_ReturnToHistoryScreen(menu, 0)
//  @0x54E635, the Input_ResetAllState tail @0x54E63F); UIScene_ReturnToHistoryScreen
//  @0x63DFA0 (no navigation without pop_extra); Input_ResetAllState @0x497ED0 (the
//  jump @0x497F22); Input_PumpAndCenterCursor @0x7616D0 (the offset @0x761718..0x76172A);
//  Input_ProcessMouseAxisBindings @0x499680 (the read @0x4996BE); the raises
//  DeathScreen_UpdateUI @0x553150 (@0x55316E), the activate's call @0x54EFD8, and
//  StatScreen_ShowCallback @0x562840 (@0x562877 / @0x5628D6); the clears
//  DeathScreen_OnDeactivate @0x553510 (UI_DispatchScreenEvent @0x54E6A0's event 6,
//  @0x54F29B), StatScreen_OnDeactivate @0x562120 (@0x54F267) and DeathScreen_OnHiddenBack
//  @0x553660 (@0x55366C); CUIScene_SelectNodeByName @0x63B6B0 (the deactivate of the
//  current screen @0x63B787, from UI_OpenMenuScreen @0x54E520's call @0x54E599); the
//  death screen's close request Render_ProcessMainSceneFrame @0x5CA0F0
//  (Game_CloseInGameScreens @0x5CAC9C)]
inline void pass_through_close_axis(int32_t client_x, int32_t client_y, int32_t (&axis)[2]) {
	axis[0] = client_x - kCursorCentreX;
	axis[1] = client_y - kCursorCentreY;
}

// Held look/turn keys advance 0x1FFFFFF BAM per tick and clamp relative to
// body slope. [orig: Entity_ApplyFreeLookRotation @0x4ae090]
void player_look_keys(int32_t &yaw, int32_t &pitch, bool left, bool right,
    bool up, bool down, bool prone, int32_t body_pitch);

} // namespace opennova::world
