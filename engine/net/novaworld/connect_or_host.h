#pragma once

// The NovaWorld menu's hosting leg ahead of its request: the gates CNapiGameSession_ConnectOrHost
// runs on the logged-in game session before ClientHostRequest ships, and the player cap it
// clamps. The request, the 60 s poll and the result map ride ClientSession and the NWEC tables
// (net/napi/session.h).

#include <net/napi/session.h> // the NWEC tags

#include <algorithm>
#include <cstdint>
#include <string>

namespace opennova {

// "NWU NOT SET UP" keeps the leg's preset tag [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 —
// the preset @0x4d4f89, the bail @0x4d4f95..0x4d4fd2].
inline constexpr char NWEC_SESSION_NOT_SET_UP[] = "NWEC01";

// The tag the hosting leg fails with before its request, or nullptr to send it: a session that
// is not set up (NWEC01), one outside states 4..8 -- flags bits 2 and 8 -- (NWEC49), and a gate
// reply missing or without a lobby name (NWEC50).
// [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 — the flags test @0x4d4fd8..0x4d56f6, the
//  gate test `dword_B5FF48 && g_LobbyName[0]` @0x4d5085..0x4d50dc]
inline const char *host_leg_refusal(bool session_set_up, uint32_t session_flags, bool gate_ok,
		const std::string &lobby_name) {
	if (!session_set_up) return NWEC_SESSION_NOT_SET_UP;
	if ((session_flags & 0x2u) == 0 || (session_flags & 0x8u) == 0) return NWEC_WRONG_SESSION_STATE;
	if (!gate_ok || lobby_name.empty()) return NWEC_HOST_GATE_NOT_OK;
	return nullptr;
}

// The leg's MaxPlayers clamp: at least 1, at most 64, or 65 for a dedicated host.
// [orig: CNapiGameSession_ConnectOrHost @0x4d504e..0x4d507b]
inline int host_leg_max_players(int max_players, bool dedicated) {
	return std::clamp(max_players, 1, dedicated ? 65 : 64);
}

} // namespace opennova
