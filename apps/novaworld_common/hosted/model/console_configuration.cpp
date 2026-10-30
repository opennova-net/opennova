#include "console_configuration.h"

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/gamecfg/server_settings.h>
#include <net/admin/admin_protocol.h>
#include <runtime/inmatch/host_settings.h> // server_settings_title

#include <algorithm>
#include <utility>

namespace opennova::novaworld_server::hosted {
namespace {
using Json = io::JsonValue;
bool eq(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
}

// The keyword retail's WEAPON SET takes for an availability name: ARMORY its armory-only word,
// NEVER 0, every other word ALWAYS (1). None reaches the mission word, so a weapon an edit sets
// to "mission" waits for the next launch.
// [orig: CAdminServer_HandleWeaponCommand @0x4045A0 - ARMORY @0x404822..0x40482E, NEVER
//  @0x40484A, any other word 1]
const char *weapon_set_keyword(std::string_view name) {
	if (name == "never")
		return "NEVER";
	if (name == "armory")
		return "ARMORY";
	return name == "always" ? "ALWAYS" : nullptr;
}

// Whether a WEAPON SET ALL sets exactly the weapons the roster offers: retail's ALL writes every
// row whose admin-table word (+936, weapon.def's loadout_selectable, kept on a subclass row too)
// is nonzero [orig: CAdminServer_HandleWeaponCommand @0x404850..0x404868, AdmDef_GetEntryByIndex
// [234]; WeaponDefs_ParseLineCallback @0x544E11], while the roster's selectable rows leave the
// subclass rows out (game_cfg_weapon_roster). So the two sets must agree, row for row, and the
// admin word be known for every row.
bool all_sets_roster(const gamecfg::WeaponRoster &weapons, size_t rows) {
	for (size_t i = 1; i < rows; ++i)
		if (weapons[i].admin_loadout_selectable < 0 ||
				(weapons[i].admin_loadout_selectable != 0) != (weapons[i].loadout_selectable != 0))
			return false;
	return true;
}

// The weapons part of a settings edit: only the weapons whose availability the patch changes
// from the saved configuration's (as a launch renders it), so a weapon left alone keeps its
// word, the mission word or any other. A changed weapon's WEAPON SET, or one WEAPON SET ALL when
// the edit leaves every selectable weapon at one keyword's availability and ALL sets exactly
// those rows (all_sets_roster); a weapon set to "mission" into `later`, the next launch's.
// `title` names the launched block's words.
void weapon_edit(const Json &patch_weapons, const gamecfg::GameCfg &launched,
		const gamecfg::ServerSettingsTitle &title, const gamecfg::WeaponRoster &weapons,
		std::vector<std::string> &commands, Json &later) {
	const size_t rows =
			std::min(weapons.size(), static_cast<size_t>(gamecfg::kWeaponAvailabilityCount));
	std::vector<std::string> target(rows);
	for (size_t i = 1; i < rows; ++i)
		if (weapons[i].loadout_selectable)
			target[i] = gamecfg::weapon_availability_name(launched.weapon_availability[i], title);
	std::vector<size_t> live;
	for (const auto &weapon : patch_weapons.object) {
		if (!weapon.value.is_string())
			continue;
		for (size_t i = 1; i < rows; ++i) {
			if (!weapons[i].loadout_selectable ||
					!eq(weapon.key, cp1252_to_utf8(weapons[i].name)) ||
					weapon.value.string == target[i])
				continue;
			target[i] = weapon.value.string;
			if (weapon_set_keyword(target[i]) != nullptr)
				live.push_back(i);
			else
				later.set(weapon.key, io::json_string(target[i]));
		}
	}
	if (live.empty())
		return;
	std::string uniform;
	bool one = true;
	for (size_t i = 1; i < rows && one; ++i)
		if (weapons[i].loadout_selectable) {
			one = uniform.empty() || target[i] == uniform;
			uniform = target[i];
		}
	if (one && live.size() > 1 && all_sets_roster(weapons, rows)) {
		commands.push_back(std::string("WEAPON SET ALL ") + weapon_set_keyword(uniform));
		return;
	}
	for (const size_t i : live)
		commands.push_back("WEAPON SET " + std::to_string(i) + " " + weapon_set_keyword(target[i]));
}
} //namespace

Json console_settings_patch(std::string_view line, const gamecfg::WeaponRoster &weapons) {
	Json patch = Json::make_object(), options = Json::make_object();
	const auto args = admin_split_command(line);
	if (args.size() >= 2 && eq(args[0], "SET")) {
		for (const auto &spec : gamecfg::server_settings()) {
			if (!*spec.command || !eq(spec.command, args[1]))
				continue;
			const std::string value(args.size() > 2 ? args[2] : "");
			Json v;
			// The server's own rules for the two text kinds (net/admin/admin_protocol.h).
			if (spec.kind == gamecfg::ServerSettingKind::Password) {
				v = io::json_string(cp1252_to_utf8(admin_password_field(value)));
			} else if (spec.text) {
				v = io::json_string(cp1252_to_utf8(admin_server_name(
						std::vector<std::string_view>(args.begin() + 2, args.end()))));
			} else if (spec.kind == gamecfg::ServerSettingKind::Boolean)
				v = Json::make_bool(io::retail_atol(value.c_str()) != 0);
			else if (spec.fraction)
				v = io::json_number(static_cast<float>(io::retail_atof(value.c_str())));
			else
				v = io::json_number(io::retail_atol(value.c_str()));
			(spec.core ? patch : options).set(spec.key, std::move(v));
			break;
		}
	} else if (args.size() >= 3 && eq(args[0], "WEAPON") && eq(args[1], "SET")) {
		const bool all = eq(args[2], "ALL");
		const auto index = static_cast<uint8_t>(io::retail_atol(std::string(args[2]).c_str()));
		// ARMORY the armory-only word, NEVER 0, any other word 1 (no keyword names the mission
		// word). [orig: CAdminServer_HandleWeaponCommand @0x4045A0 - @0x404822..0x40484A]
		const char *value = args.size() > 3 && eq(args[3], "NEVER") ? "never"
				: args.size() > 3 && eq(args[3], "ARMORY")			? "armory"
																	: "always";
		Json availability = Json::make_object();
		for (size_t i = 1; i < weapons.size(); ++i)
			if (weapons[i].loadout_selectable && (all || i == index))
				availability.set(cp1252_to_utf8(weapons[i].name), io::json_string(value));
		if (!availability.object.empty())
			options.set("weapons", std::move(availability));
	}
	if (!options.object.empty())
		patch.set("options", std::move(options));
	return patch;
}

bool console_changes_rotation(std::string_view line) {
	const auto args = admin_split_command(line);
	return args.size() >= 2 && eq(args[0], "MISSION") &&
			(eq(args[1], "ADD") || eq(args[1], "REMOVE") || eq(args[1], "CLEAR"));
}

ConfigurationEdit configuration_edit(const Json &patch, const HostedSettings &saved,
		std::string_view title, const gamecfg::WeaponRoster &weapons) {
	ConfigurationEdit edit;
	Json later = Json::make_object();
	// The commands in one order whatever order the patch's keys come in (the queue stores the
	// patch as jsonb, which reorders them): each SET in the settings schema's order
	// (gamecfg::server_settings, the form's), then the weapons'. A refusal stops the edit at its
	// command, so which fields an edit saves depends on this order alone.
	std::vector<std::pair<size_t, std::string>> sets;
	std::vector<std::string> weapon_commands;
	const auto field = [&](const std::string &key, const Json &value, bool option) {
		if (key == "weapons") {
			Json later_weapons = Json::make_object();
			weapon_edit(value, settings_block(saved, title, weapons),
					inmatch::server_settings_title(title), weapons, weapon_commands, later_weapons);
			if (!later_weapons.object.empty())
				later.set("weapons", std::move(later_weapons));
			return;
		}
		const auto *spec = gamecfg::server_setting(key);
		if (spec && *spec->command) {
			std::string argument;
			if (value.is_string()) {
				(void)utf8_to_cp1252(value.string, argument);
			} else if (value.is_bool()) {
				argument = value.boolean ? "1" : "0";
			} else {
				// The number's JSON spelling, without the line end json_write closes a document
				// with: a control byte has no place in an admin line (the link refuses it).
				argument = io::json_write(value);
				while (!argument.empty() && (argument.back() == '\n' || argument.back() == '\r'))
					argument.pop_back();
			}
			sets.emplace_back(static_cast<size_t>(spec - gamecfg::server_settings().data()),
					"SET " + std::string(spec->command) + " " + argument);
		} else
			(option ? later : edit.next_launch).set(key, value);
	};
	for (const auto &m : patch.object) {
		if (m.key == "options")
			for (const auto &o : m.value.object)
				field(o.key, o.value, true);
		else
			field(m.key, m.value, false);
	}
	std::stable_sort(sets.begin(), sets.end(),
			[](const auto &a, const auto &b) { return a.first < b.first; });
	for (auto &set : sets)
		edit.commands.push_back(std::move(set.second));
	for (auto &command : weapon_commands)
		edit.commands.push_back(std::move(command));
	if (!later.object.empty())
		edit.next_launch.set("options", std::move(later));
	return edit;
}

bool console_reply_succeeded(const std::vector<std::string> &replies) {
	bool ok = false;
	for (const auto &reply : replies) {
		if (strutil::starts_with_icase(reply, "ERROR") ||
				strutil::starts_with_icase(reply, "USAGE"))
			return false;
		ok = ok || strutil::starts_with_icase(reply, "OK");
	}
	return ok;
}

std::string console_log_text(std::string_view text) {
	return admin_log_redacted(text);
}
} //namespace opennova::novaworld_server::hosted
