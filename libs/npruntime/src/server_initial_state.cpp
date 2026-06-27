#include "npruntime/server_initial_state.h"

#include <array>
#include <cstdio>
#include <string>

#include <mission/bms.h>                  // bms::File, bms::encode_header_blob (0x0B body)
#include <netsim/entity_wire_bridge.h>    // build_pool0_organic_batch / build_pool3_spawn_marker_batch
#include <novaworld/ingame_encode.h>      // encode_organic_spawn_batch / encode_pool3_sync_batch
#include <world/world.h>

namespace opennova::np {

namespace {

// One-time notice that a witnessed-but-unported §5.2a serializer is being skipped this (structural)
// phase. NOT a fixture and NOT invented bytes — the tag is simply not emitted until its grill lands
// (faithful-port rule). Logged once per tag per process so the burst loop does not spam.
void log_deferred_once(uint8_t tag) {
	static std::array<bool, 256> logged{};
	if (logged[tag]) return;
	logged[tag] = true;
	std::fprintf(stderr,
	             "[P3] deferred §5.2a serializer 0x%02X — emitted as nothing pending the grill wave\n",
	             tag);
}

// How the machine treats one tag this step.
enum class Action { EmitEmpty, EmitBody, Defer, SkipSilent };

} // namespace

// [orig: Server_SendInitialGameStateToPlayer @0x51bba0]
InitialStateStep Server_SendInitialGameStateToPlayer(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                     uint32_t now_tick) {
	(void)now_tick; // reserved for the 0x1A timestamp body (currently deferred)
	InitialStateStep step;

	// Eligible only once the connection's pool-0 entity exists (PlayerAdded) and the World is wired.
	if (ctx.world == nullptr) return step;            // P2 unit-test path: no-op (keep-green lever)
	if (conn.phase < ConnectionPhase::PlayerAdded) return step;
	if (conn.burst.spawned) return step;              // already complete

	InitialStateBurst &b = conn.burst;
	if (b.sync_state == 0) {                            // start the player-sync track
		b.sync_state = 2;
		b.player_sync_subphase = 8;
		b.game_state = 8;                              // [orig: CNetPlayer_SetGameState(.., 8)]
	}

	uint8_t tag = 0;
	Action action = Action::SkipSilent;
	bool is_world_batch = false;

	if (b.sync_state == 2) {
		// Player-sync track: one tag per subphase (8..20), then -> world-stream.
		switch (b.player_sync_subphase) {
		case 8:  tag = 0x2C; action = Action::Defer; break;     // NetPacket_WriteTypeNameAndBaseName @0x505780
		case 9:  tag = 0x08; action = Action::Defer; break;     // ServerConfig_SerializeToPacket @0x505bd0
		case 10: case 11: case 12: case 13: case 14: case 15:
		         tag = 0x2A; action = Action::Defer; break;     // NetPacket_CopyTenBytes @0x503900 (×6)
		case 16: tag = 0x1C; action = Action::EmitEmpty; break; // [§5.2a ≥16: 0x1C empty payload]
		case 17: tag = 0x0B; action = Action::EmitBody; break;  // bms::encode_header_blob (§5.4, 616 B)
		case 18: tag = 0x66; action = Action::Defer; break;     // NetPacket_SerializeWeaponRestrictionTable @0x5102c0
		case 19: tag = 0x76; action = Action::Defer; break;     // NetPacket_WriteServerTick16 @0x510350
		case 20: tag = 0x11; action = Action::EmitEmpty; break; // [§5.2a ≥16: 0x11 empty payload, LAST of the §5.5 bundle]
		default: break;
		}
	} else if (b.sync_state == 4) {
		// World-stream track: one whole-pool batch per phase (1..7), then game-state 9.
		switch (b.world_stream_phase) {
		case 1: tag = 0x10; action = Action::EmitBody; is_world_batch = true; break;  // empty static batch header (client builds pool-2 locally, D-NET-97/98)
		case 2: tag = 0x0D; action = Action::SkipSilent; break;                       // pool-1 deliberately NOT streamed (D-NET-97/98)
		case 3: tag = 0x0C; action = Action::EmitBody; is_world_batch = true; break;  // build_pool0_organic_batch (carries entity+0x78 dcb)
		case 4: tag = 0x20; action = Action::EmitBody; is_world_batch = true; break;  // build_pool3_spawn_marker_batch
		case 5: tag = 0x45; action = Action::Defer; break;                            // sub_506570 (terrain)
		case 6: tag = 0x7E; action = Action::Defer; break;                            // sub_506620
		case 7: tag = 0x1A; action = Action::Defer; break;                            // NetPacket_WriteTimestamp @0x5046c0
		default: break;
		}
	}

	// Resolve the action into an emitted message (or not).
	switch (action) {
	case Action::EmitEmpty:
		step.messages.push_back(InitialStateMessage{tag, {}});
		break;
	case Action::EmitBody: {
		std::vector<uint8_t> body;
		if (tag == 0x0B) {
			if (ctx.mission != nullptr) {
				std::string err;
				if (!bms::encode_header_blob(*ctx.mission, body, err)) {
					log_deferred_once(tag); // could not build faithfully -> skip rather than invent
					body.clear();
					action = Action::SkipSilent;
				}
			} else {
				log_deferred_once(tag); // no mission wired -> skip 0x0B, never fabricate the header
				action = Action::SkipSilent;
			}
		} else if (tag == 0x10) {
			opennova::StaticEntityBatch batch;
			body = opennova::encode_static_entity_batch(batch);
		} else if (tag == 0x0C) {
			body = opennova::encode_organic_spawn_batch(opennova::netsim::build_pool0_organic_batch(*ctx.world));
		} else if (tag == 0x20) {
			body = opennova::encode_pool3_sync_batch(opennova::netsim::build_pool3_spawn_marker_batch(*ctx.world));
		}
		if (action == Action::EmitBody) step.messages.push_back(InitialStateMessage{tag, std::move(body)});
		break;
	}
	case Action::Defer:
		log_deferred_once(tag);
		break;
	case Action::SkipSilent:
		break;
	}

	// A world-stream message landing (incl. the empty 0x10 marker) climbs entity_batch_count — the
	// F3 readiness signal the streaming-entered latch reads.
	if (is_world_batch && (action == Action::EmitBody || action == Action::EmitEmpty)) {
		++b.entity_batch_count;
		++step.world_batches_emitted;
	}

	// Advance the cursor and handle track transitions / the terminator.
	if (b.sync_state == 2) {
		if (b.player_sync_subphase >= 20) {
			b.sync_state = 4;          // -> world-stream track
			b.world_stream_phase = 1;
		} else {
			++b.player_sync_subphase;
		}
	} else if (b.sync_state == 4) {
		if (b.world_stream_phase >= 7) {
			b.sync_state = 5;          // done
			b.game_state = 9;          // [orig: CNetPlayer_SetGameState(.., 9) — in-game]
			b.spawned = true;          // the PeerSpawned source
			conn.phase = ConnectionPhase::Spawned;
			step.reached_in_game = true;
		} else {
			++b.world_stream_phase;
		}
	}

	step.advanced = true;
	return step;
}

} // namespace opennova::np
