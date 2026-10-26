#pragma once

// THE SPECIAL-KEY CHAIN: one dequeued key-down's walk through retail's special-key handler,
// which the keyboard pass runs ahead of the binding scan for every key an open chat line
// does not take (controls::run_keyboard_pass carries the pass's order). One else-if chain
// over the HUD's words and this role's facts: the quit dialog's keys; else the held-USE
// seat digits, else the open Emotes menu's digits, else the open Radio menu's; else, once
// the round is over, the round-over keys; else, in a session, the Tab board's page keys and
// then the status page's. A key an arm whose word holds does not take (a non-digit under
// the USE hold or an open menu, any key the open quit dialog leaves, any key the session
// board arm leaves) goes on to the tail: the deploy keys, then the help and briefing pages.
// The embedder runs the outcome's device half: the digit arm's pick at once, the Exit
// Mission of the dialog's YES, the restart's splash re-run, the input event 12 queued in
// key order with the binding rows' events, the page steps, and a taken key's skip of the
// binding scan.
// [orig: Input_HandleSpecialKeys @0x49c5c0 - the quit dialog @0x49c5e6..0x49c6d1, the
//  held-USE arm @0x49c6d8, the Emotes arm @0x49c737, the Radio arm @0x49c783, the round-over
//  arm @0x49c7cf, the session board and status arms @0x49c8fd..0x49c9c7, the deploy tail
//  @0x49c9c9..0x49ca97, the help and briefing pages @0x49ca9e..0x49cb53, `return 0`
//  @0x49c602; the caller Input_ProcessKeyboardEvents @0x49d1f0 - the call @0x49d2fb, a
//  take's skip of the binding scan @0x49d305]

#include <runtime/controls/player_actions.h> // PlayerActions, PlayerActionRequest
#include <runtime/hud/hud_toggles.h>         // HudToggleState

#include <cstdint>
#include <optional>

namespace opennova::world {
struct SpawnZoneRegistry;
}

namespace opennova::inmatch {

struct RoleView;
class Role;

// One key-down as the chain reads it: its VK, the deploy keys' VK (the embedder's
// label read), and the KeyPress gametext keys' first characters (hud::HudQuitDialogKeyInput
// carries their witness).
struct SpecialKeyInput {
	int vk = 0;
	int deploy_vk = 0;
	int yes_vk = 'Y';
	int no_vk = 'N';
	int restart_vk = 'R';
};

// The page a taken PgUp / PgDn steps (the embedder's device state): the Tab board's
// (g_ScoreboardPanelPage), the F1 help's (HelpScreen_CyclePage) or the briefing's
// (HUD_CycleBriefingPage).
enum class SpecialKeyPage : uint8_t { None, Scoreboard, Help, Briefing };

struct SpecialKeyOutcome {
	// The chain returned 1: the key reaches no binding row.
	bool taken = false;
	// The quit dialog's YES ran input action 3 (Exit Mission).
	bool quit_confirmed = false;
	// The round-over RESTART out of a session: the splash re-run, which the embedder
	// shows over a custom loading background (g_LoadScreenHasCustomBg).
	bool restart_splash = false;
	// Input_QueueEvent(12, pick): -1 none; 0 the default spawn (a deploy key's X, the quit
	// dialog's RESTART out of a session, the round-over RESTART), 0xFFFE the auto team,
	// else a spawn zone's letter index + 1.
	int pick = -1;
	// The held-USE seat pick or the open menu's pick, which the embedder runs at once (the
	// chain has already closed the menu).
	std::optional<controls::PlayerActionRequest> digit_pick;
	SpecialKeyPage page = SpecialKeyPage::None;
	bool page_forward = false; // PgDn (VK_NEXT); PgUp steps back
};

// One key through the chain over this role (its view, its exit reasons), the HUD's words
// (the dialog's close and a menu pick's close are stored here), the player's action latches
// (the USE hold and its consumed word) and the spawn-zone list. A view with no kernel takes
// nothing.
SpecialKeyOutcome special_key_chain(const RoleView &view, Role &role, hud::HudToggleState &toggles,
		controls::PlayerActions &actions, const world::SpawnZoneRegistry &zones,
		const SpecialKeyInput &input);

} // namespace opennova::inmatch
