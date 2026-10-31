// The games games::install() registers (games/): every installed title resolves and speaks a
// protocol, JO's and DFX2's complete for the session and the web flow, OP: Acid Gambit speaking
// JO's very object, the unwitnessed titles refused by name, and a PN that two titles' one
// protocol carries resolving to that one object. And the rules each title runs
// (runtime/world/title_rules.h): JO's the defaults, DFX2's its witnessed knobs, OP: Acid Gambit's
// and the unwitnessed titles' JO's very object, and each protocol's bump-rule sheet (its
// ProtocolInMatch by message and the joiner-side knobs its titles share), a change to which bumps
// the protocol's version (games/CLAUDE.md).

#include <base/title/title.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/playersav/weapon_sav.h>
#include <games/acidgambit/acidgambit.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>
#include <games/novalogic_2009/generation.h>
#include <net/novacrypto/cdkey.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/host_settings.h> // server_settings_title (DFX2-10's host words)
#include <runtime/world/title_rules.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			++g_failures;                                                                          \
		}                                                                                          \
	} while (0)

// Every installed title resolves by its code and speaks an installed protocol.
void every_title_resolves() {
	CHECK(title_count() == 5);
	for (size_t i = 0; i < title_count(); ++i) {
		const Title *title = title_at(i);
		CHECK(title != nullptr && title_by_code(title->code) == title);
		const Protocol *protocol = title != nullptr ? title_protocol(title->code) : nullptr;
		CHECK(protocol != nullptr && protocol->code != nullptr && protocol->name != nullptr);
		if (protocol != nullptr)
			CHECK(protocol_by_code(protocol->code) == protocol);
	}
	CHECK(title_protocol("nosuchgame") == nullptr && protocol_by_code("nosuchgame") == nullptr);
	// The title a launch, a project or a session that names none runs as: OpenNova's own game.
	CHECK(&default_title() == title_by_code("acidgambit"));
}

// JO's and DFX2's protocols are complete for the session and the web flow, each the games'
// accessor's object over the 2009 generation, each named for its protocol line.
void retail_protocols_complete() {
	const Protocol *jo = title_protocol("jo");
	const Protocol *dfx2 = title_protocol("dfx2");
	CHECK(jo == &games::jo_protocol() && dfx2 == &games::dfx2_protocol());
	CHECK(&games::jo_title() == title_by_code("jo") &&
			&games::dfx2_title() == title_by_code("dfx2"));
	// DFX2-22: each title's mission catalog order.
	CHECK(games::jo_title().mission_list_order == MissionListOrder::FileName &&
			games::dfx2_title().mission_list_order == MissionListOrder::Title);
	for (const Protocol *protocol : { jo, dfx2 }) {
		CHECK(protocol != nullptr);
		if (protocol == nullptr)
			continue;
		CHECK(protocol_complete(*protocol, ProtocolPart::Session));
		CHECK(protocol_complete(*protocol, ProtocolPart::WebFlow));
		CHECK(protocol->generation == &games::novalogic_2009());
		CHECK(std::string(protocol_version(*protocol)) == "16");
		std::string error;
		CHECK(resolve_title_protocol(protocol->code, error) == protocol && error.empty());
	}
	CHECK(std::string(jo->name) == "JO" && std::string(dfx2->name) == "DFX2");
	CHECK(jo != dfx2);
	// DFX2's protocol carries the CD key its LAN host checks (DFX2-13); JO's has none.
	CHECK(dfx2->cd_key != nullptr && dfx2->cd_key->wire != nullptr &&
			dfx2->cd_key->registry != nullptr && dfx2->cd_key->exp1 != nullptr);
	CHECK(jo->cd_key == nullptr);
}

