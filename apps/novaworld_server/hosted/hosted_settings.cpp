#include "hosted_settings.h"

#include "loggable.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>
#include <formats/gamecfg/game_cfg.h>

#include <array>
#include <cstdio>
#include <string_view>

namespace opennova::novaworld_server::hosted {

namespace {

std::optional<SettingsRefusal> out_of_range(const char *field, int value, int min, int max) {
	if (value >= min && value <= max) return std::nullopt;
	return SettingsRefusal{ field,
		"must be " + std::to_string(min) + " to " + std::to_string(max) };
}

// A host-file value: quoted, in the host's code page (check_settings has
// refused anything a quote cannot carry). Text that does not convert (never
// after check_settings) goes as it is.
std::string quoted(const std::string &utf8) {
	std::string bytes;
	if (!utf8_to_cp1252(utf8, bytes)) bytes = utf8;
	return "\"" + bytes + "\"";
}

} // namespace

bool is_mission_file_name(std::string_view name) {
	if (name.size() < 5 || name.size() > kMissionNameMaxBytes) return false;
	if (!strutil::ends_with_icase(name, ".bms")) return false;
	for (const char c : name.substr(0, name.size() - 4)) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '_' || c == '-';
		if (!ok) return false;
	}
	return true;
}

std::optional<SettingsRefusal> check_settings(const HostedSettings &s) {
	{
		std::string bytes;
		if (has_control_byte(s.game_name) || has_c1_control(s.game_name))
			return SettingsRefusal{ "game_name", "holds a control character" };
		if (s.game_name.find('"') != std::string::npos)
			return SettingsRefusal{ "game_name", "holds a double quote" };
		if (!utf8_to_cp1252(s.game_name, bytes))
			return SettingsRefusal{ "game_name",
				"holds a character the game's code page (cp1252) has none for" };
		if (strutil::trim_view(bytes).empty())
			return SettingsRefusal{ "game_name", "is empty" };
		if (bytes.size() > kGameNameMaxBytes)
			return SettingsRefusal{ "game_name",
				"is longer than " + std::to_string(kGameNameMaxBytes) + " characters" };
	}
	if (s.game_password.size() > kGamePasswordMaxBytes)
		return SettingsRefusal{ "game_password",
			"is longer than " + std::to_string(kGamePasswordMaxBytes) + " characters" };
	for (const char c : s.game_password) {
		if (c <= ' ' || c > '~' || c == '"')
			return SettingsRefusal{ "game_password",
				"takes printable ASCII with no space or double quote" };
	}
	if (auto r = out_of_range("max_players", s.max_players, kMaxPlayersMin, kMaxPlayersMax)) return r;
	if (auto r = out_of_range("time_limit", s.time_limit, kTimeLimitMin, kTimeLimitMax)) return r;
	if (auto r = out_of_range("kill_limit", s.kill_limit, kKillLimitMin, kKillLimitMax)) return r;
	if (auto r = out_of_range("max_score", s.max_score, kMaxScoreMin, kMaxScoreMax)) return r;
	if (auto r = out_of_range("respawn", s.respawn, kRespawnMin, kRespawnMax)) return r;
	if (auto r = out_of_range("start_delay", s.start_delay, kStartDelayMin, kStartDelayMax)) return r;
	if (!s.expansion.empty() && s.expansion != kExpansionEscalation)
		return SettingsRefusal{ "expansion",
			std::string("is \"\" (Joint Operations) or \"") + kExpansionEscalation + "\"" };
	if (s.missions.empty()) return SettingsRefusal{ "missions", "names no mission" };
	if (s.missions.size() > kMissionsMax)
		return SettingsRefusal{ "missions",
			"names more than " + std::to_string(kMissionsMax) + " missions" };
	for (const std::string &m : s.missions) {
		if (!is_mission_file_name(m))
			return SettingsRefusal{ "missions",
				"names \"" + loggable(m, 40) +
						"\", which is no .bms file name of letters, digits, _ and -" };
	}
	return std::nullopt;
}

std::string join_missions(const std::vector<std::string> &missions) {
	std::string out;
	for (const std::string &m : missions) {
		if (!out.empty()) out.push_back('\n');
		out += m;
	}
	return out;
}

std::vector<std::string> split_missions(const std::string &column) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= column.size()) {
		const size_t end = column.find('\n', pos);
		const std::string line = column.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		if (!line.empty()) out.push_back(line);
		if (end == std::string::npos) break;
		pos = end + 1;
	}
	return out;
}

