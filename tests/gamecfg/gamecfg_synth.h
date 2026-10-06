// The synthetic game.cfg the minted fixture fixtures/gamecfg/synth_game.cfg is
// written from (tests/fixtures/minimal_gamecfg_gen.cpp) and the weapon roster
// both that generator and gamecfg_roundtrip read it with. Every key the writer
// emits holds a value off its default, inside the reader's folds, so a read
// lands on the same model; the write-only keys (flagreturntime, side_password,
// side_req) keep their defaults, since a read cannot restore them.
#pragma once

#include <formats/gamecfg/game_cfg.h>

namespace gamecfg_synth {

// Four weapon.def rows: three selectable, one not (it writes no line).
inline opennova::gamecfg::WeaponRoster roster() {
	return {
		{ "WPN_SynPistol", 1 },
		{ "WPN_SynRifle", 1 },
		{ "WPN_SynHidden", 0 },
		{ "WPN_SynLauncherLong", 1 },
	};
}

inline opennova::gamecfg::GameCfg config() {
	using namespace opennova::gamecfg;
	DefaultTexts texts;
	texts.untitled = "Untitled";
	texts.user_message = "Put your message here.";
	GameCfg c = defaults(texts, 2, 1);

	c.windowed = 1;
	c.hw3d_name = "Synthetic Adapter 3000";
	c.hw3d_guid = "00000000-1111-2222-333344445555";
	c.video_res = "1024x768";
	c.gamma = 1.5f;
	c.terrain_polydetail = 2;
	c.terrain_texdetail = 3;
	c.object_polydetail = 1;
	c.object_texdetail = 2;
	c.display_16x9 = 2;
	c.water_quality = 3;
	c.shadow_quality = 1;
	c.particle_density = 2;
	c.antialias_mode = 4;
	c.texfilter_level = 2;
	c.fbeffects_level = 1;
	c.shader_usage_level = 2;
	c.texcompression_level = 1;
	c.lock_framerate = 0;
	c.force_vsync = 1;
	c.reduce_mouselag = 0;
	c.enable_keyboardtips = 0;
	c.enable_gameplaytips = 0;

	c.windows_volume = 200;
	c.music_volume = 128;
	c.sfx_level = 160;
	c.dialogue_level = 224;
	c.rotor_volume = 96;
	c.audio_channels = 4;
	c.audio_rate = 22050;
	c.no_blood = 1;
	c.no_casings = 1;
	c.no_smoke = 1;

	c.crosshairs = 0;
	c.crosshairs_color = 3;
	c.hitfeedback = 0;
	c.showgun = 1;
	c.hud_color_index = 4;
	c.hud_detail = 2;

	c.mp_ip_address_string = "192.168.7.20";
	c.mp_gate_port_min = 50000;
	c.mp_gate_port_max = 50100;
	c.mp_gate_port_delta = 2;
	c.mp_gate_port_random = 1;
	c.mp_novaworld_port_min = 33000;
	c.mp_novaworld_port_max = 33100;
	c.mp_novaworld_port_delta = 3;
	c.mp_novaworld_port_random = 1;
	c.mp_lan_enum_port_min = 34000;
	c.mp_lan_enum_port_max = 34100;
	c.mp_lan_enum_port_delta = 4;
	c.mp_lan_server_port_min = 32800;
	c.mp_lan_server_port_max = 32819;
	c.mp_lan_server_port_delta = 5;
	c.mp_lan_client_port_min = 35000;
	c.mp_lan_client_port_max = 35100;
	c.mp_lan_client_port_delta = 6;
	c.mp_lan_client_port_random = 1;
	c.mp_server_to_join_port_min = 32810;
	c.mp_server_to_join_port_max = 32829;
	c.mp_server_to_join_port_delta = 7;
	c.mp_max_players = 24;
	c.mp_use_lineup_queue = 0;
	c.mp_lineup_queue_size = 40;
	c.mp_host_game_password = "gamepw";
	c.mp_host_side_password_a = "bluepw";
	c.mp_host_side_password_b = "redpw";
	c.mp_num_spectators_max = 3;
	c.mp_host_spectator_password = "watchers";
	c.mp_access_code_list = "alpha, bravo";
	c.mp_host_punt_same_pcids = 0;
	c.mp_tod_continuity = 1;
	c.mp_max_packet_size = 1200;
	c.mp_extract_extended_metric_information = 0;
	c.mp_novaworld_host_lan_only = 1;

	c.mp_eula_accepted = 1;
	c.mpattrib = 14854;
	c.join_password = "joinme";
	c.game_name = "Synthetic Server";
	c.internet_address = "10.0.0.5";
	c.networkconnecttype = 2;
	c.mp_difficulty = 1;
	c.mp_gametype = 65540;
	c.dedicated = 1;
	c.max_team_lives = 40;
	c.max_kills = 25;
	c.max_score = 3;
	c.startdelay = 45;
	c.destroybuild = 1;
	c.autobalance_on_recycle_enabled = 1;
	c.autobalance_on_recycle_diff_min = 2;
	c.autobalance_on_recycle_diff_max = 4;
	c.oneshotonekill = 1;
	c.fatbullets = 1;
	c.nwisptype = 7;
	c.lanmode = 3;
	c.allowcustomskins = 1;
	c.servermsg = "Welcome to the synthetic server, play fair";
	c.minping = 10;
	c.dominpingcheck = 1;
	c.maxping = 900;
	c.domaxpingcheck = 1;
	c.dirtyupstream = 0;
	c.dirtyupstreampost = 0;
	c.deathmes = 0;
	c.numallowablefriendlykills = 5;
	c.replay = 0;
	c.time_limit = 45;
	c.koth_limit = 12;
	c.koth_delta = 6;
	c.timeout = 8;
	c.mp_numteams = 4;
	c.contest = 1;
	c.ping = 0;
	c.sendplayerlist = 0;
	c.rememberlogin = 0;
	c.teamchange_time = 20;
	c.mp_lfp_takeoverspeed = 2;
	c.spawnregulator_psp = 3;
	c.spawnregulator_lfp = 12;
	c.unlimited_vehicles = 0;
	c.choosespawnonbegin = 1;
	c.nodefaultspawnpoints = 1;
	c.mp_allowsniperscopezoom = 1;
	c.mp_permanent_death = 1;
	c.mp_3rdperson_driver = 0;
	c.mpvoting = 1;
	c.mpvoting_min_players = 4;
	c.mpvoting_percent = 0.75f;
	c.mpvoting_period = 120;
	c.mpchangeteam = 1;
	c.mpchangeteam_interval = 240;
	c.mpchangeteam_penalty = 30;
	c.mp_verbose = 0;
	c.mp_no_char_abilities = 1;
	c.mp_no_crosshair_spread = 1;
	c.mp_no_scope_drift = 1;
	c.mp_no_weapon_recoil = 1;
	c.mp_gpsicons = 1;
	c.mp_wind = 1;
	c.mp_dropped_weapon_disappear = 1;
	c.mp_no_drop_weapons = 1;
	c.mp_no_respawn_with_primary = 1;
	c.enable_ai = 0;
	c.remote_admin_port = 4711;
	c.cl_punkbuster = 1;
	c.sv_punkbuster = 1;
	c.xhair_appearance = 2;
	c.xhair_color = 0x00FF00;
	c.xhair_spread = 0;
	c.balance_join = 0;
	c.balance_join_percent = 0.25f;
	c.armory_reuse_time = 45;
	c.farp_reuse_time = 60;

	c.class_availability[kClassRifleman] = 2;
	c.class_availability[kClassSniper] = 0;
	c.class_availability[kClassMedic] = 1;
	c.class_availability[kClassGunner] = 2;
	c.class_availability[kClassEngineer] = 0;
	c.weapon_availability[0] = 1;
	c.weapon_availability[1] = 2;
	c.weapon_availability[3] = 0; // row 2 is unselectable: no line, its default stays

	c.map_infrared = 0;
	c.map_iff = 0;
	c.map_awac = 0;

	c.preempt_pff = 1;
	c.country = "NZ";
	c.msg = "synthetic note";
	c.play_exit_credits = 0;
	c.no_anim = 1;
	c.alt_lock = 0;
	c.alt_lock_type = 2;
	return c;
}

} // namespace gamecfg_synth
