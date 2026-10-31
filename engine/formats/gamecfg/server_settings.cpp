#include "server_settings.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace opennova::gamecfg {
namespace {
using Kind = ServerSettingKind;
using Json = io::JsonValue;
ServerSetting integer(const char *key, const char *label, const char *group,
		int32_t GameCfg::*field, const char *command = "", double lo = INT32_MIN,
		double hi = INT32_MAX) {
	ServerSetting s{ key, label, group, Kind::Integer };
	s.integer = field;
	s.command = command;
	s.minimum = lo;
	s.maximum = hi;
	return s;
}
ServerSetting toggle(const char *key, const char *label, const char *group, int32_t GameCfg::*field,
		const char *command = "") {
	auto s = integer(key, label, group, field, command, 0, 1);
	s.kind = Kind::Boolean;
	return s;
}
ServerSetting number(const char *key, const char *label, const char *group, float GameCfg::*field,
		const char *command = "") {
	ServerSetting s{ key, label, group, Kind::Number };
	s.fraction = field;
	s.command = command;
	return s;
}
// A text row: its byte cap the reader's (text_limit), the bytes a game.cfg line of it keeps.
ServerSetting text(const char *key, const char *label, const char *group,
		std::string GameCfg::*field, const char *command = "", bool secret = false) {
	ServerSetting s{ key, label, group, secret ? Kind::Password : Kind::Text };
	s.text = field;
	s.maximum = static_cast<double>(text_limit(field));
	s.command = command;
	return s;
}
ServerSetting flag(const char *key, const char *label, const char *group, uint32_t bit,
		bool inverted, const char *command = "") {
	auto s = toggle(key, label, group, &GameCfg::mpattrib, command);
	s.bit = bit;
	s.inverted = inverted;
	return s;
}
const char *kind_name(Kind k) {
	switch (k) {
		case Kind::Boolean:
			return "boolean";
		case Kind::Number:
			return "number";
		case Kind::Text:
			return "text";
		case Kind::Password:
			return "password";
		default:
			return "integer";
	}
}
bool offered(const ServerSetting &s, ConfigTable table, bool hosted) {
	return !(hosted && s.local_only) && !(s.dfx2_only && table != ConfigTable::Dfx2);
}
constexpr const char *kClassNames[] = { "medic", "sniper", "gunner", "rifleman", "engineer" };
} //namespace

const char *weapon_availability_name(int32_t word, const ServerSettingsTitle &title) {
	// The list's cell text: -1 and the mission word MISSION, 0 NEVER, the armory-only word
	// ARMORY, any other word ALWAYS.
	// [orig: HostDialog_WeaponRestrictionCycleHandler @0x5576c0 - the switch: -1 and 3 MISSION,
	//  0 NEVER, 2 ARMORY, default ALWAYS]
	// [orig: @0x5558a0 (dfx2.exe) - the switch: -1 and 2 MISSION, 0 NEVER, 3 ARMORY]
	if (word == -1 || word == title.mission_word)
		return "mission";
	if (word == 0)
		return "never";
	return word == title.armory_word ? "armory" : "always";
}

bool set_weapon_availability(int32_t &word, std::string_view name, const ServerSettingsTitle &title) {
	int32_t named = 0;
	if (name == "never")
		named = 0;
	else if (name == "always")
		named = 1;
	else if (name == "armory")
		named = title.armory_word;
	else if (name == "mission")
		named = title.mission_word;
	else
		return false;
	if (name != weapon_availability_name(word, title))
		word = named;
	return true;
}

bool class_available(int32_t word) {
	// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - the class words @0x551882..0x551990:
	//  1 enables its class, 2 while the mission entry's mask holds its bit (every entry's 0x3FF,
	//  MissionList_ScanAndBuildFromFiles @0x56334A), any other word disables it]
	return word == 1 || word == 2;
}

