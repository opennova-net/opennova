#pragma once

// THE TIP SYSTEM ("MrClippy"): the one tip retail shows at a time, the
// frame countdown that fades it, the five once-counters that keep the
// repeatable tips from nagging, the option gates, and the text the draw
// expands. Producers raise EVENTS (vehicle boarding, the scope toggle, the
// NVG and binocular actions, the spectator begin); the event table maps each
// to a tip, a fade or nothing. Esc starts the fade.
// [orig: g_TipSystem (ex dword_28E1AD0, a CTipSystem) — CTipSystem_HandleEvent
//  @0x5b6ad0, CTipSystem_ShowTip @0x5b6a60 (ex sub_5B6A60), CTipSystem_TickCountdown
//  @0x5b69f0 (Game_ProcessMainFrame @0x52675d, every main frame, paused or
//  not), CTipSystem_IsShowing @0x5b6c60 (ex CGameWorld_Shutdown),
//  CTipSystem_BeginFade @0x5b6c70, CTipSystem_Reset @0x5b6940 (ex sub_5B6940,
//  Game_StartMission @0x525ded), CTipSystem_Draw @0x5b6d60,
//  TextResource_ExpandMacroVariables @0x5b6c80,
//  KeyBinding_GetDisplayStringByActionName @0x5b6a00]

#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova::hud {

// The tip ids the draw names [orig: CTipSystem_Draw's switch @0x5b6e07].
enum TipId : int32_t {
	kTipNone = 0,
	kTipGround = 2,              // KB_GROUND
	kTipGroundHorn = 3,          // KB_GROUNDH
	kTipHelo = 4,                // KB_HELO
	kTipBoat = 5,                // KB_BOAT
	kTipEmplaced = 6,            // KB_EMPLACED
	kTipNvg = 7,                 // KB_NVG
	kTipScopeElevation = 8,      // KB_SCOPEELEVATION
	kTipSpectatorBegin = 9,      // KB_SPECTATORBEGIN
	kTipAasBegin = 12,           // GP_AAS_BEGIN (no event raises it)
	kTipAasLostOurCamp = 13,     // GP_AAS_LOSTOURCAMP (no event raises it)
	kTipAasGainedOurCamp = 14,   // GP_AAS_GAINEDOURCAMP (no event raises it)
	kTipScopeUseBinoc = 15,      // GP_SCOPE_USEBINOC
	kTipBinocRange = 16,         // GP_BINOC_RANGE
	kTipMortarUseDesignator = 17, // GP_MORTAR_USEDESIGNATOR
	kTipDesignatorHelpMortar = 18, // GP_DESIGNATOR_HELPMORTAR
};

// The event ids the producers raise [orig: the CTipSystem_HandleEvent call
// sites; the event switch @0x5b6adc].
enum TipEvent : int {
	kTipEventBoardGround = 1,      // a control seat of a land vehicle
	kTipEventBoardGroundHorn = 2,  // ... whose profile carries the horn slot
	kTipEventBoardHelo = 3,        // ... of unit_type 3
	kTipEventBoardBoat = 4,        // ... of unit_type 5..8
	kTipEventBoardEmplaced = 5,    // the UseGun seat
	kTipEventDetach = 6,           // fade
	kTipEventNvgOn = 7,
	kTipEventNvgOff = 8,           // fade
	kTipEventBinocularsOn = 9,
	kTipEventBinocularsOff = 10,   // fade
	kTipEventScopeElevationOn = 11,
	kTipEventScopeElevationOff = 12, // fade
	kTipEventDesignatorWeaponOn = 13,
	kTipEventDesignatorWeaponOff = 14, // fade
	kTipEventDesignatorOn = 15,
	kTipEventDesignatorOff = 16,   // fade
	// 17..21: raised by the game-event handler, no-ops in the switch.
	kTipEventSpectatorBegin = 22,
};

// The two options (game.cfg enable_keyboardtips / enable_gameplaytips,
// default 1; the Options rows MR_CLIPPY_KEYBOARD / MR_CLIPPY_HINTS)
// [orig: g_GameConfigState.enableKeyboardTips_18C / enableGameplayTips_190 —
//  Config_SetDefaults @0x54d13b / @0x54d141, Config_ParseSettingsLine
//  @0x54fda1 / @0x54fdcc, Game_SaveConfig @0x54c6a8 / @0x54c6bd].
struct TipOptions {
	bool keyboard_tips = true;
	bool gameplay_tips = true;
};

