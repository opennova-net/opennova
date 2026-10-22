// game.cfg onto the host session (runtime/inmatch/host_config.h): the weapon
// table the avail_wpn keys address, the defaults' localized texts, and the
// session the host creates from the cfg block (CNapiGameSession_BuildAndCreateSession's
// cap, Game_ApplySessionSettingsToGlobals' arms).
#include <base/gameprofile/game_type.h>
#include <formats/def/def.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/playersav/player_sav.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_role.h> // singleplayer_game_config
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/punt_log.h>    // ConnectLog (the socket open's lines)
#include <runtime/inmatch/server_files.h> // ServerFileSink

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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
		const gamecfg::DefaultTexts none = game_cfg_default_texts(nullptr, nullptr);
		TEST_EXPECT(none.untitled == "!Untitled");
		TEST_EXPECT(none.user_message == "!Put your message here");
		rtxt::File table;
		table.sections.push_back(rtxt::Section{"Menu", 1});
		rtxt::Entry entry;
		entry.key = "UNTITLED";
		entry.text = "Untitled";
		table.entries.push_back(entry);
		const gamecfg::DefaultTexts texts = game_cfg_default_texts(&table, nullptr);
		TEST_EXPECT(texts.untitled == "Untitled");
		TEST_EXPECT(texts.user_message == "!Put your message here");
		// The override table's entry wins over the gametext's, and a key the
		// override lacks still reads the gametext's (GameText_GetStringWithFallback
		// @0x51EBB6 -> TextResource_FindEntryBySectionAndKey @0x75D27B); with no
		// gametext table the override is never searched (@0x51EB97).
		rtxt::File override_table;
		override_table.sections.push_back(rtxt::Section{"Menu", 1});
		rtxt::Entry message;
		message.key = "USERMESSAGE";
		message.text = "Expansion message";
		override_table.entries.push_back(message);
		const gamecfg::DefaultTexts over = game_cfg_default_texts(&table, &override_table);
		TEST_EXPECT(over.untitled == "Untitled");
		TEST_EXPECT(over.user_message == "Expansion message");
		const gamecfg::DefaultTexts no_gametext = game_cfg_default_texts(nullptr, &override_table);
		TEST_EXPECT(no_gametext.user_message == "!Put your message here");
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
		TEST_EXPECT(c.round_time_limit_minutes == screen.round_time_limit_minutes);
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
		TEST_EXPECT(c.use_lineup_queue == screen.use_lineup_queue &&
				c.lineup_queue_size == screen.lineup_queue_size);
		TEST_EXPECT(c.use_lineup_queue == 1 && c.lineup_queue_size == 100);
		TEST_EXPECT(c.punt_duplicate_pcids == screen.punt_duplicate_pcids &&
				c.punt_duplicate_pcids == 1);
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
		cfg.mp_host_punt_same_pcids = 0;
		const HostScreenState host = host_session_settings(cfg);
		const GameConfig &c = host.config;
		TEST_EXPECT(cfg.mp_max_players == 65); // the in-session cap, written back
		TEST_EXPECT(host.player_limit == 65 && !host.serve_and_play);
		// The session's cap is the advertised and admitted one; only the slot
		// limit carries the dedicated host's own slot.
		TEST_EXPECT(c.max_players == 65u && c.dedicated_server);
		TEST_EXPECT(c.player_slot_limit() == 66u); // 65 plus the dedicated slot
		TEST_EXPECT(host.game_type_setting == 65540);
		TEST_EXPECT(c.use_lineup_queue == 0 && c.lineup_queue_size == 12);
		TEST_EXPECT(c.punt_duplicate_pcids == 0);
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
		TEST_EXPECT(c.round_time_limit_minutes == 45u);
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

	// --- the game shell's in-process host record (shell_host_config): a LAN
	//     host's from its host-screen config, else single player's from the
	//     profile record; the cfg block's create words over either, the same
	//     words host_session_settings copies.
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		block.mp_reset = 1;
		block.mp_use_lineup_queue = 1;
		block.mp_lineup_queue_size = 6;
		block.mp_host_punt_same_pcids = 1;
		playersav::ProfileRecord profile{};
		profile.auto_medic_off = 1;

		GameConfig lan;
		lan.server_name.clear();
		lan.game_type = game_type::kTeamDeathmatch;
		ShellHostSource source;
		source.lan_config = &lan;
		source.lan_max_players = 9;
		source.lan_serve_and_play = false;
		source.lan_network_type = NetworkType::NovaWorld;
		source.profile = &profile;
		source.cfg_block = &block;
		HostConfig host = shell_host_config(source);
		// An empty name advertises "": the create copies the block word with no default
		// [orig: CNapiGameSession_BuildAndCreateSession @0x5695D2].
		TEST_EXPECT(host.config.server_name.empty());
		TEST_EXPECT(host.config.game_type == game_type::kTeamDeathmatch);
		TEST_EXPECT(host.config.max_players == 9u && host.config.dedicated_server);
		TEST_EXPECT(host.socket_mode == SocketMode::Lan && !host.serve_and_play);
		TEST_EXPECT(host.network_type == NetworkType::NovaWorld);
		TEST_EXPECT(host.config.multiplayer_reset == 1 && host.config.use_lineup_queue == 1 &&
				host.config.lineup_queue_size == 6 && host.config.punt_duplicate_pcids == 1);
		TEST_EXPECT(host.local_auto_medic_disabled == 1u);
		// The same words host_session_settings takes from the same block.
		const HostScreenState from_block = host_session_settings(block);
		TEST_EXPECT(from_block.config.multiplayer_reset == host.config.multiplayer_reset &&
				from_block.config.use_lineup_queue == host.config.use_lineup_queue &&
				from_block.config.lineup_queue_size == host.config.lineup_queue_size &&
				from_block.config.punt_duplicate_pcids == host.config.punt_duplicate_pcids);

		// Single player: the profile's session record, socketless, serve-and-play.
		ShellHostSource sp;
		sp.sp_game_type = game_type::kObjectiveCoop;
		sp.profile = &profile;
		host = shell_host_config(sp);
		const GameConfig expected = singleplayer_game_config(game_type::kObjectiveCoop, &profile);
		TEST_EXPECT(host.config.game_type == expected.game_type);
		TEST_EXPECT(host.socket_mode == SocketMode::Socketless && host.serve_and_play);
		TEST_EXPECT(host.network_type == NetworkType::Lan);
		TEST_EXPECT(host.config.multiplayer_reset == 0); // no block: the stock word
		TEST_EXPECT(host.local_auto_medic_disabled == 1u);
		sp.profile = nullptr;
		TEST_EXPECT(shell_host_config(sp).local_auto_medic_disabled == 0u);
		// Its mission file is the selected list entry's name, which its start copies into
		// g_MapFileName (the admin's CURRENT MAP, the session title's key); a load naming no
		// file keeps the config's. [orig: SinglePlayer_StartMission @0x561AF0 (the copy
		//  @0x561B69..0x561B80)]
		sp.sp_mission_file = "TRAIN01.BMS";
		TEST_EXPECT(shell_host_config(sp).config.mission_file == "TRAIN01.BMS");
		sp.sp_mission_file.clear();
		TEST_EXPECT(shell_host_config(sp).config.mission_file == GameConfig{}.mission_file);
		// Single player carries game.cfg's zoom option for the fill
		// [orig: g_GameConfigState.allowSniperScopeZoom_5EC @0x53EF1F].
		gamecfg::GameCfg zoom_block;
		zoom_block.mp_allowsniperscopezoom = 1;
		sp.cfg_block = &zoom_block;
		TEST_EXPECT(shell_host_config(sp).config.allow_sniper_scope_zoom);
	}

	// The game shell's host request is the block as it stands (host_screen_from_block): the
	// name, the rules, the attribute word, the cap unclamped (the session create clamps it).
	// [orig: UI_HandleHostSessionStart @0x556E36 / @0x556E72 -> CNapiGameSession_BuildAndCreateSession]
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		block.game_name = "Night Ops";
		block.mp_max_players = 0;
		block.mpattrib = 0x3A06;
		block.time_limit = 25;
		const HostScreenState request = host_screen_from_block(block);
		TEST_EXPECT(request.config.server_name == "Night Ops");
		TEST_EXPECT(request.config.mp_attributes == 0x3A06u);
		// time_limit is the round's minutes, g_RoundTimeLimitMinutes, which the mission start seeds
		// the round clock from (world::MatchRules::game_time_minutes, 3720 ticks a minute)
		// [orig: Game_ApplySessionSettingsToGlobals @0x551CCC..0x551CD2; Game_StartMission
		//  @0x525242..0x525251].
		TEST_EXPECT(request.config.round_time_limit_minutes == 25);
		TEST_EXPECT(request.player_limit == 0 && block.mp_max_players == 0);
		TEST_EXPECT(host_session_settings(block).player_limit == 1 && block.mp_max_players == 1);
		// The game's request takes the same clamp into the process block: a cap of 80 leaves 65
		// there, a cap of 0 leaves 1 [orig: CNapiGameSession_BuildAndCreateSession @0x56955F,
		// @0x56956C into maxPlayers_3F4].
		block.mp_max_players = 80;
		TEST_EXPECT(store_session_player_cap(&block, block.mp_max_players, gameprofile::GAME_JO) == 65 &&
				block.mp_max_players == 65);
		TEST_EXPECT(store_session_player_cap(&block, 0, gameprofile::GAME_JO) == 1 &&
				block.mp_max_players == 1);
		TEST_EXPECT(store_session_player_cap(nullptr, 99, gameprofile::GAME_JO) == 65);
		// A fresh block (Config_SetDefaults' words) makes the host screen's rule baseline.
		const GameConfig fresh = host_screen_from_block(gamecfg::defaults()).config;
		const GameConfig baseline = host_screen_default_config();
		TEST_EXPECT(fresh.round_time_limit_minutes == baseline.round_time_limit_minutes);
		TEST_EXPECT(fresh.time_limit_minutes == baseline.time_limit_minutes);
		TEST_EXPECT(fresh.replay_enabled == baseline.replay_enabled);
		TEST_EXPECT(fresh.max_team_lives == baseline.max_team_lives);
		TEST_EXPECT(fresh.score_limit == baseline.score_limit);
		TEST_EXPECT(fresh.max_score == baseline.max_score);
		TEST_EXPECT(fresh.koth_delta == baseline.koth_delta);
		TEST_EXPECT(fresh.flag_return_ticks == baseline.flag_return_ticks);
		TEST_EXPECT(fresh.flag_reset_seconds == baseline.flag_reset_seconds);
		TEST_EXPECT(fresh.armory_reuse_time == baseline.armory_reuse_time);
		TEST_EXPECT(fresh.capture_duration_seconds == baseline.capture_duration_seconds);
		TEST_EXPECT(fresh.capture_speed_setting == baseline.capture_speed_setting);
		TEST_EXPECT(fresh.spawn_wave_time_base == baseline.spawn_wave_time_base);
		TEST_EXPECT(fresh.spawn_wave_time_zone == baseline.spawn_wave_time_zone);
		TEST_EXPECT(fresh.num_teams == baseline.num_teams);
		TEST_EXPECT(fresh.respawn_timeout == baseline.respawn_timeout);
		TEST_EXPECT(fresh.start_delay == baseline.start_delay);
		TEST_EXPECT(fresh.destroy_buildings == baseline.destroy_buildings);
		TEST_EXPECT(fresh.death_messages == baseline.death_messages);
		TEST_EXPECT(fresh.mp_attributes == baseline.mp_attributes);
		TEST_EXPECT(fresh.class_allow_mask == baseline.class_allow_mask);
		TEST_EXPECT(fresh.lan_mode == baseline.lan_mode);
		TEST_EXPECT(fresh.spectator_slots == baseline.spectator_slots);
	}

	// The session socket's bind scan over the block's quads: a LAN authority the server quad
	// with random 0, a LAN non-authority the client quad, a NovaWorld session either side the
	// mpnovaworld quad; a first port in place of the min; a random word starts the scan at a
	// drawn port of the same wrapping walk.
	// [orig: CNapiNetwork_OpenTransportSocket @0x4C6A40 - @0x4C6AA2..0x4C6AB4,
	//  @0x4C6AD9..0x4C6AF0, @0x4C6AF6..0x4C6B08; NapiUdpSocket_CreateAndBind @0x62D2A0]
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		block.mp_lan_server_port_min = 40000;
		block.mp_lan_server_port_max = 40004;
		block.mp_lan_server_port_delta = 2;
		block.mp_lan_client_port_min = 41000;
		block.mp_lan_client_port_max = 41001;
		block.mp_lan_client_port_delta = 1;
		block.mp_novaworld_port_min = 42000;
		block.mp_novaworld_port_max = 42002;
		block.mp_novaworld_port_delta = 1;
		const std::vector<uint16_t> server =
				transport_bind_ports(block, NetworkType::Lan, /*authority=*/true);
		TEST_EXPECT((server == std::vector<uint16_t>{40000, 40002}));
		TEST_EXPECT((transport_bind_ports(block, NetworkType::Lan, true, 40001) ==
				std::vector<uint16_t>{40001, 40003}));
		TEST_EXPECT((transport_bind_ports(block, NetworkType::Lan, false) ==
				std::vector<uint16_t>{41000, 41001}));
		TEST_EXPECT((transport_bind_ports(block, NetworkType::NovaWorld, true) ==
				std::vector<uint16_t>{42000, 42001, 42002}));
		TEST_EXPECT((transport_bind_ports(block, NetworkType::NovaWorld, false) ==
				std::vector<uint16_t>{42000, 42001, 42002}));
		// The server quad ignores the client quad's random word.
		block.mp_lan_client_port_random = 1;
		TEST_EXPECT((transport_bind_ports(block, NetworkType::Lan, true) == server));
		// A random NovaWorld scan is the same three ports, rotated to its drawn start.
		block.mp_novaworld_port_random = 1;
		const std::vector<uint16_t> drawn =
				transport_bind_ports(block, NetworkType::NovaWorld, false);
		TEST_EXPECT(drawn.size() == 3);
		if (drawn.size() == 3) {
			for (std::size_t i = 0; i < 3; ++i)
				TEST_EXPECT(drawn[i] == 42000 + (drawn[0] - 42000 + i) % 3);
		}
		// The quad the socket open stores: the arm's words, the first port over the min.
		const TransportPortQuad nw = transport_port_quad(block, NetworkType::NovaWorld, true);
		TEST_EXPECT(nw.min == 42000 && nw.max == 42002 && nw.delta == 1 && nw.random == 1);
		const TransportPortQuad lan = transport_port_quad(block, NetworkType::Lan, true, 40001);
		TEST_EXPECT(lan.min == 40001 && lan.max == 40004 && lan.delta == 2 && lan.random == 0);
	}

	// The socket's bind address is the block's mpipaddressstring with DNS off, and the two
	// connect-log result lines name it [orig: CNapiNetwork_OpenTransportSocket @0x4C6B12;
	// Napi_ResolveAddress @0x62DF90; the lines @0x4C6BF2..0x4C6C6B, @0x4C6C7B..0x4C6CFA].
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		TEST_EXPECT(transport_bind_address(block) == 0u); // the shipped "0.0.0.0"
		block.mp_ip_address_string = "10.1.2.3";
		TEST_EXPECT(transport_bind_address(block) == 0x0302010Au);
		block.mp_ip_address_string = "a.host.name"; // no DNS: every interface
		TEST_EXPECT(transport_bind_address(block) == 0u);
		block.mp_ip_address_string = "255.255.255.255";
		TEST_EXPECT(transport_bind_address(block) == 0xFFFFFFFFu);
		const TransportPortQuad quad{40000, 40004, 2, 0};
		TEST_EXPECT(transport_socket_error_text(0x0302010Au, quad) ==
				"NET SOCKET SETUP ERROR!!!!!!!!  CANNOT CREATE SOCKET WITH PARAMS: IP=10.1.2.3, "
				"PORTMIN=40000 PORTMAX=40004, PORTDELTA=2, PORTRANDOM=0!!!!!");
		TEST_EXPECT(transport_socket_setup_text(0u, 40002) == "NET SOCKET SETUP [0.0.0.0:40002]");
	}

	// One socket open's lines in order: SETTING UP, then the bound socket's SETUP; a scan that
	// bound nothing writes the failure's line and its SetTransportMode(0)'s TEARING DOWN; a log
	// that is off writes none (D-NET-NEW-L1-30). [orig: CNapiNetwork_SetTransportMode @0x4C8750 -
	// @0x4C8796, @0x4C87A3, @0x4C877B; CNapiNetwork_OpenTransportSocket @0x4C6A40 - @0x4C6BD7,
	// @0x4C6C6B, @0x4C6CFA; CNapiNetwork_TearDownSocket @0x4C4C90 - @0x4C4CE7]
	{
		struct MemoryFiles final : ServerFileSink {
			std::string text;
			bool exists(const std::string &) override { return !text.empty(); }
			void remove(const std::string &) override { text.clear(); }
			bool write(const std::string &, std::string_view bytes, bool append) override {
				if (!append) text.clear();
				text.append(bytes.data(), bytes.size());
				return true;
			}
		};
		MemoryFiles files;
		ConnectLog log(files);
		log.set_clock([] { return std::string("T"); });
		log.start(ConnectLogHeader{});
		files.text.clear();
		const TransportPortQuad quad{32768, 32772, 1, 0};
		const opennova::PeerAddr bound{0u, 32770};
		write_transport_socket_open_lines(log, &bound, 0u, quad);
		TEST_EXPECT(files.text ==
				"T : NET SETTING UP SOCKET....\r\nT : NET SOCKET SETUP [0.0.0.0:32770]\r\n");
		files.text.clear();
		write_transport_socket_open_lines(log, nullptr, 0x0302010Au, quad);
		TEST_EXPECT(files.text ==
				"T : NET SETTING UP SOCKET....\r\n"
				"T : NET SOCKET SETUP ERROR!!!!!!!!  CANNOT CREATE SOCKET WITH PARAMS: "
				"IP=10.1.2.3, PORTMIN=32768 PORTMAX=32772, PORTDELTA=1, PORTRANDOM=0!!!!!\r\n"
				"T : NET TEARING DOWN SOCKET\r\n");
		files.text.clear();
		log.disable_at_server_connection();
		files.text.clear();
		write_transport_socket_open_lines(log, &bound, 0u, quad);
		TEST_EXPECT(files.text.empty());
	}

	// The NovaWorld menu's entry opens the session socket in mode 4 with the same lines, its
	// failure over the block's mpnovaworld quad (the non-authority's NovaWorld arm, random word
	// included) and its mpipaddressstring; the session's close tears an open socket down with its
	// line and writes nothing for a socket that is not open, a log that is off or no log.
	// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - SetTransportMode(4) @0x558EA3;
	//  UI_OnStartupScreenActivate @0x5557F0 - SetTransportMode(0) @0x55582B;
	//  CNapiNetwork_SetTransportMode @0x4C8750 - @0x4C877B; CNapiNetwork_TearDownSocket
	//  @0x4C4C90 - @0x4C4CE7]
	{
		struct MemoryFiles final : ServerFileSink {
			std::string text;
			bool exists(const std::string &) override { return !text.empty(); }
			void remove(const std::string &) override { text.clear(); }
			bool write(const std::string &, std::string_view bytes, bool append) override {
				if (!append) text.clear();
				text.append(bytes.data(), bytes.size());
				return true;
			}
		};
		MemoryFiles files;
		ConnectLog log(files);
		log.set_clock([] { return std::string("T"); });
		log.start(ConnectLogHeader{});
		files.text.clear();
		gamecfg::GameCfg block = gamecfg::defaults();
		block.mp_ip_address_string = "10.1.2.3";
		block.mp_lan_client_port_min = 41000; // the LAN client quad is not the session's
		block.mp_lan_client_port_max = 41001;
		block.mp_novaworld_port_min = 42000;
		block.mp_novaworld_port_max = 42002;
		block.mp_novaworld_port_delta = 1;
		block.mp_novaworld_port_random = 1;
		const opennova::PeerAddr bound{0x0302010Au, 42001};
		write_novaworld_socket_open_lines(log, block, &bound);
		TEST_EXPECT(files.text ==
				"T : NET SETTING UP SOCKET....\r\nT : NET SOCKET SETUP [10.1.2.3:42001]\r\n");
		files.text.clear();
		write_novaworld_socket_open_lines(log, block, nullptr);
		TEST_EXPECT(files.text ==
				"T : NET SETTING UP SOCKET....\r\n"
				"T : NET SOCKET SETUP ERROR!!!!!!!!  CANNOT CREATE SOCKET WITH PARAMS: "
				"IP=10.1.2.3, PORTMIN=42000 PORTMAX=42002, PORTDELTA=1, PORTRANDOM=1!!!!!\r\n"
				"T : NET TEARING DOWN SOCKET\r\n");
		files.text.clear();
		write_transport_socket_teardown_line(&log, /*socket_open=*/true);
		TEST_EXPECT(files.text == "T : NET TEARING DOWN SOCKET\r\n");
		files.text.clear();
		write_transport_socket_teardown_line(&log, /*socket_open=*/false);
		write_transport_socket_teardown_line(nullptr, /*socket_open=*/true);
		TEST_EXPECT(files.text.empty());
		log.disable_at_server_connection();
		files.text.clear();
		write_transport_socket_teardown_line(&log, /*socket_open=*/true);
		write_novaworld_socket_open_lines(log, block, &bound);
		TEST_EXPECT(files.text.empty());
	}

	// The gate socket's scan: the block's mpgateserverlocalport quad whatever the network type,
	// the shipped 49152 / 65536 / 1 / 0 clamped to 49152..65535 from 49152, a random word
	// starting the same walk at a drawn port. [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 -
	// @0x4CE7D2, @0x4CE7DE..0x4CE810; CNapiGateManager_CreateAndBindSocket @0x633770 -
	// @0x6337D4..0x633824; NapiUdpSocket_CreateAndBind @0x62D2A0]
	{
		gamecfg::GameCfg block = gamecfg::defaults();
		const TransportPortQuad shipped = gate_port_quad(block);
		TEST_EXPECT(shipped.min == 49152 && shipped.max == 65536 && shipped.delta == 1 &&
				shipped.random == 0);
		const std::vector<uint16_t> ports = gate_bind_ports(block);
		TEST_EXPECT(ports.size() == 16384 && ports.front() == 49152 && ports.back() == 65535);
		block.mp_gate_port_min = 50000;
		block.mp_gate_port_max = 50004;
		block.mp_gate_port_delta = 2;
		block.mp_novaworld_port_min = 42000; // the session socket's quad is not the gate's
		TEST_EXPECT((gate_bind_ports(block) == std::vector<uint16_t>{50000, 50002}));
		block.mp_gate_port_delta = 1;
		block.mp_gate_port_max = 50002;
		block.mp_gate_port_random = 1;
		const std::vector<uint16_t> drawn = gate_bind_ports(block);
		TEST_EXPECT(drawn.size() == 3);
		if (drawn.size() == 3) {
			for (std::size_t i = 0; i < 3; ++i)
				TEST_EXPECT(drawn[i] == 50000 + (drawn[0] - 50000 + i) % 3);
		}
	}

	std::printf("host_config: ok\n");
	return 0;
}
