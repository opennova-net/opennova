#include <runtime/inmatch/host_file.h>

#include <base/gameprofile/game_type.h>
#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <runtime/inmatch/host_settings.h>

#include <string_view>

namespace opennova::inmatch {

namespace {

// The keys that write a host-screen global the MULTI_PLAYER_HOST dialog also
// writes, each with that control: the file's value takes the dialog's arm, so
// the caps, the inverted rule bits and the later 500-point substitution are
// the dialog's. Text keys copy the token; the rest read it with atol.
// [orig: ServerConfig_ApplyHostSetting @0x4A6000 -- GameName strncpy 0x20 ->
//  0x2550A5D @0x4A6010..0x4A6038, MPHostGamePassword 0x11 -> 0x2550A08
//  @0x4A603C, ServerMessage 0x80 -> 0x2550C18 @0x4A6068, GameLocation 3 ->
//  0x2550A7D @0x4A6097, ConnectionSpeed -> 0x2550BF8 @0x4A60C3, Replay ->
//  0x2550B24 @0x4A6117, Delay -> 0x2550AC8 @0x4A6141, Respawn -> 0x2550B34
//  @0x4A616B, Time_Limit -> 0x2550B28 @0x4A6195, KillLimit -> 0x2550AC0
//  @0x4A61BF, MaxScore -> 0x2550AC4 @0x4A61E9, MaxFFKills -> 0x2550C98
//  @0x4A62A3, TakeoverTime -> 0x2550B78 @0x4A62CD, TeamFF 0x200 / FriendlyTag
//  0x400 / FFWarning 0x8 set on 0, TeamChoose 0x4 / ClaymorePref 0x8000 set on
//  nonzero, Tracers 0x1 set on 0 @0x4A62F7..0x4A6458, MPHostSidePasswordA
//  0x11 -> 0x2550A3B @0x4A645F, MPHostSidePasswordB 0x11 -> 0x2550A4C
//  @0x4A648B; the dialog writes the same addresses, HostDialog_ReadSettings
//  @0x555940..0x555EA3]
struct DialogKey {
	std::string_view key;
	std::string_view control;
	bool text;
};
constexpr DialogKey kDialogKeys[] = {
	{"GameName", "GAME_NAME", true},
	{"MPHostGamePassword", "SERVER_PASSWORD", true},
	{"ServerMessage", "SERVER_MESSAGE", true},
	{"GameLocation", "GAME_LOCATION", true},
	{"ConnectionSpeed", "CONNECTIONSPEED", false},
	{"Replay", "REPLAY", false},
	{"Delay", "DELAY", false},
	{"Respawn", "RESPAWN", false},
	{"Time_Limit", "TIME", false},
	{"KillLimit", "KILL_LIMIT", false},
	{"MaxScore", "MAX_SCORE", false},
	{"MaxFFKills", "MAX_FF_KILLS", false},
	{"TakeoverTime", "TAKEOVER_TIME", false},
	{"TeamFF", "TEAM_FF", false},
	{"FriendlyTag", "FRIENDLY_TAG", false},
	{"FFWarning", "FF_WARNING", false},
	{"TeamChoose", "TEAM_CHOOSE", false},
	{"ClaymorePref", "CLAYMORE_PREF", false},
	{"Tracers", "TRACERS", false},
	{"MPHostSidePasswordA", "BLUE_PW", true},
	{"MPHostSidePasswordB", "RED_PW", true},
};

// MissionRotation_Append: the pair joins the end, and a row whose code word
// carries the objective bit or lacks the team bit loses its launch option
// [orig: MissionRotation_Append @0x5019D0 -- the bounds test
//  @0x5019D5..0x5019E3, the append @0x501A86..0x501AA0, the clear
//  @0x501AA8..0x501AC1].
void append_to_rotation(MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
		size_t index, uint32_t flag) {
	if (index >= catalog.size()) return;
	rotation.entries.push_back(MissionRotationEntry{index, flag});
	const uint32_t code = game_type::for_mission_mode(catalog[index].game_mode);
	if ((code & game_type::kObjectiveBit) != 0 || (code & game_type::kTeamBit) == 0)
		rotation.launch_options[index] = 0;
}

// MissionList_FindByName: the cursor to the first entry whose catalog file
// matches, case-insensitively; the alternate cursor cleared on a hit. Retail
// walks to the list's capacity, not its count; this caller always finds the
// entry it has just appended within the count.
// [orig: MissionList_FindByName @0x4FC4C0 -- the cursor reset @0x4FC4D0,
//  the walk @0x4FC4F0..0x4FC51B, the hit @0x4FC521..0x4FC52F]
void find_in_rotation(MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
		const std::string &name) {
	rotation.cursor = -1;
	for (size_t i = 0; i < rotation.entries.size(); ++i) {
		const size_t row = rotation.entries[i].catalog_index;
		if (row < catalog.size() && strutil::iequals(catalog[row].file, name)) {
			rotation.cursor = static_cast<int32_t>(i);
			rotation.alt_cursor = -1;
			return;
		}
	}
}

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

const MissionRotationEntry *MissionRotation::current() const {
	if (cursor < 0 || static_cast<size_t>(cursor) >= entries.size()) return nullptr;
	return &entries[static_cast<size_t>(cursor)];
}

bool apply_host_file_line(const io::ConfigTokens &line, HostScreenState &host,
		MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog,
		HostFileReport *report) {
	const std::string key = line.token(0);
	const std::string value = line.token(1);
	for (const DialogKey &entry : kDialogKeys) {
		if (!strutil::iequals(key, entry.key)) continue;
		if (entry.text) return apply_host_dialog_text(host.config, entry.control, value);
		return apply_host_dialog_number(host.config, host.player_limit, host.serve_and_play,
				entry.control, io::retail_atol(value.c_str()));
	}
	// MaxPlayers writes the dialog's global without the dialog's 64 clamp; a
	// `/maxplayers N` on the command line replaces it, and nothing else reads
	// that switch (not ported: opennova-serve has no such switch).
	// [orig: @0x4A6213..0x4A624B; /maxplayers @0x4A76EF..0x4A7710 -> dword_B4C4F8]
	if (strutil::iequals(key, "MaxPlayers")) {
		host.player_limit = io::retail_atol(value.c_str());
		return true;
	}
	// [orig: GameType -> 0x2550AA8 @0x4A60ED..0x4A6113; UseLineUpQueue ->
	//  0x2550AB0 @0x4A624F..0x4A6275; LineUpQueueSize -> 0x2550AB4
	//  @0x4A6279..0x4A629F]
	if (strutil::iequals(key, "GameType")) {
		host.game_type_setting = io::retail_atol(value.c_str());
		return true;
	}
	if (strutil::iequals(key, "UseLineUpQueue")) {
		host.use_lineup_queue = io::retail_atol(value.c_str());
		return true;
	}
	if (strutil::iequals(key, "LineUpQueueSize")) {
		host.lineup_queue_size = io::retail_atol(value.c_str());
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
		if (rotation.launch_options.size() != catalog.size())
			rotation.launch_options.resize(catalog.size(), 0);
		for (size_t i = 0; i < catalog.size(); ++i) {
			if (!strutil::iequals(catalog[i].file, value)) continue;
			rotation.launch_options[i] = io::retail_atol(line.token(2));
			append_to_rotation(rotation, catalog, i, 0);
			rotation.map_file = value;
			rotation.map_source_is_loose = catalog[i].loose;
			rotation.map_launch_option = static_cast<uint8_t>(rotation.launch_options[i]);
			find_in_rotation(rotation, catalog, value);
			rotation.map_game_type = game_type::for_mission_mode(catalog[i].game_mode);
			return true;
		}
		if (report != nullptr) report->unknown_missions.push_back(value);
		return true;
	}
	if (report != nullptr) report->unknown_keys.push_back(key);
	return false;
}

HostFileReport read_host_file(const char *text, size_t size, HostScreenState &host,
		MissionRotation &rotation, const std::vector<mission_catalog::Row> &catalog) {
	HostFileReport report;
	// The callback returns 0 for every line, so the walk never stops early
	// [orig: every arm's `xor eax, eax` before its ret, e.g. @0x4A6035].
	report.lines = io::for_each_config_file_line(text, size, [&](io::ConfigTokens &line) {
		apply_host_file_line(line, host, rotation, catalog, &report);
	});
	return report;
}

} // namespace opennova::inmatch
