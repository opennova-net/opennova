#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <formats/mission/bms.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

// P3 — [orig: Server_SendInitialGameStateToPlayer @0x51bba0]. The two-track initial-state burst
// machine: it advances one connection's InitialStateBurst cursor and emits the §5.2a load sequence,
// every body produced from real World + bms::File + host-config state (ADR 0003, no fixtures). The player-sync
// serializers (0x2C server-name+map, 0x08 server-config, 0x2A table×6, 0x66 weapon-restrictions, 0x76
// class-allow mask, 0x1A timestamp) are ported from the witnessed originals (§5.2a/§5.66). The
// world-stream 0x45 terrain-delta + 0x7E briefing are emitted by the original ONLY when present and are
// faithfully absent on the headless host (no per-player terrain delta / no MissionText wired). Godot-free
// / socket-free: the caller frames each returned message (0x83 SESSION for a remote peer, raw host_send
// for the loopback).
namespace opennova::inmatch {

// One inner S2C message the burst emits this step. `body` may be empty (0x10/0x1C/0x11 are wire-valid
// empty payloads). The caller wraps it for the connection's transport.
struct InitialStateMessage {
	uint8_t tag = 0;
	std::vector<uint8_t> body;
	bool reliable = true;
};

struct InitialStateStep {
	std::vector<InitialStateMessage> messages;  // emitted this step (0 or more)
	uint32_t world_batches_emitted = 0;         // world-stream batches sent this step (drives F3 readiness)
	bool advanced = false;                      // false => burst not eligible / already complete
	bool reached_in_game = false;               // emitted the world-stream terminator (game-state 9) this step
};

// The retail CNapiServerConfig_BuildFlags computation captured by
// create_session. ServerHello P2 and the trailing dword of S2C 0x08 both read
// that snapshot; callers may use this helper to compute/inspect the live value.
uint32_t build_server_config_flags(const NapiNPServerCtx &ctx);

// Advance `conn.burst` by one phase step and return the message(s) to ship. Drive it once per frame
// per non-spawned connection until reached_in_game. No-op (advanced=false) when ctx.world == nullptr,
// the connection has not reached PlayerAdded, or its burst is already complete. Sets
// conn.burst.spawned + game_state 9 on the terminator (the PeerSpawned source). now_tick sources the
// 0x76 configured class-allow mask + 0x1A timestamp bodies. [orig: Server_SendInitialGameStateToPlayer @0x51bba0]
InitialStateStep Server_SendInitialGameStateToPlayer(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                     uint32_t now_tick = 0);

// The deploy-map LOCATION labels for every type-2044 marker in spawn order:
// the mission text's [Locations] LOCATION%03i string, else the LOCATION%03i
// key itself, each clipped to retail's 64-byte location-name slot. Every host
// embedder installs them at bring-up; the S2C 0x0F writer copies them onto a
// joining client. [orig: Entity_SpawnFromBMSRecord @0x40f182-0x40f221]
void install_mission_location_names(NapiNPServerCtx &ctx, const bms::File &mission,
                                    const std::unordered_map<int32_t, std::string> &location_texts);

} // namespace opennova::inmatch