// A Title's every field past its identity (code, display_name, title_text, executable,
// command_file, updater_login), in title.h's order. The Title is bound whole, as in_match_sheet
// binds a protocol's groups, so a field added to Title stops this building until it is named here.
auto title_fields_past_identity(const Title &t) {
	const auto &[code, display_name, title_text, executable, command_file, updater_login,
			config_table, max_players, max_players_host_screen, host_screen, absent_resource_roles,
			expansions, read_defs, hud_exp_points, hud_lone_gunner_panel, hud_waypoint_active_tint,
			hud_objective_done_fill, hud_drops_armory_prompt, hud_end_game_footer_corners,
			death_screen_loadout, hud_fonts, mission_list_order, profile_magic, profile_first_name,
			profile_word_1396, max_players_switch, profile_single_player_kit, profile_class_kits,
			map_view_zoom_max, death_map_frame_color, death_shroud_reveal_ticks, death_shroud_main,
			death_screen_team_lists, sp_campaign_tabs, mod_list_base, options_hides_advanced,
			death_respawn_penalty_line, hud_map_cycle_skips_window, map_zoom_max_q16,
			hud_kill_list_no_class, hud_scope_no_magnification, hud_crosshair_fixed,
			hud_stance_health_tint, hud_message_tags, hud_map_legend, hud_tag_ranges,
			friendly_tag_toast_literals, friendly_tags_skip_mode_1_on_death_screen,
			hud_map_no_bank_names, death_screen_game_type_line, env_cube_terrain_clearance,
			nvg_gain_rect, inset_reticle_quad, frame_fx_work_side, low_health_screen_effect,
			map_capture_rings_under_layers, death_map_zone_markers, loading_screen, controls,
			hud_sp_how_to, stat_screen_done, menu_font_scale, host_vehicle_missions,
			host_switch_starts_off, host_armory_word, armory_lists_none_row, death_map_no_names,
			player_avatar_by_viewer_side, hud_game_info_board, hud_vehicle_panel_riders,
			epilog_debriefing_screens, player_info_combo_names, player_info_combo_spins,
			player_info_preview_hover, spinmap_spawn_zoom_q16, hud_tips] = t;
	(void)code;
	(void)display_name;
	(void)title_text;
	(void)executable;
	(void)command_file;
	(void)updater_login;
	return std::tie(config_table, max_players, max_players_host_screen, host_screen,
			absent_resource_roles, expansions, read_defs, hud_exp_points, hud_lone_gunner_panel,
			hud_waypoint_active_tint, hud_objective_done_fill, hud_drops_armory_prompt,
			hud_end_game_footer_corners, death_screen_loadout, hud_fonts, mission_list_order,
			profile_magic, profile_first_name, profile_word_1396, max_players_switch,
			profile_single_player_kit, profile_class_kits, map_view_zoom_max,
			death_map_frame_color, death_shroud_reveal_ticks, death_shroud_main,
			death_screen_team_lists, sp_campaign_tabs, mod_list_base, options_hides_advanced,
			death_respawn_penalty_line, hud_map_cycle_skips_window, map_zoom_max_q16,
			hud_kill_list_no_class, hud_scope_no_magnification, hud_crosshair_fixed,
			hud_stance_health_tint, hud_message_tags, hud_map_legend, hud_tag_ranges,
			friendly_tag_toast_literals, friendly_tags_skip_mode_1_on_death_screen,
			hud_map_no_bank_names, death_screen_game_type_line, env_cube_terrain_clearance,
			nvg_gain_rect.x0,
			nvg_gain_rect.y0, nvg_gain_rect.x1, nvg_gain_rect.y1, inset_reticle_quad,
			frame_fx_work_side, low_health_screen_effect, map_capture_rings_under_layers,
			death_map_zone_markers, loading_screen, controls, hud_sp_how_to, stat_screen_done,
			menu_font_scale, host_vehicle_missions, host_switch_starts_off, host_armory_word,
			armory_lists_none_row, death_map_no_names, player_avatar_by_viewer_side,
			hud_game_info_board, hud_vehicle_panel_riders, epilog_debriefing_screens,
			player_info_combo_names, player_info_combo_spins, player_info_preview_hover);
			menu_font_scale, host_vehicle_missions, host_switch_starts_off, armory_lists_none_row,
			death_map_no_names, player_avatar_by_viewer_side, hud_game_info_board,
			hud_vehicle_panel_riders, epilog_debriefing_screens, player_info_combo_names,
			player_info_combo_spins, player_info_preview_hover, spinmap_spawn_zoom_q16, hud_tips);
}

// Each of two titles' fields past the identity equal, a failure naming the field by its index in
// title_fields_past_identity's list (nvg_gain_rect's corners four entries).
template <typename Fields, size_t... I>
void check_fields_past_identity(const Fields &a, const Fields &b, std::index_sequence<I...>) {
	const auto check = [](bool same, size_t index) {
		if (same)
			return;
		std::printf("FAIL %s:%d: Title field %zu past the identity differs\n", __FILE__, __LINE__,
				index);
		++g_failures;
	};
	(check(std::get<I>(a) == std::get<I>(b), I), ...);
}
void check_fields_past_identity(const Title &a, const Title &b) {
	using Fields = decltype(title_fields_past_identity(a));
	check_fields_past_identity(title_fields_past_identity(a), title_fields_past_identity(b),
			std::make_index_sequence<std::tuple_size<Fields>::value>{});
}

