// The HUD session (hud/hud_session.h) and its role feeds (inmatch/hud_session_feeds.h), pinned
// where they used to live in the Godot shell's HUD presenter: the boot seed from the persisted
// words, the mission effects routed into the CHAT and SYSTEM rings (the objective line, the
// banner, the triggered text, the pending cap), the device work a toggle event word names, the
// declutter's re-seed and capture transaction, the menu's pause share, the per-mission caches
// and the map queries' retry throttle.
#include <formats/gamecfg/game_cfg.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_overlay_windows.h>
#include <runtime/hud/hud_session.h>
#include <runtime/inmatch/hud_session_feeds.h>
#include <runtime/world/player_view.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace opennova;
using namespace opennova::hud;
namespace ev = opennova::hud::hud_toggle_event;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A table of section/key -> text answering the fallback for anything else.
GameTextLookup table(std::map<std::string, std::string> rows) {
	return [rows](const char *section, const char *key, const char *fallback) {
		const auto it = rows.find(std::string(section) + "/" + key);
		return it != rows.end() ? it->second : std::string(fallback);
	};
}

HudMissionEffectRecord effect(HudMissionEffect kind, int a = 0, int b = 0, int c = 0,
		std::string text = {}) {
	HudMissionEffectRecord e;
	e.kind = kind;
	e.a = a;
	e.b = b;
	e.c = c;
	e.text = std::move(text);
	return e;
}

bool ring_has(const std::vector<HudMessageLine> &ring, const std::string &text) {
	for (const HudMessageLine &line : ring)
		if (line.text == text) return true;
	return false;
}

void seed() {
	HudSession s(99, 7);
	CHECK(s.toggles().hud_color_index == clamp_hud_color_index(99));
	CHECK(s.toggles().hud_detail_level == 7); // verbatim
	CHECK(s.hud_detail_config() == 7);
	gamecfg::GameCfg cfg;
	cfg.hud_color_index = 1;
	cfg.hud_detail = 2;
	HudSession from = HudSession::from_cfg(cfg);
	CHECK(from.toggles().hud_color_index == 1 && from.toggles().hud_detail_level == 2);
	from.toggles().hud_color_index = 4;
	from.write_hud_color_word(cfg);
	CHECK(cfg.hud_color_index == 4);
	CHECK(cfg.hud_detail == 2); // the declutter word is never written back
}

void effect_kinds() {
	CHECK(hud_mission_effect("lose") == HudMissionEffect::Lose);
	CHECK(hud_mission_effect("subgoal_lost") == HudMissionEffect::SubgoalLost);
	CHECK(hud_mission_effect("mission_text_chat") == HudMissionEffect::MissionTextChat);
	CHECK(hud_mission_effect("fx2ssn") == HudMissionEffect::None);
	world::Effect e;
	e.kind = "hud_item_flash";
	e.a = 3;
	e.b = 40;
	const HudMissionEffectRecord r = inmatch::hud_mission_effect_record(e);
	CHECK(r.kind == HudMissionEffect::HudItemFlash && r.a == 3 && r.b == 40);
}

