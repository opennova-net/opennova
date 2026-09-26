#include <net/novaworld/lobby_vars.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib> // std::stoi
#include <string>

namespace opennova {

namespace {

// CNapiVarList_SetOrCreate over a vector: update the (case-insensitive) match in place, else
// append. [orig: CNapiVarList_SetOrCreate @0x6318c0 -> NapiLinkedList_FindByTypeAndName
//  @0x6304f0 (type 0 + Napi_StrCaseEqual) / CNapiVarList_CreateEntry @0x6317c0 (append)]
void set_or_create(std::vector<ClientVar> &list, const char *name, std::string value) {
	for (ClientVar &v : list) {
		if (v.fnum == 0 && strutil::iequals(v.name, name)) {
			v.value = std::move(value);
			return;
		}
	}
	list.push_back({0, name, std::move(value)});
}

} // namespace

std::vector<ClientVar> make_host_setup_var_list(const HostRegistration &cfg) {
	std::vector<ClientVar> setup;
	set_or_create(setup, "LobbyName", cfg.lobby_name);
	set_or_create(setup, "ServerName", cfg.server_name);
	set_or_create(setup, "Msg", cfg.server_message);
	set_or_create(setup, "MaxPlayers", std::to_string(cfg.max_players));
	set_or_create(setup, "Password", cfg.password ? "1" : "0");
	set_or_create(setup, "Dedicated", cfg.listen_host ? "0" : "1");
	set_or_create(setup, "AppId", std::to_string(cfg.app_id));
	set_or_create(setup, "AccessCodeList", cfg.access_code_list);
	set_or_create(setup, "PLoad", "");
	set_or_create(setup, "Exp", cfg.expansion);
	set_or_create(setup, "LAN", std::to_string(cfg.lan_only));
	return setup;
}

std::vector<ClientVar> make_host_var_list(const HostRegistration &cfg, const HostLobbyText &text,
                                          bool full) {
	std::vector<ClientVar> host;
	// [orig: BuildHostVarLists @0x4d0b50 — the +532 run]
	set_or_create(host, "LobbyName", cfg.lobby_name);
	set_or_create(host, "ServerName", cfg.server_name);
	set_or_create(host, "Msg", cfg.server_message);
	set_or_create(host, "MaxPlayers", std::to_string(cfg.max_players));
	set_or_create(host, "AppId", std::to_string(cfg.app_id));
	set_or_create(host, "PLoad", "");
	set_or_create(host, "Exp", cfg.expansion);
	set_or_create(host, "LAN", std::to_string(cfg.lan_only));
	if (!full) return host;

	// [orig: Lobby_UpdateServerInfo @0x4fe8c0]
	set_or_create(host, "LobbyName", cfg.lobby_name);
	set_or_create(host, "HostKey", cfg.host_key);
	set_or_create(host, "ServerName", cfg.server_name);
	set_or_create(host, "GameType", cfg.game_type);
	set_or_create(host, "MissionName", cfg.mission_name);
	// lod_level 0/1/2 -> STRNOVA07/08/09, anything else the literal "?" @0x4fea1f.
	std::string region = "?";
	if (cfg.region_index >= 0 && cfg.region_index <= 2)
		region = text.region[static_cast<size_t>(cfg.region_index)];
	set_or_create(host, "Region", region);
	set_or_create(host, "Players", std::to_string(cfg.player_count));
	set_or_create(host, "MaxPlayers", std::to_string(cfg.max_players));
	set_or_create(host, "MI1", std::to_string(cfg.mi1));
	set_or_create(host, "MI2", std::to_string(cfg.mi2));
	set_or_create(host, "MI3", std::to_string(cfg.mi3));
	// Dedicated = "No" when the host is itself a player (is_mp_session_peer) @0x4fec27.
	set_or_create(host, "Dedicated", cfg.listen_host ? text.no : text.yes);
	set_or_create(host, "Locked", cfg.locked ? text.yes : text.no);
	set_or_create(host, "Skins", cfg.skins ? text.yes : text.no);
	if (cfg.round_time_remaining_ticks < 0) {
		set_or_create(host, "TimeLeft", text.no_time_limit);
	} else {
		// "%i" of g_round_time_remaining / 3720 (62 ticks * 60 s: whole minutes) @0x4fedc5.
		set_or_create(host, "TimeLeft", std::to_string(cfg.round_time_remaining_ticks / 3720));
	}
	set_or_create(host, "Password", cfg.password ? text.yes : text.no);
	// (g_rules_flags & 1) is the tracers-OFF rule bit @0x4feec2.
	set_or_create(host, "Tracers", cfg.tracers ? text.yes : text.no);
	// Mod = the expansion name, or the literal " " (word_7C11E4) when none @0x4feee8.
	set_or_create(host, "Mod", cfg.expansion.empty() ? " " : cfg.expansion);
	// Country: "XX" while join-locked; else the configured code unless it is "XX" or empty,
	// which fold to " " @0x4fef0b..0x4fef46.
	std::string country;
	if (cfg.locked) country = "XX";
	else if (!strutil::iequals(cfg.country, "XX") && !cfg.country.empty()) country = cfg.country;
	else country = " ";
	set_or_create(host, "Country", country);
	set_or_create(host, "Msg", cfg.server_message);
	set_or_create(host, "Port", "-1");                       // the literal word_7C3328, read @0x4fef6d
	set_or_create(host, "AllowPing", cfg.allow_ping ? "y" : "n"); // 121 / 110 @0x4fef8b
	{
		// "%ld %2.2ld:%2.2ld:%2.2ld" over the uptime in ms @0x4ff033.
		const unsigned long s = cfg.uptime_ms / 1000u;
		char age[64];
		std::snprintf(age, sizeof(age), "%lu %2.2lu:%2.2lu:%2.2lu", s / 60u / 60u / 24u,
		              s / 60u / 60u % 24u, s / 60u % 60u, s % 60u);
		set_or_create(host, "Age", age);
	}
	{
		const size_t tod = (cfg.time_of_day >= 1 && cfg.time_of_day <= 4)
		                   ? static_cast<size_t>(cfg.time_of_day) : 0u;
		set_or_create(host, "TimeOfDay", text.time_of_day[tod]);
	}
	set_or_create(host, "AppID", std::to_string(cfg.app_id)); // folds onto "AppId"
	set_or_create(host, "PCIDKey", std::to_string(cfg.pcid_key));
	set_or_create(host, "GameServerBaffleKey", std::to_string(cfg.game_server_baffle_key));
	set_or_create(host, "Stat", "N");                        // @0x4ff1fc
	// The level-range buffer is the literal "+", which equals word_7C4BA0 ("+"), so the
	// emitted value is always " " @0x4ff22b..0x4ff251.
	set_or_create(host, "LevelRange", " ");
	set_or_create(host, "BBMode", std::to_string(cfg.bb_mode));
	set_or_create(host, "GCC", cfg.gcc);
	set_or_create(host, "GV", cfg.version);
	set_or_create(host, "Version", cfg.version);
	if (cfg.dedicated_server) {
		set_or_create(host, "CountryName", cfg.country_name);
		set_or_create(host, "Lang", cfg.language);
		set_or_create(host, "TZB", std::to_string(cfg.tz_bias));
	}
	set_or_create(host, "Ver1", "3");                        // @0x4ff3ea
	set_or_create(host, "Ver2", "2345");                     // @0x4ff3ff
	set_or_create(host, "PBServer", cfg.pb_server ? "1" : "0");
	return host;
}

std::vector<ClientVar> make_player_list(const std::vector<HostPlayerSlot> &players) {
	std::vector<ClientVar> list;
	for (const HostPlayerSlot &p : players) {
		list.push_back({p.slot, "PlayerName", p.player_name});
		list.push_back({p.slot, "PlayerIpAndPort", p.ip_and_port});
		list.push_back({p.slot, "PlayerPCID", p.pcid});
		list.push_back({p.slot, "PlayerTeam", p.team});
		list.push_back({p.slot, "PlayerType", p.type});
	}
	return list;
}

// [orig: Lobby_UpdateServerInfo @0x4ff448..0x4ff62c]
LobbyStatusBlob make_host_status_blob(const HostRegistration &cfg, const HostLobbyText &text,
                                      const std::vector<HostPlayerSlot> &players) {
	LobbyStatusBlob blob;
	blob.lobby_name = cfg.lobby_name;
	blob.host_key = cfg.host_key;
	for (const ClientVar &v : make_host_var_list(cfg, text, /*full=*/true)) {
		blob.host_vars.emplace_back(v.name, v.value);
	}
	for (const HostPlayerSlot &p : players) {
		blob.player_names.push_back(p.player_name);
	}
	blob.send_player_names = cfg.send_player_list;
	return blob;
}

std::vector<ClientVar> dirty_client_vars(const std::vector<ClientVar> &previous,
                                         const std::vector<ClientVar> &next) {
	std::vector<ClientVar> dirty;
	for (const ClientVar &n : next) {
		const auto it = std::find_if(previous.begin(), previous.end(), [&](const ClientVar &p) {
			return p.fnum == n.fnum && strutil::iequals(p.name, n.name);
		});
		// String_ExactMatch: a changed VALUE dirties; a new entry is created dirty.
		if (it == previous.end() || it->value != n.value) dirty.push_back(n);
	}
	return dirty;
}

// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
NapiMessage make_host_request(const HostRegistration &cfg, const std::vector<ClientVar> &cookie,
                              int currently_hosting) {
	HostLobbyText text;
	return make_client_host_request(currently_hosting, cookie, make_host_setup_var_list(cfg),
	                                make_host_var_list(cfg, text, /*full=*/false), {});
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
