#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <net/napi/session.h> // ClientVar / NapiMessage / make_client_host_request / make_client_host_update

// P7 Part 2 (B2) — the NovaWorld lobby var-builders + identity set, moved out of the Godot bindings
// so they become pure pumps (ADR 0010 / .agents/network.md: lobby payloads belong in engine/net/novaworld,
// sockets + signals in Godot). The host registration var-lists (the GSB row), the NW-S5 client
// identity "Cookie" set (used for BOTH the UDP verify var-list and the HTTP login cookies), and the
// UDPNOVAWORLD "host:port" split. Godot-free. The bindings pass their GDScript-set config in.
//
// Faithful port of NovaWorldHost::build_host_vars()/send_host_request()/send_host_update() and
// NovaWorldClient::begin_session()'s identity assembly + az_fingerprint. Literals are byte-preserved.
namespace opennova {

// The GDScript-set host fields the binding holds (the NovaWorldHost members). Defaults match the
// binding's property defaults so the libs builders reproduce the same bytes when a field is unset.
struct HostRegistration {
	std::string server_name = "OpenNova Host";
	std::string mission_name;            // optional — emitted only when non-empty
	int max_players = 32;
	std::string region = "us";
	std::string player_name = "Host";
	int game_port = 32768;
	std::string advertise_ip;            // optional ServerIP — emitted only when non-empty
	std::string app_id = "28";           // JO (pfid 28); HostSetup.AppId
	std::string lobby_name = "jop_2_consumer";
	int player_count = 1;                // the host itself is the first player
};

// The "Host" var-list shared by the host request + update. Order is load-bearing:
// ServerName, ServerPortNumber, Players, MaxPlayers, Region, [ServerIP], [MissionName].
// [orig: NovaWorldHost::build_host_vars — the GSB-row Host(this+532) ClientVarList]
std::vector<ClientVar> make_host_var_list(const HostRegistration &cfg);

// ClientHostRequest: CurrentlyHosting=1, Cookie{NWUID}, HostSetup{AppId,LobbyName,MaxPlayers,
// ServerPortNumber}, Host(make_host_var_list), PlayerList{Slot0:player_name}.
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
NapiMessage make_host_request(const HostRegistration &cfg, const std::string &server_nwuid);

// ClientHostUpdate: Host + PlayerList only (the heartbeat refresh; no Cookie/HostSetup).
// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860]
NapiMessage make_host_update(const HostRegistration &cfg);

// Deterministic [A-Z] fallback for callers without platform machine inputs.
// The Godot binding supplies the retail transform below on Windows.
std::string az_fingerprint(uint32_t seed, int len);

// Inputs consumed by retail's two stable-machine token encoders. Platform
// discovery (GetVolumeInformation/GetAdaptersInfo in the original) stays in
// the binding; these byte transforms are portable and testable.
// [orig: CDKey_GenerateHardwareFingerprint @ 0x4a4a00 /
// generate_hardware_fingerprint @ 0x4a4d00]
struct RetailMachineInputs {
	uint32_t volume_serial = 0;
	uint32_t maximum_component_length = 0;
	uint32_t filesystem_flags = 0;
	std::string volume_name;
	std::string filesystem_name;
	std::array<uint8_t, 6> ethernet_address{};
	bool has_ethernet_address = false;
};

struct LobbyMachineTokens {
	std::string nwpssk;
	std::string nwusid;
};

LobbyMachineTokens make_retail_machine_tokens(const RetailMachineInputs &in);

// Inputs for the NW-S5 identity "Cookie" set. Environment collection belongs to the binding;
// this portable builder only preserves the witnessed names/order and values it is given. The
// client_index/client_key fallback exists for non-Godot callers, but bindings should supply the
// stable machine-derived NWPSSK/NWUSID explicitly.
struct LobbyIdentityParams {
	uint32_t client_index = 0;
	uint32_t client_key = 0;
	std::string tz_bias = "0";
	std::string country = "United States";
	std::string language = "English";
	std::string my_installed_exp_bits = "0";
	std::string nwpssk;
	std::string nwusid;
	std::string nwhwi = "OpenNova$0$2048$1920x1080$1920x1080";
};

// The wire-load-bearing NWPSSK/NWUSID token lengths: retail's stable-machine
// token encoders always emit exactly these many [A-Z] characters, and any
// fallback generator must match or the verify Cookie is malformed.
// [orig: CDKey_GenerateHardwareFingerprint @ 0x4a4a00 /
// generate_hardware_fingerprint @ 0x4a4d00 output widths]
inline constexpr int kNwpsskLen = 23;
inline constexpr int kNwusidLen = 16;

// The NW-S5 10-var identity "Cookie" set (capture frame 10166), built once and reused for BOTH the
// UDP verify var-list (ClientSession::Config::verify_cookie_vars) and the HTTP login cookies. NWUID
// is left empty here (the consumer substitutes the SessionInit nwuid at use). The XOR masks + the
// lengths kNwpsskLen/kNwusidLen (23/16) on NWPSSK/NWUSID are load-bearing.
// [orig: NovaWorldClient::begin_session identity build]
std::vector<std::pair<std::string, std::string>> make_lobby_identity_vars(const LobbyIdentityParams &p);

// Split a "host:port" string (the gate's UDPNOVAWORLD). Returns false on a missing colon or a
// non-numeric port; the port is the faithful truncating static_cast<uint16_t>(stoi(...)).
// [orig: the inline UDPNOVAWORLD split in NovaWorldClient/Host::poll_gate]
bool parse_host_port(const std::string &in, std::string &host, uint16_t &port);

} // namespace opennova
