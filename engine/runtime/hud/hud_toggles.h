#pragma once

// The HUD's key-driven toggles and cycles: the retail globals the action
// dispatch flips (the declutter level, the color scheme, the showhud flags,
// the friendly-tag mode, the objectives panel, the three overlay windows)
// and the per-frame edge polls that flip them. One state, one poll; the
// embedder samples the bound keys, hands them in, and applies the device
// side effects the returned events name (the overlay restamps, the config
// write, the toast, the FP gun bit, the camera preference).
// [orig: the first-match key scan @0x49d42f; Input_HandleActionBinding /
//  _0 dispatch cases @0x4E0601 (huddetail), @0x49afc7 (hudcolor), @0x4E0561
//  (showhud), @0x49b68b (goals), @0x49c073 / @0x49c0d9 / @0x49c0f6 (the view
//  actions), @0x49bb68 (playerlist), @0x49b55a (OldMessages), @0x49bd29
//  (ShowScore), @0x49b573 (friendly tags); the respawn init
//  Game_InitRespawnState @0x499360 the window actions run through the
//  keeping wrapper @0x4993c0 clears @0x499381 / @0x499395 / @0x49939a /
//  @0x49939f and, for a session peer, @0x4993ae]

#include <runtime/hud/hud_config_tokens.h>
#include <runtime/hud/hud_math.h>

#include <cstdint>

namespace opennova::hud {

// One action row's down-edge latch with the witnessed gate: the row fires
// on the press edge while gameplay input is active and no Shift/Alt chord is
// held (our debug picks ride Shift+F6; Ctrl is a binding modifier the sampler
// already resolved). The latch follows the UNGATED key state, so a press held
// across an armory or F3 window cannot re-fire when the gate reopens.
struct HudKeyEdge {
	bool was_down = false;
	// Advances the latch; true on the gated press edge.
	bool step(bool down, bool active, bool chorded);
	void reset() { was_down = false; }
};

// The toggles' shared state — the retail globals.
struct HudToggleState {
	int hud_color_index = kHudColorIndexDefault;      // [orig: g_hudActiveColor's index]
	int hud_detail_level = kHudDetailLevelDefault;    // the LIVE declutter level
	uint32_t showhud_flags = kShowHudFlagsDefault;    // [orig: g_FpWeaponViewFlags]
	FriendlyTagMode friendly_tag_mode = kFriendlyTagModeDefault; // [orig: g_friendlyTagsMode]
	bool objectives_visible = false;  // [orig: dword_24C18CC, toggled 0 <-> 0xFF]
	bool scoreboard_open = false;     // [orig: g_scoreboardPanelVisible]
	bool message_log_open = false;    // [orig: g_showMessageLog @0x24C18C0]
	bool end_round_stats_open = false; // [orig: dword_24C18AC]

	HudKeyEdge huddetail, hudcolor, showhud, dotsize, goals;
	HudKeyEdge view1st, viewwithgun, viewchase;
	HudKeyEdge playerlist, old_messages, show_score;
};

// One frame's sampled key states for the poll: each bound row's down state,
// whether the huddetail and hudcolor rows currently resolve to a common key
// (retail's scan is FIRST-MATCH-WINS by catalog row, huddetail 50 before
// hudcolor 76, so a shared key lets huddetail consume the edge — the D-CTRL-4
// adjudication), the chord and gate bits, and whether an MP session is live
// (the ShowScore toggle is SP-only [orig: the !is_in_session gate @0x49bd29];
// the respawn init closes the player list only for a session peer
// [orig: is_in_session && is_mp_session_peer @0x4993a4..0x4993ae] — every
// HUD-bearing process in a session is that peer).
struct HudKeyPoll {
	bool huddetail = false;
	bool hudcolor = false;
	bool rows_share_key = false;
	bool showhud = false;
	bool dotsize = false;
	bool goals = false;
	bool view1st = false;
	bool viewwithgun = false;
	bool viewchase = false;
	bool playerlist = false;
	bool old_messages = false;
	bool show_score = false;
	bool chorded = false;
	bool active = false;
	bool in_session = false;
};

// What a poll flipped, for the embedder's device side effects.
namespace hud_toggle_event {
inline constexpr uint32_t kHudDetailCycled = 0x1;   // restamp the overlay's live level
inline constexpr uint32_t kHudColorCycled = 0x2;    // restamp the overlay; write the config token
inline constexpr uint32_t kShowHudCycled = 0x4;     // restamp the overlay flags; the FP gun bit
inline constexpr uint32_t kDotsizeCycled = 0x8;     // the overlay's sight-scale cycle
inline constexpr uint32_t kObjectivesToggled = 0x10;
inline constexpr uint32_t kGunBitChanged = 0x20;    // a view action rewrote showhud bit 0
inline constexpr uint32_t kFirstPersonSelected = 0x40;
inline constexpr uint32_t kThirdPersonSelected = 0x80;
inline constexpr uint32_t kScoreboardToggled = 0x100;
inline constexpr uint32_t kMessageLogToggled = 0x200;
inline constexpr uint32_t kShowScoreToggled = 0x400;
// A window action ran the respawn init: the embedder closes the sim's map
// overlay mode beside the windows this state already cleared
// [orig: g_mapOverlayMode = 0 @0x499395].
inline constexpr uint32_t kOverlayWindowsCleared = 0x800;
} // namespace hud_toggle_event

// One frame's poll: advances every latch, applies the cycles and toggles to
// the state, returns the hud_toggle_event bits that fired.
uint32_t hud_toggles_poll(HudToggleState &state, const HudKeyPoll &keys);

// The mission teardown clears the four overlay windows and their latches,
// keeping the color, detail, showhud and friendly-tag globals.
void hud_toggles_reset_mission(HudToggleState &state);

// The death-screen edge forces the declutter level to the blank level through
// the same live seam the cycle uses [orig: NapiNPClientMsg_0x00F
// @0x42E410..0x42E41C — level = 3, then the visibility rebuild].
void hud_toggles_death_screen(HudToggleState &state);

// The friendly-tags cycle 0->1->2->3->0 with its retail toast key (gametext
// Misc/STRMISC_FRIENDLYTAGS_*) [orig: Input_HandleActionBinding case 30
// @0x49b573 -> Chat_AddDebugMessage @0x49bc60; the keys @0x49b596 /
// @0x49b5c1 / @0x49b5d0 / @0x49b5da]. Returns the toast key for the new mode.
const char *hud_toggles_cycle_friendly_tags(HudToggleState &state);
const char *friendly_tag_toast_key(FriendlyTagMode mode);

} // namespace opennova::hud
