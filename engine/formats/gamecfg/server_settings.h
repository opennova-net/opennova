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
	// The retail host dialog's control that shows and reads back this setting, a
	// MULTI_PLAYER_HOST control name (inmatch::host_dialog_controls); empty for a setting
	// the dialog never touches, which only the file and the supplemental editor reach.
	const char *host_control = "";
};

const std::vector<ServerSetting> &server_settings();
const ServerSetting *server_setting(std::string_view key);
io::JsonValue server_setting_value(const GameCfg &cfg, const ServerSetting &setting);
// Strict validation, no coercion or truncation. False leaves cfg untouched.
bool set_server_setting(
		GameCfg &cfg, const ServerSetting &setting, const io::JsonValue &value, std::string &error);
// Metadata includes the title's defaults. hosted excludes local-only fields.
io::JsonValue server_settings_schema(ConfigTable table, bool hosted);
io::JsonValue server_settings_values(const GameCfg &cfg, bool hosted);
// Validate the whole patch before changing the block. weapons is a name ->
// availability map, classes a class name -> bool map, handled by the same seam.
bool apply_server_settings(GameCfg &cfg, const io::JsonValue &patch, bool hosted,
		const WeaponRoster &weapons, std::string &field, std::string &error);

} // namespace opennova::gamecfg
