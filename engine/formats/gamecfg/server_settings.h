#pragma once

// The editable server slice of game.cfg. This is UI metadata over the existing
// retail block, not another implementation of the session rules. Values crossing
// the UI boundary are UTF-8; the block and its writers use CP1252.
#include <base/io/json.h>
#include <formats/gamecfg/game_cfg.h>

namespace opennova::gamecfg {

enum class ServerSettingKind { Integer, Number, Boolean, Text, Password };
struct ServerSetting {
	const char *key;
	const char *label;
	const char *group;
	ServerSettingKind kind;
	int32_t GameCfg::*integer = nullptr;
	float GameCfg::*fraction = nullptr;
	std::string GameCfg::*text = nullptr;
	double minimum = INT32_MIN;
	double maximum = INT32_MAX;
	uint32_t bit = 0;
	bool inverted = false;
	const char *command = ""; // SET key; empty means next session
	bool local_only = false;
	bool dfx2_only = false;
	bool core = false; // hosted launch's established top-level field
	const char *note = "";
	const char *depends_on = ""; // the toggle or count this setting does nothing without
};

// What the schema takes of the title a server runs, which this format library cannot see: its
// game.cfg table, its host screen's MAX_PLAYERS cap (Title::max_players_host_screen) and the two
// availability words its binary swaps (DFX2-10: Jointops.exe's armory-only 2 and mission 3,
// dfx2.exe's 3 and 2; ProtocolInMatch::armory_only_availability). The embedder resolves it from the
// title (inmatch::server_settings_title).
struct ServerSettingsTitle {
	ConfigTable table = ConfigTable::Jo;
	int32_t max_players = 0;
	int32_t armory_word = 0;
	int32_t mission_word = 0;
};

const std::vector<ServerSetting> &server_settings();
const ServerSetting *server_setting(std::string_view key);
io::JsonValue server_setting_value(const GameCfg &cfg, const ServerSetting &setting);
// Strict validation, no coercion or truncation (max_players under the title's cap). False leaves
// cfg untouched.
bool set_server_setting(GameCfg &cfg, const ServerSetting &setting, const io::JsonValue &value,
		const ServerSettingsTitle &title, std::string &error);
// Metadata includes the title's defaults. hosted excludes local-only fields.
io::JsonValue server_settings_schema(const ServerSettingsTitle &title, bool hosted);
io::JsonValue server_settings_values(const GameCfg &cfg, bool hosted);

// A weapon's availability word by its name, as the host screen's WEAPON_LIST shows it: "never" 0,
// "always" 1, "armory" the table's armory-only word and "mission" its mission word (the
// mission's own item availability decides), the title's. -1 reads as "mission" too and any other
// word as "always", the list's default arm.
const char *weapon_availability_name(int32_t word, const ServerSettingsTitle &title);
// `name`'s word into `word`: untouched when `name` is the name `word` already reads as (the raw
// word kept, -1 or any other), else the name's own word. False, `word` untouched, for no such name.
bool set_weapon_availability(
		int32_t &word, std::string_view name, const ServerSettingsTitle &title);
// A class word as the session's class mask takes it: on for ALWAYS (1) and MISSION (2), which
// every mission entry's mask enables alike; off for NEVER (0) and any other word.
bool class_available(int32_t word);

// Validate the whole patch before changing the block. weapons is a weapon name ->
// availability name map (weapon_availability_name's), classes a class name -> bool map
// (class_available's; an unchanged one keeps its raw word), handled by the same seam.
bool apply_server_settings(GameCfg &cfg, const io::JsonValue &patch, bool hosted,
		const WeaponRoster &weapons, const ServerSettingsTitle &title, std::string &field,
		std::string &error);

} // namespace opennova::gamecfg
