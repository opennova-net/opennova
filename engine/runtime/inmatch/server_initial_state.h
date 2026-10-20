#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/mission/bms.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/mission/mission_text.h>

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
	// Not a record: the join's map-checksum punt at this place among its sends
	// (Server_LogCRCMismatchPunt's type, 9 HMCRC or 10 CMCRC, and the punt-log reason), which
	// the 0x0B receiver stages on the connection (D-NET-NEW-L1-5).
	uint32_t host_punt = 0;
	const char *host_punt_reason = nullptr;
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

// The join: the C2S 0x0B's (a remote joiner's, from its dispatch; the host's own client's,
// in-process, from the producer). Writes the slot's join words (chat stamp, the +89912 state
// byte's bits, game state 10, and on the first state-6 entry the quality reset, the network
// reset, the armory cooldown zero, the RTT countdown and the entry stamp), enters slot state 6
// and returns this connection's game-start messages in order (0x41s, 0x42, 0x0F, 0x4D, 0x61,
// 0x3E); the 0x4D's copies to the other joined clients go out on their transports. The 0x0B's
// two map checksums (`client_checksums`, 0 where its body ends first) are compared against the
// mission's after the 0x0F: a remote slot's mismatch is a punt among the messages
// (InitialStateMessage::host_punt).
// [orig: Server_OnPlayerJoin @0x51A680]
std::vector<InitialStateMessage> Server_OnPlayerJoin(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		uint32_t now_tick, const std::array<uint32_t, 2> &client_checksums);

// The S2C 0x0F body for `conn`: its body's pose and the game flags, its
// ammo-pool image, the waypoint records (a team-1 body: the mission's player
// route, any game type, D-NET-NEW-LP-3) and the host's location names.
// [orig: NetPacket_WriteWorldStateLoad0x0F @0x502D10]
std::vector<uint8_t> serialize_world_state_load(NapiNPServerCtx &ctx, const NapiNPConnection &conn,
		uint32_t now_tick);

// The deploy-map LOCATION labels for every type-2044 marker the promoted
// `world` holds (the spawn's admitted records), in spawn order: the mission
// text's [Locations] string keyed by the record's own name_index, else that
// LOCATION%03i key itself, each copied whole into its 64-byte slot of
// g_LocationNames and kept as that slot reads (location_slot_strings). Every
// host embedder installs them at bring-up, after the promote; the S2C 0x0F
// writer copies them onto a joining client, and the host's own client's 0x0F
// copy replaces them on a serve-and-play host (HostRole's frame).
// [orig: Entity_SpawnFromBMSRecord @0x40f182-0x40f221]
void install_mission_location_names(NapiNPServerCtx &ctx, const world::World &world,
                                    const bms::File &mission, const mission::MissionText &text);

// What each slot of g_LocationNames reads to its NUL once the spawn has
// copied `names` whole into consecutive 64-byte slots in order: a name of
// fewer than 64 bytes (or the last) reads whole, a longer one as its first
// 64 bytes and then the next slot's read.
std::vector<std::string> location_slot_strings(const std::vector<std::string> &names);

} // namespace opennova::inmatch
