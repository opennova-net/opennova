#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <novaworld/protocol_message.h>

#include "npruntime/napi_np_connection.h"

// P8 — the reactive in-match gameplay-message reply path, ported off the retired
// libs/novaworld/game_session.cpp (`GameSession`) + game_server_runtime.cpp (`GameServerRuntime`).
// This is the faithful translation of the gameplay-layer C2S dispatch table [orig: g_msginfo_table
// @0x82B5D8 → NapiNPServerMsg_0x0NN]: a 0x43 SESSION packet carries gameplay messages, each routed to
// its server handler, which queues reactive replies via NapiNPServer_SendFiltered. It produces the
// §5.1 handshake / server-info / mission-metadata / loadout / spawn-confirm replies a retail joiner
// expects.
//
// What it does NOT do: the world-stream / spawn-gate burst. The original emits that ONE-SHOT on
// player-add [orig: Server_OnPlayerJoin @0x51a680 → Server_SendInitialGameStateToPlayer @0x51bba0];
// the npruntime equivalent is `Server_SendInitialGameStateToPlayer` over `conn.burst`. The retired
// game_session.cpp grew an empirical per-tick phase machine (queue_mission_bootstrap /
// queue_state4_loading_gate / the 0x10/0x0A/0x57 tick cadence) with no original-engine counterpart;
// that machine is dropped (net-re §5.45 / D-NET-127). Reply BODIES are carried verbatim from the old
// builders (captured-from-observation fixtures, D-NET-127) pending the per-body grill wave.
namespace opennova::np {

// The in-match session config the reactive reply handlers read (the §5.1 reply slice of the retired
// GameSessionConfig). Server / mission / player identity + the host-advertised spawn. The
// replicated-entity / spawn-point inputs are NOT here — those fed the retired burst machine; the
// faithful world-stream burst sources its bodies from `World` + `bms::File`.
struct SessionReplyConfig {
	std::string server_name = "OpenNova Dev";
	std::string mission_name = "AS - Dormant Volcano Isle";
	std::string mission_file = "ASH_I5A.BMS";
	std::string expansion = "jox01";
	std::string player_name = "DevUser";
	std::string pcid;             // [orig player+0x250] the player's PCID string (0x7A body /
	                              // 0x7B field 2). Empty on a dev host -> the 0x7A body is a single NUL.
	uint32_t gametype = 0x00010010u;
	uint32_t mpattrib = 14854u;
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	std::vector<std::string> spawn_names;
	std::vector<uint8_t> mission_header_blob;
};

// Dispatch the decoded in-match gameplay `messages` for `conn` to their reply handlers and return the
// reactive replies to frame onto the connection. Caches the joiner's pre-spawn C2S 0x0C pose into
// `conn.reply`; gates the spawn-confirm replies on `conn.burst` (the faithful spawn authority). A
// remote (type-1) connection and the host's own type-2 loopback both run this the same way.
// [orig: per-message NapiNPServerMsg_0x0NN handlers reached from the 0x43 SESSION dispatch]
std::vector<ProtocolMessage> dispatch_session_replies(const SessionReplyConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick);

// Install/refresh the per-connection player binding (slot + entity handle) the roster (0x46/0x16) +
// spawn-confirm (0x51) replies read. [orig: a player bound to its allocated slot/entity at add]
bool bind_session_reply_player(NapiNPConnection &conn, std::string player_name, uint8_t player_slot,
                               uint16_t entity_handle);

} // namespace opennova::np