const std::vector<ServerSetting> &server_settings() {
	// Field semantics and defaults: [orig: Config_SetDefaults @0x54D030] and
	// [orig: Config_ParseSettingsLine @0x54F740] (game_cfg.cpp),
	// [orig: HostDialog_ReadSettings @0x555940] (host_settings.cpp), and the SET verbs'
	// names [orig: CAdminServer_HandleSetCommand @0x405A60] (admin_console.cpp).
	static const std::vector<ServerSetting> settings = [] {
		std::vector<ServerSetting> v = {
			text("game_name", "Server name", "Server", &GameCfg::game_name, "ServerName"),
			text("game_password", "Game password", "Players and access",
					&GameCfg::mp_host_game_password, "ServerPassword", true),
			integer("max_players", "Maximum players", "Players and access",
					&GameCfg::mp_max_players, "", 1),
			// A setting with a SET verb takes its signed word whole (the range loop below), so
			// those rows name no range of their own.
			integer("time_limit", "Round time (minutes)", "Rules", &GameCfg::time_limit, "GameTime"),
			integer("kill_limit", "Kill limit", "Rules", &GameCfg::max_kills, "KillLimit"),
			integer("max_score", "Score limit", "Rules", &GameCfg::max_score, "MaxScore"),
			// The reader takes the word whole (atol), and a death waits 3 seconds for any word
			// below 3 [orig: GameEvent_PlayerDeath @0x516EB6..0x516EC6 - `g_RespawnTimeout < 3`
			// stores 3], so 0 up offers every wait the session makes.
			integer("respawn", "Respawn delay (seconds)", "Rules", &GameCfg::timeout, "", 0),
			integer("start_delay", "Start delay (seconds)", "Rules", &GameCfg::startdelay,
					"StartDelay"),
			flag("team_ff", "Friendly fire", "Rules", 0x200, true, "FriendlyFire"),
			text("server_message", "Server message", "Server", &GameCfg::servermsg),
			text("country", "Country code", "Server", &GameCfg::country),
			text("side_a_password", "Blue team password", "Teams",
					&GameCfg::mp_host_side_password_a, "SideAPassword", true),
			text("side_b_password", "Red team password", "Teams", &GameCfg::mp_host_side_password_b,
					"SideBPassword", true),
			text("spectator_password", "Spectator password", "Players and access",
					&GameCfg::mp_host_spectator_password, "", true),
			integer("spectator_slots", "Spectators (0 off, -1 unlimited)", "Players and access",
					&GameCfg::mp_num_spectators_max),
			toggle("use_lineup_queue", "Use lineup queue", "Players and access",
					&GameCfg::mp_use_lineup_queue),
			integer("lineup_queue_size", "Lineup queue size", "Players and access",
					&GameCfg::mp_lineup_queue_size),
			toggle("punt_duplicate_pcids", "Reject duplicate player IDs", "Players and access",
					&GameCfg::mp_host_punt_same_pcids),
			toggle("replay", "Repeat rotation", "Rotation", &GameCfg::replay),
			integer("koth_limit", "King of the Hill limit (minutes)", "Rules", &GameCfg::koth_limit,
					"KOTHLimit"),
			integer("koth_delta", "King of the Hill scoring interval", "Rules",
					&GameCfg::koth_delta),
			// The session takes both, and both ride to every joiner: the team lives in the initial
			// state block (g_MaxTeamLives), the difficulty word as the S2C 0x08 block's and the
			// S2C 0x04's byte (g_SessionDifficulty) [orig: Game_ApplySessionSettingsToGlobals
			// @0x551500 - g_MaxTeamLives @0x551B67, g_SessionDifficulty @0x551E80]
			// (runtime/inmatch/host_config.cpp, server_initial_state.cpp, server_dispatch_join.cpp).
			integer("max_team_lives", "Team lives", "Rules", &GameCfg::max_team_lives),
			integer("difficulty", "Difficulty", "Rules", &GameCfg::mp_difficulty),
			// The authority's mission start after a map change runs on the clock the teardown kept
			// instead of the map's own start time (runtime/inmatch/host_boot.h kept_mission_clock),
			// which every joiner's clock follows [orig: Game_StartMission @0x524360 - @0x5253A1;
			//  NetPacket_WritePlayerState @0x4FFB7E -> NapiNPClientMsg_0x00A @0x4302AE].
			toggle("time_of_day_continuity", "Keep the time of day between maps", "Rules",
					&GameCfg::mp_tod_continuity),
			integer("max_friendly_kills", "Allowed friendly kills", "Rules",
					&GameCfg::numallowablefriendlykills, "MaxFriendlyKills"),
			integer("capture_duration", "Capture duration (seconds)", "Teams",
					&GameCfg::teamchange_time, "ChangeTeamDelay"),
			integer("capture_speed", "Capture speed", "Teams", &GameCfg::mp_lfp_takeoverspeed),
			flag("friendly_tags", "Friendly tags", "Rules", 0x400, true, "FriendlyTags"),
			flag("ff_warning", "Friendly fire warning", "Rules", 0x8, true),
			flag("team_choose", "Players choose teams", "Teams", 0x4, false, "ChangeTeam"),
			flag("team_trigger_claymore", "Team members trigger claymores", "Rules", 0x8000, false,
					"TeamTriggerClaymore"),
			flag("tracers", "Tracers", "Rules", 0x1, true, "Tracers"),
			flag("auto_scope_zero", "Automatic scope zero", "Rules", 0x10000, false),
			toggle("auto_balance", "Balance teams on recycle", "Teams",
					&GameCfg::autobalance_on_recycle_enabled, "AutoBalanceOnRecycle"),
			integer("balance_min_difference", "Balance minimum difference", "Teams",
					&GameCfg::autobalance_on_recycle_diff_min),
			integer("balance_trigger_difference", "Balance trigger difference", "Teams",
					&GameCfg::autobalance_on_recycle_diff_max),
			toggle("balance_join", "Balance teams when joining", "Teams", &GameCfg::balance_join),
			number("balance_join_percent", "Join balance threshold", "Teams",
					&GameCfg::balance_join_percent),
			integer("change_team_interval", "Team change interval (seconds)", "Teams",
					&GameCfg::mpchangeteam_interval, "ChangeTeamInterval"),
			integer("change_team_penalty", "Team change penalty (seconds)", "Teams",
					&GameCfg::mpchangeteam_penalty, "ChangeTeamPenalty"),
			integer("num_teams", "Number of teams", "Teams", &GameCfg::mp_numteams),
			toggle("punt_vote", "Allow kick votes", "Players and access", &GameCfg::mpvoting,
					"PuntVote"),
			number("vote_percent", "Vote threshold", "Players and access",
					&GameCfg::mpvoting_percent, "VotePercent"),
			integer("vote_min_players", "Players required to vote", "Players and access",
					&GameCfg::mpvoting_min_players, "VoteNumPlayersReq"),
			toggle("min_ping_check", "Enforce minimum ping", "Players and access",
					&GameCfg::dominpingcheck, "DoMinPingCheck"),
			integer("min_ping", "Minimum ping (ms)", "Players and access", &GameCfg::minping,
					"MinPing"),
			toggle("max_ping_check", "Enforce maximum ping", "Players and access",
					&GameCfg::domaxpingcheck, "DoMaxPingCheck"),
			integer("max_ping", "Maximum ping (ms)", "Players and access", &GameCfg::maxping,
					"MaxPing"),
			toggle("fat_bullets", "Fat bullets", "Rules", &GameCfg::fatbullets, "FatBullets"),
			toggle("one_shot_kill", "One-shot kills", "Rules", &GameCfg::oneshotonekill,
					"OneShotKill"),
			integer("armory_timer", "Armory reuse time", "Classes and weapons",
					&GameCfg::armory_reuse_time, "ArmoryTimer"),
			integer("farp_timer", "FARP reuse time", "Rules", &GameCfg::farp_reuse_time),
			toggle("destroy_buildings", "Destructible buildings", "Rules", &GameCfg::destroybuild),
			toggle("death_messages", "Death messages", "Rules", &GameCfg::deathmes),
			toggle("custom_skins", "Custom skins", "Players and access",
					&GameCfg::allowcustomskins),
			toggle("unlimited_vehicles", "Unlimited vehicles", "Rules",
					&GameCfg::unlimited_vehicles),
			integer("spawn_wave_base", "Base spawn wave interval", "Rules",
					&GameCfg::spawnregulator_psp),
			integer("spawn_wave_zone", "Zone spawn wave interval", "Rules",
					&GameCfg::spawnregulator_lfp),
			toggle("no_default_spawn", "Disable default spawn points", "Rules",
					&GameCfg::nodefaultspawnpoints),
			integer("flag_return_time", "Flag return time (ticks)", "Rules",
					&GameCfg::flag_return_time),
			integer("flag_reset_time", "Flag carry time (seconds)", "Rules",
					&GameCfg::flag_reset_time),
			toggle("permanent_death", "Permanent death", "Rules", &GameCfg::mp_permanent_death),
			toggle("sniper_scope_zoom", "Sniper scope zoom", "Classes and weapons",
					&GameCfg::mp_allowsniperscopezoom),
			toggle("no_char_abilities", "Disable class abilities", "Classes and weapons",
					&GameCfg::mp_no_char_abilities),
			toggle("no_weapon_recoil", "Disable weapon recoil", "Classes and weapons",
					&GameCfg::mp_no_weapon_recoil),
			toggle("no_crosshair_spread", "Disable crosshair spread", "Classes and weapons",
					&GameCfg::mp_no_crosshair_spread),
			toggle("no_scope_drift", "Disable scope drift", "Classes and weapons",
					&GameCfg::mp_no_scope_drift),
			integer("dfx_category", "DFX category", "Server", &GameCfg::mp_dfx_category),
			toggle("publish_players", "Publish player list", "Server", &GameCfg::sendplayerlist),
			toggle("listing_ping", "Allow ping in listing", "Server", &GameCfg::ping),
			// The fold the network init applies to the word at use: 0 is 1300, below 100 is
			// 100, above 0x4000 is 0x4000 [orig: CNapiNetwork_Init @0x4CAA53..0x4CAA76]. The NP
			// connection's init folds it the same way under a ceiling of 0x10000
			// [orig: CNapiGameSession_InitNPConnection @0x4D3DF4..0x4D3E17, 0x10000 @0x4D3E10];
			// the form keeps the network init's 100..0x4000.
			integer("max_packet_size", "Maximum packet size", "Network",
					&GameCfg::mp_max_packet_size, "", 100, 0x4000),
			toggle("extended_metrics", "Extended network metrics", "Network",
					&GameCfg::mp_extract_extended_metric_information),
			// The reader's fold: a mode outside 1..4 is 2 [orig: Config_ParseSettingsLine @0x54F740 -
			//  the lanmode fold @0x550425..0x55044C].
			integer("lan_mode", "LAN network mode", "Network", &GameCfg::lanmode, "", 1, 4),
			toggle("lan_only", "LAN-only admission", "Server",
					&GameCfg::mp_novaworld_host_lan_only),
			toggle("dedicated", "Serve only", "Server", &GameCfg::dedicated),
			integer("lan_port_min", "First LAN server port", "Network",
					&GameCfg::mp_lan_server_port_min, "", 1, 65535),
			integer("lan_port_max", "Last LAN server port", "Network",
					&GameCfg::mp_lan_server_port_max, "", 1, 65535),
			integer("lan_port_step", "LAN port step", "Network", &GameCfg::mp_lan_server_port_delta,
					"", 1, 65535),
		};
		// The hosted launch's established top-level fields.
		constexpr std::string_view kCore[] = { "game_name", "game_password", "max_players",
			"time_limit", "kill_limit", "max_score", "respawn", "start_delay", "team_ff" };
		for (auto &s : v) {
			const std::string_view key(s.key);
			s.core = std::find(std::begin(kCore), std::end(kCore), key) != std::end(kCore);
			// SET consumes signed 32-bit words, including retail sentinel values.
			// Do not substitute the old website's guessed slider ranges.
			if (*s.command && s.kind == Kind::Integer) {
				s.minimum = INT32_MIN;
				s.maximum = INT32_MAX;
			}
			// A hosted server always runs the NovaWorld channel, which never reads lanmode.
			s.local_only = key == "dedicated" || key == "lan_only" || key == "lan_mode" ||
					key.substr(0, 9) == "lan_port_";
			s.dfx2_only = key == "dfx_category";
			if (key == "country" || key == "dfx_category" || key == "publish_players" ||
					key == "listing_ping")
				s.note = "Shown in the NovaWorld listing.";
			if (key == "server_message")
				s.note = "Shown in the NovaWorld listing and to players as they join.";
		}
		// What a setting does nothing without: the key of a toggle or count that must be on.
		// The side passwords are checked only under the team-choose bit and the spectator
		// password only with spectator slots [orig: Server_ValidatePlayerJoinRequest @0x512100
		//  - `test al, 4` @0x51249A, the spectator test @0x5124A2] (server_join_check.cpp).
		const std::pair<const char *, const char *> depends[] = {
			{ "side_a_password", "team_choose" },
			{ "side_b_password", "team_choose" },
			{ "spectator_password", "spectator_slots" },
			{ "lineup_queue_size", "use_lineup_queue" },
			{ "vote_percent", "punt_vote" },
			{ "vote_min_players", "punt_vote" },
			{ "balance_min_difference", "auto_balance" },
			{ "balance_trigger_difference", "auto_balance" },
			{ "balance_join_percent", "balance_join" },
			{ "min_ping", "min_ping_check" },
			{ "max_ping", "max_ping_check" },
		};
		for (const auto &[key, parent] : depends)
			for (auto &s : v)
				if (std::string_view(s.key) == key)
					s.depends_on = parent;
		return v;
	}();
	return settings;
}

