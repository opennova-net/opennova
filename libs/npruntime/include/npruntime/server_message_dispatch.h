#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <novaworld/protocol_message.h>

#include "npruntime/game_config.h" // np::GameConfig — the reactive reply handlers read it
#include "npruntime/napi_np_connection.h"

// The roster identity (team @entity+344, wire handle) is read THROUGH each connection's
// link.owned_entity from the live registry (D-NET-132 / §6.9). Forward-declared so world/world.h stays
// out of this light header.
namespace opennova::world {
class World;
}

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

// Dispatch the decoded in-match gameplay `messages` for `conn` to their reply handlers and return the
// reactive replies to frame onto the connection. Caches the joiner's pre-spawn C2S 0x0C pose into
// `conn.reply`; gates the spawn-confirm replies on `conn.burst` (the faithful spawn authority). A
// remote (type-1) connection and the host's own type-2 loopback both run this the same way.
// `world` is MUTABLE: the witnessed handlers write server state (the 0x2F loadout handler stamps
// entity+660 playerClass [orig: @0x515ab0]) — matching the original, whose handlers mutate the
// live entity/player tables directly. `roster` is MUTABLE for the same reason: broadcast handlers
// (the 0x25 reload relay) stage an S2C send on EVERY in-match connection's transport [orig:
// NapiNPServer_SendFiltered @0x4C87E0 walks the connection list doing one SendToConn per node];
// each recipient's flush frames the body with its own sequencing.
// [orig: per-message NapiNPServerMsg_0x0NN handlers reached from the 0x43 SESSION dispatch]
std::vector<ProtocolMessage> dispatch_session_replies(const GameConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick,
                                                      std::vector<NapiNPConnection> &roster,
                                                      world::World *world);

// Build the S2C 0x16 PLAYER-LIST for the current roster (every PlayerAdded connection: host loopback
// slot 0 + joiners 1+). Public so the per-tick host loop can RE-PUSH it when a joiner spawns (the
// golden re-sends 0x16 with the grown roster just before the joiner deploys). [orig: NapiNPServerMsg_0x016
// @0x42FAE0 reads the dword_A87048 player table]
ProtocolMessage build_player_list_message(const GameConfig &config,
                                          const std::vector<NapiNPConnection> &roster,
                                          const world::World *world);

// Install/refresh the per-connection player binding the roster (0x46/0x16) + spawn-confirm (0x51)
// replies read: stamps the bare wire `entity_handle` onto conn.link.owned_entity (the SINGLE binding,
// D-NET-132) plus the roster name/slot. The World-path spawn (Server_BuildPlayerInfoAndAdd) binds
// owned_entity to a LIVE registry entity; this pre-World reactive path (a World-less session-responder
// / test host) binds it to the bare handle, so both resolve team/handle the same way (team defaults
// when no live entity backs the handle). [orig: a player bound to its allocated slot/entity at add]
bool bind_session_reply_player(NapiNPConnection &conn, std::string player_name, uint8_t player_slot,
                               uint16_t entity_handle);

} // namespace opennova::np