// OP: Acid Gambit speaks JO's protocol: the same object, no copied literals, so a
// JOINTOPERATIONS peer resolves to the one protocol whichever title registered it.
void acidgambit_speaks_jo() {
	const Protocol *jo = title_protocol("jo");
	CHECK(title_protocol("acidgambit") == jo);
	CHECK(&games::acidgambit_protocol() == &games::jo_protocol());
	CHECK(protocol_by_pn("JOINTOPERATIONS") == jo && protocol_by_pn("jointoperations") == jo);
	std::string error;
	CHECK(resolve_title_protocol("acidgambit", error) == jo);
	const Title &ag = games::acidgambit_title();
	CHECK(title_by_code("ACIDGAMBIT") == &ag);
	CHECK(std::string(ag.display_name) == "OP: Acid Gambit" &&
			std::string(ag.title_text) == "OP: Acid Gambit");
	CHECK(std::string(ag.command_file) == "ACIDGAMBIT.CMD" && ag.executable == nullptr);
	// Past its identity its Title is Joint Operations' row (acidgambit_title.cpp starts from
	// jo_title()), every field of it, so a value JO's row sets is OP: Acid Gambit's too.
	check_fields_past_identity(ag, games::jo_title());
	CHECK(ag.mission_list_order == games::jo_title().mission_list_order);
	// DFX2-29: each witnessed title names its fresh profile's kit pages; JO's are the format's
	// eleven, Acid Gambit's JO's; the unwitnessed name none.
	CHECK(games::jo_title().profile_single_player_kit == playersav::kJointOpsSinglePlayerKit &&
			games::jo_title().profile_class_kits == playersav::kJointOpsClassKits);
	CHECK(ag.profile_single_player_kit == games::jo_title().profile_single_player_kit &&
			ag.profile_class_kits == games::jo_title().profile_class_kits);
	CHECK(games::dfx2_title().profile_single_player_kit != nullptr &&
			games::dfx2_title().profile_class_kits != nullptr);
	for (const char *code : { "dfx", "bhd" })
		CHECK(title_by_code(code) == nullptr || (title_by_code(code)->profile_single_player_kit == nullptr &&
				title_by_code(code)->profile_class_kits == nullptr));
	CHECK(protocol_by_pn("DELTAFORCEXTREME2") == title_protocol("dfx2"));
	// The PLAYER_INFO screen's knobs: JO's, Acid Gambit's JO's, dfx2.exe's its own (DFX2-74,
	// DFX2-75, DFX2-76).
	CHECK(games::jo_title().player_info_combo_names && ag.player_info_combo_names &&
			!games::dfx2_title().player_info_combo_names);
	CHECK(!games::jo_title().player_info_combo_spins && !ag.player_info_combo_spins &&
			games::dfx2_title().player_info_combo_spins);
	CHECK(games::jo_title().player_info_preview_hover && ag.player_info_preview_hover &&
			!games::dfx2_title().player_info_preview_hover);
	// The updater login the service serves OpenNova's own content under is its title's; a retail
	// title's folder names its own (Title::updater_login).
	CHECK(title_by_code("acidgambit")->updater_login != nullptr &&
			std::string(title_by_code("acidgambit")->updater_login) == "ACIDGAMBIT");
	CHECK(title_by_code("jo")->updater_login == nullptr &&
			title_by_code("dfx2")->updater_login == nullptr);
}

// The titles no binary has been read for: refused by name, as before the registry.
void unwitnessed_titles_refused() {
	std::string error;
	CHECK(resolve_title_protocol("bhd", error) == nullptr);
	CHECK(error.find("the bhd title's session identity is not witnessed yet: NVS") == 0);
	CHECK(resolve_title_protocol("dfx", error) == nullptr);
	CHECK(error.find("the dfx title's session identity is not witnessed yet: NVS") == 0);
	CHECK(resolve_title_protocol("nosuchgame", error) == nullptr);
	CHECK(error == "unknown game 'nosuchgame'");
}

// Every installed title runs installed rules: JO's for jo and for the titles that run JO's (the
// same object), DFX2's its own; an empty or unknown code runs the default title's.
void every_title_runs_rules() {
	const world::TitleRules &jo = world::title_rules("jo");
	CHECK(&jo == &games::jo_rules());
	CHECK(&world::title_rules("JO") == &jo);
	CHECK(&world::title_rules("dfx2") == &games::dfx2_rules() && &games::dfx2_rules() != &jo);
	for (const char *code : { "acidgambit", "dfx", "bhd" })
		CHECK(&world::title_rules(code) == &jo);
	CHECK(&games::acidgambit_rules() == &jo);
	CHECK(&world::title_rules("") == &jo && &world::title_rules("nosuchgame") == &jo);
}

