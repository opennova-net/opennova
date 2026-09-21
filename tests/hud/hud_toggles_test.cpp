// The HUD's key-driven toggles (hud/hud_toggles.h), pinned where they used to
// live in the Godot shell's HUD presenter and panel lanes (ADR 0040 ladder
// E3): the gated down-edge latch, the huddetail/hudcolor shared-key
// shadowing (D-CTRL-4), the cycles and their wraps, the view actions' gun bit
// and camera preference, the three overlay window toggles with ShowScore's
// SP-only gate and sibling close, the respawn reset, the death-screen force
// and the friendly-tags cycle with its toast keys.
#include <runtime/hud/hud_toggles.h>

#include <cstdio>
#include <cstring>

using namespace opennova::hud;
using namespace opennova::hud::hud_toggle_event;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

HudKeyPoll keys() {
	HudKeyPoll k;
	k.active = true;
	return k;
}

void test_edge_latch() {
	HudKeyEdge e;
	CHECK(e.step(true, true, false));   // the press edge fires
	CHECK(!e.step(true, true, false));  // a held key does not re-fire
	CHECK(!e.step(false, true, false));
	CHECK(!e.step(true, true, true));   // a chorded press never fires ...
	CHECK(!e.step(true, true, false));  // ... and the latch followed the ungated key
	CHECK(!e.step(false, true, false));
	CHECK(!e.step(true, false, false)); // inactive input: ignored, latch set
	CHECK(!e.step(true, true, false));  // so reopening the gate cannot re-fire it
	CHECK(!e.step(false, true, false));
	CHECK(e.step(true, true, false));
	e.reset();
	CHECK(e.step(true, true, false)); // reset forgets the held key
}

void test_huddetail_cycle_and_shared_key_shadowing() {
	HudToggleState s;
	s.hud_detail_level = 2;
	s.hud_color_index = 5;
	HudKeyPoll k = keys();
	k.huddetail = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_detail_level == 3);
	k.huddetail = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_detail_level == 0); // 3 wraps to 0
	// Both rows down on a shared key: huddetail (row 50) consumes the edge and
	// hudcolor stays dormant; the hudcolor latch follows the shadowed state.
	k.huddetail = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	k.hudcolor = true;
	k.rows_share_key = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_color_index == 5);
	k.huddetail = false;
	k.hudcolor = false;
	hud_toggles_poll(s, k);
	// hudcolor alone (Ctrl+F6) cycles and wraps 5 -> 0.
	k.hudcolor = true;
	CHECK(hud_toggles_poll(s, k) == kHudColorCycled);
	CHECK(s.hud_color_index == 0);
	// Distinct keys leave both rows live.
	k.huddetail = false;
	k.hudcolor = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	k.hudcolor = true;
	k.rows_share_key = false;
	CHECK(hud_toggles_poll(s, k) == (kHudDetailCycled | kHudColorCycled));
	CHECK(s.hud_detail_level == 2 && s.hud_color_index == 1);
}

void test_showhud_goals_dotsize_and_view_actions() {
	HudToggleState s;
	CHECK(s.showhud_flags == 3);
	HudKeyPoll k = keys();
	k.showhud = true;
	CHECK(hud_toggles_poll(s, k) == kShowHudCycled);
	CHECK(s.showhud_flags == 0); // (3 + 1) & 3
	k.showhud = false;
	k.dotsize = true;
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) ==
			(kDotsizeCycled | kObjectivesToggled | kOverlayWindowsCleared));
	CHECK(s.objectives_visible);
	k.dotsize = false;
	k.goals = false;
	hud_toggles_poll(s, k);
	// view1st clears the gun bit and selects first person; viewwithgun sets it.
	s.showhud_flags = 3;
	k.view1st = true;
	CHECK(hud_toggles_poll(s, k) == (kGunBitChanged | kFirstPersonSelected));
	CHECK(s.showhud_flags == 2);
	k.view1st = false;
	k.viewwithgun = true;
	CHECK(hud_toggles_poll(s, k) == (kGunBitChanged | kFirstPersonSelected));
	CHECK(s.showhud_flags == 3);
	k.viewwithgun = false;
	k.viewchase = true;
	CHECK(hud_toggles_poll(s, k) == kThirdPersonSelected);
	CHECK(s.showhud_flags == 3); // chase never touches the gun bit
}

