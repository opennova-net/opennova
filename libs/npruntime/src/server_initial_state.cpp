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
#include <world/entity.h>                 // world::Entity (0x0F spawn pose)
#include <world/geom.h>                   // world::to_fixed (0x0F spawn pose)
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

// [orig: Server_SendEntityStateToPlayer @0x517ba0 / NapiNPClientMsg_0x00F @0x42E200; §5.29] S2C 0x0F
// world-state-load — the game-start deploy unsticker. Carries the joiner's spawn pose, game flags, and
// the fixed 128-entry team-score block; the client handler clears its dword_81474C load-gate and queues
// the post-load C2S burst (0x22/0x23/0x28/0x29/0x2D/0x32) that lets it deploy. Without it a retail
// joiner world-loads but stays undeployed (floods C2S 0x0f) — the observed live "stuck at 7%". Minimal
// faithful body: waypointCount 0 + teamNameCount 0 (the TDM/DM default; co-op waypoint records + the
// mission's zone/team names are a tracked follow-up — cosmetic/objective, not deploy-gating). Layout is
// the §5.29 field map / the WorldStateLoad struct decode_world_state_load consumes.
std::vector<uint8_t> serialize_world_state_load(NapiNPServerCtx &ctx, const NapiNPConnection &conn,
                                                uint32_t now_tick) {
	int32_t px = static_cast<int32_t>(ctx.session_config.spawn_x);
	int32_t py = static_cast<int32_t>(ctx.session_config.spawn_y);
	int32_t pz = static_cast<int32_t>(ctx.session_config.spawn_z);
	int16_t yaw = 0;
	// Prefer the joiner's live spawned pool-0 entity (bound by Server_ProcessPendingPlayerSpawns before
	// the burst); fall back to the host-advertised spawn from the session config.
	if (ctx.world != nullptr && conn.link.owned_entity.valid()) {
		if (const world::Entity *e = ctx.world->registry.get(conn.link.owned_entity)) {
			px = world::to_fixed(e->position.x);
			py = world::to_fixed(e->position.y);
			pz = world::to_fixed(e->position.z);
			// §5.29 wire yaw is an i16 the client <<16 to a 16.16 BAM = the high half of the engine-frame
			// heading (90 - mission_yaw); matches snapshot_of / pose_for_conn (D-NET-86).
			constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360
			yaw = static_cast<int16_t>((static_cast<int64_t>(90 - e->yaw) * kBamPerDegree) >> 16);
		}
	}
	std::vector<uint8_t> b;
	b.reserve(539);
	put_u32(b, now_tick);                        // sessionTick
	put_u32(b, static_cast<uint32_t>(px));        // spawn pos (16.16)
	put_u32(b, static_cast<uint32_t>(py));
	put_u32(b, static_cast<uint32_t>(pz));
	put_u16(b, static_cast<uint16_t>(yaw));       // yaw  (i16, client <<16)
	put_u16(b, 0);                                // pitch
	put_u16(b, 0);                                // roll
	b.push_back(0x01);                            // gameFlags bit0 (matches the golden 0x0F)
	for (int i = 0; i < 128; ++i) put_u32(b, 0);  // fixed 128-entry team-score block (zeroed)
	put_u16(b, 0);                                // waypointCount = 0 (TDM/DM default)
	put_u16(b, 0);                                // teamNameCount = 0
	return b;                                     // 4 + 12 + 6 + 1 + 512 + 2 + 2 = 539 B
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

// Page a world-stream pool into per-datagram batches and push each as its own InitialStateMessage.
// The original caps each S2C world-stream datagram at ~650 B and advances a pool cursor across calls
// [orig: Server_SendInitialGameStateToPlayer @0x51bba0]; the golden's 0x10/0x20 bodies top out ~640 B.
// `encode_page(off, cnt)` slices the pool's records [off, off+cnt) into a sub-batch and returns its
// encoded body. Each page climbs the F3 entity_batch_count. An empty pool still emits one header-only
// batch (a faithful empty-batch marker). Instruments the host log with the record/page/byte counts so
// a join's world-load stream is visible directly (e.g. an empty 0x10 vs the full paged static set).
// PACED: emit pages of one pool starting from the record cursor `b.phase_loop_counter`, stopping when
// the per-tick budget (`step.messages.size() >= budget`) is hit OR the pool is exhausted. Returns true
// if the pool is fully emitted (cursor reset to 0), false if the budget was hit mid-pool (cursor saved
// for resume next tick). The original PACES the world-stream onto the wire ~1 batch/2 frames over ~150
// frames so a joining client can interleave its join-FSM C2S chain (0x03 enter-Game-Loop / 0x0A
// initial-sync / 0x22 / 0x2F / 0x0C) BETWEEN server messages — a one-shot blast denies it those points.
template <typename EncodePage>
bool emit_paged_pool(uint8_t tag, std::size_t n_records, EncodePage encode_page,
                     InitialStateStep &step, InitialStateBurst &b, std::size_t budget) {
	constexpr std::size_t kMaxPageBytes = 640;
	if (n_records == 0) {
		// Empty pool: one header-only marker (emitted on first visit), then done.
		step.messages.push_back(InitialStateMessage{tag, encode_page(0, 0)});
		++b.entity_batch_count;
		++step.world_batches_emitted;
		b.phase_loop_counter = 0;
		return true;
	}
	std::size_t i = b.phase_loop_counter;
	while (i < n_records && step.messages.size() < budget) {
		std::size_t cnt = 1;
		std::vector<uint8_t> body = encode_page(i, cnt);
		while (i + cnt < n_records) {
			std::vector<uint8_t> grown = encode_page(i, cnt + 1);
			if (grown.size() > kMaxPageBytes) break;
			body.swap(grown);
			++cnt;
		}
		i += cnt;
		step.messages.push_back(InitialStateMessage{tag, std::move(body)});
		++b.entity_batch_count;
		++step.world_batches_emitted;
	}
	if (i >= n_records) {
		b.phase_loop_counter = 0;
		return true; // pool exhausted
	}
	b.phase_loop_counter = static_cast<uint16_t>(i); // resume here next tick
	return false; // per-tick budget hit mid-pool
}

// Advance the §5.2a burst by exactly ONE phase: resolve the current (tag, action) for the cursor,
// emit it into `step` (or not), climb the world-batch counter, then move the cursor (handling the
// player-sync -> world-stream transition and the terminator). The one-shot driver below loops this
// until the burst completes. `b` is conn.burst.
void advance_burst_one_phase(NapiNPServerCtx &ctx, NapiNPConnection &conn, InitialStateStep &step,
                             uint32_t now_tick, std::size_t budget) {
	InitialStateBurst &b = conn.burst;
	uint8_t tag = 0;
	Action action = Action::SkipSilent;
	bool is_world_batch = false;
	bool world_pool_done = true; // false when a world-stream pool was paused mid-pool (budget hit)

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
		// World-stream track: stream EACH pool in FULL, PAGED into per-datagram batches. A retail joiner
		// needs the complete static / pool-1 / pool-3 world to finish loading the map — the prior
		// minimal stream (empty 0x10, skipped 0x0D, spawn-marker-only 0x20) stalled a live retail join at
		// 7%. This reverses the D-NET-97/98 shortcut (was sized to our own client; a stock client needs
		// the real world-state). [orig: Server_SendInitialGameStateToPlayer @0x51bba0, phases
		// 0x10 -> 0x0D -> 0x0C -> 0x20]. The paged emits push directly; the action switch is a no-op here.
		action = Action::SkipSilent;
		switch (b.world_stream_phase) {
		case 1: { // 0x10 pool-2 static structures [orig: sub_5042F0]
			const opennova::StaticEntityBatch full = opennova::netsim::build_pool2_static_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x10, full.records.size(), [&](std::size_t off, std::size_t cnt) {
				opennova::StaticEntityBatch p;
				p.start_index = static_cast<uint16_t>(full.start_index + off);
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				return opennova::encode_static_entity_batch(p);
			}, step, b, budget);
			break;
		}
		case 2: { // 0x0D pool-1 destructibles / items / vehicles [orig: serialize_entity_pool_to_packet_0 @0x503940]
			const opennova::PoolSpawnBatch full = opennova::netsim::build_pool1_spawn_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x0D, full.records.size(), [&](std::size_t off, std::size_t cnt) {
				opennova::PoolSpawnBatch p;
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				return opennova::encode_pool_spawn_batch(p);
			}, step, b, budget);
			break;
		}
		case 3: { // 0x0C pool-0 organics (carry entity+0x78 dcb for the joiner name-match) [orig: serialize_entity_states_to_buffer @0x5030a0]
			const opennova::OrganicSpawnBatch full = opennova::netsim::build_pool0_organic_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x0C, full.records.size(), [&](std::size_t off, std::size_t cnt) {
				opennova::OrganicSpawnBatch p;
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<uint16_t>(p.records.size());
				return opennova::encode_organic_spawn_batch(p);
			}, step, b, budget);
			break;
		}
		case 4: { // 0x20 pool-3 markers / waypoints / nav nodes (FULL, not just spawn markers) [orig: serialize_entity_pool_to_packet @0x503460]
			const opennova::Pool3SyncBatch full = opennova::netsim::build_pool3_marker_batch(*ctx.world);
			world_pool_done = emit_paged_pool(0x20, full.records.size(), [&](std::size_t off, std::size_t cnt) {
				opennova::Pool3SyncBatch p;
				p.start_index = static_cast<uint16_t>(off);
				p.records.assign(full.records.begin() + static_cast<std::ptrdiff_t>(off),
				                 full.records.begin() + static_cast<std::ptrdiff_t>(off + cnt));
				p.entity_count = static_cast<int16_t>(p.records.size());
				return opennova::encode_pool3_sync_batch(p);
			}, step, b, budget);
			break;
		}
		case 5: break; // 0x45 terrain-tile delta — deferred (no per-player terrain delta wired yet)
		case 6: break; // 0x7E briefing text — deferred (no MissionText wired)
		case 7: tag = 0x1A; action = Action::EmitBody; break; // NetPacket_WriteTimestamp @0x5046c0
		case 8: { // GAME-START BUNDLE [orig: Server_OnPlayerJoin @0x51a680 tail] — the deploy unsticker.
			// Without it a retail joiner world-loads but stays undeployed (floods C2S 0x0f, "stuck at 7%").
			// 0x42 input-flags, 0x0F world-state-load (clears the client's dword_81474C load-gate + queues
			// its deploy burst), 0x4D player-index, 0x61 session-key/seed, 0x3E terminator. The per-frame
			// 0x0A the original interleaves here is covered by Server_TickUpdate's 0x0A fan (starts once
			// spawned), so it is not re-emitted in the bundle. [§5.29 / §5.43 / D-NET-114]
			std::vector<uint8_t> wsl = serialize_world_state_load(ctx, conn, now_tick);
			const std::size_t wsl_sz = wsl.size();
			step.messages.push_back(InitialStateMessage{0x42, {0x00, 0x00}}); // input-state-flags=0x0000 [NetPacket_WriteInputStateFlags @0x505ba0]
			step.messages.push_back(InitialStateMessage{0x0F, std::move(wsl)}); // world-state-load (§5.29)
			step.messages.push_back(InitialStateMessage{0x4D, {static_cast<uint8_t>(conn.reply.player_slot)}}); // player-index [NapiNPClientMsg_0x04D @0x4317B0]
			const uint32_t seed = ctx.np_protocol.session_seed_id;
			step.messages.push_back(InitialStateMessage{0x61, {static_cast<uint8_t>(seed & 0xFFu),
			                                                    static_cast<uint8_t>((seed >> 8) & 0xFFu),
			                                                    static_cast<uint8_t>((seed >> 16) & 0xFFu),
			                                                    static_cast<uint8_t>((seed >> 24) & 0xFFu)}}); // session-key/seed [Server_SendRandomSeedToPlayer @0x5101a0]
			step.messages.push_back(InitialStateMessage{0x3E, {}}); // terminator
			std::fprintf(stderr,
			             "[burst] game-start bundle: 0x42(2) 0x0F(%zu) 0x4D(1) 0x61(4) 0x3E(0) -> drives joiner deploy\n",
			             wsl_sz);
			break;
		}
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
		// A paged world-stream pool (phases 1-4) that hit the per-tick budget mid-pool is NOT done —
		// stay on this phase and resume from b.phase_loop_counter next tick (the pacing resume point).
		if (!world_pool_done) {
			// remain on the same world_stream_phase; cursor saved in b.phase_loop_counter
		} else if (b.world_stream_phase >= 8) { // phase 8 = the game-start bundle (deploy unsticker)
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

	// [orig: Server_SendInitialGameStateToPlayer @0x51bba0] PACED — the original streams the §5.2a
	// burst onto the wire over ~150 frames (~1 batch/2 frames), NOT one-shot. Emit at most
	// kMaxMsgsPerTick datagrams per call (per host tick) and resume next tick (player-sync subphase /
	// world_stream_phase / the phase_loop_counter page cursor persist on conn.burst). This gives a
	// joining client the interleave points to drive its join-FSM C2S chain (0x03 enter-Game-Loop /
	// 0x0A initial-sync / 0x22 / 0x2F / 0x0C) BETWEEN server messages — a one-shot blast denies them and
	// the client never completes to deploy. The host's own loopback (type 2) drains fast (no remote
	// client to pace for) via a large budget.
	// Phase 7→8 loadout gate (golden f316-318): after 0x1A (phase 7) the host WAITS for the client's
	// C2S 0x2F before the game-start bundle (phase 8); type-2 loopback bypasses it.
	constexpr std::size_t kPacedMsgsPerTick = 1; // ~1 datagram/host-frame; golden is ~0.4 batch/frame
	const bool is_remote = (conn.type == 1);
	const std::size_t budget = is_remote ? kPacedMsgsPerTick : 0xFFFFu; // loopback: effectively unpaced
	while (b.sync_state != 5) {
		if (is_remote && b.sync_state == 4 && b.world_stream_phase == 8 && !b.loadout_received) {
			// WAIT for the client's C2S 0x2F (loadout select) -> S2C 0x5A before the game-start, so
			// the player deploys WITH a loadout (golden order: 0x5A precedes the game-start). The
			// retail client + the opennova joiner both send 0x2F; this long fallback only guards
			// against a client that never selects (avoids an infinite stall). ~10 s @ 62 Hz.
			constexpr uint16_t kLoadoutWaitFallbackTicks = 600;
			if (++b.loadout_wait_ticks < kLoadoutWaitFallbackTicks) break;
			b.loadout_received = true;
		}
		if (step.messages.size() >= budget) break; // per-tick pacing cap — resume next tick
		advance_burst_one_phase(ctx, conn, step, now_tick, budget);
	}

	// Instrumentation (paced): log this tick's emitted tags, and a one-line summary when the burst
	// finishes, so the host log shows the world-load stream cadence directly.
	if (!step.messages.empty()) {
		std::string seq;
		seq.reserve(step.messages.size() * 8);
		for (const InitialStateMessage &m : step.messages) {
			char buf[24];
			std::snprintf(buf, sizeof(buf), "%02X(%u) ", static_cast<unsigned>(m.tag),
			              static_cast<unsigned>(m.body.size()));
			seq += buf;
		}
		std::fprintf(stderr, "[burst] tick: %zu msg(s) | %s%s\n", step.messages.size(), seq.c_str(),
		             b.sync_state == 5 ? "(§5.2a COMPLETE)" : "");
	}

	step.advanced = true;
	return step;
}

} // namespace opennova::np