const ServerSetting *server_setting(std::string_view key) {
	for (const auto &s : server_settings())
		if (key == s.key)
			return &s;
	return nullptr;
}

io::JsonValue server_setting_value(const GameCfg &cfg, const ServerSetting &s) {
	if (s.text)
		return io::json_string(cp1252_to_utf8(cfg.*s.text));
	if (s.fraction)
		return io::json_number(cfg.*s.fraction);
	if (s.kind == Kind::Boolean) {
		const bool on =
				s.bit ? (static_cast<uint32_t>(cfg.*s.integer) & s.bit) != 0 : cfg.*s.integer != 0;
		return Json::make_bool(on != s.inverted);
	}
	return io::json_number(cfg.*s.integer);
}

bool set_server_setting(GameCfg &cfg, const ServerSetting &s, const Json &v,
		const ServerSettingsTitle &title, std::string &error) {
	error.clear();
	if (s.text) {
		std::string bytes;
		if (!v.is_string() || !utf8_to_cp1252(v.string, bytes))
			error = "Use text supported by the game's CP1252 character set.";
		else if (bytes.size() > static_cast<size_t>(s.maximum))
			error = "Text exceeds the game's byte limit.";
		else if (std::any_of(bytes.begin(), bytes.end(), [](unsigned char c) {
					 return c < 32 ||
							 (c == 127 || c == 129 || c == 141 || c == 143 || c == 144 ||
									 c == 157) ||
							 c == '"';
				 }))
			error = "Control characters and double quotes are not supported.";
		else if (s.kind == Kind::Password &&
				std::any_of(bytes.begin(), bytes.end(),
						[](unsigned char c) { return c <= 32 || c == 160; }))
			error = "Passwords cannot contain whitespace.";
		else
			cfg.*s.text = bytes;
	} else if (s.kind == Kind::Boolean) {
		if (!v.is_bool())
			error = "Choose on or off.";
		else if (s.bit) {
			uint32_t word = static_cast<uint32_t>(cfg.*s.integer);
			cfg.*s.integer =
					static_cast<int32_t>((v.boolean != s.inverted) ? word | s.bit : word & ~s.bit);
		} else
			cfg.*s.integer = v.boolean ? 1 : 0;
	} else {
		const double maximum = std::string_view(s.key) == "max_players" ? title.max_players : s.maximum;
		if (!v.is_number() || !std::isfinite(v.number) || v.number < s.minimum ||
				v.number > maximum || (s.kind == Kind::Integer && std::floor(v.number) != v.number))
			error = "Value is outside the supported range.";
		else if (s.fraction)
			cfg.*s.fraction = static_cast<float>(v.number);
		else
			cfg.*s.integer = static_cast<int32_t>(v.number);
	}
	return error.empty();
}