// The overlay windows keep ONE up: every window action runs the respawn init
// through the keeping wrapper [orig: Game_InitRespawnState @0x499360;
// Game_InitRespawnStateKeepingToggle @0x4993c0], which closes the others and
// orders the sim's map overlay closed; the player list is closed by the
// others only for a session peer [orig: @0x4993a4..0x4993ae].
void test_overlay_windows() {
	HudToggleState s;
	HudKeyPoll k = keys();
	// Out of a session (SP): Tab and J on one frame — Tab's open edge runs the
	// init first, then J's flip closes nothing but the (SP-safe) player list
	// survives J's init.
	k.playerlist = true;
	k.old_messages = true;
	CHECK(hud_toggles_poll(s, k) ==
			(kScoreboardToggled | kMessageLogToggled | kOverlayWindowsCleared));
	CHECK(s.scoreboard_open && s.message_log_open);
	k.playerlist = false;
	k.old_messages = false;
	hud_toggles_poll(s, k);
	// G (objectives) closes J's message log and keeps the SP player list.
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) == (kObjectivesToggled | kOverlayWindowsCleared));
	CHECK(s.objectives_visible && !s.message_log_open && s.scoreboard_open);
	k.goals = false;
	hud_toggles_poll(s, k);
	// ShowScore is SP-only: in a session the press is swallowed (latch set).
	k.show_score = true;
	k.in_session = true;
	CHECK(hud_toggles_poll(s, k) == 0);
	CHECK(!s.end_round_stats_open && s.objectives_visible);
	k.show_score = false;
	hud_toggles_poll(s, k);
	// Out of a session it opens and the respawn-init wrapper closes the
	// objectives panel beside it.
	k.show_score = true;
	k.in_session = false;
	CHECK(hud_toggles_poll(s, k) == (kShowScoreToggled | kOverlayWindowsCleared));
	CHECK(s.end_round_stats_open && !s.objectives_visible && s.scoreboard_open);
	k.show_score = false;
	hud_toggles_poll(s, k);
	// In a session (a peer): J closes the Show Score panel AND the player
	// list; Tab's open edge then closes J's log; Tab's close edge clears only
	// the list.
	k.in_session = true;
	k.old_messages = true;
	CHECK(hud_toggles_poll(s, k) == (kMessageLogToggled | kOverlayWindowsCleared));
	CHECK(s.message_log_open && !s.end_round_stats_open && !s.scoreboard_open);
	k.old_messages = false;
	hud_toggles_poll(s, k);
	k.playerlist = true;
	CHECK(hud_toggles_poll(s, k) == (kScoreboardToggled | kOverlayWindowsCleared));
	CHECK(s.scoreboard_open && !s.message_log_open);
	k.playerlist = false;
	hud_toggles_poll(s, k);
	k.old_messages = true;
	hud_toggles_poll(s, k); // J closes the list (peer) and opens the log
	k.old_messages = false;
	hud_toggles_poll(s, k);
	k.playerlist = true;
	hud_toggles_poll(s, k); // Tab open edge closes the log
	k.playerlist = false;
	hud_toggles_poll(s, k);
	s.message_log_open = true; // a log opened by other means ...
	k.playerlist = true;
	CHECK(hud_toggles_poll(s, k) == kScoreboardToggled); // ... survives Tab's close edge
	CHECK(!s.scoreboard_open && s.message_log_open);
	k.playerlist = false;
	hud_toggles_poll(s, k);
	// The mission teardown clears the four windows and their latches; the
	// color, detail, showhud and friendly-tag globals and the HUD-row latches
	// survive.
	s.hud_color_index = 4;
	s.hud_detail_level = 2;
	k.playerlist = true;
	hud_toggles_poll(s, k);
	s.objectives_visible = true;
	s.huddetail.was_down = true;
	hud_toggles_reset_mission(s);
	CHECK(!s.scoreboard_open && !s.message_log_open && !s.end_round_stats_open &&
			!s.objectives_visible);
	CHECK(s.hud_color_index == 4 && s.hud_detail_level == 2);
	CHECK(!s.show_score.was_down && !s.playerlist.was_down && s.huddetail.was_down);
}

void test_death_screen_and_friendly_tags() {
	HudToggleState s;
	s.hud_detail_level = 1;
	hud_toggles_death_screen(s);
	CHECK(s.hud_detail_level == 3);
	CHECK(s.friendly_tag_mode == FriendlyTagMode::kFull);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_BRIEF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_OFF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_FARBRIEF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_FULL") == 0);
	CHECK(s.friendly_tag_mode == FriendlyTagMode::kFull);
}

} // namespace

int main() {
	test_edge_latch();
	test_huddetail_cycle_and_shared_key_shadowing();
	test_showhud_goals_dotsize_and_view_actions();
	test_overlay_windows();
	test_death_screen_and_friendly_tags();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_toggles_test OK\n");
	return 0;
}
