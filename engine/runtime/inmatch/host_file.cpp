#include <runtime/inmatch/host_file.h>

#include <base/gameprofile/game_type.h>
#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/gamecfg/game_cfg.h>

#include <string_view>
#include <utility>

namespace opennova::inmatch {

namespace {

using gamecfg::GameCfg;

// `strncpy(field, value, n)` into a block field of `capacity` bytes. A value
// shorter than n is copied with its NUL; a longer one fills n bytes and no
// NUL, so the field's old bytes past n still read as part of it. GameCfg holds
// a field's text up to its size less one byte (D-GAMECFG-1's bound): where the
// copy fills the field, retail's text runs on into the next field.
void strncpy_field(std::string &field, const std::string &value, size_t n, size_t capacity) {
	if (value.size() < n) {
		field = value;
	} else {
		std::string out = value.substr(0, n);
		if (field.size() > n) out.append(field, n, std::string::npos);
		field = std::move(out);
	}
	if (field.size() > capacity - 1) field.resize(capacity - 1);
}

// The text keys: each strncpy's count and its field [orig:
// ServerConfig_ApplyHostSetting @0x4A6000 -- GameName strncpy 0x20 ->
// serverName_3A5 (+0x3A5, char[32]) @0x4A6010..0x4A6038, MPHostGamePassword
// 0x11 -> hostGamePassword_350 (+0x350, [17]) @0x4A603C, ServerMessage 0x80 ->
// serverMessage_560 (+0x560, [128]) @0x4A6068, GameLocation 3 -> country_3C5
// (+0x3C5, [8]) @0x4A6097, MPHostSidePasswordA 0x11 -> sideAPassword_383
// (+0x383, [17]) @0x4A645F, MPHostSidePasswordB 0x11 -> sideBPassword_394
// (+0x394, [17]) @0x4A648B].
struct TextKey {
	std::string_view key;
	std::string GameCfg::*field;
	size_t count;
	size_t capacity;
};
const TextKey kTextKeys[] = {
	{"GameName", &GameCfg::game_name, 0x20, 32},
	{"MPHostGamePassword", &GameCfg::mp_host_game_password, 0x11, 17},
	{"ServerMessage", &GameCfg::servermsg, 0x80, 128},
	{"GameLocation", &GameCfg::country, 3, 8},
	{"MPHostSidePasswordA", &GameCfg::mp_host_side_password_a, 0x11, 17},
	{"MPHostSidePasswordB", &GameCfg::mp_host_side_password_b, 0x11, 17},
};

// The number keys: atol into the field [orig: ServerConfig_ApplyHostSetting
// -- ConnectionSpeed -> nwiSpType_540 @0x4A60C3..0x4A60E1, GameType ->
// gameType_3F0 @0x4A60ED..0x4A6113, Replay -> unknownModeOption_46C (cfg
// `replay`) @0x4A6117, Delay -> startDelay_410 @0x4A6141, Respawn ->
// respawnTimeout_47C (cfg `timeout`) @0x4A616B, Time_Limit -> timeLimit_470
// @0x4A6195, KillLimit -> maxKills_408 @0x4A61BF, MaxScore -> maxScore_40C
// @0x4A61E9, UseLineUpQueue -> useLineupQueue_3F8 @0x4A624F..0x4A6275,
// LineUpQueueSize -> lineupQueueSize_3FC @0x4A6279..0x4A629F, MaxFFKills ->
// allowableFriendlyKills_5E0 @0x4A62A3, TakeoverTime -> teamChangeTime_4C0
// (cfg `teamchange_time`) @0x4A62CD].
struct NumberKey {
	std::string_view key;
	int32_t GameCfg::*field;
};
const NumberKey kNumberKeys[] = {
	{"ConnectionSpeed", &GameCfg::nwisptype},
	{"GameType", &GameCfg::mp_gametype},
	{"Replay", &GameCfg::replay},
	{"Delay", &GameCfg::startdelay},
	{"Respawn", &GameCfg::timeout},
	{"Time_Limit", &GameCfg::time_limit},
	{"KillLimit", &GameCfg::max_kills},
	{"MaxScore", &GameCfg::max_score},
	{"UseLineUpQueue", &GameCfg::mp_use_lineup_queue},
	{"LineUpQueueSize", &GameCfg::mp_lineup_queue_size},
	{"MaxFFKills", &GameCfg::numallowablefriendlykills},
	{"TakeoverTime", &GameCfg::teamchange_time},
};

// The mpattrib switches: TeamFF, FriendlyTag, FFWarning and Tracers set their
// bit on 0 and clear it on nonzero; TeamChoose and ClaymorePref set theirs on
// nonzero [orig: ServerConfig_ApplyHostSetting @0x4A62F7..0x4A6458 onto
// multiplayerAttributeFlags_34C: TeamFF 0x200, FriendlyTag 0x400, FFWarning
// 0x8, TeamChoose 0x4, ClaymorePref 0x8000, Tracers 0x1].
struct FlagKey {
	std::string_view key;
	uint32_t bit;
	bool set_on_zero;
};
constexpr FlagKey kFlagKeys[] = {
	{"TeamFF", GameConfig::kMpAttribNoFriendlyFire, true},
	{"FriendlyTag", GameConfig::kMpAttribNoFriendlyTag, true},
	{"FFWarning", GameConfig::kMpAttribFFWarningSuppress, true},
	{"TeamChoose", GameConfig::kMpAttribTeamChoose, false},
	{"ClaymorePref", GameConfig::kMpAttribClaymorePref, false},
	{"Tracers", GameConfig::kMpAttribNoTracers, true},
};

} // namespace

GameConfig host_screen_default_config() {
	GameConfig config;
	config.respawn_time = game_rules::kDefaultRespawnTime;
	config.time_limit_minutes = game_rules::kDefaultTimeLimitMinutes;
	config.replay_enabled = game_rules::kDefaultReplayEnabled;
	config.max_team_lives = game_rules::kDefaultMaxTeamLives;
	config.score_limit = game_rules::kDefaultScoreLimit;
	config.max_score = game_rules::kDefaultMaxScore;
	config.koth_delta = game_rules::kDefaultKothDelta;
	config.flag_return_ticks = game_rules::kDefaultFlagReturnTicks;
	config.capture_duration_seconds = game_rules::kDefaultCaptureDurationSeconds;
	config.capture_speed_setting = game_rules::kDefaultCaptureSpeedSetting;
	config.spawn_wave_time_base = game_rules::kDefaultSpawnWaveTimeBase;
	config.spawn_wave_time_zone = game_rules::kDefaultSpawnWaveTimeZone;
	config.default_spawn_requires_no_team_zone = game_rules::kDefaultSpawnRequiresNoTeamZone;
	config.num_teams = static_cast<uint8_t>(game_rules::kDefaultNumTeams);
	config.respawn_timeout = game_rules::kDefaultRespawnTimeout;
	config.start_delay = game_rules::kDefaultStartDelay;
	config.destroy_buildings = game_rules::kDefaultDestroyBuildings;
	config.death_messages = game_rules::kDefaultDeathMessages;
	config.custom_text = game_rules::kCustomTextDefault;
	return config;
}

bool apply_host_file_line(const io::ConfigTokens &line, GameCfg &cfg,
		HostRotation &host_rotation, const std::vector<mission_catalog::Row> &catalog,
		HostFileReport *report) {
	const std::string key = line.token(0);
	const std::string value = line.token(1);
	for (const TextKey &entry : kTextKeys) {
		if (!strutil::iequals(key, entry.key)) continue;
		strncpy_field(cfg.*entry.field, value, entry.count, entry.capacity);
		return true;
	}
	for (const NumberKey &entry : kNumberKeys) {
		if (!strutil::iequals(key, entry.key)) continue;
		cfg.*entry.field = io::retail_atol(value.c_str());
		return true;
	}
	// MaxPlayers takes the value without the host screen's 64 clamp; a
	// `/maxplayers N` on the command line replaces it, and nothing else reads
	// that switch (not ported: opennova-serve has no such switch).
	// [orig: maxPlayers_3F4 @0x4A6213..0x4A624B; /maxplayers @0x4A76EF..0x4A7710
	//  -> g_MaxPlayerCommandLineArg @0xB4C4F8]
	if (strutil::iequals(key, "MaxPlayers")) {
		cfg.mp_max_players = io::retail_atol(value.c_str());
		return true;
	}
	for (const FlagKey &entry : kFlagKeys) {
		if (!strutil::iequals(key, entry.key)) continue;
		const bool set = (io::retail_atol(value.c_str()) == 0) == entry.set_on_zero;
		uint32_t attrib = static_cast<uint32_t>(cfg.mpattrib);
		attrib = set ? (attrib | entry.bit) : (attrib & ~entry.bit);
		cfg.mpattrib = static_cast<int32_t>(attrib);
		return true;
	}
	// `Mission <file> <launch option>`: the first catalog row whose file
	// matches takes the launch option, joins the rotation with flag 0, and
	// names the map the session starts on -- so the LAST Mission line is the
	// starting map, and the cursor sits on it.
	// [orig: @0x4A64B4..0x4A6593 -- the catalog walk @0x4A64E0..0x4A6509 over
	//  g_MissionList @0x2551118 (stride 0x11E8, count @0x255111C), the launch
	//  option `entry+0x1140 = atol(token 2)` @0x4A6511..0x4A652B, the append
	//  @0x4A6532, g_MapFileName @0x4A653D..0x4A654E, g_MissionSourceIsLoose
	//  (entry+0x111C) @0x4A6555..0x4A655C, g_MissionLaunchOption (the byte at
	//  entry+0x1140) @0x4A6562..0x4A6569, MissionList_FindByName @0x4A6572,
	//  g_GameType (entry+0x1128) @0x4A6577..0x4A658D]
	if (strutil::iequals(key, "Mission")) {
		MissionRotation &rotation = host_rotation.list;
		for (size_t i = 0; i < catalog.size(); ++i) {
			if (!strutil::iequals(catalog[i].file, value)) continue;
			const int32_t row = static_cast<int32_t>(i);
			rotation.launch_option(catalog, row) = io::retail_atol(line.token(2));
			rotation.append(catalog, row, 0);
			rotation.take_row(catalog, row);
			// g_MapFileName takes the line's own token, not the row's spelling.
			rotation.map_file = value;
			rotation.find_by_name(catalog, value);
			host_rotation.previous_game_type = rotation.map_game_type; // [orig: @0x4A658D]
			return true;
		}
		if (report != nullptr) report->unknown_missions.push_back(value);
		return true;
	}
	if (report != nullptr) report->unknown_keys.push_back(key);
	return false;
}

HostFileReport read_host_file(const char *text, size_t size, GameCfg &cfg,
		HostRotation &host_rotation, const std::vector<mission_catalog::Row> &catalog) {
	HostFileReport report;
	// The callback returns 0 for every line, so the walk never stops early
	// [orig: every arm's `xor eax, eax` before its ret, e.g. @0x4A6035].
	report.lines = io::for_each_config_file_line(text, size, [&](io::ConfigTokens &line) {
		apply_host_file_line(line, cfg, host_rotation, catalog, &report);
	});
	return report;
}

} // namespace opennova::inmatch
