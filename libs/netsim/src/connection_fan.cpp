#include "netsim/connection_fan.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <novaworld/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <novaworld/ingame_encode.h> // FrameUpdate / network_compress_fixedpoint / encode_frame_update
#include <world/geom.h>              // to_fixed

#include "netsim/entity_wire_bridge.h" // health_classification_byte (the field-17 pack)

namespace opennova::netsim {

namespace {

// The per-frame S2C 0x0A field-driven §5.9 frame: a 12-byte position anchor, the phase-selected header
// sub-block, the 7-byte local-player tail, then one tag=1 compact record per replicated entity (each
// position compressed relative to the anchor). Faithful port of the header writer
// [orig: NetPacket_WritePlayerState @0x4ff6b0]: `flags2` is the per-connection phase byte and
// `flags2 & 3` selects the sub-block (0 weapon / 1 server-status / 2 env / 3 gametype). The entity
// loop mirrors serialize_entity_states_to_packet @0x50f070 (priority/budget port = step 2).
// [orig: NapiNPClientMsg_0x00A @0x42FEC0 (reader) / NetPacket_SerializePlayerState case 1 @0x4C09C0]
std::vector<uint8_t> build_0a_frame(const PlayerReplicationState &ctx,
                                    const std::vector<GameEntitySnapshot> &entities, uint8_t flags2,
                                    std::vector<RoundEventRecord> round_events = {}) {
	FrameUpdate fu;
	const int32_t ax = int32_t(ctx.spawn_x);
	const int32_t ay = int32_t(ctx.spawn_y);
	const int32_t az = int32_t(ctx.spawn_z);
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	fu.flags1 = 0x00; // state_flags (death/spectator/load signals) — event-driven, 0 in steady play.
	fu.flags2 = flags2;

	// Header sub-block, selected by `flags2 & 3` [orig: NetPacket_WritePlayerState @0x4ff6b0 phase
	// switch]. emit_connection_s2c drives flags2 from the per-connection phase counter.
	switch (flags2 & 0x03) {
	case 0:
		// Weapon/ammo/uniform block [orig: @0x4ff81b phase-0: preround timer + weapon slots 360/368/
		// 364/356/460 + ammo + CWeaponSlotManager_GetUniformTeamMask]. Our host does not model the
		// recipient's weapon-slot state yet, so emit the golden-witnessed co-op steady value: all-zero
		// slots + uniform team mask 8 — the exact bytes a retail co-op host sends steadily for a
		// standard-loadout player (golden ASH_I5A, re-verified against the v13 capture diff: golden
		// uniformMask=0x8 on every phase-0 frame; ours was 0). (Renamed from the FrameAimBlock
		// misnomer to FrameWeaponBlock, grill 2026-07-01.)
		fu.weapon.present = true;
		fu.weapon.uniform_team_mask = 0x8;
		break;
	case 1:
		// Server-status block [orig: @0x4ff9d5 phase-1]. LOAD-BEARING — carries the client's
		// fall-damage tolerance dword_C6EAE4. Left at its BSS default 0, the body motor's landing check
		// `velZ <= C6EAE4 * -1057` has threshold 0, so per-frame micro-gravity trips fall damage EVERY
		// grounded frame -> constant screen-red + shake + minimap-red (Player_OnDamageReceived), though
		// the player never dies (health loss is authority-gated). The client PERSISTS these between
		// updates, so sending them once per phase cycle suffices. [orig: Entity_UpdateInfantryPlayerBody
		// landing check @0x4b7cf4-0x4b7d2d; NapiNPClientMsg_0x00A phase-1 read @0x4301a1-0x4301bc;
		// defaults @0x4f638b C6EAE0=20/C6EAE4=13; grill 2026-06-28.]
		fu.timer.present = true;
		fu.timer.state0 = 20;        // dword_C6EAE0 (retail default)
		fu.timer.state1 = 13;        // dword_C6EAE4 = fall-damage tolerance (0 => constant fall dmg)
		fu.timer.state2 = 62;        // g_serverFps (cosmetic netgraph)
		fu.timer.state3 = 0;         // g_serverCpuPct (cosmetic netgraph)
		fu.timer.timer_seconds = -1; // dword_24C1958 = -1 -> no round time limit
		break;
	default:
		// Sub-block 3 (gametype): 0 bytes on the wire for a non-objective gametype [orig gate
		// g_GameType & 0x20000 @0x4ffc2d — off for co-op]. Sub-block 2 (env) is DEFERRED and never
		// selected here (see emit_connection_s2c): our host does not author world.env, so emitting it
		// would clobber the client's mission-loaded sky.
		break;
	}

	fu.state_flag_byte = 0x00; // 7-byte tail: not mounted, no stance bits yet (crouch/prone echo TBD)
	fu.mount_handle = 0xFFFF;
	// TAIL health (v121) -> g_local_player_entity->Health [orig: @0x4305df]. Send the player's healthMax
	// so the HUD reads 100% and the tail decrease-detector (`if v121 < Health` -> a brief damage flash
	// [orig: @0x43059a]) cannot self-trigger from an under-max tail. NOTE: this is only the minor health
	// flash — the CONSTANT screen-red/shake/minimap-red was the fall-damage tolerance C6EAE4 (the
	// flags2=1 sub-block above), NOT the tail. The per-entity record health byte is a DON'T-CARE for the
	// LOCAL player (NetPacket_SerializePlayerState skips the health-byte apply for g_local_player_entity
	// [orig: @0x4c11ac]). class-8 healthMax=150; real damage-driven health is a follow-up (thread the
	// connection's live health here).
	constexpr int kPlayerHealthMax = 150;
	fu.health = kPlayerHealthMax;
	fu.state_word = 0;

	for (const GameEntitySnapshot &e : entities) {
		const uint16_t cx = network_compress_fixedpoint(e.x - ax);
		const uint16_t cy = network_compress_fixedpoint(e.y - ay);
		const uint16_t cz = network_compress_fixedpoint(e.z - az);
		// Coarse wire heading from the engine BAM (D-NET-86). The write sides differ per class:
		// the PLAYER record TRUNCATES the high byte [orig: `*(u8*)(entity+19)` @0x4c0c5d — no
		// rounding]; the INFANTRY record ROUNDS `(v+0x800000)>>24` [orig: @0x4c08f8]; the VEHICLE
		// record rounds the high i16 `(v+0x8000)>>16` [orig: @0x460d0a].
		const uint8_t yaw_byte_trunc = uint8_t(uint32_t(e.euler_z) >> 24);
		const uint8_t yaw_byte_round = uint8_t((uint32_t(e.euler_z) + 0x00800000u) >> 24);
		const int16_t yaw_bam16 = int16_t((uint32_t(e.euler_z) + 0x00008000u) >> 16);
		FrameUpdateRecord rec;
		// The authoritative wire handle carried off the registry Entity (snapshot_of), not
		// re-derived here — EntityRegistry stays the one source of handle truth. Equals the
		// former (pool<<12 | slot) reconstruction by construction, so the bytes are unchanged.
		rec.handle = e.wire_handle;
		rec.type_id = e.type_id;
		rec.cls = e.entity_class;
		// op1 carrier select: the RIDDEN vehicle (entity+0x16C) wins, else the standing-on
		// ground entity (entity+0x28) [orig: @0x4c0a08]. With a live carrier the record's
		// position is CARRIER-LOCAL (Entity_TransformWorldToLocal @0x4c0b07) and its yaw
		// byte is the LOCAL heading's high byte (sar 24 @0x4c0b85); the client mirrors the
		// carrier back into its own groundEntity (@0x4c1353) — echoing 0xFFFF at a grounded
		// client detaches + hard-snaps it to the record position (the v26 origin teleport,
		// D-NET-151). A carrier whose pose we could not resolve (stale handle) falls back
		// to the free-standing form.
		const uint16_t player_carrier =
				(e.mount_handle != 0xFFFFu) ? e.mount_handle : e.ground_handle;
		switch (e.entity_class) {
		case EntityClass::Player:
			// 18-B player compact record, field sources witnessed in the case-1 write path
			// [orig: NetPacket_SerializePlayerState @0x4C09C0 case 1]. vehicle_bone (entity+343,
			// mounted only @0x4c0a1a) and seat_type (mount attribute bits @0x4c0a39) stay 0 —
			// seat-bone/attribute data is not modeled on the snapshot; 0 is the witnessed
			// unmounted AND grounded-standing value (a grounded record is bone=0 seat=0 +
			// carrier handle).
			if (player_carrier != 0xFFFFu && e.carrier_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						e.x, e.y, e.z, e.carrier_x, e.carrier_y, e.carrier_z,
						uint32_t(e.carrier_yaw_bam), uint32_t(e.carrier_pitch_bam), 0u);
				rec.player.carrier_handle = player_carrier;
				rec.player.pos_x_compressed = network_compress_fixedpoint(local.x);
				rec.player.pos_y_compressed = network_compress_fixedpoint(local.y);
				rec.player.pos_z_compressed = network_compress_fixedpoint(local.z);
				// LOCAL heading = own - carrier (the transform's out[3]); the wire byte is
				// its arithmetic high byte [orig: sar eax,18h @0x4c0b85].
				rec.player.yaw_byte = uint8_t(
						(uint32_t(e.euler_z) - uint32_t(e.carrier_yaw_bam)) >> 24);
			} else {
				rec.player.carrier_handle = 0xFFFFu;
				rec.player.pos_x_compressed = cx;
				rec.player.pos_y_compressed = cy;
				rec.player.pos_z_compressed = cz;
				rec.player.yaw_byte = yaw_byte_trunc; // truncated high byte [orig: @0x4c0c5d]
			}
			rec.player.pitch_byte =
					uint8_t((uint32_t(e.pitch_bam) + 0x00800000u) >> 24); // [orig: @0x4c0c77]
			// Movement-input byte (entity+0x12C): remote players are MOTOR-DRIVEN from this
			// replicated input [orig: write @0x4c0c9c; remote apply @0x4c11ec]. Echoes the
			// owning client's uplinked byte (apply_player_intent ingests it); 0 = no input
			// (idle) for players without an uplink source (the host's own player until its
			// input state is exported).
			rec.player.move_input_byte = e.move_input_byte;
			rec.player.state_flags = e.state_flags;     // entity+0x24 low byte, unmasked
			                                            // [orig: @0x4c0c7d; read-side masks
			                                            // local 0xE1 / remote 0xFD; bit 0x02 =
			                                            // dead/undeployed, spawn hook on 1->0]
			rec.player.anim_state_id = 0x2B;   // body/weapon anim-STATE id, entity+0x2B8 ?:
			                                   // +0x2BC vs the dword_8139E8 state-flags table
			                                   // [orig: @0x4c0cc7 / apply @0x4c1153]. The body
			                                   // FSM is unmodeled — send the retail spawn/idle
			                                   // default 0x2B (43) [orig: PlayerClass_InitEntity
			                                   // @0x4B1116 sets +0x2BC = 0x2B], NOT 0: state 0
			                                   // has no table flags and pins the body to the
			                                   // null clip (the v15 flicker). Live anim states
			                                   // need the peer body motor (D-NET-143 residual).
			rec.player.anim_channel_ratio = 0; // entity+0x188 channel ratio [orig: @0x4c0cf2];
			                                   // anim channel unmodeled — 0 is the witnessed
			                                   // null-object value
			rec.player.anim_def_index = e.equipped_adm_index;
			                                   // ADM anim-def index, entity+0x2B0 = the player's
			                                   // equipped-weapon adm index (the extended-uplink
			                                   // echo / the WPN_M4AUTO spawn default). 0 is a
			                                   // VALID index — 0xFF is the none sentinel the
			                                   // apply skips [orig: echo @0x4C20A3, apply-skip
			                                   // @0x4c11f2] (D-NET-143)
			// §5.10 health-classification byte (field 17): PACKED `(tier<<4)|(playerClass&0xF)`,
			// witnessed server-side in Entity_GetHealthClassification @0x4AD4E0 (called from the
			// case-1 compact write @0x4c0d71, byte store @0x4c0d89 — D-NET-138 FIXED). The client
			// apply @0x4AD580 writes the low nibble to entity->playerClass (+660) and immediately
			// re-resolves itemDef/ItemTypeIndex FROM playerClass (@0x4c1248-0x4c12be) — the old raw
			// clamped-health byte (e.g. 0x64 -> class 4) therefore mis-classed every remote player
			// on every applied frame and re-broke the @0x4307c4 cross-check right after each 0x18
			// repair: the retail-join C2S 0x0F flood. The LOCAL player skips the apply [orig:
			// @0x4c11ac]. The class nibble is 5..9 (player_class_for_wire), so a player's byte is
			// always non-zero — keeps the joiner's C2S 0x0C uplink gate (entity->Health != 0) alive.
			rec.player.health_class_byte =
					health_classification_byte(e.health, e.health_max, e.player_class);
			break;
		case EntityClass::Vehicle:
			// 15/21-B vehicle compact record [orig: Entity_SerializeMountedVehicleState @0x460560
			// op 1]. parent (entity+0x28 @0x460ba1) unmodeled -> 0xFFFF = world-frame position;
			// flags = entity+36 low byte verbatim [orig: @0x460d22] — its bit 0x04 selects the
			// 4-B rider-Euler tail over the 10-B weapon tail (which encode_vehicle_compact_record
			// mirrors), and bit 0x02 is the DESTROYED state whose wire transitions drive
			// Entity_KillBySlotId / Entity_RespawnVehicle on the client [orig: @0x460a25/@0x460918].
			rec.vehicle.parent_slot_handle = 0xFFFF;
			rec.vehicle.flags_byte = e.state_flags;
			rec.vehicle.pos_x_compressed = cx;
			rec.vehicle.pos_y_compressed = cy;
			rec.vehicle.pos_z_compressed = cz;
			rec.vehicle.euler_z = yaw_bam16; // heading i16 [orig: @0x460d0a]
			// entity+286 = the vehicle HEALTH word, stored back verbatim by the read
			// [orig: write @0x460d9b, read store @0x460aff]. Sending 0 here zeroed every
			// vehicle's health each frame — live-witnessed as all map vehicles dying
			// repeatedly (retail-join v12). The old `turret_pitch_raw` name was an
			// unwitnessed decode-era guess (D-NET-63 correction).
			rec.vehicle.health_word = static_cast<uint16_t>(
					e.health > 0 ? (e.health < 0xFFFF ? e.health : 0xFFFF) : 0);
			// Weapon-aim tail fields (entity+160 / vehicleData 132/135/136) stay 0 —
			// turret state is unmodeled.
			break;
		case EntityClass::Infantry:
			rec.infantry.vehicle_slot_handle = 0xFFFF;
			rec.infantry.pos_x_compressed = cx;
			rec.infantry.pos_y_compressed = cy;
			rec.infantry.pos_z_compressed = cz;
			rec.infantry.yaw_byte = yaw_byte_round; // rounded [orig: @0x4c08f8]
			break;
		case EntityClass::Guided:
		case EntityClass::NoNetworkCallback:
		case EntityClass::Unknown:
		default:
			continue; // no production 0x0A compact body
		}
		fu.records.push_back(std::move(rec));
	}
	// Tag-2 fired-round events, already recipient-selected + wire-converted by
	// select_round_events [orig: the g_round_event_refs interleave @0x50f312].
	fu.round_events = std::move(round_events);
	return encode_frame_update(fu);
}

// The S2C 0x0A anchor for one connection: its owned entity's live position (so the compact
// records compress small deltas around that client's own player), or the passed fallback when
// the connection has no owned entity yet (the host's own loopback, or a pre-spawn joiner).
// [orig: the per-player send descriptor / 12-byte frame anchor = player.spawn_x/y/z, §5.2a.]
PlayerReplicationState anchor_for_connection(const world::World &w, const Connection &conn,
                                             const PlayerReplicationState &fallback) {
	if (!conn.owned_entity.valid()) return fallback;
	const world::Entity *e = w.registry.get(conn.owned_entity);
	if (e == nullptr) return fallback;
	PlayerReplicationState a = fallback; // keep the non-position fields
	a.spawn_x = static_cast<uint32_t>(world::to_fixed(e->position.x));
	a.spawn_y = static_cast<uint32_t>(world::to_fixed(e->position.y));
	a.spawn_z = static_cast<uint32_t>(world::to_fixed(e->position.z));
	return a;
}

// ---------------------------------------------------------------------------
// Per-frame entity SELECTION for one recipient — the priority + aging + budget
// half of the original per-recipient send [orig: Server_BuildEntityPriorityList
// @ 0x50e590 (build + shell-sort the priority pairlist) and the budget-limited
// loop of serialize_entity_states_to_packet @ 0x50f070].
// ---------------------------------------------------------------------------

// g_entity_send_budget @0xC8FC50: the per-frame 0x0A byte cap, INCLUDING the header
// bytes (the original measures packet[3]-packet[0] where the header is already
// written). Default 600, set in Server_InitNewRoundState @0x51ca7c; runtime-writable
// via the BANDWIDTH server command (100-1600). The new/stale-recipient halving
// (budget >>= 1 iff slot+89876 congestion flag or connection uptime > 2000
// [orig: @0x517c62]) is deferred — no congestion-callback model yet.
constexpr int kEntitySendBudget = 600;

// Age-array index for one entity: pool-0 ages [0..255], pool-1 [256..511], slot & 0xFF
// [orig: idx = handle & 0xFFF, +256 if pool 1, @0x50f15c].
inline std::size_t age_index(const GameEntitySnapshot &e) {
	return (e.pool == 1 ? 256u : 0u) + (std::size_t(e.slot) & 0xFFu);
}

// Encoded wire size of one tag-1 record: [u8 1][u16 handle][u16 type] + the class body.
// Bodies are fixed-width per class (vehicle: flags bit 0x04 selects the 4-B tail over
// the 10-B weapon tail — see encode_vehicle_compact_record).
std::size_t record_wire_size(const GameEntitySnapshot &e) {
	switch (e.entity_class) {
	case EntityClass::Player:   return 5 + 18;
	case EntityClass::Vehicle:  return 5 + 11 + ((e.state_flags & 0x04) ? 4 : 10);
	case EntityClass::Infantry: return 5 + 14;
	default:                    return 0; // not emitted
	}
}

// Select and order this frame's tag-1 records for one connection. A faithful structural
// port of the original per-recipient chain, with the inputs our headless world does not
// model documented at their use sites (docs/net/novaworld-net-re.md D-NET-139):
//  1. saturating-age every tracked entity byte [orig: paddusb sweep @0x50e60f];
//  2. admit + score each replicable entity against the recipient anchor;
//  3. sort descending by key [orig: CPairList_ShellSortByValue @0x526cf0 — a Knuth-gap
//     shell sort; std::stable_sort is order-equivalent for our distinct keys];
//  4. take records until the byte budget (header included) is exhausted — the budget is
//     a SOFT cap checked after each record [orig: @0x50f34b], so the record in flight
//     completes; a selected entity's age resets to 0 [orig: @0x50f168].
// Round-robin across frames is EMERGENT from aging: starved entities' keys climb and
// age >= 50 force-admits them past the distance gate.
std::vector<GameEntitySnapshot> select_frame_entities(Connection &conn,
                                                      const std::vector<GameEntitySnapshot> &entities,
                                                      const PlayerReplicationState &anchor,
                                                      std::size_t header_bytes) {
	// 1. Age sweep [orig: @0x50e60f, saturating +1 over both pools' age arrays].
	for (uint8_t &a : conn.s2c_entity_age) {
		if (a != 0xFF) ++a;
	}

	struct Scored {
		int64_t key;
		const GameEntitySnapshot *snap;
	};
	std::vector<Scored> scored;
	scored.reserve(entities.size());

	const int64_t ax = int32_t(anchor.spawn_x);
	const int64_t ay = int32_t(anchor.spawn_y);
	const int64_t az = int32_t(anchor.spawn_z);

	for (const GameEntitySnapshot &e : entities) {
		if (record_wire_size(e) == 0) continue; // no compact form
		const uint8_t age = conn.s2c_entity_age[age_index(e)];

		// distanceTiles = (sqrt(dx^2 + dy^2 + (dz/2)^2) - boundRadius) >> 16 against the
		// recipient anchor [orig: @0x50e925; z half-weighted]. boundRadius (entity+0) is
		// unmodeled (0). The original anchor is the recipient EYE pos (entity pos + camera
		// offset entity+0x6C..); ours is the owned entity's position — the header carries
		// whichever anchor was used, so decompression stays exact either way.
		const int64_t dx = int64_t(e.x) - ax;
		const int64_t dy = int64_t(e.y) - ay;
		const int64_t dz = (int64_t(e.z) - az) >> 1;
		const int64_t dist =
				static_cast<int64_t>(std::sqrt(double(dx * dx + dy * dy + dz * dz)));
		const int32_t distance_tiles = int32_t(dist >> 16);

		// Distance gate [orig: @0x50e925]: skip unless within 1124 tiles, a tracked-handle
		// priority floor holds it (tracked handles unmodeled -> 0), or age force-admits.
		if (distance_tiles > 1124 && age < 50) continue;

		// Score [orig: pool-0 @0x50eb5f, pool-1 @0x50f008]. Ported terms: the distance
		// score, the (entity+36 & 1) >> 4 damp, the own-entity +1000 boost (pool 0 only),
		// and the age combine. The view/interest terms — angleScore (recipient view yaw),
		// LOS raycast, enemy/team bonuses, velocity/heading delta caches, and the +200
		// view-distance bonus (word_26C681E) — need recipient-view and LOS models the
		// headless host does not have; they contribute 0 here (D-NET-139).
		int64_t v = distance_tiles < 1124 ? (1124 - distance_tiles) : 0;
		if ((e.state_flags & 0x01) != 0) v >>= 4; // [orig: @0x50ebb0 region]
		const bool own = conn.owned_entity.valid() &&
		                 e.wire_handle == conn.owned_entity.packed &&
		                 e.entity_class == EntityClass::Player;
		const int64_t boost = own ? 1000 : 0; // [orig: pool-0 own-entity boost]
		const int64_t key = age + boost + v + ((age * (boost + v)) >> 8);
		scored.push_back({key, &e});
	}

	// 3. Highest priority first [orig: descending shell sort @0x526cf0].
	std::stable_sort(scored.begin(), scored.end(),
	                 [](const Scored &a, const Scored &b) { return a.key > b.key; });

	// 4. Budget walk (soft cap, header included) + age reset on selection.
	std::vector<GameEntitySnapshot> selected;
	std::size_t written = header_bytes;
	for (const Scored &s : scored) {
		selected.push_back(*s.snap);
		conn.s2c_entity_age[age_index(*s.snap)] = 0; // [orig: @0x50f168]
		written += record_wire_size(*s.snap);
		if (written >= std::size_t(kEntitySendBudget)) break; // [orig: @0x50f34b]
	}
	return selected;
}

// ---------------------------------------------------------------------------
// Per-frame ROUND-EVENT selection for one recipient — the tag-2 half of the
// per-recipient send [orig: Server_BuildRoundEventListForPlayer @0x4ffee0,
// called from Server_BuildEntityPriorityList @0x50e59c; records serialized by
// NetPacket_SerializeRoundEvent @0x504820 in the @0x50f070 interleave]. Walks
// the world round ring for events newer than this connection's watermark,
// SKIPS the recipient's own rounds (its client already simulated them
// [orig: @0x4fff97 shooter==recipient reject]), scores each by how close its
// line of fire passes to the recipient, sorts descending, and converts the
// survivors to wire records compressed against the recipient anchor.
// ---------------------------------------------------------------------------
std::vector<RoundEventRecord> select_round_events(const world::World &w, Connection &conn,
                                                  const PlayerReplicationState &anchor,
                                                  std::size_t budget_left) {
	std::vector<RoundEventRecord> out;
	const world::RoundRing &ring = w.rounds;

	// Watermark arm gate [orig: the playerSlot+97544 non-zero gate @0x4ffee8 — a
	// fresh player is armed at the current sequence, so the pre-join ring backlog
	// is never replayed to a joiner].
	if (!conn.round_watermark_armed) {
		conn.round_watermark_armed = true;
		conn.round_watermark = ring.last_stat();
		return out;
	}
	if (ring.count == 0 || budget_left == 0) {
		conn.round_watermark = ring.last_stat();
		return out;
	}

	const int64_t ax = int32_t(anchor.spawn_x);
	const int64_t ay = int32_t(anchor.spawn_y);
	const int64_t az = int32_t(anchor.spawn_z);
	const uint16_t own_handle =
			conn.owned_entity.valid() ? conn.owned_entity.packed : 0xFFFFu;

	struct ScoredRound {
		int32_t score;
		const world::RoundEvent *ev;
	};
	std::vector<ScoredRound> scored;

	for (int i = 0; i < ring.count; ++i) {
		const world::RoundEvent &ev = ring.records[size_t(i)];
		if (ev.stat <= conn.round_watermark) continue; // already swept [orig: @0x4fff3f]
		if (ev.shooter_handle == 0xFFFF) continue;     // [orig: @0x4fff4c]
		if (ev.shooter_handle == own_handle) continue; // own fire [orig: @0x4fff97]

		// Line-of-fire proximity score [orig: @0x4fff9d..@0x500126]. The original
		// builds the forward vector on the x87 (double sin/cos scaled 2^22, products
		// >> 22 then >> 6 -> 16.16); component convention X=sinYaw*cosPitch,
		// Y=cosYaw*cosPitch, Z=sinPitch per the round spawners
		// [orig: Weapon_SpawnSingleProjectile @0x4ebf51 / RoundData_SpawnRound @0x4ec5e9].
		constexpr double kBamToRad = 1.4629627251502471e-09; // [orig: dbl_7C3608 = 2pi/2^32]
		constexpr double kTrigScale = 4194304.0;             // [orig: dbl_7C3600 = 2^22]
		const double yaw = double(ev.dir_yaw) * kBamToRad;
		const double pitch = double(ev.dir_pitch) * kBamToRad;
		const int64_t sy = int64_t(std::sin(yaw) * kTrigScale);
		const int64_t cy = int64_t(std::cos(yaw) * kTrigScale);
		const int64_t sp = int64_t(std::sin(pitch) * kTrigScale);
		const int64_t cp = int64_t(std::cos(pitch) * kTrigScale);
		const int64_t fwd_x = ((sy * cp) >> 22) >> 6;
		const int64_t fwd_y = ((cy * cp) >> 22) >> 6;
		const int64_t fwd_z = sp >> 6;

		const int64_t dx = ax - ev.origin_x; // recipient - fire origin [orig: @0x500015]
		const int64_t dy = ay - ev.origin_y;
		const int64_t dz = az - ev.origin_z;

		// Projection of the recipient onto the fire line, clamped to [0, 1000u]
		// [orig: @0x500072 rounding-summed dot; clamp @0x500081].
		int64_t proj = ((fwd_x * dx + 0x8000) >> 16) + ((fwd_y * dy + 0x8000) >> 16) +
		               ((fwd_z * dz + 0x8000) >> 16);
		int64_t rx = dx, ry = dy, rz = dz;
		if (proj > 0) {
			if (proj > 65536000) proj = 65536000;
			rx = dx - ((fwd_x * proj + 0x8000) >> 16); // [orig: @0x5000a1]
			ry = dy - ((fwd_y * proj + 0x8000) >> 16);
			rz = dz - ((fwd_z * proj + 0x8000) >> 16);
		}
		const int64_t rz_half = rz >> 1; // z half-weight [orig: @0x5000d7]
		const int64_t lateral = static_cast<int64_t>(
				std::sqrt(double(rx * rx + ry * ry + rz_half * rz_half)));
		int32_t score = 0x4000 - int32_t(lateral >> 12); // [orig: @0x500115]
		if (score < 0) score = 0;
		scored.push_back({score, &ev});
	}

	// Closest-to-the-bullet-line first [orig: CPairList_ShellSortByValue @0x50016a].
	std::stable_sort(scored.begin(), scored.end(),
	                 [](const ScoredRound &a, const ScoredRound &b) { return a.score > b.score; });

	// Convert to wire records — capped at 255 [orig: the g_round_event_refs array
	// @0x500190] and by the remaining frame budget (soft cap, like the tag-1 walk
	// [orig: @0x50f34b]).
	std::size_t written = 0;
	for (const ScoredRound &s : scored) {
		if (out.size() >= 255) break;
		const world::RoundEvent &ev = *s.ev;
		RoundEventRecord rec;
		rec.flags = ev.mode_flags;
		rec.adm_index = ev.adm_index;
		rec.subtype = ev.subtype;
		rec.shooter_handle = ev.shooter_handle;
		if (ev.slot_byte != 0) { // [orig: @0x5048bb]
			rec.flags |= 0x80;
			rec.slot_byte = ev.slot_byte;
		}
		// The shooter's fire target is read LIVE off its entity at serialize time
		// [orig: @0x50485a reads shooter+104->+12; a freed shooter reads as no target].
		const world::Entity *shooter = w.registry.get(world::EntityHandle{ev.shooter_handle});
		if (shooter != nullptr && shooter->last_fire_target.valid()) {
			rec.flags |= 0x40;
			rec.target_handle = shooter->last_fire_target.packed;
		}
		rec.shot_seq = ev.shot_seq;
		// Fire origin compressed against the SAME anchor the frame header carries
		// [orig: @0x504994 subtracts g_priority_ref_x/y/z — the recipient refs].
		rec.pos_x_compressed = network_compress_fixedpoint(int32_t(ev.origin_x - int32_t(ax)));
		rec.pos_y_compressed = network_compress_fixedpoint(int32_t(ev.origin_y - int32_t(ay)));
		rec.pos_z_compressed = network_compress_fixedpoint(int32_t(ev.origin_z - int32_t(az)));
		// Direction BAM high words, rounded [orig: @0x504a18/@0x504a3c].
		rec.yaw_bam_high = uint16_t((uint32_t(ev.dir_yaw) + 0x8000u) >> 16);
		rec.pitch_bam_high = uint16_t((uint32_t(ev.dir_pitch) + 0x8000u) >> 16);

		const std::size_t wire = 1 /*tag*/ + 17 + ((rec.flags & 0x80) ? 1u : 0u) +
		                         ((rec.flags & 0x40) ? 2u : 0u);
		out.push_back(rec);
		written += wire;
		if (written >= budget_left) break;
	}

	conn.round_watermark = ring.last_stat(); // [orig: @0x5001ae stamps stat_id]
	return out;
}

// Header wire size for a given flags2, mirroring encode_frame_update: 12-B anchor +
// 2 flag bytes + the phase sub-block (0 weapon 11 B / 1 timer 6 B / 2 env 11 B /
// 3 gametype 0 B) + the 7-B local tail + the 1-B event-loop terminator. The passenger
// block ((flags2 & 0xF) == 8) never fires on the {1,0,3} safe cycle.
std::size_t frame_header_bytes(uint8_t flags2) {
	switch (flags2 & 0x03) {
	case 0: return 12 + 2 + 11 + 7 + 1;
	case 1: return 12 + 2 + 6 + 7 + 1;
	case 2: return 12 + 2 + 11 + 7 + 1;
	default: return 12 + 2 + 0 + 7 + 1;
	}
}

} // namespace