// JO's rules are the defaults (Jointops.exe's, the baseline the ports were witnessed on); DFX2's
// each knob's dfx2.exe value (docs/titles/dfx2-vs-jo-re.md §8, the rows each names).
void retail_rules_values() {
	const world::TitleRules &jo = world::title_rules("jo");
	CHECK(jo.waypoint_completion == world::WaypointCompletion::LinkedSpawnPoints);
	CHECK(!jo.coop_checkpoint_spawn && jo.waypoint_goals_met_advance &&
			!jo.waypoint_advance_needs_done && !jo.route_done_records);
	CHECK(jo.score_values == &world::builtin_score_values && jo.score_exp_fanfare == 0 &&
			!jo.round_win_score_multiplier && !jo.vehicle_attach_kill_scored);
	CHECK(jo.hit_zone_damage_scale == &world::hit_zone_damage_multiplier &&
			!jo.impact_damage_clamp_first && jo.occupant_damage_scale);
	CHECK(jo.round_end_linger_ticks == 2790u && jo.deploy_target_protection_ticks == 620);
	CHECK(jo.medic_heal_and_revive && jo.downed_state_resend && jo.revive_window);
	CHECK(jo.emplaced_turret_update == world::EmplacedTurretUpdate::Plain &&
			jo.emplaced_solo_planar_slew == 0 && !jo.narrow_still_player_radius &&
			!jo.emplaced_solo_holds_fire);
	CHECK(jo.ai_vehicle_near_reach == 0xC0000 && jo.ai_vehicle_write_off_count == 3410 &&
			jo.spawn_kit == nullptr);
	CHECK(!jo.bms_table_warnings && jo.bms_loadout_sanitized && jo.mission_start_sets_difficulty);
	CHECK(jo.part_spin.full == 186413 && jo.part_spin.mid == 163110 && jo.part_spin.low == 139809 &&
			jo.part_spin.ground_decay == 186413 && jo.part_spin.helo_decay == 46603);
	CHECK(!jo.parachute_opens_on_jump && !jo.airborne_steer_glide &&
			!jo.tracked_impact_speed_xz_first);
	CHECK(jo.round_init_look_yaw);
	CHECK(!jo.spawn_reset_stores_def_health);
	CHECK(jo.scope_zero_seed == world::ScopeZeroSeed::DefaultStep);

	const world::TitleRules &dfx2 = world::title_rules("dfx2");
	CHECK(dfx2.waypoint_completion == world::WaypointCompletion::CoopCheckpoint); // DFX2-9
	CHECK(dfx2.coop_checkpoint_spawn && !dfx2.waypoint_goals_met_advance &&
			dfx2.waypoint_advance_needs_done && dfx2.route_done_records);
	CHECK(dfx2.score_values != &world::builtin_score_values && // DFX2-15
			dfx2.score_values(0x10000u)[3] == 2 && dfx2.score_values(0x10000u)[37] == 6 &&
			dfx2.score_exp_fanfare == (4u | (10u << 8)) && dfx2.round_win_score_multiplier &&
			dfx2.vehicle_attach_kill_scored);
	CHECK(dfx2.hit_zone_damage_scale(0) == 1.0 && dfx2.hit_zone_damage_scale(13) == 3.0 && // DFX2-16
			dfx2.impact_damage_clamp_first && !dfx2.occupant_damage_scale);
	CHECK(dfx2.round_end_linger_ticks == 1550u && dfx2.deploy_target_protection_ticks == 310); // 17
	CHECK(!dfx2.medic_heal_and_revive && !dfx2.downed_state_resend && !dfx2.revive_window); // 19, 20
	CHECK(dfx2.emplaced_turret_update == world::EmplacedTurretUpdate::CounterRotate && // DFX2-25
			dfx2.emplaced_solo_planar_slew == 0x3AB948 && dfx2.narrow_still_player_radius); // 26, 31
	CHECK(dfx2.emplaced_solo_holds_fire); // DFX2-37
	CHECK(dfx2.ai_vehicle_near_reach == 0x40000 && dfx2.ai_vehicle_write_off_count == 1860); // 32
	CHECK(dfx2.bms_table_warnings); // DFX2-24
	CHECK(!dfx2.bms_loadout_sanitized); // DFX2-27
	CHECK(!dfx2.mission_start_sets_difficulty); // DFX2-28
	CHECK(dfx2.parachute_opens_on_jump && dfx2.airborne_steer_glide && // DFX2-111..113
			dfx2.tracked_impact_speed_xz_first);
	CHECK(dfx2.part_spin.full == 372827 && dfx2.part_spin.mid == 326222 && // DFX2-23
			dfx2.part_spin.low == 279619 && dfx2.part_spin.ground_decay == 372827 &&
			dfx2.part_spin.helo_decay == 93206);
	size_t kit = 0;
	while (dfx2.spawn_kit != nullptr && dfx2.spawn_kit[kit] != nullptr) ++kit; // DFX2-33
	CHECK(kit == 8 && std::string(dfx2.spawn_kit[2]) == "WPN_MK23");
	// The page is dfx2.exe's one table (0x832368), which the rules' spawn and the Title's fresh
	// profile each point at, neither reading the other's row (DFX2-29, DFX2-33).
	CHECK(dfx2.spawn_kit == games::dfx2_single_player_kit() &&
			games::dfx2_title().profile_single_player_kit == games::dfx2_single_player_kit());
	CHECK(!dfx2.round_init_look_yaw); // DFX2-123
	CHECK(dfx2.spawn_reset_stores_def_health); // DFX2-30
	CHECK(dfx2.scope_zero_seed == world::ScopeZeroSeed::Rangefinder); // DFX2-120
}