std::string render_host_file(const HostedSettings &s, int64_t server_id) {
	std::string out;
	auto line = [&out](const std::string &text) {
		out += text;
		out += "\r\n";
	};
	line("// Hosted server " + std::to_string(server_id) +
			", rendered by the NovaWorld service at its start (apps/novaworld_server/hosted).");
	line("GameName " + quoted(s.game_name));
	line("MaxPlayers " + std::to_string(s.max_players));
	line("MPHostGamePassword " + quoted(s.game_password));
	line("Time_Limit " + std::to_string(s.time_limit));
	line("KillLimit " + std::to_string(s.kill_limit));
	line("MaxScore " + std::to_string(s.max_score));
	line("Respawn " + std::to_string(s.respawn));
	line("Delay " + std::to_string(s.start_delay));
	line(std::string("TeamFF ") + (s.team_ff ? "1" : "0"));
	// Always on: the rotation starts over at its end, so the session never ends
	// by itself (with Replay 0 it ends and opennova-serve exits 0).
	line("Replay 1");
	// The last Mission line is the starting map: the first map's line goes last,
	// so the rotation then plays the rest in order.
	for (size_t i = 1; i < s.missions.size(); ++i) line("Mission " + s.missions[i] + " 0");
	if (!s.missions.empty()) line("Mission " + s.missions.front() + " 0");
	return out;
}

std::string render_game_cfg(const MachinePlan &plan) {
	gamecfg::GameCfg cfg = gamecfg::defaults();
	cfg.networkconnecttype = 1; // the NovaWorld screen's network type: list on the gate
	cfg.mp_novaworld_host_lan_only = 0;
	cfg.mp_novaworld_port_min = plan.nw_port;
	cfg.mp_novaworld_port_max = plan.nw_port;
	cfg.mp_novaworld_port_delta = 1;
	cfg.mp_novaworld_port_random = 0;
	cfg.remote_admin_port = plan.admin_port;
	return gamecfg::write(cfg, {});
}

std::string render_admin_cfg(const MachinePlan &plan, const std::string &admin_password) {
	char rights[16];
	std::snprintf(rights, sizeof(rights), "%X", static_cast<unsigned>(kAdminRights));
	std::string out = "// The NovaWorld service's admin link; rendered at every start.\r\n";
	out += std::string(kAdminUser) + " " + admin_password + " " + rights + "\r\n";
	out += "ip_restrict = " + plan.admin_allowed_ip + "\r\n";
	return out;
}

std::string render_credentials(const std::string &username, const std::string &password) {
	return "NOVAWORLD_USER=" + username + "\nNOVAWORLD_PASS=" + password + "\n";
}

LaunchSpec render_launch(const HostedSettings &settings, const RenderInputs &in) {
	LaunchSpec spec;
	spec.server_id = in.server_id;
	spec.files.push_back({ kHostFileName, render_host_file(settings, in.server_id), true });
	spec.files.push_back({ kGameCfgFileName, render_game_cfg(in.plan), false });
	spec.files.push_back({ kAdminCfgFileName, render_admin_cfg(in.plan, in.admin_password), true });
	spec.files.push_back({ kCredentialsFileName,
			render_credentials(in.service_username, in.service_password), true });
	spec.serve_args = { "/HOST", kHostFileName, "--credentials", kCredentialsFileName,
		"--master-host", in.master_host, "--master-gate-port",
		std::to_string(in.master_gate_port) };
	if (!settings.expansion.empty()) {
		spec.serve_args.push_back("/exp");
		spec.serve_args.push_back(settings.expansion);
	}
	return spec;
}

std::string random_secret(size_t length) {
	static constexpr char kAlphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	constexpr unsigned kAlphabetSize = sizeof(kAlphabet) - 1; // 62
	// Bytes past the largest multiple of 62 are drawn again, so every character
	// is equally likely.
	constexpr unsigned kAccept = 256 - 256 % kAlphabetSize;
	std::string out;
	out.reserve(length);
	std::array<uint8_t, 64> pool{};
	while (out.size() < length) {
		os_random_bytes(pool.data(), pool.size());
		for (const uint8_t b : pool) {
			if (b >= kAccept) continue;
			out.push_back(kAlphabet[b % kAlphabetSize]);
			if (out.size() == length) break;
		}
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
