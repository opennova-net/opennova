#include "console_configuration.h"

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <net/admin/admin_protocol.h>

namespace opennova::novaworld_server::hosted {
namespace {
using Json = io::JsonValue;
bool eq(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
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
			if (spec.kind == gamecfg::ServerSettingKind::Password) {
				// CAdminServer copies at most 17 bytes, preserving case.
				v = io::json_string(cp1252_to_utf8(value.substr(0, 17)));
			} else if (spec.text) {
				std::string name;
				for (size_t i = 2; i < args.size(); ++i)
					if (name.size() + args[i].size() + 2 < 32)
						name += std::string(args[i]) + " ";
				v = io::json_string(cp1252_to_utf8(name));
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

ConfigurationEdit configuration_edit(const Json &patch, const gamecfg::WeaponRoster &weapons) {
	ConfigurationEdit edit;
	Json later = Json::make_object();
	const auto field = [&](const std::string &key, const Json &value, bool option) {
		if (key == "weapons") {
			for (const auto &weapon : value.object)
				for (size_t i = 1; i < weapons.size(); ++i)
					if (eq(weapon.key, cp1252_to_utf8(weapons[i].name)))
						edit.commands.push_back(
								"WEAPON SET " + std::to_string(i) + " " + weapon.value.string);
			return;
		}
		const auto *spec = gamecfg::server_setting(key);
		if (spec && *spec->command) {
			std::string argument;
			if (value.is_string())
				(void)utf8_to_cp1252(value.string, argument);
			else
				argument = value.is_bool() ? (value.boolean ? "1" : "0") : io::json_write(value);
			edit.commands.push_back("SET " + std::string(spec->command) + " " + argument);
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