// Drain + read-apply the queued C2S 0x0C player uplinks on one connection's transport. Takes the
// whole Connection (not a bare transport) so it can enforce the per-connection owner gate and
// null-checks the transport internally — symmetric with emit_connection_s2c.
void drain_connection_c2s(world::World &world, const Connection &conn) {
	if (conn.transport == nullptr) return;
	Datagram dg;
	while (conn.transport->host_recv(dg)) {
		// §5.10 player-input uplink only this increment (other in-match C2S tags TBD).
		if (dg.tag != 0x0C) continue;

		// 5-byte sub-header [u16 handle][u16 itemTypeId][u8 sub_op], then the 43-B body.
		std::size_t consumed = 0;
		EntityPacketSubHeader hdr;
		if (!decode_entity_packet_sub_header(dg.body.data(), dg.body.size(), hdr, consumed))
			continue;
		if (hdr.sub_op != 0x0A) continue; // 0x0A=extended (type 10); 0x0B compact = later

		// [D-NET-119] Owner gate: a connection may only SNAP its OWN entity. The original resolves
		// the wire handle (pool<<12|slot) to an entity and verifies `entity == *owner_ctx` (the
		// connection's authorized entity) before invoking the +356 read-apply callback; a handle
		// naming any other entity is silently ignored — no apply, rejection, or disconnect (returns
		// 0 @0x4d6b7e). An invalid owner (the host's own loopback, or a pre-spawn joiner) matches
		// nothing, mirroring the original's `owner_ctx != null` guard @0x4d6ad3. [orig:
		// dispatch_entity_packet_callback @0x4D6A80 `entity == *owner_ctx` @0x4d6b08; owner_ctx <-
		// NapiNPServerMsg_0x00C @0x501c30 connCtx+0x160 -> +0xC0 -> *.]
		if (world::EntityHandle{hdr.handle} != conn.owned_entity) continue;

		PlayerExtendedUplink up;
		std::size_t body_consumed = 0;
		if (!decode_player_extended_uplink(dg.body.data() + consumed, dg.body.size() - consumed,
		                                   up, body_consumed))
			continue;

		PlayerIntent intent;
		intent.entity_handle = hdr.handle;
		intent.item_type_id = hdr.item_type_id;
		intent.carrier_handle = up.carrier_handle; // ground entity — pos/heading are
		                                           // carrier-local when set (D-NET-151)
		intent.pos_x = up.pos_x;
		intent.pos_y = up.pos_y;
		intent.pos_z = up.pos_z;
		intent.heading = up.heading;
		intent.pitch = up.pitch;
		intent.move_input = up.move_input_byte; // entity+0x12C — echoed in the 0x0A off-12
		intent.state_flags = up.state_flags_byte; // raw entity+0x24 low byte; bits 2-4 replace
		                                          // ours [orig: @0x4c1e4d] (crouch/prone family)
		intent.equipped_adm_index = up.equipped_adm_index; // entity+0x2B0 — echoed at 0x0A off-16
		                                                   // [orig: @0x4C20A3] (D-NET-143)
		intent.buttons = 0; // extended uplink carries state/anim bytes, not a buttons word
		apply_player_intent(world, intent);
	}
}