// The Title's knobs read on the local player's input alone (docs/titles/dfx2-vs-jo-re.md §8, the
// three with no catalog id): JO's the defaults, DFX2's dfx2.exe's, OP: Acid Gambit's JO's (its
// every field past the identity, acidgambit_speaks_jo).
void retail_local_input_title_values() {
	const Title &jo = games::jo_title();
	CHECK(jo.map_zoom_max_q16 == 0x100000); // section 7.12
	// Section 5.3: every mode's toast the gametext key's, no death-screen step.
	for (const char *literal : jo.friendly_tag_toast_literals) CHECK(literal == nullptr);
	CHECK(!jo.friendly_tags_skip_mode_1_on_death_screen);

	const Title &dfx2 = games::dfx2_title();
	CHECK(dfx2.map_zoom_max_q16 == 0x80000); // section 7.12
	// Section 5.3: modes 1 and 2 the literal lines, the death screen's step past mode 1.
	CHECK(dfx2.friendly_tag_toast_literals[0] == nullptr &&
			dfx2.friendly_tag_toast_literals[3] == nullptr);
	CHECK(std::string(dfx2.friendly_tag_toast_literals[1]) == "Friendly Tags: On" &&
			std::string(dfx2.friendly_tag_toast_literals[2]) == "Friendly Tags: Smart");
	CHECK(dfx2.friendly_tags_skip_mode_1_on_death_screen);
}

// The Title's HUD rows of DFX2-121 and after (docs/titles/dfx2-vs-jo-re.md §8):
// JO's the defaults, DFX2's dfx2.exe's, OP: Acid Gambit's JO's (acidgambit_speaks_jo).
void retail_gap_title_values() {
	const Title &jo = games::jo_title();
	const Title &dfx2 = games::dfx2_title();
	CHECK(jo.spinmap_spawn_zoom_q16 == 0 && dfx2.spinmap_spawn_zoom_q16 == 0x40000); // DFX2-121
	CHECK(jo.hud_tips && !dfx2.hud_tips); // DFX2-122
}

// A protocol's in-match sheet: every knob of its ProtocolInMatch, and its CD key (Protocol::cd_key,
// the C2S 0x00 JOIN's LAN check), under every message net/protocol/protocol.h's kProtocolDialect
// names it for, one line per message. Every group is bound whole, so a knob added to a group, or a
// group added, stops this building until the sheet names it; a knob the index leaves out, or one
// it names that the sheet does not, fails.
std::string in_match_sheet(const Protocol &protocol) {
	const ProtocolInMatch &m = *protocol.in_match;
	const auto &[s2c_0a, s2c_0f, s2c_14, s2c_25, s2c_5a, s2c_64, s2c_66, c2s_2f, slot_state,
			person_index] = m;
	const auto &[objective_checkpoint] = s2c_0a;
	const auto &[spawn_zone_flag_always, deploy_screen_always] = s2c_0f;
	const auto &[chat_squad_tag_wrapped] = s2c_14;
	const auto &[end_round_linger_ticks] = s2c_25;
	const auto &[select_last_primary_row] = s2c_5a;
	const auto &[rules_word_bits] = s2c_64;
	const auto &[armory_only_availability] = s2c_66;
	const auto &[loadout_class_fixed] = c2s_2f;
	const auto &[join_state_fixed] = slot_state;
	const auto &[pool0_wire_capacity] = person_index;
	char rules_word[16];
	std::snprintf(rules_word, sizeof(rules_word), "0x%X", static_cast<unsigned>(rules_word_bits));
	const std::vector<std::pair<std::string, std::string>> knobs = {
		{ "s2c_0a.objective_checkpoint", std::to_string(objective_checkpoint) },
		{ "s2c_0f.spawn_zone_flag_always", std::to_string(spawn_zone_flag_always) },
		{ "s2c_0f.deploy_screen_always", std::to_string(deploy_screen_always) },
		{ "s2c_14.chat_squad_tag_wrapped", std::to_string(chat_squad_tag_wrapped) },
		{ "s2c_25.end_round_linger_ticks", std::to_string(end_round_linger_ticks) },
		{ "s2c_5a.select_last_primary_row", std::to_string(select_last_primary_row) },
		{ "s2c_64.rules_word_bits", rules_word },
		{ "s2c_66.armory_only_availability", std::to_string(armory_only_availability) },
		{ "c2s_2f.loadout_class_fixed", std::to_string(loadout_class_fixed) },
		{ "slot_state.join_state_fixed", std::to_string(join_state_fixed) },
		{ "person_index.pool0_wire_capacity", std::to_string(pool0_wire_capacity) },
		{ "cd_key", std::to_string(protocol.cd_key != nullptr) },
	};
	for (const auto &knob : knobs) {
		bool indexed = false;
		for (const ProtocolDialectRow &row : kProtocolDialect)
			indexed = indexed || knob.first == row.knob;
		CHECK(indexed);
	}
	std::string out;
	for (size_t i = 0; i < std::size(kProtocolDialect); ++i) {
		const ProtocolDialectRow &row = kProtocolDialect[i];
		const ProtocolDialectRow *prev = i > 0 ? &kProtocolDialect[i - 1] : nullptr;
		// The index runs S2C then C2S, each by id, so a message's rows sit together.
		const bool same_message = prev != nullptr && prev->dir == row.dir && prev->tag == row.tag;
		CHECK(prev == nullptr || same_message || (prev->dir == row.dir && prev->tag < row.tag) ||
				(prev->dir == 'S' && row.dir == 'C'));
		const auto knob = std::find_if(
				knobs.begin(), knobs.end(), [&](const auto &k) { return k.first == row.knob; });
		CHECK(knob != knobs.end());
		if (knob == knobs.end())
			continue;
		if (!same_message) {
			char head[16];
			std::snprintf(
					head, sizeof(head), "%c 0x%02X:", row.dir, static_cast<unsigned>(row.tag));
			out += (out.empty() ? "" : "\n") + std::string(head);
		}
		out += " " + knob->first + "=" + knob->second;
	}
	return out;
}

