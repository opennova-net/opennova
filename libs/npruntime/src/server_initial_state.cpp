#include "npruntime/server_initial_state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <mission/bms.h>                  // bms::File, bms::encode_header_blob (0x0B body)
#include <netsim/entity_wire_bridge.h>    // build_pool0_organic_batch / build_pool3_spawn_marker_batch
#include <novaworld/ingame_encode.h>      // encode_organic_spawn_batch / encode_pool3_sync_batch
#include <world/world.h>

namespace opennova::np {

namespace {

// ---------------------------------------------------------------------------
// §5.2a player-sync serializers. These were left "emit nothing / deferred" through P3-P6; ported
// here from the witnessed originals (grilled 2026-06-27, cross-checked vs the retail-lan-host-join
// golden frames 144-160). Each writes the host's own config the way the original writes its g_*
// globals — bytes a stock client accepts; golden byte-parity needs the host config seeded.
// ---------------------------------------------------------------------------

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFFu));
	b.push_back(uint8_t((v >> 8) & 0xFFu));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(uint8_t(v & 0xFFu));
	b.push_back(uint8_t((v >> 8) & 0xFFu));
	b.push_back(uint8_t((v >> 16) & 0xFFu));
	b.push_back(uint8_t((v >> 24) & 0xFFu));
}
void put_cstr(std::vector<uint8_t> &b, const std::string &s) {
	b.insert(b.end(), s.begin(), s.end());
	b.push_back(0);
}

// [orig: NetPacket_WriteServerNameAndMapFile @0x505780] S2C 0x2C: server name + map file, two
// NUL-terminated C strings (was Kong-misnamed "TypeNameAndBaseName"). golden frame 144 = "Untitled\0"
// + "TDH_I5A.BMS\0". Sourced from the session config (the host's own server name + mission file).
std::vector<uint8_t> serialize_server_name_map(const SessionReplyConfig &cfg) {
	std::vector<uint8_t> b;
	put_cstr(b, cfg.server_name);
	put_cstr(b, cfg.mission_file);
	return b;
}

// [orig: CNapiServerConfig_BuildFlags @0x4c4dc0] The trailing flags dword of the 0x08 block.
uint32_t build_server_config_flags(const NapiNPServerCtx &ctx) {
	const ServerRules &r = ctx.rules;
	const NapiGameSettings &gs = ctx.game_settings;
	uint32_t flags = 0;
	if (!ctx.is_in_session) return flags;     // gated on is_in_session (+0x58)
	if (r.config_flag_2550A04) flags = 4;
	switch (static_cast<uint32_t>(ctx.transport_mode)) {
	case 1: flags |= 0x400u; break;           // single-player host
	case 2: flags |= 0x100u; break;           // LAN
	case 3: flags |= 0x200u; break;           // (mode 3)
	default: break;
	}
	flags |= 0x800u;
	if (static_cast<uint32_t>(ctx.transport_mode) == 1) flags &= ~0x800u; // SP clears it
	if (!gs.server_password.empty()) flags |= 0x8u;
	if ((gs.game_type & 0x10000u) != 0) {     // team game
		if ((gs.mp_attributes & 4u) != 0) flags |= 0x4u;
		if (!gs.side_a_password.empty()) flags |= 0x20u;
		if (!gs.side_b_password.empty()) flags |= 0x10u;
	}
	if (r.squad_enforced) {
		flags |= 0x2000u;
		if (!r.squad_required_tag.empty()) flags |= 0x4000u;
	}
	if (r.permanent_death) flags |= 0x8000u;
	if (r.config_flag_2550CA4) flags |= 0x10000u;
	return flags;
}

// [orig: ServerConfig_SerializeToPacket @0x505bd0] S2C 0x08: 10 rule dwords + 7 bytes + flags dword.
std::vector<uint8_t> serialize_server_config(const NapiNPServerCtx &ctx) {
	const ServerRules &r = ctx.rules;
	std::vector<uint8_t> b;
	b.reserve(51);
	put_u32(b, r.respawn_time);
	put_u32(b, r.time_limit_minutes);
	put_u32(b, r.config_word_2);
	put_u32(b, r.game_type);
	put_u32(b, r.config_word_4);
	put_u32(b, r.score_limit);
	put_u32(b, r.config_word_6);
	put_u32(b, r.start_delay);
	put_u32(b, r.config_word_8);
	put_u32(b, r.config_word_9);
	for (int i = 0; i < 7; ++i) b.push_back(r.config_bytes[i]);
	put_u32(b, build_server_config_flags(ctx));
	return b; // 51 bytes
}

// [orig: NetPacket_SerializeWeaponRestrictionTable @0x5102c0] S2C 0x66: count byte, then (index,value)
// pairs for each restricted weapon (value 0 or 2). The original scans a 255-entry table; we hold the
// restricted set directly (empty = count 0 -> single byte 0, golden frame 160).
std::vector<uint8_t> serialize_weapon_restrictions(const NapiNPServerCtx &ctx) {
	std::vector<uint8_t> b;
	b.push_back(static_cast<uint8_t>(ctx.weapon_restrictions.size()));
	for (const auto &e : ctx.weapon_restrictions) {
		b.push_back(e.first);  // weapon index
		b.push_back(e.second); // restriction value
	}
	return b;
}

