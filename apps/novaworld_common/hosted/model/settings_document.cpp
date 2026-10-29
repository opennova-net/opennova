#include "hosted_settings.h"
#include <base/io/cp1252.h>
#include <base/title/title.h>
#include <stdexcept>

namespace opennova::novaworld_server::hosted {
namespace {
using Json = io::JsonValue;
struct IntField {
	const char *key;
	int HostedSettings::*member;
};
constexpr IntField integers[] = { { "max_players", &HostedSettings::max_players },
	{ "time_limit", &HostedSettings::time_limit }, { "kill_limit", &HostedSettings::kill_limit },
	{ "max_score", &HostedSettings::max_score }, { "respawn", &HostedSettings::respawn },
	{ "start_delay", &HostedSettings::start_delay } };
} //namespace

io::JsonValue settings_document(const HostedSettings &s) {
	Json out = Json::make_object();
	out.set("product_id", io::json_number(static_cast<double>(s.product_id)));
	out.set("expansion_id",
			s.expansion_id ? io::json_number(static_cast<double>(*s.expansion_id)) : Json{});
	out.set("game_name", io::json_string(s.game_name));
	out.set("game_password", io::json_string(s.game_password));
	for (const auto &f : integers)
		out.set(f.key, io::json_number(s.*f.member));
	out.set("team_ff", Json::make_bool(s.team_ff));
	out.set("list_on_novalogic", Json::make_bool(s.list_on_novalogic));
	Json rotation = Json::make_array();
	for (size_t i = 0; i < s.missions.size(); ++i) {
		Json entry = Json::make_object();
		entry.set("file", io::json_string(s.missions[i]));
		entry.set("swap_sides",
				Json::make_bool(i < s.mission_options.size() && s.mission_options[i] != 0));
		rotation.push(std::move(entry));
	}
	out.set("rotation", std::move(rotation));
	Json options = s.options;
	if (!options.get("replay")) options.set("replay", Json::make_bool(true));
	out.set("options", std::move(options));
	return out;
}

bool read_settings_document(
		const Json &doc, HostedSettings &settings, std::string &field, std::string &error) {
	if (!doc.is_object()) {
		error = "Expected a settings object.";
		return false;
	}
	HostedSettings s = settings;
	for (const auto &m : doc.object) {
		field = m.key;
		bool known = false;
		for (const auto &f : integers)
			if (m.key == f.key) {
				if (!io::json_exact_whole_in(m.value, INT32_MIN, INT32_MAX)) {
					error = "Expected a whole number.";
					return false;
				}
				s.*f.member = static_cast<int>(m.value.number);
				known = true;
			}
		if (known)
			continue;
		if (m.key == "game_name" || m.key == "game_password") {
			if (!m.value.is_string()) {
				error = "Expected text.";
				return false;
			}
			(m.key == "game_name" ? s.game_name : s.game_password) = m.value.string;
		} else if (m.key == "team_ff" || m.key == "list_on_novalogic") {
			if (!m.value.is_bool()) {
				error = "Expected a boolean.";
				return false;
			}
			(m.key == "team_ff" ? s.team_ff : s.list_on_novalogic) = m.value.boolean;
		} else if (m.key == "product_id" || m.key == "expansion_id") {
			if (m.key == "expansion_id" && m.value.is_null()) {
				s.expansion_id.reset();
				continue;
			}
			if (!io::json_exact_whole_in(m.value, 0, io::kJsonSafeWholeMax)) {
				error = "Expected a product ID.";
				return false;
			}
			if (m.key == "product_id")
				s.product_id = static_cast<int64_t>(m.value.number);
			else
				s.expansion_id = static_cast<int64_t>(m.value.number);
		} else if (m.key == "options") {
			if (!m.value.is_object()) {
				error = "Expected server options.";
				return false;
			}
			for (const auto &option : m.value.object) {
				if (const auto *spec = gamecfg::server_setting(option.key); spec && spec->core) {
					field = option.key;
					error = "Core settings belong at the top level.";
					return false;
				}
				if ((option.key == "classes" || option.key == "weapons") &&
						option.value.is_object()) {
					Json merged = s.options.get(option.key) ? *s.options.get(option.key)
															: Json::make_object();
					for (const auto &entry : option.value.object)
						merged.set(entry.key, entry.value);
					s.options.set(option.key, std::move(merged));
				} else
					s.options.set(option.key, option.value);
			}
		} else if (m.key == "rotation") {
			if (!m.value.is_array()) {
				error = "Expected a rotation.";
				return false;
			}
			s.missions.clear();
			s.mission_options.clear();
			for (const auto &entry : m.value.array) {
				const auto *file = entry.get("file");
				const auto *swap = entry.get("swap_sides");
				if (!entry.is_object() || !file || !file->is_string() || !swap ||
						!swap->is_bool() || entry.object.size() != 2) {
					error = "Each entry needs file and swap_sides.";
					return false;
				}
				s.missions.push_back(file->string);
				s.mission_options.push_back(swap->boolean ? 1 : 0);
			}
		} else {
			error = "Unknown settings field.";
			return false;
		}
	}
	settings = std::move(s);
	field.clear();
	error.clear();
	return true;
}

gamecfg::ConfigTable title_config_table(std::string_view title) {
	const Title *row = title_by_code(title);
	return (row != nullptr ? *row : default_title()).config_table;
}

gamecfg::GameCfg settings_block(
		const HostedSettings &s, std::string_view title, const gamecfg::WeaponRoster &weapons) {
	auto cfg = gamecfg::defaults({}, 0, 0, title_config_table(title));
	std::string field, error;
	// Request validation precedes rendering. A missing override keeps retail's default.
	if (!gamecfg::apply_server_settings(cfg, s.options, true, weapons, field, error))
		throw std::invalid_argument("server configuration " + field + ": " + error);
	(void)utf8_to_cp1252(s.game_name, cfg.game_name);
	(void)utf8_to_cp1252(s.game_password, cfg.mp_host_game_password);
	cfg.mp_max_players = s.max_players;
	cfg.time_limit = s.time_limit;
	cfg.max_kills = s.kill_limit;
	cfg.max_score = s.max_score;
	cfg.timeout = s.respawn;
	cfg.startdelay = s.start_delay;
	cfg.mpattrib = s.team_ff ? cfg.mpattrib & ~0x200 : cfg.mpattrib | 0x200;
	cfg.dedicated = 1;
	return cfg;
}
} // namespace opennova::novaworld_server::hosted