// The TitleRules fields marked joiner-side (read on a joiner's world or its replica, not only on
// the authority): what a joiner of the protocol does as its host's game does.
std::string joiner_rules_row(const world::TitleRules &r) {
	std::string kit;
	for (const char *const *name = r.spawn_kit; name != nullptr && *name != nullptr; ++name)
		kit += std::string(kit.empty() ? "" : ",") + *name;
	return "waypoint_goals_met_advance=" + std::to_string(r.waypoint_goals_met_advance) +
			" waypoint_advance_needs_done=" + std::to_string(r.waypoint_advance_needs_done) +
			" route_done_records=" + std::to_string(r.route_done_records) +
			" emplaced_turret_update=" +
			std::to_string(static_cast<int>(r.emplaced_turret_update)) +
			" emplaced_solo_planar_slew=" + std::to_string(r.emplaced_solo_planar_slew) +
			" narrow_still_player_radius=" + std::to_string(r.narrow_still_player_radius) +
			" emplaced_solo_holds_fire=" + std::to_string(r.emplaced_solo_holds_fire) +
			" part_spin=" + std::to_string(r.part_spin.full) + "/" +
			std::to_string(r.part_spin.mid) + "/" + std::to_string(r.part_spin.low) + "/" +
			std::to_string(r.part_spin.ground_decay) + "/" +
			std::to_string(r.part_spin.helo_decay) +
			" parachute_opens_on_jump=" + std::to_string(r.parachute_opens_on_jump) +
			" airborne_steer_glide=" + std::to_string(r.airborne_steer_glide) +
			" tracked_impact_speed_xz_first=" + std::to_string(r.tracked_impact_speed_xz_first) +
			" spawn_kit=" + (r.spawn_kit == nullptr ? std::string("jo_single_player_page") : kit) +
			" script_voice_volume=" + std::to_string(r.script_voice_volume) +
			" trail_surface_alternate=" + std::to_string(r.trail_surface_alternate) +
			" scope_zero_seed=" + std::to_string(static_cast<int>(r.scope_zero_seed)) +
			" spawn_reset_stores_def_health=" + std::to_string(r.spawn_reset_stores_def_health) +
			" round_init_look_yaw=" + std::to_string(r.round_init_look_yaw);
}

