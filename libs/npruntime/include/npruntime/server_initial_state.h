#pragma once

#include <cstdint>
#include <vector>

#include "npruntime/napi_np_connection.h"
#include "npruntime/napi_np_server_ctx.h"

// P3 — [orig: Server_SendInitialGameStateToPlayer @0x51bba0]. The two-track initial-state burst
// machine: it advances one connection's InitialStateBurst cursor and emits the §5.2a load sequence,
// every body produced from real World + bms::File state (ADR 0003, no fixtures). Replaces
// game_session.cpp's fixture-driven queue_mission_bootstrap / queue_state4_loading_gate. Witnessed
// bodies only this (structural) phase — the unwitnessed §5.2a serializers are skipped + logged as a
// tracked grill-wave gap (faithful-port rule). Godot-free / socket-free: the caller frames each
// returned message (0x83 SESSION for a remote peer, raw host_send for the loopback).
namespace opennova::np {

// One inner S2C message the burst emits this step. `body` may be empty (0x10/0x1C/0x11 are wire-valid
// empty payloads). The caller wraps it for the connection's transport.
struct InitialStateMessage {
	uint8_t tag = 0;
	std::vector<uint8_t> body;
};

struct InitialStateStep {
	std::vector<InitialStateMessage> messages;  // emitted this step (0 or more)
	uint32_t world_batches_emitted = 0;         // world-stream batches sent this step (drives F3 readiness)
	bool advanced = false;                      // false => burst not eligible / already complete
	bool reached_in_game = false;               // emitted the world-stream terminator (game-state 9) this step
};

// Advance `conn.burst` by one phase step and return the message(s) to ship. Drive it once per frame
// per non-spawned connection until reached_in_game. No-op (advanced=false) when ctx.world == nullptr,
// the connection has not reached PlayerAdded, or its burst is already complete. Sets
// conn.burst.spawned + game_state 9 on the terminator (the PeerSpawned source). now_tick is reserved
// for the 0x1A timestamp body (currently deferred). [orig: Server_SendInitialGameStateToPlayer @0x51bba0]
InitialStateStep Server_SendInitialGameStateToPlayer(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                     uint32_t now_tick = 0);

} // namespace opennova::np