// The state [orig: g_TipSystem +0 tip, +4 countdown, +0x2C..+0x3C the five
// once-counters]. The draw rect (+0xC..+0x18), the box and icon handles
// (+8, +0x1C..+0x28) and the hudpos slots (+0x40..+0x5C) are the draw's.
struct TipSystem {
	int32_t tip = kTipNone;
	int32_t countdown = 0;
	int32_t nvg_count = 0;          // +0x2C, event 7
	int32_t scope_count = 0;        // +0x30, event 11
	int32_t binoculars_count = 0;   // +0x34, event 9
	int32_t designator_weapon_count = 0; // +0x38, event 13
	int32_t designator_count = 0;   // +0x3C, event 15
	TipOptions options;
};

// The countdowns [orig: @0x5b6aad (620) / @0x5b6abc (1240) / the fade @0x5b6a94].
inline constexpr int32_t kTipKeyboardFrames = 620;
inline constexpr int32_t kTipGameplayFrames = 1240;
inline constexpr int32_t kTipFadeFrames = 64;

// Show one tip, or fade (tip 0): the fade clamps the countdown to 64; tips
// 1..10 need the keyboard option (620 frames), 11..19 the gameplay option
// (1240); a refused tip writes nothing [orig: CTipSystem_ShowTip @0x5b6a60].
void tip_show(TipSystem &tips, int32_t tip);
// One event [orig: CTipSystem_HandleEvent @0x5b6ad0]. A once-counter counts
// even when the option refused its tip (the increment follows the call).
void tip_handle_event(TipSystem &tips, int event);
// One main frame [orig: CTipSystem_TickCountdown @0x5b69f0].
void tip_tick_countdown(TipSystem &tips);
// [orig: CTipSystem_IsShowing @0x5b6c60 — countdown > 64]
bool tip_is_showing(const TipSystem &tips);
// [orig: CTipSystem_BeginFade @0x5b6c70 — countdown = 64]
void tip_begin_fade(TipSystem &tips);
// [orig: CTipSystem_Reset @0x5b6940 — the tip and the countdown; the counters
// too with the flag (Game_StartMission's arg: 0 a mission start, 1 the SP
// restart Game_RestartRoundSP @0x5263db)]
void tip_reset(TipSystem &tips, bool clear_counters);

// What the draw resolves for a tip: the Tips section's header key and body
// key, and which icon. False for every id the draw skips (0, 1, 10, 11 and
// 19+; ids outside the switch draw nothing) [orig: CTipSystem_Draw
// @0x5b6d9c..0x5b6e75 — 2..9 StrKBTip / k_tip, 12..18 StrGPTip / g_tip].
struct TipText {
	const char *header_key = nullptr;
	const char *body_key = nullptr;
	bool gameplay = false;
};
bool tip_text_keys(int32_t tip, TipText &out);

// The draw alpha [orig: @0x5b6d76..0x5b6d88 — 4 * countdown, capped 255].
int tip_alpha(int32_t countdown);

// The body's expansion [orig: TextResource_ExpandMacroVariables @0x5b6c80]:
// "\t" and "\n" (backslash pairs) become a tab and a newline; "$name$" is
// replaced by the display string of the binding whose token matches `name`
// (the lookup answers "???" on a miss [orig:
// KeyBinding_GetDisplayStringByActionName @0x5b6a34]); an unterminated
// "$name" swallows the rest of the text as the name. Every other byte copies.
using TipKeyDisplay = std::function<std::string(const std::string &token)>;
std::string tip_expand_macros(const std::string &text, const TipKeyDisplay &key_display);

// The draw's two strings for a tip: the Tips-section header (StrKBTip /
// StrGPTip) and the body, each the key itself on a miss, the body run
// through the expansion. False (both cleared) for an id the draw skips
// [orig: CTipSystem_Draw — GameText_GetStringWithFallback("Tips", key, key)
//  @0x5b6df4 / @0x5b6e75, TextResource_ExpandMacroVariables @0x5b6e8f].
bool tip_draw_text(int32_t tip, const GameTextLookup &gametext, const TipKeyDisplay &key_display,
		std::string &header, std::string &body);

} // namespace opennova::hud