// The bump-rule sheets (ADR 0076 decision 6), one per protocol: what its joiner reads or writes as
// it does, so a change to one lands with a protocol of its own and the next protocol version in
// its <code>_protocol.cpp (games/CLAUDE.md). A sheet is the protocol's in-match sheet and the
// joiner-side TitleRules its titles run, which every title speaking one Protocol object shares: a
// joiner of one title meets a host of another as that protocol, so a joiner-side difference is a
// protocol of its own. JO's and DFX2's sheets are retail-pinned and never change.
void bump_rule_sheet_per_protocol() {
	// Each protocol with an in-match part, and the titles that speak it.
	std::vector<std::pair<const Protocol *, std::vector<std::string>>> speakers;
	for (size_t i = 0; i < title_count(); ++i) {
		const Title *title = title_at(i);
		const Protocol *protocol = title != nullptr ? title_protocol(title->code) : nullptr;
		if (protocol == nullptr || !protocol->in_match.has_value())
			continue;
		auto it = std::find_if(speakers.begin(), speakers.end(),
				[&](const auto &s) { return s.first == protocol; });
		if (it == speakers.end()) {
			speakers.push_back({ protocol, {} });
			it = std::prev(speakers.end());
		}
		it->second.push_back(title->code);
	}
	for (const auto &speaker : speakers) {
		const std::string rules = joiner_rules_row(world::title_rules(speaker.second.front()));
		for (const std::string &code : speaker.second)
			CHECK(joiner_rules_row(world::title_rules(code)) == rules);
	}
	// A protocol's sheet, by its code: its in-match sheet and its titles' joiner-side rules.
	const auto sheet = [&](const char *code) {
		const Protocol *protocol = protocol_by_code(code);
		const auto it = std::find_if(speakers.begin(), speakers.end(),
				[&](const auto &s) { return s.first == protocol; });
		if (it == speakers.end())
			return std::string("no in-match protocol");
		const std::string rules = joiner_rules_row(world::title_rules(it->second.front()));
		return in_match_sheet(*protocol) + "\njoiner: " + rules;
	};
	const std::string jo =
			"S 0x0A: s2c_0a.objective_checkpoint=0 slot_state.join_state_fixed=0\n"
			"S 0x0F: s2c_0f.spawn_zone_flag_always=0 s2c_0f.deploy_screen_always=0\n"
			"S 0x14: s2c_14.chat_squad_tag_wrapped=0\n"
			"S 0x1E: person_index.pool0_wire_capacity=256\n"
			"S 0x25: s2c_25.end_round_linger_ticks=0\n"
			"S 0x2D: person_index.pool0_wire_capacity=256\n"
			"S 0x33: person_index.pool0_wire_capacity=256\n"
			"S 0x46: person_index.pool0_wire_capacity=256\n"
			"S 0x5A: s2c_5a.select_last_primary_row=0\n"
			"S 0x64: s2c_64.rules_word_bits=0x0\n"
			"S 0x66: s2c_66.armory_only_availability=2\n"
			"S 0x6D: person_index.pool0_wire_capacity=256\n"
			"C 0x00: cd_key=0\n"
			"C 0x2F: c2s_2f.loadout_class_fixed=0 s2c_66.armory_only_availability=2 "
			"slot_state.join_state_fixed=0\n"
			"joiner: waypoint_goals_met_advance=1 waypoint_advance_needs_done=0 "
			"route_done_records=0 emplaced_turret_update=0 emplaced_solo_planar_slew=0 "
			"narrow_still_player_radius=0 emplaced_solo_holds_fire=0 "
			"part_spin=186413/163110/139809/186413/46603 parachute_opens_on_jump=0 "
			"airborne_steer_glide=0 tracked_impact_speed_xz_first=0 "
			"spawn_kit=jo_single_player_page script_voice_volume=210 trail_surface_alternate=0 "
			"scope_zero_seed=0 "
			"spawn_reset_stores_def_health=0 "
			"round_init_look_yaw=1";
	const std::string dfx2 =
			"S 0x0A: s2c_0a.objective_checkpoint=1 slot_state.join_state_fixed=1\n"
			"S 0x0F: s2c_0f.spawn_zone_flag_always=1 s2c_0f.deploy_screen_always=1\n"
			"S 0x14: s2c_14.chat_squad_tag_wrapped=1\n"
			"S 0x1E: person_index.pool0_wire_capacity=256\n"
			"S 0x25: s2c_25.end_round_linger_ticks=1240\n"
			"S 0x2D: person_index.pool0_wire_capacity=256\n"
			"S 0x33: person_index.pool0_wire_capacity=256\n"
			"S 0x46: person_index.pool0_wire_capacity=256\n"
			"S 0x5A: s2c_5a.select_last_primary_row=1\n"
			"S 0x64: s2c_64.rules_word_bits=0x10000\n"
			"S 0x66: s2c_66.armory_only_availability=3\n"
			"S 0x6D: person_index.pool0_wire_capacity=256\n"
			"C 0x00: cd_key=1\n"
			"C 0x2F: c2s_2f.loadout_class_fixed=1 s2c_66.armory_only_availability=3 "
			"slot_state.join_state_fixed=1\n"
			"joiner: waypoint_goals_met_advance=0 waypoint_advance_needs_done=1 "
			"route_done_records=1 emplaced_turret_update=1 emplaced_solo_planar_slew=3848520 "
			"narrow_still_player_radius=1 emplaced_solo_holds_fire=1 "
			"part_spin=372827/326222/279619/372827/93206 parachute_opens_on_jump=1 "
			"airborne_steer_glide=1 tracked_impact_speed_xz_first=1 "
			"spawn_kit=WPN_M4AUTO,WPN_M4,WPN_MK23,WPN_AT4,"
			"WPN_GRENADEFB,WPN_GRENADEHE,WPN_GRENADESM,WPN_KNIFE script_voice_volume=240 "
			"trail_surface_alternate=1 "
			"scope_zero_seed=1 "
			"spawn_reset_stores_def_health=1 "
			"round_init_look_yaw=0";
	CHECK(sheet("jo") == jo);
	CHECK(sheet("dfx2") == dfx2);
	// Two protocols have an in-match part: JO's, which OP: Acid Gambit speaks (the one object, at
	// JO's protocol version), and DFX2's.
	CHECK(speakers.size() == 2);
	for (const auto &speaker : speakers) {
		if (speaker.first == title_protocol("jo"))
			CHECK((speaker.second == std::vector<std::string>{ "jo", "acidgambit" }));
		else
			CHECK(speaker.first == title_protocol("dfx2") &&
					(speaker.second == std::vector<std::string>{ "dfx2" }));
	}
	CHECK(std::string(protocol_version(*title_protocol("acidgambit"))) ==
			protocol_version(*title_protocol("jo")));
	// The unwitnessed titles' protocols have no in-match part: their session part is refused by
	// name.
	for (const char *code : { "dfx", "bhd" })
		CHECK(sheet(code) == "no in-match protocol");
}