void mission_effects() {
	HudSession s;
	const GameTextLookup gametext = table({{"Misc/STRMISC_LOSE", "{hot}Mission lost"},
			{"Misc/STRMISC_NEWOBJECTIVE", "New objective"}});
	const GameTextLookup mission = table({{"WinConditions/STRWINDIRECTIVE001", "Take the hill"},
			{"WinConditions/STRWINMSG002", "Hill taken"},
			{"LoseConditions/STRLOSEMSG003", "Hill lost"},
			{"Triggered Text/ID005", "Triggered five"}});
	const TextKeyLookup keys = [](const std::string &key) {
		return key == "RELAY" ? std::string("Relayed") : std::string();
	};
	HudEffectOverlayWork work;
	s.apply_mission_effect(effect(HudMissionEffect::Text, 0, 0, 0, "Go north"), gametext, mission,
			keys, work);
	CHECK(s.objective_line() == "Go north");
	s.apply_mission_effect(effect(HudMissionEffect::Text, 5), gametext, mission, keys, work);
	s.apply_mission_effect(effect(HudMissionEffect::DebugText, 0, 0, 0, "dbg"), gametext, mission,
			keys, work);
	s.apply_mission_effect(effect(HudMissionEffect::Lose, 0, 0, 0, "STRMISC_LOSE"), gametext,
			mission, keys, work);
	CHECK(s.banner_line() == "{hot}Mission lost"); // the entry's text as the table holds it
	s.apply_mission_effect(effect(HudMissionEffect::Objective, 0, 1, 1), gametext, mission, keys,
			work);
	s.apply_mission_effect(effect(HudMissionEffect::MissionTextChat, 0, 0, 0, "RELAY"), gametext,
			mission, keys, work);
	s.apply_mission_effect(effect(HudMissionEffect::MissionTextChat, 0, 0, 0, "NONE"), gametext,
			mission, keys, work);
	s.apply_mission_effect(effect(HudMissionEffect::SubgoalWon, 0, 2, 1), gametext, mission, keys,
			work);
	s.apply_mission_effect(effect(HudMissionEffect::SubgoalWon, 0, 2, 0), gametext, mission, keys,
			work); // the round is over: nothing
	CHECK(s.banner_line() == "{hot}Mission lost"); // a won subgoal leaves the banner
	s.apply_mission_effect(effect(HudMissionEffect::SubgoalLost, 0, 3, 1), gametext, mission, keys,
			work);
	CHECK(s.banner_line() == "Hill lost");
	s.apply_mission_effect(effect(HudMissionEffect::HudItemFlash, 2, 50), gametext, mission, keys,
			work);
	s.apply_mission_effect(effect(HudMissionEffect::LocalRoundReset), gametext, mission, keys,
			work);
	CHECK(work.reset_overlay_buffers);
	CHECK(work.item_flashes.size() == 1 && work.item_flashes[0].first == 2 &&
			work.item_flashes[0].second == 50);
	// Go north, Mission lost, New objective, Take the hill, Relayed, Hill taken, Hill lost:
	// seven chat lines; the triggered id and the debug line ride the SYSTEM ring.
	CHECK(s.pending_messages().size() == 9);
	CHECK(s.pending_chat_line_count() == 7);

	HudFrameCompiler compiler;
	CHECK(s.flush_pending_messages(compiler, 10, mission) == 9);
	CHECK(s.pending_messages().empty());
	CHECK(ring_has(compiler.chat_lines(), "Go north"));
	CHECK(ring_has(compiler.chat_lines(), "Take the hill"));
	CHECK(ring_has(compiler.feed_lines(), "Triggered five"));
	CHECK(ring_has(compiler.feed_lines(), "dbg"));
	CHECK(s.objective_line() == "Triggered five"); // the triggered text is the latest line

	// A triggered id the mission lacks posts nothing.
	s.apply_mission_effect(effect(HudMissionEffect::Text, 9), gametext, mission, keys, work);
	CHECK(s.flush_pending_messages(compiler, 11, mission) == 0);
}

void lose_miss() {
	// GameText_GetString answers a miss empty: the banner takes it, the chat ring posts nothing.
	HudSession s;
	HudEffectOverlayWork work;
	s.apply_mission_effect(effect(HudMissionEffect::Text, 0, 0, 0, "x"), table({}), table({}), {},
			work);
	s.apply_mission_effect(effect(HudMissionEffect::Lose, 0, 0, 0, "STRMISC_ABSENT"), table({}),
			table({}), {}, work);
	CHECK(s.banner_line().empty());
	CHECK(s.pending_chat_line_count() == 1);
}

void pending_cap() {
	HudSession s;
	HudEffectOverlayWork work;
	for (int i = 0; i < 50; ++i)
		s.apply_mission_effect(effect(HudMissionEffect::DebugText, 0, 0, 0, std::to_string(i)),
				table({}), table({}), {}, work);
	CHECK(s.pending_messages().size() == HudSession::kMaxPendingMessages);
	CHECK(s.pending_messages().front().text == "10"); // the oldest drop first
}

void toggle_work() {
	const uint64_t events = ev::kHudColorCycled | ev::kThirdPersonSelected |
			ev::kFirstPersonSelected | ev::kFriendlyTagsCycled | ev::kMouseScaleStepped |
			ev::kMouseScaleNegative | (uint64_t{7} << ev::kMouseScaleParamShift) |
			ev::kJoinLockToggled | ev::kPauseCleared | ev::kOverlayWindowsCleared;
	const HudToggleWork w = hud_toggle_work(events);
	CHECK(w.restamp_color && !w.restamp_detail);
	CHECK(w.view_action_count == 2);
	CHECK(w.view_actions[0] == world::kViewActionFirstPerson);
	CHECK(w.view_actions[1] == world::kViewActionChase);
	CHECK(w.friendly_tags_cycled && !w.verbose_toggled);
	CHECK(hud_toggle_work(ev::kVerboseToggled).verbose_toggled);
	CHECK(w.step_mouse_scale && w.mouse_scale_negative && w.mouse_scale_param == 7);
	CHECK(w.toggle_join_lock && w.apply_pause && w.close_map);
	CHECK(hud_toggle_work(ev::kGunBitChanged).restamp_showhud);
	CHECK(hud_toggle_work(ev::kCommandMapOpened).open_command_map);
	CHECK(std::strcmp(lock_game_toast_key(1), "STRMISC_LOCKGAME_ON") == 0);
	CHECK(std::strcmp(lock_game_toast_key(0), "STRMISC_LOCKGAME_OFF") == 0);
	CHECK(lock_game_toast_key(-1) == nullptr);
}

