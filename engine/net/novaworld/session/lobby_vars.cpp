#include <net/novaworld/lobby_vars.h>

#include <algorithm>
#include <array>
#include <cstdio>
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

namespace {

void xor_u32_le(std::array<uint8_t, 16> &bytes, std::size_t offset, uint32_t value) {
	for (std::size_t i = 0; i < 4; ++i) {
		bytes[offset + i] ^= static_cast<uint8_t>(value >> (i * 8u));
	}
}

std::string encode_nwpssk(const std::array<uint8_t, 16> &bytes) {
	std::string out;
	out.reserve(23);
	for (std::size_t group = 0; group < 4; ++group) {
		const std::size_t i = group * 4;
		out.push_back(static_cast<char>('A' + (bytes[i] & 0x0Fu)));
		out.push_back(static_cast<char>('C' + (bytes[i + 1] & 0x0Fu)));
		out.push_back(static_cast<char>('E' + ((bytes[i] ^ bytes[i + 1]) >> 4u)));
		out.push_back(static_cast<char>('A' + (bytes[i + 2] & 0x0Fu)));
		out.push_back(static_cast<char>('C' + (bytes[i + 3] & 0x0Fu)));
		if (group != 3) {
			out.push_back(static_cast<char>('E' + ((bytes[i + 2] ^ bytes[i + 3]) >> 4u)));
		}
	}
	return out;
}

} // namespace

LobbyMachineTokens make_retail_machine_tokens(const RetailMachineInputs &in) {
	std::array<uint8_t, 16> pssk_bytes{};
	char serial_hex[9]{};
	std::snprintf(serial_hex, sizeof(serial_hex), "%08X", static_cast<unsigned>(in.volume_serial));

	// Retail first folds the eight printable serial-hex bytes into four DWORDs.
	pssk_bytes[3] ^= static_cast<uint8_t>(serial_hex[3]);
	pssk_bytes[4] ^= static_cast<uint8_t>(serial_hex[4]);
	pssk_bytes[5] ^= static_cast<uint8_t>(serial_hex[5]);
	pssk_bytes[0] ^= static_cast<uint8_t>(serial_hex[0]);
	pssk_bytes[15] ^= static_cast<uint8_t>(serial_hex[0]);
	pssk_bytes[1] ^= static_cast<uint8_t>(serial_hex[1]);
	pssk_bytes[2] ^= static_cast<uint8_t>(serial_hex[2]);
	pssk_bytes[14] ^= static_cast<uint8_t>(serial_hex[1]);
	pssk_bytes[6] ^= static_cast<uint8_t>(serial_hex[6]);
	pssk_bytes[13] ^= static_cast<uint8_t>(serial_hex[2]);
	pssk_bytes[12] ^= static_cast<uint8_t>(serial_hex[3]);
	pssk_bytes[11] ^= static_cast<uint8_t>(serial_hex[4]);
	pssk_bytes[10] ^= static_cast<uint8_t>(serial_hex[5]);
	pssk_bytes[9] ^= static_cast<uint8_t>(serial_hex[6]);
	pssk_bytes[8] ^= static_cast<uint8_t>(serial_hex[7]);
	pssk_bytes[7] ^= static_cast<uint8_t>(serial_hex[7]); // serial_hex[8] is the NUL

	xor_u32_le(pssk_bytes, 0, in.volume_serial);
	xor_u32_le(pssk_bytes, 4, in.maximum_component_length ^ (in.volume_serial >> 1u));
	xor_u32_le(pssk_bytes, 8, in.filesystem_flags ^ (in.volume_serial >> 2u));
	xor_u32_le(pssk_bytes, 12, (in.volume_serial ^ 0x0B24B390u) >> 3u);

	for (std::size_t i = 0; i < std::min<std::size_t>(16, in.volume_name.size()); ++i) {
		pssk_bytes[i] ^= static_cast<uint8_t>(in.volume_name[i]);
	}
	for (std::size_t i = 0; i < std::min<std::size_t>(16, in.filesystem_name.size()); ++i) {
		pssk_bytes[i] ^= static_cast<uint8_t>(in.filesystem_name[i]);
	}

	std::array<uint8_t, 8> usid_bytes{};
	const uint32_t usid_high = in.maximum_component_length ^ (in.volume_serial >> 1u);
	for (std::size_t i = 0; i < 4; ++i) {
		usid_bytes[i] = static_cast<uint8_t>(in.volume_serial >> (i * 8u));
		usid_bytes[4 + i] = static_cast<uint8_t>(usid_high >> (i * 8u));
	}
	usid_bytes[0] = 72;
	if (in.has_ethernet_address) {
		usid_bytes[0] = 77;
		std::copy(in.ethernet_address.begin(), in.ethernet_address.end(), usid_bytes.begin() + 1);
		usid_bytes[7] = 169;
	}

	std::string nwusid;
	nwusid.reserve(16);
	for (std::size_t i = 0; i < usid_bytes.size(); ++i) {
		nwusid.push_back(static_cast<char>('A' + i + (usid_bytes[i] & 0x0Fu)));
		nwusid.push_back(static_cast<char>('A' + i + (usid_bytes[i] >> 4u)));
	}

	return LobbyMachineTokens{encode_nwpssk(pssk_bytes), std::move(nwusid)};
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
		{"NWPSSK", p.nwpssk.empty() ? az_fingerprint(p.client_index ^ 0x5053534Bu, kNwpsskLen) : p.nwpssk},
		{"NWUSID", p.nwusid.empty() ? az_fingerprint(p.client_key ^ 0x55534944u, kNwusidLen) : p.nwusid},
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