// DFX2-10's host-screen words are the Title's (Title::host_armory_word, its MISSION the other of
// 2 and 3), and the game.cfg defaults fill every weapon with the executable's MISSION word, keyed
// by the title's config table, as formats/gamecfg cannot read a Title. Each title's config table
// fills its own host screen's MISSION word, so the two never part. (The word S2C 0x66 carries is
// the protocol's, another axis: no title's protocol is read here.)
// [orig: Config_SetDefaults @0x54D030 - memset32(3, 0xFF) @0x54D410]
// [orig: Config_SetDefaults @0x54a2c0 - memset32(dword_254F25C, 2, 0xFF) (dfx2.exe)]
void config_table_fills_the_titles_mission_word() {
	for (size_t i = 0; i < title_count(); ++i) {
		const Title &title = *title_at(i);
		const gamecfg::GameCfg cfg = gamecfg::defaults({}, 0, 0, title.config_table);
		CHECK(cfg.weapon_availability[0] == inmatch::server_settings_title(title).mission_word);
	}
	// Jointops.exe's ARMORY 2 and MISSION 3, dfx2.exe's 3 and 2; OP: Acid Gambit's are JO's.
	CHECK(games::jo_title().host_armory_word == 2 && games::dfx2_title().host_armory_word == 3);
	CHECK(inmatch::server_settings_title(games::jo_title()).mission_word == 3);
	CHECK(inmatch::server_settings_title(games::dfx2_title()).mission_word == 2);
}

// A version line's title parts (title_versions): the title's name where the line names no game,
// the protocol it speaks with its PV2 where its session part is witnessed; OP: Acid Gambit's is
// JO's; an unwitnessed protocol and an unknown title add no protocol.
void title_parts_of_the_version_line() {
	Versions jo;
	title_versions("jo", jo);
	CHECK(jo.game == "Joint Operations" && jo.protocol == "JO" && jo.protocol_version == "16");
	Versions dfx2;
	title_versions("DFX2", dfx2);
	CHECK(dfx2.game == "Delta Force Xtreme 2" && dfx2.protocol == "DFX2" &&
			dfx2.protocol_version == "16");
	Versions named;
	named.game = "My Game";
	title_versions("acidgambit", named);
	CHECK(named.game == "My Game" && named.protocol == "JO" && named.protocol_version == "16");
	Versions dfx;
	title_versions("dfx", dfx);
	CHECK(dfx.game == "Delta Force: Xtreme" && dfx.protocol.empty() &&
			dfx.protocol_version.empty());
	Versions unknown;
	title_versions("nosuchgame", unknown);
	title_versions("", unknown);
	CHECK(unknown.game.empty() && unknown.protocol.empty());
}

} // namespace

int main() {
	every_title_resolves();
	retail_protocols_complete();
	acidgambit_speaks_jo();
	unwitnessed_titles_refused();
	every_title_runs_rules();
	retail_rules_values();
	retail_local_input_title_values();
	retail_gap_title_values();
	bump_rule_sheet_per_protocol();
	config_table_fills_the_titles_mission_word();
	title_parts_of_the_version_line();
	if (g_failures == 0) {
		std::printf("games: all checks passed\n");
		return 0;
	}
	std::printf("games: %d check(s) failed\n", g_failures);
	return 1;
}
