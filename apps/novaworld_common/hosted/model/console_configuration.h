#pragma once

#include "hosted_settings.h"

namespace opennova::novaworld_server::hosted {
// Translate an accepted retail command into the canonical saved configuration.
// Rotation mutations are read back separately: indices include temporary maps.
io::JsonValue console_settings_patch(std::string_view line, const gamecfg::WeaponRoster &weapons);
bool console_changes_rotation(std::string_view line);
bool console_reply_succeeded(const std::vector<std::string> &replies);
struct ConfigurationEdit {
	io::JsonValue next_launch = io::JsonValue::make_object();
	std::vector<std::string> commands;
};
// A settings patch as the live commands that apply it and the fields that wait for the next
// launch. `saved` is the configuration the patch edits (its server's title `title`, `weapons` the
// roster its pinned trees list): a weapon the patch leaves at the availability the saved
// configuration launches with sends nothing. Input has passed the same validation as a launch
// configuration.
ConfigurationEdit configuration_edit(const io::JsonValue &patch, const HostedSettings &saved,
		std::string_view title, const gamecfg::WeaponRoster &weapons);
// Password-bearing command/reply text must never enter the persistent console.
std::string console_log_text(std::string_view text);
} //namespace opennova::novaworld_server::hosted
