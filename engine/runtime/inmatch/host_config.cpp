#include <runtime/inmatch/host_config.h>

#include <formats/def/def.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/inmatch/host_settings.h>

namespace opennova::inmatch {

gamecfg::WeaponRoster game_cfg_weapon_roster(const def::DefWeaponsFile &weapons) {
	gamecfg::WeaponRoster roster;
	// [orig: WeaponDef_LoadAll @0x54DD1C..0x54DD45: the table zeroed, the count
	//  1, row 0 named "None"]
	roster.push_back(gamecfg::WeaponRosterEntry{"None", 0});
	int32_t subclass_rows = 0;
	for (size_t i = 0; i < weapons.count; ++i) {
		const def::DefWeaponDef &w = weapons.entries[i];
		// A row counts only at its `end` (DefWeaponDef::unclosed).
		if (w.unclosed != 0) continue;
		// [orig: WeaponDef_ParseProperty @0x54D79A..0x54D7A2: a running countdown
		//  ends the row as a subclass row; its loadout keys were skipped @0x54D8F1]
		if (subclass_rows != 0) {
			--subclass_rows;
			roster.push_back(gamecfg::WeaponRosterEntry{w.weapon_name, 0});
			continue;
		}
		roster.push_back(gamecfg::WeaponRosterEntry{w.weapon_name, w.loadout_selectable});
		// [orig: @0x54D7B8..0x54D7BF: the row's loadout_subclasses starts the countdown]
		if (w.loadout_subclasses != 0) subclass_rows = w.loadout_subclasses;
	}
	return roster;
}

gamecfg::DefaultTexts game_cfg_default_texts(const rtxt::File *gametext) {
	gamecfg::DefaultTexts texts;
	if (gametext == nullptr) return texts;
	// [orig: GameText_GetStringWithFallback @0x51EB90: the fallback when the
	//  table lacks the entry, else the entry's text]
	if (const rtxt::Entry *entry = gametext->find_in_section("Menu", "UNTITLED"))
		texts.untitled = entry->text;
	if (const rtxt::Entry *entry = gametext->find_in_section("Menu", "USERMESSAGE"))
		texts.user_message = entry->text;
	return texts;
}

HostScreenState host_session_settings(gamecfg::GameCfg &cfg) {
	// In session, the cap goes into 1..65 before the apply, written back into
	// the block [orig: CNapiGameSession_BuildAndCreateSession @0x569554
	// (is_in_session), @0x56955D..0x56955F (below 1 -> 1), @0x56956A..0x56956C
	// (above 65 -> 65)].
	constexpr int32_t kSessionCap = static_cast<int32_t>(kMaxPlayersCap);
	if (cfg.mp_max_players < 1) cfg.mp_max_players = 1;
	else if (cfg.mp_max_players > kSessionCap) cfg.mp_max_players = kSessionCap;

	HostScreenState host;
	GameConfig &config = host.config;
	host.player_limit = cfg.mp_max_players;
	// SERVERTYPE: the cfg's `dedicated` word [orig: dedicatedServer_400, read
	// by the apply's cap @0x551B30].
	host.serve_and_play = cfg.dedicated == 0;
	host.game_type_setting = cfg.mp_gametype;
	// [orig: game_settings.use_lineup_queue @0x569632, lineup_queue_size @0x569646]
	host.use_lineup_queue = cfg.mp_use_lineup_queue;
	host.lineup_queue_size = cfg.mp_lineup_queue_size;

	// The identity. The name is copied 31 bytes deep into both of its homes
	// [orig: Game_ApplySessionSettingsToGlobals @0x551FEB strncpy(g_ServerNameStr,
	// serverName_3A5, 0x1F); CNapiGameSession_BuildAndCreateSession @0x5695D2
	// Napi_CopyString(game_settings.server_name, .., 32)], the passwords into
	// the settings [orig: @0x5695E3, @0x5695F4, @0x569608], the message
	// [orig: @0x551C28 Napi_CopyString(g_ServerCustomText, serverMessage_560,
	// 128)] and the country code [orig: @0x55200D strncpy(g_SessionCountryCode,
	// country_3C5, 3)].
	config.server_name = cfg.game_name.substr(0, 31);
	config.server_password = cfg.mp_host_game_password;
	config.side_a_password = cfg.mp_host_side_password_a;
	config.side_b_password = cfg.mp_host_side_password_b;
	config.custom_text = cfg.servermsg.substr(0, 127);
	config.country = cfg.country.substr(0, 3);
	// The block words the join gate reads directly: the spectator limit and
	// password [orig: maxSpectators_26C and spectatorPassword_270, read by
	// Server_ValidatePlayerJoinRequest @0x51235C / @0x5123E7 and
	// CNapiServerConfig_BuildFlags @0x4C4E82 / @0x4C4E8C], LAN-only
	// [orig: hostLanOnly_340, NapiNPServerMsg_0x002 @0x513134], PunkBuster
	// [orig: game_settings.reserved_D4 = serverPunkBuster_A20 @0x569667].
	config.spectator_slots = cfg.mp_num_spectators_max;
	config.spectator_password = cfg.mp_host_spectator_password;
	config.server_lan_only = cfg.mp_novaworld_host_lan_only;
	config.server_punkbuster = cfg.sv_punkbuster;
	// Two words no session code reads, kept as the host screen keeps them
	// [orig: nwiSpType_540 and enableAi_A14: their only readers are the screen,
	// UI_PopulateHostSettingsFromConfig @0x556187 / @0x556541, and the save].
	config.connection_speed = cfg.nwisptype;
	config.allow_ai = cfg.enable_ai != 0;
	config.max_friendly_kills = cfg.numallowablefriendlykills; // dword_24D2244 @0x551CA4
	// [orig: dword_24D5A00 = timeOfDayContinuity_334 on the authority @0x551E1F]
	config.time_of_day_continuity = cfg.mp_tod_continuity;

	// The published cap: the dedicated slot added [orig: @0x551B2B..0x551B48];
	// in session the cap is already 1..65, so the apply's 65 ceiling never cuts.
	config.max_players = host_player_slot_limit(host.player_limit, host.serve_and_play);
	config.num_teams = static_cast<uint8_t>(cfg.mp_numteams); // g_NumTeamsConfig @0x551BAC
	config.mp_attributes = static_cast<uint32_t>(cfg.mpattrib); // game_settings @0x56967F
	config.fat_bullets = cfg.fatbullets != 0;         // g_FatBullets @0x551C01
	config.one_shot_kill = cfg.oneshotonekill != 0;   // g_OneShotKill @0x551BF5
	config.unlimited_vehicles = cfg.unlimited_vehicles != 0; // dword_24D2258 @0x551D91
	config.voting_enabled = cfg.mpvoting != 0;        // g_VoteKickEnabled @0x551DC3
	config.voting_min_players = cfg.mpvoting_min_players; // @0x551DCF
	config.voting_percent = cfg.mpvoting_percent;     // g_VoteKickPercent @0x551D43
	config.change_team_interval_seconds = cfg.mpchangeteam_interval; // dword_24D2280 @0x551DEB
	config.change_team_penalty_seconds = cfg.mpchangeteam_penalty;   // dword_24D2284 @0x551DF1
	config.auto_balance_enabled = cfg.autobalance_on_recycle_enabled != 0; // @0x551BCE
	config.auto_balance_min_difference = cfg.autobalance_on_recycle_diff_min; // @0x551BDA
	config.auto_balance_trigger_difference = cfg.autobalance_on_recycle_diff_max; // @0x551BEB

	// The rule globals [orig: Game_ApplySessionSettingsToGlobals]: g_RespawnTime
	// from `time_limit` @0x551CD2, g_TimeLimitMinutes from `koth_limit`
	// @0x551CD8..0x551CDF, g_ReplayEnabled @0x551CBA, g_MaxTeamLives @0x551B67,
	// g_ScoreLimit from `max_kills` and g_KillLimit from `max_score`
	// @0x551B6D..0x551B8C, g_KothDelta @0x551CF4, g_FlagReturnTime2 @0x551CFE,
	// g_WeaponViolationLimit from `mp_flagresettime` @0x551D0A.
	config.respawn_time = static_cast<uint32_t>(cfg.time_limit);
	config.time_limit_minutes = session_koth_limit_minutes(cfg.koth_limit);
	config.replay_enabled = static_cast<uint32_t>(cfg.replay);
	config.max_team_lives = static_cast<uint32_t>(cfg.max_team_lives);
	config.score_limit = session_point_limit(cfg.max_kills);
	config.max_score = session_point_limit(cfg.max_score);
	config.koth_delta = static_cast<uint32_t>(cfg.koth_delta);
	config.flag_return_ticks = static_cast<uint32_t>(cfg.flag_return_time);
	config.flag_reset_seconds = cfg.flag_reset_time;
	// Read off the block by their handlers [orig: armoryReuseTime_A38 by
	// NapiNPServerMsg_HandlePlayerLoadout @0x515BA0; balanceJoin_A30 /
	// balanceJoinPercent_A34 by CNapiServer_ProcessPendingPlayerSpawns @0x4C8EFA
	// / @0x4C8F38].
	config.armory_reuse_time = static_cast<uint32_t>(cfg.armory_reuse_time);
	config.balance_join = cfg.balance_join != 0;
	config.balance_join_percent = cfg.balance_join_percent;
	// [orig: g_MinPing / g_DoMinPingCheck / g_MaxPing / g_DoMaxPingCheck
	//  @0x551C3E..0x551C5D]
	config.do_min_ping_check = cfg.dominpingcheck != 0;
	config.min_ping = static_cast<uint32_t>(cfg.minping);
	config.do_max_ping_check = cfg.domaxpingcheck != 0;
	config.max_ping = static_cast<uint32_t>(cfg.maxping);
	config.multiplayer_reset = cfg.mp_reset;
	// [orig: net_config.max_packet_bytes = maxPacketSize_338 @0x569840;
	//  CNapiNetwork_Init @0x4CAA53]
	config.max_packet_size = cfg.mp_max_packet_size;
	// [orig: g_CaptureDuration from `teamchange_time` @0x551D65,
	//  g_CaptureSpeedSetting @0x551D6F, g_SpawnWaveTimeBase / Zone @0x551D7B /
	//  @0x551D85, g_RespawnRequiresTeamDead from `nodefaultspawnpoints` @0x551DA7]
	config.capture_duration_seconds = cfg.teamchange_time;
	config.capture_speed_setting = cfg.mp_lfp_takeoverspeed;
	config.spawn_wave_time_base = cfg.spawnregulator_psp;
	config.spawn_wave_time_zone = cfg.spawnregulator_lfp;
	config.default_spawn_requires_no_team_zone = static_cast<uint32_t>(cfg.nodefaultspawnpoints);
	// [orig: g_RespawnTimeout from `timeout` on the authority @0x551E8B,
	//  g_StartDelay @0x551BB8, g_DestroyBuildings @0x551BC4, g_DeathMessages
	//  @0x551CC6]
	config.respawn_timeout = static_cast<uint32_t>(cfg.timeout);
	config.start_delay = static_cast<uint32_t>(cfg.startdelay);
	config.destroy_buildings = static_cast<uint32_t>(cfg.destroybuild);
	config.death_messages = static_cast<uint32_t>(cfg.deathmes);
	// The session difficulty word, the S2C 0x08 block's seventh byte
	// [orig: dword_24D2110 = multiplayerDifficulty_484 on the authority
	// @0x551E80 / @0x551F75].
	config.config_bytes[6] = static_cast<uint8_t>(cfg.mp_difficulty);
	// [orig: byte_A821EF = permanentDeath_5E4 @0x5522AA, byte_A821F0 =
	//  allowSniperScopeZoom_5EC != 0 @0x5522B9, both in session on the authority]
	config.permanent_death = cfg.mp_permanent_death != 0;
	config.allow_sniper_scope_zoom = cfg.mp_allowsniperscopezoom != 0;
	// The LAN send cadence reads the block's word [orig:
	// NapiNPServer_GetSendHoldoffTicks @0x4C4AD1, lanMode_544].
	config.lan_mode = static_cast<uint32_t>(cfg.lanmode);
	return host;
}

} // namespace opennova::inmatch