io::JsonValue server_settings_schema(const ServerSettingsTitle &title, bool hosted) {
	Json list = Json::make_array();
	const GameCfg cfg = defaults({}, 0, 0, title.table);
	for (const auto &s : server_settings()) {
		if (!offered(s, title.table, hosted))
			continue;
		Json row = Json::make_object();
		row.set("key", io::json_string(s.key));
		row.set("label", io::json_string(s.label));
		row.set("group", io::json_string(s.group));
		row.set("kind", io::json_string(kind_name(s.kind)));
		row.set("default", server_setting_value(cfg, s));
		row.set("min", io::json_number(s.minimum));
		row.set("max",
				io::json_number(std::string_view(s.key) == "max_players" ? title.max_players : s.maximum));
		row.set("command", io::json_string(s.command));
		row.set("core", Json::make_bool(s.core));
		row.set("note", io::json_string(s.note));
		row.set("depends_on", io::json_string(s.depends_on));
		list.push(std::move(row));
	}
	return list;
}

io::JsonValue server_settings_values(const GameCfg &cfg, bool hosted) {
	Json out = Json::make_object();
	for (const auto &s : server_settings())
		if (offered(s, cfg.table, hosted))
			out.set(s.key, server_setting_value(cfg, s));
	Json classes = Json::make_object();
	for (int i = 0; i < 5; ++i)
		classes.set(kClassNames[i], Json::make_bool(class_available(cfg.class_availability[kClassMedic + i])));
	out.set("classes", std::move(classes));
	return out;
}

