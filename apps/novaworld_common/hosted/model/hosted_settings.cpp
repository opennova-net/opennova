#include "hosted_settings.h"
#include "hosted/protocol/slots.h"

#include "core/loggable.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_settings.h> // server_settings_title

#include <cstdio>
#include <algorithm>
#include <string_view>

namespace opennova::novaworld_server::hosted {

namespace {

std::optional<SettingsRefusal> out_of_range(const char *field, int value, const SettingRange &range) {
	if (value >= range.min && value <= range.max) return std::nullopt;
	return SettingsRefusal{ field,
		"must be " + std::to_string(range.min) + " to " + std::to_string(range.max) };
}

// Whether `name` is among `missions` by file name, without case (the archives
// spell their entries in their own case).
bool lists_mission(const std::vector<TreeMission> &missions, std::string_view name) {
	for (const TreeMission &m : missions) {
		if (strutil::iequals(m.file, name)) return true;
	}
	return false;
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

std::optional<SettingsRefusal> check_settings(
		const HostedSettings &s, const std::vector<OfferedProduct> &offered) {
	const OfferedProduct *product = find_offered(offered, s.product_id);
	if (!product) return SettingsRefusal{ "product_id", "is no game this service offers" };
	if (s.list_on_novalogic && !novalogic_lists(*product))
		return SettingsRefusal{ "list_on_novalogic",
			"NovaLogic's NovaWorld lists Joint Operations servers only" };
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
		// NovaLogic's service refuses a server name holding "nova" in any case with 1009,
		// SERVERNAMENOTACCEPTED (witnessed live 2026-10-25, ADR 0068), so a listing there
		// asked over one would list nowhere: the start asks for another name.
		if (s.list_on_novalogic && strutil::ifind(s.game_name, "nova") != std::string_view::npos)
			return SettingsRefusal{ "game_name",
				"holds \"Nova\", which NovaLogic's NovaWorld does not accept in a server name: "
				"rename the server or untick the NovaLogic box" };
	}
	if (s.game_password.size() > kGamePasswordMaxBytes)
		return SettingsRefusal{ "game_password",
			"is longer than " + std::to_string(kGamePasswordMaxBytes) + " characters" };
	for (const char c : s.game_password) {
		if (c <= ' ' || c > '~' || c == '"')
			return SettingsRefusal{ "game_password",
				"takes printable ASCII with no space or double quote" };
	}
	const HostLimits limits = host_limits_for_title(product->title);
	if (auto r = out_of_range("max_players", s.max_players, limits.max_players)) return r;
	if (auto r = out_of_range("time_limit", s.time_limit, limits.time_limit)) return r;
	if (auto r = out_of_range("kill_limit", s.kill_limit, limits.kill_limit)) return r;
	if (auto r = out_of_range("max_score", s.max_score, limits.max_score)) return r;
	if (auto r = out_of_range("respawn", s.respawn, limits.respawn)) return r;
	if (auto r = out_of_range("start_delay", s.start_delay, limits.start_delay)) return r;
	if (auto r = out_of_range("send_holdoff_ticks", s.send_holdoff_ticks, kSendHoldoffTicksRange)) return r;
	if (auto r = out_of_range("entity_send_budget", s.entity_send_budget, kEntitySendBudgetRange)) return r;
	if (auto r = out_of_range("max_packet_size", s.max_packet_size, kMaxPacketSizeRange)) return r;
	if (s.options.get("max_packet_size"))
		return SettingsRefusal{ "options.max_packet_size", "Core settings belong at the top level." };
	const OfferedExpansion *expansion = nullptr;
	if (s.expansion_id) {
		expansion = find_offered_expansion(*product, *s.expansion_id);
		if (!expansion)
			return SettingsRefusal{ "expansion_id", "is no expansion of this game with data" };
	}
	const gamecfg::ServerSettingsTitle settings_title =
			inmatch::server_settings_title(product->title);
	auto block = gamecfg::defaults({}, 0, 0, settings_title.table);
	std::string field, error;
	const auto &weapons =
			server_weapon_roster(product->weapons, expansion ? &expansion->weapons : nullptr);
	if (!gamecfg::apply_server_settings(
				block, s.options, true, weapons, settings_title, field, error))
		return SettingsRefusal{ "options." + field, error };
	if (s.mission_options.size() > s.missions.size() ||
			std::any_of(s.mission_options.begin(), s.mission_options.end(),
					[](int v) { return v != 0 && v != 1; }))
		return SettingsRefusal{ "rotation", "invalid side-switch option" };
	if (s.missions.empty()) return SettingsRefusal{ "missions", "names no mission" };
	if (s.missions.size() > kMissionsMax)
		return SettingsRefusal{ "missions",
			"names more than " + std::to_string(kMissionsMax) + " missions" };
	for (const std::string &m : s.missions) {
		if (!is_mission_file_name(m))
			return SettingsRefusal{ "missions",
				"names \"" + loggable(m, 40) +
						"\", which is no .bms file name of letters, digits, _ and -" };
		if (!lists_mission(product->missions, m) && !(expansion && lists_mission(expansion->missions, m)))
			return SettingsRefusal{ "missions",
				"names \"" + loggable(m, 40) + "\", which is no mission of this game" };
	}
	for (size_t i = 0; i < s.missions.size(); ++i) {
		const int option = i < s.mission_options.size() ? s.mission_options[i] : 0;
		for (size_t j = 0; j < i; ++j)
			if (strutil::iequals(s.missions[i], s.missions[j]) &&
					option != (j < s.mission_options.size() ? s.mission_options[j] : 0))
				return SettingsRefusal{"rotation", "Side switching is shared by repeated occurrences of the same map."};
	}
	return std::nullopt;
}

std::string render_host_file(const HostedSettings &s, int64_t server_id) {
	std::string out;
	auto line = [&out](const std::string &text) {
		out += text;
		out += "\r\n";
	};
	line("// Hosted server " + std::to_string(server_id) +
			", rendered by the NovaWorld service at its start (apps/novaworld_common/hosted).");
	line("GameName " + quoted(s.game_name));
	line("MaxPlayers " + std::to_string(s.max_players));
	line("MPHostGamePassword " + quoted(s.game_password));
	line("Time_Limit " + std::to_string(s.time_limit));
	line("KillLimit " + std::to_string(s.kill_limit));
	line("MaxScore " + std::to_string(s.max_score));
	line("Respawn " + std::to_string(s.respawn));
	line("Delay " + std::to_string(s.start_delay));
	line(std::string("TeamFF ") + (s.team_ff ? "1" : "0"));
	// With Replay 0 the rotation ends normally and releases the allocation.
	line(std::string("Replay ") + (s.options.get_bool("replay", true) ? "1" : "0"));
	// --rotation-first selects entry zero after the retail host-file read.
	const auto mission_line = [&](size_t i) {
        line("Mission " + s.missions[i] + " " + std::to_string(i < s.mission_options.size() ? s.mission_options[i] : 0));
    };
    for (size_t i = 0; i < s.missions.size(); ++i) mission_line(i);
	return out;
}

std::string render_game_cfg(const SlotPlan &plan, const HostedSettings &settings, std::string_view title, const gamecfg::WeaponRoster &weapons) {
	gamecfg::GameCfg cfg = settings_block(settings, title, weapons);
	cfg.networkconnecttype = 1; // the NovaWorld screen's network type: list on the gate
	cfg.mp_novaworld_host_lan_only = 0;
	cfg.mp_novaworld_port_min = plan.game_port;
	cfg.mp_novaworld_port_max = plan.game_port;
	cfg.mp_novaworld_port_delta = 1;
	cfg.mp_novaworld_port_random = 0;
	cfg.remote_admin_port = plan.admin_port;
	// Retail's writer omits/misspells these two readable keys. Launch files
    // explicitly include them; the retail format writer itself stays faithful.
    return gamecfg::write(cfg, weapons) + "mp_flagreturntime = " + std::to_string(cfg.flag_return_time) +
            "\r\nmp_flagresettime = " + std::to_string(cfg.flag_reset_time) + "\r\n";
}

std::string render_admin_cfg(const SlotPlan &plan, const std::string &admin_password) {
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

HostedSettings launched_settings(const HostedSettings &settings, MachineKind kind) {
	HostedSettings launched = settings;
	if (kind != MachineKind::Own) launched.lock_framerate = true;
	return launched;
}

LaunchSpec render_launch(const HostedSettings &owner_settings, std::string_view title,
		std::string_view expansion_code, const RenderInputs &in) {
	const HostedSettings settings = launched_settings(owner_settings, in.machine_kind);
	LaunchSpec spec;
	spec.server_id = in.server_id;
	spec.files.push_back({ kHostFileName, render_host_file(settings, in.server_id), true });
	spec.files.push_back({ kGameCfgFileName, render_game_cfg(in.plan, settings, title, in.weapons), true });
	spec.files.push_back({ kAdminCfgFileName, render_admin_cfg(in.plan, in.admin_password), true });
	spec.files.push_back({ kCredentialsFileName,
			render_credentials(in.service_username, in.service_password), true });
	if (lists_on_novalogic(settings, in.novalogic))
		spec.files.push_back({ kNovaLogicCredentialsFileName,
				render_credentials(in.novalogic.username, in.novalogic.password), true });
	spec.serve_args = render_serve_args(
			settings, title, expansion_code, in.master_host, in.master_gate_port, in.novalogic);
	return spec;
}

bool lists_on_novalogic(const HostedSettings &settings, const NovaLogicListing &novalogic) {
	return settings.list_on_novalogic && novalogic.offered();
}

std::vector<std::string> render_serve_args(const HostedSettings &settings, std::string_view title,
		std::string_view expansion_code, const std::string &master_host, uint16_t master_gate_port,
		const NovaLogicListing &novalogic) {
	// A supervised server's: no status page (its lines would ride the server's
	// log stream every 200 ms), its tick health and its exit's line for the
	// agent (hosted/protocol/slots.h).
	std::vector<std::string> args = { "/HOST", kHostFileName, "--credentials", kCredentialsFileName,
		"--master-host", master_host, "--master-gate-port", std::to_string(master_gate_port),
		"--status-page", "off", "--stats-file", kTickStatsFileName, "--exit-file", kExitFileName,
		"--send-holdoff-ticks", std::to_string(settings.send_holdoff_ticks),
		"--entity-send-budget", std::to_string(settings.entity_send_budget),
		"--rotation-first", "/game", std::string(title) };
	if (!expansion_code.empty()) {
		args.push_back("/exp");
		args.emplace_back(expansion_code);
	}
	// An owner who asked sees the listing's state on the server's page: the second listing's
	// file for the agent, which says off when the service offers no listing this start.
	if (settings.list_on_novalogic)
		args.insert(args.end(), { "--second-listing-file", kSecondListingFileName });
	if (lists_on_novalogic(settings, novalogic)) {
		args.insert(args.end(), { "--also-master-host", novalogic.gate_host,
				"--also-master-gate-port", std::to_string(novalogic.gate_port), "--also-credentials",
				kNovaLogicCredentialsFileName, "--allow-public" });
	}
	return args;
}

} // namespace opennova::novaworld_server::hosted
