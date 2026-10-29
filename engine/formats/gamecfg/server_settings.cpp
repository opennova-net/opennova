#include "server_settings.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <algorithm>
#include <cmath>

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
ServerSetting text(const char *key, const char *label, const char *group,
		std::string GameCfg::*field, int cap, const char *command = "", bool secret = false) {
	ServerSetting s{ key, label, group, secret ? Kind::Password : Kind::Text };
	s.text = field;
	s.maximum = cap;
	s.command = command;
	return s;
}
ServerSetting flag(
		const char *key, const char *label, uint32_t bit, bool inverted, const char *command = "") {
	auto s = toggle(key, label, "Rules", &GameCfg::mpattrib, command);
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

const std::vector<ServerSetting> &server_settings() {
	// Field semantics and defaults: [orig: Config_SetDefaults @0x54D030] and
	// [orig: Config_ParseSettingsLine @0x54F740] (game_cfg.cpp),
	// [orig: HostDialog_ReadSettings @0x555940] (host_settings.cpp), and the SET verbs'
	// names [orig: CAdminServer_HandleSetCommand @0x405A60] (admin_console.cpp).
	static const std::vector<ServerSetting> settings = [] {
		std::vector<ServerSetting> v = {
			text("game_name", "Server name", "Server", &GameCfg::game_name, 31, "ServerName"),
			text("game_password", "Game password", "Players and access",
					&GameCfg::mp_host_game_password, 16, "ServerPassword", true),
			integer("max_players", "Maximum players", "Players and access",
					&GameCfg::mp_max_players, "", 1, 64),
			integer("time_limit", "Round time (minutes)", "Rules", &GameCfg::time_limit, "GameTime",
					0),
			integer("kill_limit", "Kill limit", "Rules", &GameCfg::max_kills, "KillLimit", 0),
			integer("max_score", "Score limit", "Rules", &GameCfg::max_score, "MaxScore", 0),
			integer("respawn", "Respawn delay (seconds)", "Rules", &GameCfg::timeout, "", 0),
			integer("start_delay", "Start delay (seconds)", "Rules", &GameCfg::startdelay,
					"StartDelay", 0, 300),
			flag("team_ff", "Friendly fire", 0x200, true, "FriendlyFire"),
			text("server_message", "Server message", "Server", &GameCfg::servermsg, 127),
			text("country", "Country code", "Server", &GameCfg::country, 3),
			integer("connection_speed", "Connection speed", "Advanced", &GameCfg::nwisptype, "", 0,
					18),
			text("side_a_password", "Blue team password", "Players and access",
					&GameCfg::mp_host_side_password_a, 16, "SideAPassword", true),
			text("side_b_password", "Red team password", "Players and access",
					&GameCfg::mp_host_side_password_b, 16, "SideBPassword", true),
			text("spectator_password", "Spectator password", "Players and access",
					&GameCfg::mp_host_spectator_password, 63, "", true),
			integer("spectator_slots", "Spectators (0 off, -1 unlimited)", "Players and access",
					&GameCfg::mp_num_spectators_max, "", -1, 255),
			toggle("use_lineup_queue", "Use lineup queue", "Players and access",
					&GameCfg::mp_use_lineup_queue),
			integer("lineup_queue_size", "Lineup queue size", "Players and access",
					&GameCfg::mp_lineup_queue_size, "", 0, 65535),
			toggle("punt_duplicate_pcids", "Reject duplicate player IDs", "Players and access",
					&GameCfg::mp_host_punt_same_pcids),
			text("access_codes", "Access code list", "Players and access",
					&GameCfg::mp_access_code_list, 127, "", true),
			toggle("replay", "Repeat rotation", "Rotation", &GameCfg::replay),
			integer("koth_limit", "King of the Hill limit (minutes)", "Rules", &GameCfg::koth_limit,
					"KOTHLimit"),
			integer("koth_delta", "King of the Hill scoring interval", "Rules",
					&GameCfg::koth_delta),
			integer("max_team_lives", "Team lives", "Rules", &GameCfg::max_team_lives),
			integer("max_friendly_kills", "Allowed friendly kills", "Rules",
					&GameCfg::numallowablefriendlykills, "MaxFriendlyKills"),
			integer("capture_duration", "Capture duration (seconds)", "Teams",
					&GameCfg::teamchange_time, "ChangeTeamDelay"),
			integer("capture_speed", "Capture speed", "Teams", &GameCfg::mp_lfp_takeoverspeed),
			flag("friendly_tags", "Friendly tags", 0x400, true, "FriendlyTags"),
			flag("ff_warning", "Friendly fire warning", 0x8, true),
			flag("team_choose", "Players choose teams", 0x4, false, "ChangeTeam"),
			flag("team_trigger_claymore", "Team members trigger claymores", 0x8000, false,
					"TeamTriggerClaymore"),
			flag("tracers", "Tracers", 0x1, true, "Tracers"),
			flag("auto_scope_zero", "Automatic scope zero", 0x10000, false),
			toggle("allow_ai", "AI enabled", "Advanced", &GameCfg::enable_ai),
			toggle("time_of_day_continuity", "Time of day continuity", "Rules",
					&GameCfg::mp_tod_continuity),
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
			integer("num_teams", "Number of teams", "Teams", &GameCfg::mp_numteams, "", 1, 4),
			toggle("punt_vote", "Allow kick votes", "Players and access", &GameCfg::mpvoting,
					"PuntVote"),
			number("vote_percent", "Vote threshold", "Players and access",
					&GameCfg::mpvoting_percent, "VotePercent"),
			integer("vote_min_players", "Players required to vote", "Players and access",
					&GameCfg::mpvoting_min_players, "VoteNumPlayersReq", 0),
			integer("vote_period", "Vote period", "Players and access", &GameCfg::mpvoting_period),
			toggle("min_ping_check", "Enforce minimum ping", "Players and access",
					&GameCfg::dominpingcheck, "DoMinPingCheck"),
			integer("min_ping", "Minimum ping (ms)", "Players and access", &GameCfg::minping,
					"MinPing", 0),
			toggle("max_ping_check", "Enforce maximum ping", "Players and access",
					&GameCfg::domaxpingcheck, "DoMaxPingCheck"),
			integer("max_ping", "Maximum ping (ms)", "Players and access", &GameCfg::maxping,
					"MaxPing", 0),
			toggle("fat_bullets", "Fat bullets", "Rules", &GameCfg::fatbullets, "FatBullets"),
			toggle("one_shot_kill", "One-shot kills", "Rules", &GameCfg::oneshotonekill,
					"OneShotKill"),
			integer("armory_timer", "Armory reuse time", "Classes and weapons",
					&GameCfg::armory_reuse_time, "ArmoryTimer", 0),
			integer("farp_timer", "FARP reuse time", "Rules", &GameCfg::farp_reuse_time, "", 0),
			toggle("destroy_buildings", "Destructible buildings", "Rules", &GameCfg::destroybuild),
			toggle("death_messages", "Death messages", "Rules", &GameCfg::deathmes),
			toggle("custom_skins", "Custom skins", "Advanced", &GameCfg::allowcustomskins),
			toggle("unlimited_vehicles", "Unlimited vehicles", "Rules",
					&GameCfg::unlimited_vehicles),
			integer("spawn_wave_base", "Base spawn wave interval", "Rules",
					&GameCfg::spawnregulator_psp),
			integer("spawn_wave_zone", "Zone spawn wave interval", "Rules",
					&GameCfg::spawnregulator_lfp),
			toggle("choose_spawn", "Choose spawn at start", "Rules", &GameCfg::choosespawnonbegin),
			toggle("no_default_spawn", "Disable default spawn points", "Rules",
					&GameCfg::nodefaultspawnpoints),
			integer("flag_return_time", "Flag return time (ticks)", "Rules",
					&GameCfg::flag_return_time),
			integer("flag_reset_time", "Flag carry time (seconds)", "Rules",
					&GameCfg::flag_reset_time),
			toggle("permanent_death", "Permanent death", "Rules", &GameCfg::mp_permanent_death),
			toggle("sniper_scope_zoom", "Sniper scope zoom", "Classes and weapons",
					&GameCfg::mp_allowsniperscopezoom),
			toggle("third_person_driver", "Third-person driving", "Rules",
					&GameCfg::mp_3rdperson_driver),
			toggle("no_char_abilities", "Disable class abilities", "Classes and weapons",
					&GameCfg::mp_no_char_abilities),
			toggle("no_weapon_recoil", "Disable weapon recoil", "Classes and weapons",
					&GameCfg::mp_no_weapon_recoil),
			toggle("no_crosshair_spread", "Disable crosshair spread", "Classes and weapons",
					&GameCfg::mp_no_crosshair_spread),
			toggle("no_scope_drift", "Disable scope drift", "Classes and weapons",
					&GameCfg::mp_no_scope_drift),
			integer("gps_icons", "GPS icons", "Advanced", &GameCfg::mp_gpsicons),
			integer("wind", "Wind", "Rules", &GameCfg::mp_wind),
			integer("dropped_weapon_disappear", "Dropped weapon lifetime", "Classes and weapons",
					&GameCfg::mp_dropped_weapon_disappear),
			toggle("no_drop_weapons", "Disable dropped weapons", "Classes and weapons",
					&GameCfg::mp_no_drop_weapons),
			toggle("no_respawn_primary", "Respawn without primary weapon", "Classes and weapons",
					&GameCfg::mp_no_respawn_with_primary),
			integer("difficulty", "Difficulty", "Rules", &GameCfg::mp_difficulty, "", 0, 255),
			integer("game_type", "Host game type preference", "Advanced", &GameCfg::mp_gametype),
			integer("dfx_category", "DFX category", "Server", &GameCfg::mp_dfx_category),
			toggle("publish_players", "Publish player list", "Server", &GameCfg::sendplayerlist),
			integer("listing_ping", "Listing ping", "Server", &GameCfg::ping),
			integer("max_packet_size", "Maximum packet size", "Advanced",
					&GameCfg::mp_max_packet_size, "", 64, 65507),
			toggle("extended_metrics", "Extended network metrics", "Advanced",
					&GameCfg::mp_extract_extended_metric_information),
			integer("lan_mode", "LAN network mode", "Advanced", &GameCfg::lanmode, "", 1, 4),
			toggle("lan_only", "LAN-only admission", "Server",
					&GameCfg::mp_novaworld_host_lan_only),
			toggle("dedicated", "Serve only", "Server", &GameCfg::dedicated),
			integer("lan_port_min", "First LAN server port", "Advanced",
					&GameCfg::mp_lan_server_port_min, "", 1, 65535),
			integer("lan_port_max", "Last LAN server port", "Advanced",
					&GameCfg::mp_lan_server_port_max, "", 1, 65535),
			integer("lan_port_step", "LAN port step", "Advanced",
					&GameCfg::mp_lan_server_port_delta, "", 1, 65535),
		};
		for (size_t i = 0; i < 9; ++i)
			v[i].core = true;
		for (auto &s : v) {
			const std::string_view key(s.key);
			// SET consumes signed 32-bit words, including retail sentinel values.
			// Do not substitute the old website's guessed slider ranges.
			if (*s.command && s.kind == Kind::Integer) {
				s.minimum = INT32_MIN;
				s.maximum = INT32_MAX;
			}
			s.local_only =
					key == "dedicated" || key == "lan_only" || key.substr(0, 9) == "lan_port_";
			s.dfx2_only = key == "dfx_category";
			if (key == "connection_speed" || key == "allow_ai")
				s.note = "Retail stores this option; it has no session-side effect.";
			if (key == "game_type")
				s.note = "The selected map determines the actual game mode.";
		}
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

bool set_server_setting(GameCfg &cfg, const ServerSetting &s, const Json &v, std::string &error) {
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
		const double maximum =
				std::string_view(s.key) == "max_players" && cfg.table == ConfigTable::Dfx2
				? 100
				: s.maximum;
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

io::JsonValue server_settings_schema(ConfigTable table, bool hosted) {
	Json list = Json::make_array();
	const GameCfg cfg = defaults({}, 0, 0, table);
	for (const auto &s : server_settings()) {
		if (!offered(s, table, hosted))
			continue;
		Json row = Json::make_object();
		row.set("key", io::json_string(s.key));
		row.set("label", io::json_string(s.label));
		row.set("group", io::json_string(s.group));
		row.set("kind", io::json_string(kind_name(s.kind)));
		row.set("default", server_setting_value(cfg, s));
		row.set("min", io::json_number(s.minimum));
		row.set("max",
				io::json_number(
						std::string_view(s.key) == "max_players" && table == ConfigTable::Dfx2
								? 100
								: s.maximum));
		row.set("command", io::json_string(s.command));
		row.set("core", Json::make_bool(s.core));
		row.set("note", io::json_string(s.note));
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
		classes.set(kClassNames[i], Json::make_bool(cfg.class_availability[i + 5] != 0));
	out.set("classes", std::move(classes));
	return out;
}

bool apply_server_settings(GameCfg &cfg, const Json &patch, bool hosted,
		const WeaponRoster &weapons, std::string &field, std::string &error) {
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
							next.class_availability[i + 5] = entry.value.boolean ? 1 : 0;
							found = true;
						}
				} else {
					const int armory = cfg.table == ConfigTable::Dfx2 ? 3 : 2;
					if (entry.value.is_string() &&
							(entry.value.string == "always" || entry.value.string == "never" ||
									entry.value.string == "armory")) {
						for (size_t i = 1; i < weapons.size() && i < kWeaponAvailabilityCount;
								++i) {
							if (weapons[i].loadout_selectable &&
									strutil::iequals(entry.key, cp1252_to_utf8(weapons[i].name))) {
								next.weapon_availability[i] = entry.value.string == "never" ? 0
										: entry.value.string == "armory"					? armory
																							: 1;
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
		if (!set_server_setting(next, *s, m.value, error))
			return false;
	}
	cfg = std::move(next);
	field.clear();
	error.clear();
	return true;
}

} // namespace opennova::gamecfg