bool apply_server_settings(GameCfg &cfg, const Json &patch, bool hosted,
		const WeaponRoster &weapons, const ServerSettingsTitle &title, std::string &field,
		std::string &error) {
	if (!patch.is_object()) {
		error = "Expected a settings object.";
		return false;
	}
	GameCfg next = cfg;
	for (const auto &m : patch.object) {
		field = m.key;
		if (m.key == "classes" || m.key == "weapons") {
			if (!m.value.is_object()) {
				error = "Expected an availability object.";
				return false;
			}
			for (const auto &entry : m.value.object) {
				field = m.key + "." + entry.key;
				bool found = false;
				if (m.key == "classes") {
					for (int i = 0; i < 5; ++i)
						if (entry.key == kClassNames[i] && entry.value.is_bool()) {
							// A class left as it reads keeps its raw word (MISSION's 2 among them).
							int32_t &word = next.class_availability[kClassMedic + i];
							if (entry.value.boolean != class_available(word))
								word = entry.value.boolean ? 1 : 0;
							found = true;
						}
				} else {
					int32_t probe = 0;
					if (entry.value.is_string() &&
							set_weapon_availability(probe, entry.value.string, title)) {
						for (size_t i = 1; i < weapons.size() && i < kWeaponAvailabilityCount;
								++i) {
							if (weapons[i].loadout_selectable &&
									strutil::iequals(entry.key, cp1252_to_utf8(weapons[i].name))) {
								set_weapon_availability(
										next.weapon_availability[i], entry.value.string, title);
								found = true;
							}
						}
					}
				}
				if (!found) {
					error = "Unknown class or weapon, or invalid availability.";
					return false;
				}
			}
			continue;
		}
		const auto *s = server_setting(m.key);
		if (!s || !offered(*s, cfg.table, hosted)) {
			error = "This setting is unavailable for this server.";
			return false;
		}
		if (!set_server_setting(next, *s, m.value, title, error))
			return false;
	}
	cfg = std::move(next);
	field.clear();
	error.clear();
	return true;
}

} // namespace opennova::gamecfg