// Serialize the live world into one S2C 0x0A frame for `conn` and host_send it. anchor_for_connection
// is file-static; the per-connection emit body is shared by the legacy listen-server binding and
// npruntime's Server_TickUpdate fan over connection_list.
void emit_connection_s2c(const world::World &w, Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         const PlayerReplicationState &fallback_anchor) {
	if (conn.transport == nullptr) return;
	const PlayerReplicationState anchor = anchor_for_connection(w, conn, fallback_anchor);

	// Advance the per-connection 0x0A sub-block phase and select this frame's header sub-block
	// [orig: ++playerSlot+100566 then NetPacket_WritePlayerState writes it as flags2, phase&3 =
	// sub-block]. The original free-runs an 8-bit counter, so phase&3 cycles all four sub-blocks
	// (0 weapon / 1 server-status / 2 env / 3 gametype) evenly and phase&0xF==8 emits the passenger
	// block every 16th frame. We cycle a SAFE 3-value subset {1,0,3} for now — env (2) is DEFERRED
	// because our host does not yet author world.env, so sending it would OVERWRITE the client's
	// correct mission-loaded sky (fog/time-of-day); the passenger block needs vehicle-mount modeling.
	// Both slot back into the free counter once those land. First send is phase 1 (server-status), so
	// the load-bearing fall-damage tolerance reaches the client on frame 1 (matches the original, which
	// increments to 1 before its first write @0x517be8).
	static constexpr uint8_t kSafeSubCycle[3] = {1, 0, 3}; // -> sub 1(status) / 0(weapon) / 3(gametype)
	const uint8_t flags2 = kSafeSubCycle[conn.s2c_phase % 3u];
	++conn.s2c_phase;

	// Priority + aging + byte-budget selection of this frame's tag-1 records [orig:
	// Server_BuildEntityPriorityList @ 0x50e590 + the serialize_entity_states_to_packet
	// @ 0x50f070 budget loop]. NOT ported: the original writes NO entity records to the
	// LISTEN HOST's OWN local player (@0x50f07e early return; the priority build is also
	// skipped @0x517c1b) — its local client reads process memory. Our serve-and-play local
	// view RENDERS FROM the loopback 0x0A fold (ADR 0011), so the loopback connection gets
	// the full record set; that frame never leaves the process, so retail interop is
	// unaffected (D-NET-140).
	const std::size_t header_bytes = frame_header_bytes(flags2);
	const std::vector<GameEntitySnapshot> selected =
			select_frame_entities(conn, ents, anchor, header_bytes);
	// Remaining frame budget for the tag-2 round events. The original interleaves
	// tag-1/tag-2 pairs under ONE budget [orig: @0x50f070 loop]; ours selects
	// entities first, rounds into the remainder — same cap, grouped order (the
	// retail decode loop is tag-driven either way). (D-NET-152)
	std::size_t used = header_bytes;
	for (const GameEntitySnapshot &e : selected) used += record_wire_size(e);
	const std::size_t round_budget =
			used < std::size_t(kEntitySendBudget) ? std::size_t(kEntitySendBudget) - used : 0;
	std::vector<RoundEventRecord> rounds = select_round_events(w, conn, anchor, round_budget);
	conn.transport->host_send(kTag0aFrameUpdate,
	                          build_0a_frame(anchor, selected, flags2, std::move(rounds)));
}

} // namespace opennova::netsim
