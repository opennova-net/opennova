#include "novaworld/lobby_vars.h"

#include <cstdlib> // std::stoi
#include <stdexcept>
#include <string>

namespace opennova {

// [orig: NovaWorldHost::build_host_vars] — the Host(this+532) ClientVarList, in retail's column order.
std::vector<ClientVar> make_host_var_list(const HostRegistration &cfg) {
	std::vector<ClientVar> host;
	host.push_back({0, "ServerName", cfg.server_name});
	host.push_back({0, "ServerPortNumber", std::to_string(cfg.game_port)});
	host.push_back({0, "Players", std::to_string(cfg.player_count)});
	host.push_back({0, "MaxPlayers", std::to_string(cfg.max_players)});
	host.push_back({0, "Region", cfg.region});
	if (!cfg.advertise_ip.empty()) {
		host.push_back({0, "ServerIP", cfg.advertise_ip});
	}
	if (!cfg.mission_name.empty()) {
		host.push_back({0, "MissionName", cfg.mission_name});
	}
	return host;
}

// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
NapiMessage make_host_request(const HostRegistration &cfg, const std::string &server_nwuid) {
	std::vector<ClientVar> cookie = {{0, "NWUID", server_nwuid}};
	std::vector<ClientVar> host_setup = {
		{0, "AppId", cfg.app_id},
		{0, "LobbyName", cfg.lobby_name},
		{0, "MaxPlayers", std::to_string(cfg.max_players)},
		{0, "ServerPortNumber", std::to_string(cfg.game_port)},
	};
	std::vector<ClientVar> player_list = {{0, "Slot0", cfg.player_name}};
	return make_client_host_request(/*CurrentlyHosting*/ 1, cookie, host_setup,
	                                make_host_var_list(cfg), player_list);
}

// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860]
NapiMessage make_host_update(const HostRegistration &cfg) {
	std::vector<ClientVar> player_list = {{0, "Slot0", cfg.player_name}};
	return make_client_host_update(make_host_var_list(cfg), player_list);
}

// [orig: NovaWorldClient::az_fingerprint]
std::string az_fingerprint(uint32_t seed, int len) {
	static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint32_t x = seed ? seed : 0x12345678u;
	std::string s;
	s.reserve(static_cast<size_t>(len < 0 ? 0 : len));
	for (int i = 0; i < len; ++i) {
		x = x * 1664525u + 1013904223u;
		s.push_back(kAlpha[(x >> 24) % 26u]);
	}
	return s;
}

// [orig: NovaWorldClient::begin_session identity build] — the NW-S5 10-var "Cookie" set.
std::vector<std::pair<std::string, std::string>> make_lobby_identity_vars(const LobbyIdentityParams &p) {
	return {
		{"CountryName", p.country},
		{"Language", p.language},
		{"TimeZoneBias", p.tz_bias},
		{"MyInstalledExpBits", p.my_installed_exp_bits},
		{"NWUID", ""},        // echoed from the ServerSessionInit at use
		{"NWCDKIID", ""},     // empty in retail; verify is not CD-key-gated
		{"NWCDKIIDEXP1", ""},
		{"NWPSSK", az_fingerprint(p.client_index ^ 0x5053534Bu, 23)},
		{"NWUSID", az_fingerprint(p.client_key ^ 0x55534944u, 16)},
		{"NWHWI", p.nwhwi},
	};
}

// [orig: the inline UDPNOVAWORLD split in NovaWorldClient/Host::poll_gate]
bool parse_host_port(const std::string &in, std::string &host, uint16_t &port) {
	const std::size_t colon = in.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	host = in.substr(0, colon);
	try {
		port = static_cast<uint16_t>(std::stoi(in.substr(colon + 1)));
	} catch (...) {
		return false;
	}
	return true;
}

} // namespace opennova
