#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <npwire/protocol_message.h>

#include "npruntime/game_config.h" // np::GameConfig — the reactive reply handlers read it
#include "npruntime/napi_np_connection.h"

// The roster identity (team @entity+344, wire handle) is read THROUGH each connection's
// link.owned_entity from the live registry (D-NET-132 / §6.9). Forward-declared so world/world.h stays
// out of this light header.
namespace opennova::world {
class World;
struct EntityHandle;
}

// P8 — the reactive in-match gameplay-message reply path, ported off the retired
// engine/net/novaworld/game_session.cpp (`GameSession`) + game_server_runtime.cpp (`GameServerRuntime`).
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

using MissionMetadataBlob = std::array<uint8_t, 180>;

// Build the session-owned S2C 0x64 raw content. The two random regions and
// nonzero session id are minted once by create_session; callers then retain the
// returned block for every chunk request. [orig: sub_51E880 @0x51E880 +
// CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0]
MissionMetadataBlob build_mission_metadata_blob(
		const GameConfig &config, bool is_mp_session_peer);

// Build the second, pending-player-spawn boundary of a retail join. The C2S
// 0x02 handler deliberately does not return these records: retail processes an
// intervening client frame before CNapiServer_ProcessPendingPlayerSpawns emits
// 0x03(reset), settings x2, 0x05, 0x04 and the 0x7B replay. The host tick calls
// this only after the player's live entity/team exists.
std::vector<ProtocolMessage> build_spawn_pump_metadata(
		const GameConfig &config, NapiNPConnection &conn,
		const std::vector<NapiNPConnection> &roster, world::World *world);

// Project the world-owned wave list into the requester-specific S2C 0x6E
// body. Public because both the reactive 0x0E queue-join reply and the 1 Hz
// host producer must use the exact same writer.
// [orig: NetPacket_WriteSpawnWaveStatus @0x507490]
std::vector<uint8_t> build_spawn_wave_status_body(
		const world::World &world, world::EntityHandle requester);

// Execute the ONE deploy-release transaction shared by immediate C2S 0x0E
// picks and SpawnWaveList_Tick releases. `target_zone` invalid selects the
// team's marker chain. Returns the private 0x5A/0x61/frontier bundle for the
// owning connection; callers decide whether it is returned reactively or
// staged on a transport.
// [orig: Server_ProcessPlayerDeath deploy leg @0x517740]
std::vector<ProtocolMessage> Server_ReleasePlayerDeployment(
		const GameConfig &config, NapiNPConnection &conn,
		world::World &world, world::EntityHandle target_zone);

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
// The host-context values the reply handlers read beyond `config`: every
// member is a read-only view onto the owning NapiNPServerCtx (the session
// clock, the 0x64 mission blob, the frozen 0x56 board stream). A null pointer
// means the host has nothing to serve for that leg.
struct ServerDispatchInputs {
	uint32_t session_uptime_ms = 0;
	const MissionMetadataBlob *mission_metadata_blob = nullptr;
	// stru_C947D8: the board frozen by the round-end producer; the 0x2B service
	// cuts chunks from it and never rebuilds. [orig: NapiNPServerMsg_0x02B
	// @0x514FE0 -> NetPacket_WriteReplayStreamChunk @0x506F60]
	const std::vector<uint8_t> *round_end_board_stream = nullptr;
};

std::vector<ProtocolMessage> dispatch_session_replies(const GameConfig &config,
                                                      NapiNPConnection &conn,
                                                      const std::vector<ProtocolMessage> &messages,
                                                      uint32_t now_tick,
                                                      std::vector<NapiNPConnection> &roster,
                                                      world::World *world,
                                                      const ServerDispatchInputs &inputs = {});

// Build the S2C 0x16 PLAYER-LIST for the current roster (every IN-MATCH connection: host loopback
// slot 0 + joiners 1+; a still-loading joiner is excluded until its burst completes). Public so the
// per-tick host loop can RE-PUSH it when a joiner spawns (the golden re-sends 0x16 with the grown
// roster just before the joiner deploys). [orig: Server_BuildAndBroadcastScoreboard @0x50D960 /
// client NapiNPClientMsg_PlayerList @0x42FAE0]
ProtocolMessage build_player_list_message(const GameConfig &config,
                                          const std::vector<NapiNPConnection> &roster,
                                          world::World *world);

// Broadcast one just-spawned player's 0x46 slot-state (fieldFlags 0x1CF7) to every OTHER in-match
// connection — the join-time roster push that lets existing clients ACCEPT the new player's 0x16
// row without the unknown-slot 0x22 retry churn. [orig: Server_PlayerAdd @0x51CBC0 broadcasts 0x46
// fieldFlags 0x1CF7 to all in-game @0x51D296 (`push 7415` @0x51d2bf); the 0x32 name broadcast
// stays deferred with D-NET-149]
void broadcast_player_sync_on_join(const GameConfig &config,
                                   std::vector<NapiNPConnection> &roster,
                                   const NapiNPConnection &joined, const world::World *world);

// Install/refresh the per-connection player binding the roster (0x46/0x16)
// replies read: stamps the bare wire `entity_handle` onto conn.link.owned_entity (the SINGLE binding,
// D-NET-132) plus the roster name/slot. The World-path spawn (Server_BuildPlayerInfoAndAdd) binds
// owned_entity to a LIVE registry entity; this pre-World reactive path (a World-less session-responder
// / test host) binds it to the bare handle, so both resolve team/handle the same way (team defaults
// when no live entity backs the handle). [orig: a player bound to its allocated slot/entity at add]
bool bind_session_reply_player(NapiNPConnection &conn, std::string player_name, uint8_t player_slot,
                               uint16_t entity_handle);

} // namespace opennova::np