// [orig: NetPacket_WriteServerTick16 @0x510350] S2C 0x76: the server tick low 16 bits.
std::vector<uint8_t> serialize_server_tick16(uint32_t now_tick) {
	std::vector<uint8_t> b;
	put_u16(b, static_cast<uint16_t>(now_tick & 0xFFFFu));
	return b;
}

// [orig: NetPacket_WriteTimestamp @0x5046c0] S2C 0x1A: a 4-byte timestamp. The original writes
// GetTickCount() (an OS primitive — excluded from the faithful-port rule); the headless host uses
// its monotonic logic tick.
std::vector<uint8_t> serialize_timestamp(uint32_t now_tick) {
	std::vector<uint8_t> b;
	put_u32(b, now_tick);
	return b;
}

// [orig: NetPacket_CopyTenBytes @0x503900 over the table @0x82F1D8] S2C 0x2A: one 10-byte record.
// The table is 6 records sharing this payload (threshold 0 -> all 6 sent); golden frames 148-158.
constexpr std::array<uint8_t, 10> k0x2aRecord = {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};

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

// Advance the §5.2a burst by exactly ONE phase: resolve the current (tag, action) for the cursor,
// emit it into `step` (or not), climb the world-batch counter, then move the cursor (handling the
// player-sync -> world-stream transition and the terminator). The one-shot driver below loops this
// until the burst completes. `b` is conn.burst.
void advance_burst_one_phase(NapiNPServerCtx &ctx, NapiNPConnection &conn, InitialStateStep &step,
                             uint32_t now_tick) {
	InitialStateBurst &b = conn.burst;
	uint8_t tag = 0;
	Action action = Action::SkipSilent;
	bool is_world_batch = false;

	if (b.sync_state == 2) {
		// Player-sync track: one tag per subphase (8..20), then -> world-stream.
		switch (b.player_sync_subphase) {
		case 8:  tag = 0x2C; action = Action::EmitBody; break;  // NetPacket_WriteServerNameAndMapFile @0x505780
		case 9:  tag = 0x08; action = Action::EmitBody; break;  // ServerConfig_SerializeToPacket @0x505bd0
		case 10: case 11: case 12: case 13: case 14: case 15:
		         tag = 0x2A; action = Action::EmitBody; break;  // NetPacket_CopyTenBytes @0x503900 (×6, table @0x82F1D8)
		case 16: tag = 0x1C; action = Action::EmitEmpty; break; // [§5.2a ≥16: 0x1C empty payload]
		case 17: tag = 0x0B; action = Action::EmitBody; break;  // bms::encode_header_blob (§5.4, 616 B)
		case 18: tag = 0x66; action = Action::EmitBody; break;  // NetPacket_SerializeWeaponRestrictionTable @0x5102c0
		case 19: tag = 0x76; action = Action::EmitBody; break;  // NetPacket_WriteServerTick16 @0x510350
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
		// 0x45 terrain-tile delta (NetPacket_WriteTerrainTiles @0x506570) and 0x7E briefing text
		// (NetPacket_WriteBriefingText @0x506620) are emitted by the original ONLY when there is a
		// terrain delta to stream / mission briefing text present; both return 0 (and the orig skips)
		// otherwise. The headless host streams no per-player terrain delta and wires no MissionText, so
		// both are faithfully absent here — matching the golden, which carries neither (frames after 0x20).
		case 5: tag = 0x45; action = Action::SkipSilent; break;                       // no terrain delta (faithful conditional)
		case 6: tag = 0x7E; action = Action::SkipSilent; break;                       // no briefing text (faithful conditional)
		case 7: tag = 0x1A; action = Action::EmitBody; break;                         // NetPacket_WriteTimestamp @0x5046c0
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
		} else if (tag == 0x2C) {
			body = serialize_server_name_map(ctx.session_config);
		} else if (tag == 0x08) {
			body = serialize_server_config(ctx);
		} else if (tag == 0x2A) {
			body.assign(k0x2aRecord.begin(), k0x2aRecord.end());
		} else if (tag == 0x66) {
			body = serialize_weapon_restrictions(ctx);
		} else if (tag == 0x76) {
			body = serialize_server_tick16(now_tick);
		} else if (tag == 0x1A) {
			body = serialize_timestamp(now_tick);
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
}

} // namespace

// [orig: Server_SendInitialGameStateToPlayer @0x51bba0]
InitialStateStep Server_SendInitialGameStateToPlayer(NapiNPServerCtx &ctx, NapiNPConnection &conn,
                                                     uint32_t now_tick) {
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

	// [orig: Server_OnPlayerJoin @0x51a680; D-NET-114] The §5.2a burst is ONE-SHOT per join — the
	// original emits the whole player-sync + world-stream sequence synchronously in a single call (one
	// engine tick), NOT one phase per tick. Drain both tracks here (advance_burst_one_phase walks the
	// cursor to the sync_state==5 terminator); the caller invokes this once per connection per tick and
	// skips the connection afterwards (burst.spawned).
	while (b.sync_state != 5) advance_burst_one_phase(ctx, conn, step, now_tick);

	step.advanced = true;
	return step;
}

} // namespace opennova::np
