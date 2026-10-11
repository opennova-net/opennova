// game.cfg onto the host session (runtime/inmatch/host_config.h): the weapon
// table the avail_wpn keys address, the defaults' localized texts, and the
// session the host creates from the cfg block (CNapiGameSession_BuildAndCreateSession's
// cap, Game_ApplySessionSettingsToGlobals' arms).
#include <base/gameprofile/game_type.h>
#include <formats/def/def.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_settings.h>

#include "common/test_expect.h"

#include <cstdio>
#include <string>

using namespace opennova;
using namespace opennova::inmatch;

int main() {
	// --- the weapon table: row 0 `None`, then every closed weapon.def row in
	//     file order; a subclass row is never selectable and starts no
	//     countdown; an entry no `end` closed is no row.
	{
		const std::string text =
				"weapon \"WPN_PISTOL\"\r\nloadout_selectable 1\r\nend\r\n"
				"weapon \"WPN_HIDDEN\"\r\nend\r\n"
				"weapon \"WPN_PARENT\"\r\nloadout_selectable 1\r\nloadout_subclasses 2\r\nend\r\n"
				"weapon \"WPN_SUB_A\"\r\nloadout_selectable 1\r\nloadout_subclasses 3\r\nend\r\n"
				"weapon \"WPN_SUB_B\"\r\nloadout_selectable 1\r\nend\r\n"
				"weapon \"WPN_RIFLE\"\r\nloadout_selectable 1\r\nend\r\n"
				"weapon \"WPN_OPEN\"\r\nloadout_selectable 1\r\n";
		def::DefWeaponsFile weapons = {};
		TEST_EXPECT(def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()),
				text.size(), &weapons) == 0);
		const gamecfg::WeaponRoster roster = game_cfg_weapon_roster(weapons);
		def::def_free_weapons(&weapons);
		TEST_EXPECT(roster.size() == 7);
		TEST_EXPECT(roster.size() == 7 && roster[0].name == "None" && roster[0].loadout_selectable == 0);
		TEST_EXPECT(roster.size() == 7 && roster[1].name == "WPN_PISTOL" && roster[1].loadout_selectable == 1);
		TEST_EXPECT(roster.size() == 7 && roster[2].name == "WPN_HIDDEN" && roster[2].loadout_selectable == 0);
		TEST_EXPECT(roster.size() == 7 && roster[3].name == "WPN_PARENT" && roster[3].loadout_selectable == 1);
		TEST_EXPECT(roster.size() == 7 && roster[4].name == "WPN_SUB_A" && roster[4].loadout_selectable == 0);
		TEST_EXPECT(roster.size() == 7 && roster[5].name == "WPN_SUB_B" && roster[5].loadout_selectable == 0);
		TEST_EXPECT(roster.size() == 7 && roster[6].name == "WPN_RIFLE" && roster[6].loadout_selectable == 1);

		// The avail_wpn keys address the table by row: WPN_RIFLE is row 6.
		const std::string cfg_text = "version = 29\r\navail_wpn_rifle = 2\r\navail_wpn_sub_a = 0\r\n";
		gamecfg::LoadOptions options;
		options.weapons = roster;
		const gamecfg::LoadResult loaded = gamecfg::load(cfg_text.data(), cfg_text.size(), options);
		TEST_EXPECT(*gamecfg::weapon_slot(loaded.cfg, 6) == 2);
		TEST_EXPECT(*gamecfg::weapon_slot(loaded.cfg, 4) == 0);
		// The writer prints the selectable rows only.
		const std::string written = gamecfg::write(loaded.cfg, roster);
		TEST_EXPECT(written.find("avail_wpn_RIFLE ") != std::string::npos);
		TEST_EXPECT(written.find("avail_wpn_PISTOL ") != std::string::npos);
		TEST_EXPECT(written.find("avail_wpn_SUB_A") == std::string::npos);
		TEST_EXPECT(written.find("avail_wpn_HIDDEN") == std::string::npos);
	}

	// --- the default texts: the `!` fallbacks with no table or no entry, the
	//     entry's text when the Menu section holds it.
	{
		const gamecfg::DefaultTexts none = game_cfg_default_texts(nullptr);
		TEST_EXPECT(none.untitled == "!Untitled");
		TEST_EXPECT(none.user_message == "!Put your message here");
		rtxt::File table;
		table.sections.push_back(rtxt::Section{"Menu", 1});
		rtxt::Entry entry;
		entry.key = "UNTITLED";
		entry.text = "Untitled";
		table.entries.push_back(entry);
		const gamecfg::DefaultTexts texts = game_cfg_default_texts(&table);
		TEST_EXPECT(texts.untitled == "Untitled");
		TEST_EXPECT(texts.user_message == "!Put your message here");
	}

	// --- the defaults block lands on the host screen's rule baseline: the
	//     apply over Config_SetDefaults is host_screen_default_config.
	{
		gamecfg::DefaultTexts texts;
		texts.untitled = "Untitled";
		texts.user_message = game_rules::kCustomTextDefault;
		gamecfg::GameCfg cfg = gamecfg::defaults(texts);
		const HostScreenState host = host_session_settings(cfg);
		const GameConfig screen = host_screen_default_config();
		const GameConfig &c = host.config;
		TEST_EXPECT(c.respawn_time == screen.respawn_time);
		TEST_EXPECT(c.time_limit_minutes == screen.time_limit_minutes);
		TEST_EXPECT(c.replay_enabled == screen.replay_enabled);
		TEST_EXPECT(c.max_team_lives == screen.max_team_lives);
		TEST_EXPECT(c.score_limit == screen.score_limit);
		TEST_EXPECT(c.max_score == screen.max_score);
		TEST_EXPECT(c.koth_delta == screen.koth_delta);
		TEST_EXPECT(c.flag_return_ticks == screen.flag_return_ticks);
		TEST_EXPECT(c.capture_duration_seconds == screen.capture_duration_seconds);
		TEST_EXPECT(c.capture_speed_setting == screen.capture_speed_setting);
		TEST_EXPECT(c.spawn_wave_time_base == screen.spawn_wave_time_base);
		TEST_EXPECT(c.spawn_wave_time_zone == screen.spawn_wave_time_zone);
		TEST_EXPECT(c.default_spawn_requires_no_team_zone == screen.default_spawn_requires_no_team_zone);
		TEST_EXPECT(c.num_teams == screen.num_teams);
		TEST_EXPECT(c.respawn_timeout == screen.respawn_timeout);
		TEST_EXPECT(c.start_delay == screen.start_delay);
		TEST_EXPECT(c.destroy_buildings == screen.destroy_buildings);
		TEST_EXPECT(c.death_messages == screen.death_messages);
		TEST_EXPECT(c.custom_text == screen.custom_text);
		TEST_EXPECT(c.mp_attributes == screen.mp_attributes);
		TEST_EXPECT(c.max_packet_size == screen.max_packet_size);
		TEST_EXPECT(c.unlimited_vehicles == screen.unlimited_vehicles);
		TEST_EXPECT(c.lan_mode == screen.lan_mode);
		// The cfg table's words: the stock cap, the lineup queue, the server
		// type a stock game.cfg carries (`dedicated = 0`).
		const HostScreenState stock;
		TEST_EXPECT(host.player_limit == stock.player_limit);
		TEST_EXPECT(host.serve_and_play == stock.serve_and_play);
		TEST_EXPECT(host.use_lineup_queue == stock.use_lineup_queue);
		TEST_EXPECT(host.lineup_queue_size == stock.lineup_queue_size);
		TEST_EXPECT(host.game_type_setting == stock.game_type_setting);
		TEST_EXPECT(c.server_name == "Untitled");
		TEST_EXPECT(c.max_players == 64u && !c.dedicated_server && c.player_slot_limit() == 64u);
		// Config_SetDefaults' join balance (balance_join 1, 0.5) and vote words.
		TEST_EXPECT(c.balance_join && c.balance_join_percent == 0.5f);
		TEST_EXPECT(!c.voting_enabled && c.voting_min_players == 6 && c.voting_percent == 0.66f);
		TEST_EXPECT(c.armory_reuse_time == 30u);
		TEST_EXPECT(c.flag_reset_seconds == 420);
	}

	// --- a block off its defaults: every mapped word, the sentinels, and the
	//     in-session cap written back into the block.
	{
		gamecfg::GameCfg cfg = gamecfg::defaults();
		cfg.game_name = "A name of thirty-one characters";
		cfg.game_name += "!";
		cfg.mp_host_game_password = "pw";
		cfg.mp_host_side_password_a = "blue";
		cfg.mp_host_side_password_b = "red";
		cfg.mp_host_spectator_password = "spec";
		cfg.servermsg = "hello";
		cfg.country = "GBR";
		cfg.mp_num_spectators_max = 4;
		cfg.mp_novaworld_host_lan_only = 1;
		cfg.sv_punkbuster = 1;
		cfg.nwisptype = 7;
		cfg.enable_ai = 0;
		cfg.numallowablefriendlykills = 9;
		cfg.mp_tod_continuity = 1;
		cfg.mp_max_players = 100;
		cfg.dedicated = 1;
		cfg.mp_numteams = 4;
		cfg.mpattrib = 0x8004;
		cfg.fatbullets = 1;
		cfg.oneshotonekill = 1;
		cfg.unlimited_vehicles = 0;
		cfg.mpvoting = 1;
		cfg.mpchangeteam_interval = 120;
		cfg.mpchangeteam_penalty = 20;
		cfg.autobalance_on_recycle_enabled = 1;
		cfg.autobalance_on_recycle_diff_min = 2;
		cfg.autobalance_on_recycle_diff_max = 3;
		cfg.time_limit = 45;
		cfg.koth_limit = 0;
		cfg.replay = 0;
		cfg.max_kills = 500;
		cfg.max_score = 12;
		cfg.flag_return_time = 99;
		cfg.flag_reset_time = 77;
		cfg.armory_reuse_time = 11;
		cfg.balance_join = 0;
		cfg.dominpingcheck = 1;
		cfg.minping = 5;
		cfg.domaxpingcheck = 1;
		cfg.maxping = 900;
		cfg.mp_max_packet_size = 1200;
		cfg.teamchange_time = 33;
		cfg.mp_lfp_takeoverspeed = 2;
		cfg.spawnregulator_psp = 4;
		cfg.spawnregulator_lfp = 8;
		cfg.nodefaultspawnpoints = 1;
		cfg.timeout = 6;
		cfg.startdelay = 10;
		cfg.destroybuild = 1;
		cfg.deathmes = 0;
		cfg.mp_difficulty = 2;
		cfg.mp_permanent_death = 1;
		cfg.mp_allowsniperscopezoom = 1;
		cfg.lanmode = 3;
		cfg.mp_gametype = 65540;
		cfg.mp_use_lineup_queue = 0;
		cfg.mp_lineup_queue_size = 12;
		const HostScreenState host = host_session_settings(cfg);
		const GameConfig &c = host.config;
		TEST_EXPECT(cfg.mp_max_players == 65); // the in-session cap, written back
		TEST_EXPECT(host.player_limit == 65 && !host.serve_and_play);
		// The session's cap is the advertised and admitted one; only the slot
		// limit carries the dedicated host's own slot.
		TEST_EXPECT(c.max_players == 65u && c.dedicated_server);
		TEST_EXPECT(c.player_slot_limit() == 66u); // 65 plus the dedicated slot
		TEST_EXPECT(host.game_type_setting == 65540);
		TEST_EXPECT(host.use_lineup_queue == 0 && host.lineup_queue_size == 12);
		TEST_EXPECT(c.server_name == "A name of thirty-one characters"); // 31 bytes
		TEST_EXPECT(c.server_password == "pw");
		TEST_EXPECT(c.side_a_password == "blue" && c.side_b_password == "red");
		TEST_EXPECT(c.spectator_password == "spec" && c.spectator_slots == 4);
		TEST_EXPECT(c.custom_text == "hello");
		TEST_EXPECT(c.country == "GBR");
		TEST_EXPECT(c.server_lan_only == 1 && c.server_punkbuster == 1);
		TEST_EXPECT(c.connection_speed == 7 && !c.allow_ai);
		TEST_EXPECT(c.max_friendly_kills == 9 && c.time_of_day_continuity == 1);
		TEST_EXPECT(c.num_teams == 4 && c.mp_attributes == 0x8004u);
		TEST_EXPECT(c.fat_bullets && c.one_shot_kill && c.unlimited_vehicles == 0);
		TEST_EXPECT(c.voting_enabled);
		TEST_EXPECT(c.change_team_interval_seconds == 120 && c.change_team_penalty_seconds == 20);
		TEST_EXPECT(c.auto_balance_enabled && c.auto_balance_min_difference == 2 &&
				c.auto_balance_trigger_difference == 3);
		TEST_EXPECT(c.respawn_time == 45u);
		TEST_EXPECT(c.time_limit_minutes == kSessionNoKothLimitMinutes); // KOTH 0
		TEST_EXPECT(c.replay_enabled == 0u);
		TEST_EXPECT(c.score_limit == kSessionNoPointLimit && c.max_score == 12u);
		TEST_EXPECT(c.flag_return_ticks == 99u && c.flag_reset_seconds == 77);
		TEST_EXPECT(c.armory_reuse_time == 11u && !c.balance_join);
		TEST_EXPECT(c.do_min_ping_check && c.min_ping == 5u && c.do_max_ping_check && c.max_ping == 900u);
		TEST_EXPECT(c.max_packet_size == 1200);
		TEST_EXPECT(c.capture_duration_seconds == 33 && c.capture_speed_setting == 2);
		TEST_EXPECT(c.spawn_wave_time_base == 4 && c.spawn_wave_time_zone == 8);
		TEST_EXPECT(c.default_spawn_requires_no_team_zone == 1u);
		TEST_EXPECT(c.respawn_timeout == 6u && c.start_delay == 10u);
		TEST_EXPECT(c.destroy_buildings == 1u && c.death_messages == 0u);
		TEST_EXPECT(c.config_bytes[6] == 2);
		TEST_EXPECT(c.permanent_death && c.allow_sniper_scope_zoom);
		TEST_EXPECT(c.lan_mode == 3u);
		TEST_EXPECT(c.effective_send_holdoff_ticks(GameSessionChannel::Lan) == 4u);

		// A blank cap is 1 in session (a slot limit of 2 with the dedicated slot).
		gamecfg::GameCfg blank = gamecfg::defaults();
		blank.mp_max_players = 0;
		blank.dedicated = 1;
		blank.unlimited_vehicles = 7; // the raw word, as retail's block carries it
		const HostScreenState blank_host = host_session_settings(blank);
		TEST_EXPECT(blank.mp_max_players == 1 && blank_host.config.max_players == 1u);
		TEST_EXPECT(blank_host.config.player_slot_limit() == 2u);
		TEST_EXPECT(blank_host.config.unlimited_vehicles == 7);
	}

	std::printf("host_config: ok\n");
	return 0;
}
