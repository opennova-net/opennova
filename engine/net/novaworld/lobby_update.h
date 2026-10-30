#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova {

struct GateResponse;

// The plaintext host-status heartbeat a hosting retail client posts to the
// gate's POSTIPADDRESS:POSTIPPORT every ~30 s (1860 sim ticks)
// [orig: Lobby_UpdateServerInfo @0x4fe8c0, the UDP leg @0x4ff448..0x4ff62c;
//  Server_TickUpdate @0x51d92c reloads the 1860-tick timer]. The wire form
// is one line of text inside the NAPI CRC envelope (no NWU):
//
//   "<LobbyName> " + " HostKey = <hk>" + (" <K> = <V>")* + (" p=<name>")* | " p="
//
// where the K/V pairs are the host's whole Host ClientVarList in list order
// (so LobbyName and HostKey each appear a second time as ordinary entries),
// every key and value is lobby-sanitized (String_SanitizeForLobby
// @0x4fe750: ' ', '?', '@', '=' -> '+'; empty -> "---"), and the player
// suffix is the PlayerList's PlayerName values when the send-players flag
// (g_NWSendPlayerList) is set, else absent.
struct LobbyStatusBlob {
	std::string lobby_name;
	std::string host_key;
	// The Host ClientVarList, in wire order, unsanitized (the build sanitizes).
	std::vector<std::pair<std::string, std::string>> host_vars;
	// PlayerName values of the PlayerList.
	std::vector<std::string> player_names;
	// g_NWSendPlayerList: emit the " p=" suffix at all.
	bool send_player_names = true;
};

// String_SanitizeForLobby @0x4fe750.
std::string lobby_sanitize_value(std::string_view value);

// Build the blob text exactly as the retail host sends it.
std::string lobby_update_build(const LobbyStatusBlob &blob);

// Build the POST heartbeat with its NAPI CRC envelope, without NWU/session
// framing. Empty when the gate has no usable POST endpoint.
std::vector<uint8_t> lobby_update_build_datagram(const GateResponse &gate,
                                                 const LobbyStatusBlob &blob);

} // namespace opennova