void declutter() {
	HudSession s(2, 1);
	s.set_hud_detail_level(2);
	CHECK(s.begin_hidden_capture());
	CHECK(!s.begin_hidden_capture()); // busy
	CHECK(s.toggles().hud_detail_level == kHudDetailLevelBlank);
	s.finish_hidden_capture();
	CHECK(s.toggles().hud_detail_level == 2);
	hud_toggles_death_screen(s.toggles());
	s.reapply_persisted_hud_detail();
	CHECK(s.toggles().hud_detail_level == 1); // the death force ends with its mission
}

void menu_pause() {
	HudSession s;
	CHECK(s.set_menu_pause(true, false));
	CHECK(s.toggles().paused);
	CHECK(!s.set_menu_pause(false, true)); // a session keeps no pause word
	CHECK(s.toggles().paused);
	CHECK(!s.set_menu_pause(false, false));
	CHECK(!s.toggles().paused);
}

void caches() {
	HudSession s;
	CHECK(s.tip_frames_since(100) == 0);
	CHECK(s.tip_frames_since(103) == 3);
	CHECK(s.tip_frames_since(103) == 0);
	CHECK(s.slot_bar_labels_due(4));
	CHECK(!s.slot_bar_labels_due(4));
	CHECK(s.weapon_changed("m4"));
	CHECK(!s.weapon_changed("m4"));
	const HudMapLegendText &legend = s.map_legend(table({{"hud/hud_map_legend", "Legend"}}));
	CHECK(legend.title == "Legend");
	CHECK(legend.labels.size() == static_cast<size_t>(kHudMapLegendIconCount));
	CHECK(s.map_legend(table({})).title == "Legend"); // once per mission
	s.begin_mission();
	CHECK(s.slot_bar_labels_due(4)); // the new mission formats again
	CHECK(s.map_legend(table({})).title.empty());
	s.end_mission(false);
	CHECK(s.tip_frames_since(500) == 0);
}

void map_throttle() {
	HudSession s;
	s.begin_mission();
	CHECK(s.map_grid_origin_due(0));
	CHECK(!s.map_grid_origin_due(61));
	CHECK(s.map_grid_origin_due(62));
	s.set_map_grid_origin_present(true);
	CHECK(!s.map_grid_origin_due(1000));
	CHECK(s.map_footprints_due(5));
	CHECK(!s.map_footprints_due(66));
	CHECK(s.map_footprints_due(67));
	s.set_map_footprints_fed();
	CHECK(!s.map_footprints_due(1000));
	// With no kernel the feeds find nothing and keep retrying.
	HudSession t;
	t.begin_mission();
	HudFrameState state;
	inmatch::hud_session_footprints_feed(t, inmatch::RoleView{}, 0, state);
	CHECK(!t.map_footprints_fed());
	inmatch::hud_session_grid_origin_feed(t, inmatch::RoleView{}, 0, true, state);
	CHECK(!t.map_grid_origin_present() && !state.minimap.grid_origin_present);
}

void roles() {
	const inmatch::RoleView none;
	const inmatch::HudSessionRole joiner = inmatch::hud_session_role(inmatch::RoleKind::Joiner, none);
	CHECK(!joiner.authority && joiner.mp_session_peer && !joiner.in_session);
	const inmatch::HudSessionRole host =
			inmatch::hud_session_role(inmatch::RoleKind::DedicatedHost, none);
	CHECK(host.authority && !host.mp_session_peer);
	const inmatch::HudSessionRole sp = inmatch::hud_session_role(inmatch::RoleKind::SinglePlayer, none);
	CHECK(sp.authority && sp.mp_session_peer);
}

void teardown() {
	HudSession s;
	HudEffectOverlayWork work;
	s.apply_mission_effect(effect(HudMissionEffect::Text, 0, 0, 0, "x"), table({}), table({}), {},
			work);
	s.toggles().scoreboard_open = true;
	s.end_mission(true);
	CHECK(s.pending_messages().empty());
	CHECK(s.objective_line().empty() && s.banner_line().empty());
	CHECK(!s.toggles().scoreboard_open);
	CHECK(!s.toggles().server_status_view);
}

} // namespace

int main() {
	seed();
	effect_kinds();
	mission_effects();
	lose_miss();
	pending_cap();
	toggle_work();
	declutter();
	menu_pause();
	caches();
	map_throttle();
	roles();
	teardown();
	if (failures != 0) return 1;
	std::printf("OK: hud session\n");
	return 0;
}
