#pragma once

#include <cstdint>
#include <string>

// The standard-weapon SIGHTS card's per-row draw modes and the sight-scale
// cycle [orig: draw_weapon_sight_overlays @0x4dce00]. Each authored weapon.def
// SIGHTS row (36 B at WeaponDef+0x1C8, stride 0x24 @0x4dce57: x1/y1/x2/y2 at
// +8..+0x14, the slide frame count +0x18, the `scale` flag +0x1C, the `slide`
// flag +0x20 [orig: WeaponDefs_ParseLineCallback `sights` @0x544b77..0x544bcc])
// draws in one of three modes, resolved here in the 1024x768 design space
// before the per-corner Viewport_ScaleToVirtualCoords pass the embedder
// applies (hud_math.h scale_axis). The device leg (the shell's card rows or
// the compiler's sights element) only rasterises the rectangle this returns.

namespace opennova::hud {

// Localized optical readouts use 32-bit integer conversions, including
// Win32 %ld/%li/%lu. Preserve %% and unsupported conversions literally.
std::string sight_integer_text(const std::string &format, int32_t value);

// ---------------------------------------------------------------------------
// The per-player sight-scale index (retail dword_B76780): selects which of
// the three SCALED-row sizes draw — (index + 2) / 8 of the authored extent
// each side of the row centre, so half, three-quarter and full box for
// 0 / 1 / 2. Default 1 at player init [orig: Player_InitPlayer `mov esi,1`
// @0x4e160f, `mov dword_B76780,esi` @0x4e178c]; the `dotsize` action (catalog
// row 38, dispatch code 216, default key O) adds 1 and keeps the sum while it
// is (signed) below 3, else stores 0 [orig: Input_HandleActionBinding_0 case
// 216 @0x4e0c31..0x4e0c46: `add eax,ebx` (ebx = 1), `cmp eax,3`, `jl` keep].

inline constexpr int kSightScaleIndexDefault = 1;
inline constexpr int kSightScaleIndexCount = 3;
int next_sight_scale_index(int index);

// The scaled-mode half-extent of one axis: ((x2 - x1) * (index + 2)) >> 3
// [orig: `mov eax,dword_B76780; add eax,2; imul; sar 3` @0x4dcead..0x4dcecc
//  (x) and @0x4dceef..0x4dcf21 (y)].
int32_t sight_scaled_half_extent(int32_t extent, int sight_scale_index);

// ---------------------------------------------------------------------------
// One row as the drawer reads it (DefSightEntry's rect and flags).

struct SightRowSpec {
	int32_t x1 = 0;
	int32_t y1 = 0;
	int32_t x2 = 0;
	int32_t y2 = 0;
	bool scale = false;       // row+0x1C: the `scale` token
	bool slide = false;       // row+0x20: the `slide` token
	int32_t slide_frames = 0; // row+0x18: the `slide` token's frame count
};

// The resolved corners, design space (x1, y1) .. (x2, y2).
struct SightRect {
	int32_t x1 = 0;
	int32_t y1 = 0;
	int32_t x2 = 0;
	int32_t y2 = 0;
};

// The row's draw rectangle. SCALED (row+0x1C): half-extents from
// sight_scaled_half_extent around the centre ((x1 + x2) >> 1, (y1 + y2) >> 1)
// [orig: the gate @0x4dce8d; the centre `sar 1` pair @0x4dcec4/@0x4dceca; the
//  corners @0x4dcecf..0x4dcf2c]. SLIDE (row+0x20): slide_frames * the
// scope-zero multiplier (sight_slide_multiplier) added to BOTH y1 and y2, x
// untouched [orig: the gate @0x4dcf42; `imul edi,[esi+18h]` @0x4dcfac /
//  @0x4dcff4 / @0x4dcffc; the y1/y2 adds @0x4dd01b/@0x4dd049]. Otherwise the
// plain authored rect [orig: @0x4dd050..0x4dd09f]. A row flagged both draws
// scaled: the scale test runs first.
SightRect sight_row_rect(const SightRowSpec &row, int sight_scale_index,
		int32_t slide_multiplier);

// Pixel-space corners after viewport rounding and the card's Y correction.
struct SightViewportRect {
	float x1 = 0.0f;
	float y1 = 0.0f;
	float x2 = 0.0f;
	float y2 = 0.0f;
};

// The card scales Y about half the viewport height by 3/(4*selected_ratio),
// after both corners pass the virtual-coordinate scaler. Other modes use H/W.
// [orig: draw_weapon_sight_overlays @0x4dd0ad..0x4dd0f7;
// Render_SetAspectRatioMode @0x58d8c9..0x58d8d9 native mode]
SightViewportRect sight_rect_to_viewport(const SightRect &rect, float width, float height, int aspect_mode = -1);

// ---------------------------------------------------------------------------
// The SLIDE multiplier — retail's scope-zero system as the drawer samples it
// [orig: @0x4dcf4c..0x4dcffc]. The parsed scope_max_zero fields supply the
// default-zero path; the native player supplies manual zero and range. With every def field
// and the zero word at 0, the multiplier is 0, the plain rect.

struct ScopeZeroInputs {
	// MountSlot+0x60, the equipped slot's scope-zero word (IDA's
	// `fireInterval`): 0 = the def's default zero, -1 = the rangefinder arm
	// (Weapon_GetScopeZoomLevel @0x422ff1 keys the same value), any other
	// signed value = that many zero steps [orig: `movzx ecx,word ptr
	// [ebx+60h]` @0x4dcf4c].
	int16_t slot_zero_word = 0;
	// The three atol'd tokens of the weapon.def `scope_max_zero` key
	// [orig: WeaponDefs_ParseLineCallback @0x544e8b..0x544ed9]: token 1 ->
	// WeaponDef+0x84 (the step cap), token 2 -> +0x9C (metres per zero
	// step), token 3 -> +0xA0 (the default zero distance, metres).
	int32_t scope_max_zero_steps = 0; // WeaponDef+0x84
	int32_t scope_zero_step = 0;      // WeaponDef+0x9C
	int32_t scope_zero_default = 0;   // WeaponDef+0xA0
	// dword_B76808, Q16 world units: the aim ray's hit distance from the
	// player body, restamped every sixteen body ticks [orig: Entity_UpdateInfantryPlayerBody
	// @0x4b5056..0x4b50ad], seeded with WeaponDef+0xA0 when a Flags &
	// 0x20000000 weapon mounts [orig: Player_MountWeaponSlot @0x4dfb3b..0x4dfb44].
	int32_t rangefinder_q16 = 0;
	// byte_24D217C: read here and by Score_GetMultiplierValue @0x4fc447 (a
	// set byte zeroes every score multiplier); no writer in the image.
	bool scoring_disabled = false;
};

// (WeaponDef+0xA0 << 16) / this = the default zero in 25 m units: the
// divisor is 25 << 16 [orig: the `imul 51EB851Fh; sar edx,13h` magic
// division @0x4dcf75..0x4dcf84].
inline constexpr int32_t kScopeZeroDefaultDivisor = 1638400;
// The metres each Weapon_GetScopeZoomLevel default step spans [orig: `imul
// eax,19h` @0x4dcf8d].
inline constexpr int32_t kScopeZeroDefaultStepMetres = 25;

// zero word 0 with a non-zero +0xA0 and scoring enabled: Weapon_GetScopeZoomLevel(0,
// (+0xA0 << 16) / 1638400) — which returns its default untouched with
// weaponActive 0 [orig: @0x422fd1..0x422fd5] — times 25, divided by +0x9C
// [orig: @0x4dcf57..0x4dcfac]; scoring disabled short-circuits to 0
// [orig: `cmp byte_24D217C,cl; jnz` @0x4dcf64 with edi = 0]. zero word -1:
// rangefinder_q16 / (+0x9C << 16), negative -> 0, else capped at +0x84
// [orig: @0x4dcfb7..0x4dcff7]. Otherwise the sign-extended word itself
// [orig: `movsx edi,cx` @0x4dcff9]. A zero +0x9C divisor faults retail's
// `idiv`; no shipped row authors one, and the port answers 0 there.
int32_t sight_slide_multiplier(const ScopeZeroInputs &in);

} // namespace opennova::hud
