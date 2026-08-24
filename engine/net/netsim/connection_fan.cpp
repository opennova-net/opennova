#include "netsim/connection_fan.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <npwire/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <npwire/ingame_encode.h> // FrameUpdate / network_compress_fixedpoint / encode_frame_update
#include <npwire/game_type.h>
#include <npwire/ingame_message_id.h>
#include <npwire/wire_handle.h>
#include <world/geom.h>              // to_fixed
#include <world/vehicle_mount.h>

#include "netsim/entity_wire_bridge.h" // health_classification_byte (the field-17 pack)

namespace opennova::netsim {

namespace {

// The per-connection 0x0A header state emit_connection_s2c derives from the RECIPIENT — the
// flags1 signal byte and the 7-byte local-player tail's stance/mount fields.
struct FrameHeaderState {
	// flags1 [orig: NetPacket_WritePlayerState @0x4ff793-0x4ff7dd]: bit0 = spectator
	// (slot+100567 — unmodeled 0), bit1 = RESPAWN-PENDING (slot+89912 & 0x10) — re-asserted
	// EVERY frame; the client's deploy screen is g_deploy_screen_active = (flags1 & 2) != 0 each frame,
	// so one bit1=0 frame closes it [orig: NapiNPClientMsg_0x00A @0x42ff82]. bit2 = the
	// one-shot load hint (entity+44 & 0x1000 — unmodeled). (D-NET-156)
	uint8_t flags1 = 0;
	// Tail state byte bits 0-1 = the recipient's OWN [prone, crouch] echo — the client
	// re-latches its stance from this EVERY frame [orig: tail read @0x4303e5 (byte << 8 ->
	// MoveOrder bits 8-9) -> latches @0x430562/@0x430570]; a hardcoded 0 force-stands a
	// crouched retail client each frame (the pre-v32 crouch/prone bug).
	uint8_t tail_state_byte = 0;
	// Tail mount handle = the recipient's OWN carrier (its ridden vehicle), 0xFFFF free.
	uint16_t tail_mount_handle = 0xFFFF;
	// Tail health = the recipient's LIVE Health — the client STORES it as its own
	// (g_local_player_entity->Health @0x4305df); 0 is the victim's death signal (with the
	// record byte13 dead bit). The pre-v34 hardcoded 150 meant a killed client never
	// learned it died. A decrease also fires the brief damage flash [orig: @0x43059a].
	int16_t tail_health = 150;
	// Phase-2 global environment and phase-8 recipient mount-ammo state. These are
	// derived once before budgeting so the conditional header width and bytes agree.
	uint8_t preround_delay_seconds = 0;
	uint8_t respawn_delay_seconds = 0;
	uint8_t downed_revive_seconds = 0;
	uint8_t spawn_target_hold_seconds = 0;
	FrameEnv env{};
	FrameMountAmmo mount_ammo{};
};

// The per-frame S2C 0x0A field-driven §5.9 frame: a 12-byte position anchor, the phase-selected header
// sub-block, the 7-byte local-player tail, then one tag=1 compact record per replicated entity (each
// position compressed relative to the anchor). Faithful port of the header writer
// [orig: NetPacket_WritePlayerState @0x4ff6b0]: `flags2` is the per-connection phase byte and
// `flags2 & 3` selects the sub-block (0 weapon / 1 server-status / 2 env / 3 gametype). The entity
// loop mirrors serialize_entity_states_to_packet @0x50f070 (priority/budget port = step 2).
// [orig: NapiNPClientMsg_0x00A @0x42FEC0 (reader) / NetPacket_SerializePlayerState case 1 @0x4C09C0]
std::vector<uint8_t> build_0a_frame(const PlayerReplicationState &ctx,
                                    const std::vector<GameEntitySnapshot> &entities, uint8_t flags2,
                                    const FrameHeaderState &hdr,
                                    uint32_t game_type,
                                    const world::World::SubgoalState &subgoals,
                                    std::vector<RoundEventRecord> round_events = {}) {
	FrameUpdate fu;
	const int32_t ax = int32_t(ctx.spawn_x);
	const int32_t ay = int32_t(ctx.spawn_y);
	const int32_t az = int32_t(ctx.spawn_z);
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	fu.flags1 = hdr.flags1; // per-recipient signal byte (deploy hold / spectator / load hint)
	fu.flags2 = flags2;

	// Header sub-block, selected by `flags2 & 3` [orig: NetPacket_WritePlayerState @0x4ff6b0 phase
	// switch]. emit_connection_s2c drives flags2 from the per-connection phase counter.
	switch (flags2 & 0x03) {
	case 0:
		// Weapon/ammo/uniform block [orig: @0x4ff81b phase-0: preround timer + weapon slots 360/368/
		// 364/356/460 + ammo + ZoneSlotChain_GetOwnedZoneMask]. Our host does not model the
		// recipient's +356/+460 weapon-slot state yet; +360/+368/+364 are the live
		// respawn/downed counters from its player slot. The uniform
		// mask is the recipient's OWNED-ZONE mask from the zone chain (net-re §5.61) — the rep-state
		// default 0x8 is the golden ASH_I5A steady value (zone 3 wholly owned), so a chain-less host
		// still emits the witnessed byte. (Renamed from the FrameAimBlock misnomer to
		// FrameWeaponBlock, grill 2026-07-01.)
		fu.weapon.present = true;
		// The global whole-second countdown is truncated directly to the wire
		// byte; values above 255 wrap rather than clamp.
		// [orig: NetPacket_WritePlayerState @0x4FF82D..0x4FF837]
		fu.weapon.preround_timer = hdr.preround_delay_seconds;
		fu.weapon.slot_state360 = hdr.respawn_delay_seconds;
		fu.weapon.slot_state368 = hdr.downed_revive_seconds;
		fu.weapon.slot_state364 = hdr.spawn_target_hold_seconds;
		fu.weapon.uniform_team_mask = ctx.uniform_team_mask;
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
	case 2:
		// Retail-native World state is quantized only at this wire boundary.
		fu.env = hdr.env;
		break;
	default: // case 3
		// Sub-block 3 (gametype): four objective i32s only when the shared
		// g_GameType bit 0x20000 is set (Co-op 0x30020 is the captured case).
		// The masks are the authoritative World::subgoals state consumed by
		// the objective HUD on each recipient.
		fu.objective.present = opennova::game_type::is_objective(game_type);
		fu.objective.state[0] = static_cast<int32_t>(subgoals.won);
		fu.objective.state[1] = static_cast<int32_t>(subgoals.lost);
		fu.objective.state[2] = static_cast<int32_t>(subgoals.show_win);
		fu.objective.state[3] = static_cast<int32_t>(subgoals.show_lose);
		break;
	}

	// 7-byte tail: the recipient's OWN stance echo (bits 0-1 = prone/crouch — the client
	// re-latches from it every frame) + its own carrier handle. See FrameHeaderState.
	fu.state_flag_byte = hdr.tail_state_byte;
	fu.mount_handle = hdr.tail_mount_handle;
	// TAIL health (v121) -> g_local_player_entity->Health [orig: @0x4305df] — the recipient's
	// LIVE health (FrameHeaderState.tail_health): the client STORES it as its own, so damage
	// reads red (the decrease-detector flash [orig: @0x43059a]) and 0 is the authoritative
	// death signal (paired with the record byte13 dead bit — the v33 "killee never knows"
	// fix). The per-entity record health byte stays a DON'T-CARE for the LOCAL player
	// (NetPacket_SerializePlayerState skips that apply [orig: @0x4c11ac]); the tail is the
	// one channel its own health rides.
	fu.health = hdr.tail_health;
	fu.state_word = 0;
	fu.passenger = hdr.mount_ammo;

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
				(e.mount_handle != wire_handle::kInvalid) ? e.mount_handle : e.ground_handle;
		switch (e.entity_class) {
		case EntityClass::Player:
			// 18-B player compact record, field sources witnessed in the case-1 write path
			// [orig: NetPacket_SerializePlayerState @0x4C09C0 case 1]. vehicle_bone = the raw
			// attach bone (entity+0x157, mounted only @0x4c0a1a) — the client resolves ITS
			// seat from it (Entity_TryAttachOrDetach @0x436610; bone 0 or no carrier =
			// detach). A designated-G carried EWeap overloads seat_type as the live
			// MountSlot route echo: 1 = child slot, 2 = groundEntity vehicle slot
			// [orig: ItemDef type/attrib and +0x326/+0x312 gates @0x4c0a39].
			rec.player.vehicle_bone = e.veh_bone;
			rec.player.seat_type = e.mounted_weapon_seat_type;
			if (player_carrier != wire_handle::kInvalid && e.carrier_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						e.x, e.y, e.z, e.carrier_x, e.carrier_y, e.carrier_z,
						uint32_t(e.carrier_yaw_bam), uint32_t(e.carrier_pitch_bam),
						uint32_t(e.carrier_roll_bam));
				rec.player.carrier_handle = player_carrier;
				rec.player.pos_x_compressed = network_compress_fixedpoint(local.x);
				rec.player.pos_y_compressed = network_compress_fixedpoint(local.y);
				rec.player.pos_z_compressed = network_compress_fixedpoint(local.z);
				// LOCAL heading = own - carrier (the transform's out[3]); the wire byte is
				// its arithmetic high byte [orig: sar eax,18h @0x4c0b85].
				rec.player.yaw_byte = uint8_t(
						(uint32_t(e.euler_z) - uint32_t(e.carrier_yaw_bam)) >> 24);
			} else {
				rec.player.carrier_handle = wire_handle::kInvalid;
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
			                                            // dead/undeployed, spawn hook on 1->0;
			                                            // bit0 = hidden while respawn-pending
			                                            // (the golden pre-deploy 0x01 byte13)]
			// Body-anim state id, pending-wins [orig: @0x4c0cc7 reads +0x2B8 ?: +0x2BC; client
			// apply @0x4c1153 arbitrates vs the g_animStateFlagsTable table]. Live states come from the
			// infantry motor's wire mirror — the authority selection pass drives remote
			// players from their replicated input (D-NET-159; the 43-hardcode was v31's
			// frozen-body defect).
			rec.player.anim_state_id =
					e.anim_pending_id != 0 ? e.anim_pending_id : e.anim_state_id;
			rec.player.anim_channel_ratio = e.anim_channel_ratio;
			                                   // entity+0x188 channel elapsed-ticks-in-loop,
			                                   // clamp 255 [orig: @0x4c0cf2]
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
			// 15/21-B vehicle compact record [orig: Entity_SerializeVehicleState @0x460560
			// op 1]. parent (entity+40 @0x460b4d) unmodeled -> 0xFFFF = world-frame position;
			// flags = entity+36 low byte verbatim [orig: @0x460d22]. Bit 0x04 selects the 4-B
			// DEAD-POSE euler tail over the 10-B weapon tail (encode_vehicle_compact_record
			// mirrors the split): the death family sets Flags |= 6, so the short form is the
			// WRECK pose (drive-authority witness 2026-07-04 — a LIVE driven vehicle stays
			// full-form; the old "mounted form" reading was the D-NET-63-era misnomer). Bit
			// 0x02's wire transitions drive Entity_KillBySlotId / Entity_RespawnVehicle on the
			// client [orig: @0x460a25/@0x460918] — our route_round_deaths does not yet kill
			// vehicles, so live emission always takes the full form (correct for ridden ones).
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
			// entity+160 (weapon X) remains zero until turret aim is modeled.
			// Live prediction registers: fixed-point forward/lateral commands use
			// the retail 16-bit codec; steering is the rounded BAM32 high word.
			// [orig: @0x460dc2..0x460e10; net-re section 5.13]
			rec.vehicle.weapon_aim_y =
					network_compress_fixedpoint(e.vehicle_forward_speed_reg);
			rec.vehicle.weapon_aim_z =
					network_compress_fixedpoint(e.vehicle_lateral_speed_reg);
			rec.vehicle.weapon_heading_bam = static_cast<int16_t>(
					(static_cast<uint32_t>(e.vehicle_steer_target_bam) +
					 0x00008000u) >> 16);
			break;
		case EntityClass::Infantry: {
			// Existing 14-byte infantry record, with the exact witnessed field
			// sources [orig: NetPacket_SerializeInfantryEntityState @0x4C0320].
			// A mounted entity writes its raw bone, vehicle handle, and local pose;
			// no overlay-specific protocol extension is needed.
			rec.infantry.seat_bone_idx =
					e.mount_handle != wire_handle::kInvalid ? e.veh_bone : 0;
			if (e.mount_handle != wire_handle::kInvalid && e.carrier_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						e.x, e.y, e.z, e.carrier_x, e.carrier_y, e.carrier_z,
						uint32_t(e.carrier_yaw_bam), uint32_t(e.carrier_pitch_bam),
						uint32_t(e.carrier_roll_bam));
				rec.infantry.vehicle_slot_handle = e.mount_handle;
				rec.infantry.pos_x_compressed = network_compress_fixedpoint(local.x);
				rec.infantry.pos_y_compressed = network_compress_fixedpoint(local.y);
				rec.infantry.pos_z_compressed = network_compress_fixedpoint(local.z);
				const uint32_t local_heading =
						uint32_t(e.euler_z) - uint32_t(e.carrier_yaw_bam);
				rec.infantry.yaw_byte =
						uint8_t((local_heading + 0x00800000u) >> 24);
			} else {
				rec.infantry.vehicle_slot_handle = wire_handle::kInvalid;
				rec.infantry.pos_x_compressed = cx;
				rec.infantry.pos_y_compressed = cy;
				rec.infantry.pos_z_compressed = cz;
				rec.infantry.yaw_byte = yaw_byte_round; // rounded [orig: @0x4c08f8]
			}
			rec.infantry.flags_byte = e.state_flags; // entity+36 low byte [orig: @0x4c0917]
			// Clamp entity+748 to entity+16 +/- 536870880, then pack the
			// rounded absolute BAM high byte [orig: @0x4c0921..0x4c0960].
			// Unsigned arithmetic preserves the original BAM wrap before the
			// signed comparisons.
			int32_t target_heading = e.infantry_target_heading_bam;
			const int32_t target_delta = static_cast<int32_t>(
					uint32_t(target_heading) - uint32_t(e.euler_z));
			constexpr int32_t kInfantryTargetClamp = 536870880;
			if (target_delta > kInfantryTargetClamp) {
				target_heading = static_cast<int32_t>(
						uint32_t(e.euler_z) + uint32_t(kInfantryTargetClamp));
			} else if (target_delta < -kInfantryTargetClamp) {
				target_heading = static_cast<int32_t>(
						uint32_t(e.euler_z) - uint32_t(kInfantryTargetClamp));
			}
			rec.infantry.pitch_byte = uint8_t(
					(uint32_t(target_heading) + 0x00800000u) >> 24);
			rec.infantry.aim_yaw_byte = uint8_t(
					(uint32_t(e.infantry_aim_pitch_bam) + 0x00800000u) >> 24);
			rec.infantry.anim_byte =
					e.anim_pending_id != 0 ? e.anim_pending_id : e.anim_state_id;
			break;
		}
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

// Resolve the one legal S2C 0x0A anchor: this connection's live player
// allocation. Retail's wrapper is state==6 gated and immediately reads that
// player's eye/reference position; an unbound/freed slot never enters the
// writer and therefore never emits a map-centre fallback frame.
// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate and recipient
// eye stores @0x517BF5..0x517C13]
// [orig: the per-player send descriptor / 12-byte frame anchor = player.spawn_x/y/z, §5.2a.]
const world::Entity *owned_entity_for_emit(const world::World &w,
                                           const Connection &conn) {
	if (!conn.owned_entity.valid()) return nullptr;
	const world::Entity *entity = w.registry.get(conn.owned_entity);
	if (entity == nullptr) return nullptr;
	if (conn.owned_entity_spawn_id != 0 &&
	    entity->registry_spawn_id != conn.owned_entity_spawn_id)
		return nullptr;
	return entity;
}

PlayerReplicationState anchor_for_owned_entity(const world::Entity &entity) {
	PlayerReplicationState anchor;
	anchor.spawn_x = static_cast<uint32_t>(world::to_fixed(entity.position.x));
	anchor.spawn_y = static_cast<uint32_t>(world::to_fixed(entity.position.y));
	anchor.spawn_z = static_cast<uint32_t>(world::to_fixed(entity.position.z));
	return anchor;
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
int g_entity_send_budget = 600;

// word_26C681E — the environment's draw/view distance in world units, read by the
// priority score for the LOS gate and the +200 inside-view bonus. It is ZERO in a
// fresh image (env-written at load), and zero disables both terms in retail too —
// so an embedder that never wires it gets the faithful unwired behavior. The sim
// feeds it from the same env value the occlusion camera uses.
// [orig: word_26C681E reads @0x50eabb/@0x50eb62; env/render block writer]
int g_view_distance_units = 0;

} // namespace

void set_entity_send_budget(int bytes) {
	// The witnessed BANDWIDTH server-command clamp [orig: 100-1600 onto
	// g_entity_send_budget @0xC8FC50].
	if (bytes < 100) bytes = 100;
	if (bytes > 1600) bytes = 1600;
	g_entity_send_budget = bytes;
}

int entity_send_budget() { return g_entity_send_budget; }

void set_view_distance_units(int units) {
	g_view_distance_units = units < 0 ? 0 : units;
}

int view_distance_units() { return g_view_distance_units; }

namespace {

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
	case EntityClass::Vehicle:  return 5 + 11 + ((e.state_flags & kVehicleCompactFlagDeadPose) ? 4 : 10);
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
// The 32-bit wrapping abs the score's BAM deltas use [orig: cdq/xor/sub].
inline int32_t prio_iabs32(int32_t v) {
	return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v;
}

std::vector<GameEntitySnapshot> select_frame_entities(const world::World &w,
                                                      Connection &conn,
                                                      const std::vector<GameEntitySnapshot> &entities,
                                                      const PlayerReplicationState &anchor,
                                                      std::size_t header_bytes,
                                                      std::size_t hard_frame_limit) {
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

	// The recipient's view + relations come from its OWN entity's snapshot in the
	// frame list [orig: the playerState-> entity reads @0x50e64e..0x50e689 — the
	// original returns EMPTY with no player entity; our pre-spawn/loopback frames
	// keep flowing with the view-dependent terms zeroed].
	const GameEntitySnapshot *self = nullptr;
	if (conn.owned_entity.valid()) {
		for (const GameEntitySnapshot &e : entities) {
			if (e.wire_handle == conn.owned_entity.packed) { self = &e; break; }
		}
	}
	const int32_t view_yaw = self ? self->euler_z : 0;
	const int32_t view_pitch = self ? self->pitch_bam : 0;
	const uint8_t self_team = self ? self->team : 0;
	// The +1000 term targets the recipient's CARRIER — mount wins over ground
	// [orig: targetEntity = playerEntity+0x28, overridden by +0x16C @0x50e65e..0x50e66b] —
	// the vehicle you ride (or deck you stand on) streams at top priority.
	const uint16_t carrier_handle = self
			? (self->mount_handle != 0xFFFF ? self->mount_handle : self->ground_handle)
			: 0xFFFF;
	// The dead-or-spectator flag: the recipient entity's DEAD bit (flags & 2 —
	// the everyday between-death-and-respawn state) OR the slot spectator flag
	// playerState[89912] & 0x10 (that MODE is unmodeled and stays a D-NET-139
	// residual) [orig: @0x50e677..0x50e693].
	const bool self_dead_or_spectator =
			self != nullptr && (self->state_flags & 0x02) != 0;

	for (const GameEntitySnapshot &e : entities) {
		if (record_wire_size(e) == 0) continue; // no compact form
		const uint8_t age = conn.s2c_entity_age[age_index(e)];

		// distanceTiles = (sqrt(dx^2 + dy^2 + (dz/2)^2) - boundRadius) >> 16 against the
		// recipient anchor [orig: @0x50e925; z half-weighted]. boundRadius (entity+0) is
		// unmodeled (0). The original anchor is the recipient EYE pos (entity pos + camera
		// offset entity+0x6C..); ours is the owned entity's position — the header carries
		// whichever anchor was used, so decompression stays exact either way.
		// Squared in double like retail's fild/fmul — the int64 product can
		// overflow at map-corner separations.
		const int64_t dx = int64_t(e.x) - ax;
		const int64_t dy = int64_t(e.y) - ay;
		const int64_t dzh = (int64_t(e.z) - az) >> 1;
		const int64_t dist = static_cast<int64_t>(
				std::sqrt(double(dx) * double(dx) + double(dy) * double(dy) +
				          double(dzh) * double(dzh)));
		const int32_t distance_tiles = int32_t(dist >> 16);

		// The recipient's requested-interest floor: a row named by one of its 4
		// tracked slots keeps that score as a priority floor and passes the
		// distance gate regardless of range. The invalid-slot 0x12 despawn leg
		// of the original's walk stays a D-NET-139 residual.
		// [orig: the tracked-slot walk @0x50e5f4.. -> priorPriority]
		int32_t prior_priority = 0;
		for (int t = 0; t < 4; ++t) {
			if (conn.tracked_handle[t] != 0xFFFF &&
			    conn.tracked_handle[t] == e.wire_handle) {
				prior_priority = conn.tracked_score[t];
				break;
			}
		}

		// Distance gate [orig: @0x50e925]: skip unless within 1124 tiles, the
		// tracked-handle priority floor holds it, or age force-admits
		// [orig: `dist <= 1124 || priorPriority || age >= 50` @0x50e93a].
		if (distance_tiles > 1124 && prior_priority == 0 && age < 50) continue;

		// ---- Score, full terms (D-NET-139 port 2026-08-06)
		// [orig: pool-0 @0x50eb5f, pool-1 @0x50f008 — both expand to
		//  distScore + 2*angle + 50*enemy (+50*isPlayer, pool 0)
		//            + 100*LOS (+100*standing, pool 0)
		//            + 1000*(entity == recipient carrier) (+200*occupied, pool 1)
		//            (+200 inside the view distance)
		//            + 3*speedDelta + 2*headingDelta].
		int64_t v = distance_tiles < 1124 ? (1124 - distance_tiles) : 0;

		if (self_dead_or_spectator) {
			// A dead-or-spectator recipient REPLACES the whole positional score
			// (distScore, angle, LOS, +200-in-view) with a flat social score;
			// the motion-delta boosts and the bit0 damp below still apply.
			// [orig: pool-0 @0x50eb28..0x50eb3f = 600*mounted (entity+0x16C != 0)
			//  + 200*sameTeam; pool-1 @0x50efcf..0x50efe6 = 1000*carrier
			//  + 300*occupied + 100*sameTeam]
			const bool same_team = e.team != 0 && e.team == self_team;
			if (e.pool == 1) {
				const bool is_carrier = e.wire_handle == carrier_handle;
				v = 1000 * (is_carrier ? 1 : 0) + 300 * (e.occupied ? 1 : 0) +
				    100 * (same_team ? 1 : 0);
			} else {
				v = 600 * (e.mounted ? 1 : 0) + 200 * (same_team ? 1 : 0);
			}
		} else if (self != nullptr) {
			// View-angle score: 256 - |elev - Pitch|>>25 - yawTerm, where yawTerm =
			// |bearing - Yaw|>>24 folded +64 past 64 [orig: the two fpatan blocks
			// @0x50e94a..0x50e9e2; the x(-2^31/pi) scale dbl_7C57B8 folds to plain
			// BAM deltas]. Can go negative behind the viewer.
			// Squared in double like retail's fild/fmul — the int64 product can
			// overflow at map-corner separations.
			const double planar = std::sqrt(double(dx) * double(dx) +
			                                double(dy) * double(dy));
			const int32_t bearing_bam = static_cast<int32_t>(
					std::llround(std::atan2(double(dy), double(dx)) * 683565275.5764316));
			const int32_t elev_bam = static_cast<int32_t>(std::llround(
					std::atan2(double(int64_t(e.z) - az), planar) * 683565275.5764316));
			// abs(0x80000000) stays NEGATIVE (the cdq/xor/sub form), so a bearing
			// EXACTLY astern yields yaw_term -128 and angle 384 — a witnessed
			// retail singularity, kept as-is [orig: the abs32 idiom @0x50e9e2].
			// Unsigned arithmetic preserves the original mod-2^32 BAM wrap (both
			// operands span the full circle; a signed difference is UB here).
			int32_t yaw_term = prio_iabs32(static_cast<int32_t>(
					uint32_t(bearing_bam) - uint32_t(view_yaw))) >> 24;
			if (yaw_term > 64) yaw_term += 64;
			const int32_t pitch_term = prio_iabs32(static_cast<int32_t>(
					uint32_t(elev_bam) - uint32_t(view_pitch))) >> 25;
			const int32_t angle = 256 - pitch_term - yaw_term;

			// Enemy bonus [orig: team byte +0x162... +354 compare @0x50ea7a/@0x50ef45].
			const bool enemy = e.team != 0 && e.team != self_team;
			// LOS: gated on facing (+angle > 128) AND inside the view distance; the ray is
			// the ported terrain+sector raycast standing in for
			// Entity_CheckLineOfSightTerrainAndEntities @0x53b130 (interior witness = a
			// tracked follow-up; the AI ray's terrain+collision legs match its role)
			// [orig: gate @0x50eac5, call @0x50eadb].
			bool los = false;
			if (angle > 128 && g_view_distance_units > 0 &&
					distance_tiles < g_view_distance_units && w.ai != nullptr &&
					self != nullptr) {
				// The 0.9 u lift both ends keeps this ray off the ground plane
				// (@0x53b130's own endpoint recipe is the tracked follow-up above)
				// — explicit at the call site since line_of_sight_clear takes
				// exact endpoints (2026-08-13).
				constexpr int32_t kSightLift = 0xE666; // 0.9 u
				const int32_t a3[3] = {int32_t(ax), int32_t(ay), int32_t(az) + kSightLift};
				const int32_t b3[3] = {e.x, e.y, e.z + kSightLift};
				// The ray's collision scratch is write-only bookkeeping; the world is
				// otherwise untouched (the AiSystem method itself is const).
				los = w.ai->line_of_sight_clear(const_cast<world::World &>(w), a3, b3,
				                                conn.owned_entity,
				                                world::EntityHandle{e.wire_handle});
			}
			const bool is_carrier = e.wire_handle == carrier_handle;

			int64_t base = 2 * int64_t(angle) + 50 * (enemy ? 1 : 0) +
			               100 * (los ? 1 : 0) + 1000 * (is_carrier ? 1 : 0);
			if (e.pool == 1) {
				base += 200 * (e.occupied ? 1 : 0); // [orig: entity+0x170 @0x50efc9]
			} else {
				base += 50 * (e.entity_class == EntityClass::Player ? 1 : 0);
				// The standing term: unmounted, or mounted on an EWEAP
				// [orig: @0x50eb08..0x50eb15].
				base += 100 * (e.mounted_non_eweap ? 0 : 1);
			}
			if (g_view_distance_units > 0 && distance_tiles < g_view_distance_units)
				base += 200; // [orig: @0x50eb6c/@0x50f00b]
			v += base;
		}

		// Motion-change boosts off the last-SENT caches [orig: reads @0x50e905/@0x50ea77;
		// 3*speedDelta + 2*headingDelta @0x50eb80].
		const std::size_t idx = age_index(e);
		const int32_t cached_heading_bam =
				static_cast<int32_t>(uint32_t(conn.s2c_entity_heading[idx]) << 24);
		// Unsigned difference: the cached byte reconstructs to exactly INT_MIN
		// for heading 0x80, and euler_z spans the circle — signed sub is UB.
		const int32_t heading_delta =
				prio_iabs32(static_cast<int32_t>(
						uint32_t(cached_heading_bam) - uint32_t(e.euler_z))) >> 24;
		const int32_t speed_delta =
				std::abs(int(conn.s2c_entity_speed[idx]) - int(e.tick_speed_q6));
		v += 3 * int64_t(speed_delta) + 2 * int64_t(heading_delta);

		if ((e.state_flags & 0x01) != 0) v >>= 4; // [orig: @0x50eb83]
		// The tracked-handle floor lands AFTER the bit0 damp, before the age
		// key composition [orig: `if (priorPriority > v48) v48 = priorPriority`
		// @0x50eb8c].
		if (prior_priority > v) v = prior_priority;
		if (v < 0) v = 0; // CPairList keys are unsigned; a behind-the-viewer negative floors
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

	// 4. Budget walk (soft cap, header included) + age reset and last-sent cache
	// stamps on selection [orig: @0x50f168 age; @0x50f17c heading (Yaw+0x800000)>>24;
	// the @0x50f22x speed-metric tail].
	std::vector<GameEntitySnapshot> selected;
	std::size_t written = header_bytes;
	for (const Scored &s : scored) {
		const std::size_t record_bytes = record_wire_size(*s.snap);
		if (hard_frame_limit != 0 &&
				(written > hard_frame_limit ||
				 record_bytes > hard_frame_limit - written))
			continue;
		selected.push_back(*s.snap);
		const std::size_t idx = age_index(*s.snap);
		conn.s2c_entity_age[idx] = 0;
		conn.s2c_entity_heading[idx] = static_cast<uint8_t>(
				(static_cast<uint32_t>(s.snap->euler_z) + 0x800000u) >> 24);
		conn.s2c_entity_speed[idx] = s.snap->tick_speed_q6;
		written += record_bytes;
		if (written >= std::size_t(g_entity_send_budget)) break; // [orig: @0x50f34b]
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
                                                  std::size_t budget_left,
                                                  std::size_t hard_budget_left) {
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
			conn.owned_entity.valid() ? conn.owned_entity.packed : wire_handle::kInvalid;

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
		if (hard_budget_left != std::numeric_limits<std::size_t>::max() &&
				(written > hard_budget_left || wire > hard_budget_left - written))
			continue;
		out.push_back(rec);
		written += wire;
		if (written >= budget_left) break;
	}

	conn.round_watermark = ring.last_stat(); // [orig: @0x5001ae stamps stat_id]
	return out;
}

// Header wire size for a given flags2, mirroring encode_frame_update: 12-B anchor +
// 2 flag bytes + the phase sub-block (0 weapon 11 B / 1 timer 6 B / 2 env 11 B /
// 3 gametype 0/16 B, selected by the off-wire objective gate) + the 7-B local tail +
// the conditional phase-8 mount record (2 B on foot / 6 B mounted) + the 1-B
// event-loop terminator.
std::size_t frame_header_bytes(uint8_t flags2, uint32_t game_type,
                               const FrameHeaderState &hdr) {
	std::size_t bytes = 0;
	switch (flags2 & 0x03) {
	case 0: bytes = 12 + 2 + 11 + 7 + 1; break;
	case 1: bytes = 12 + 2 + 6 + 7 + 1; break;
	case 2: bytes = 12 + 2 + 11 + 7 + 1; break;
	default: bytes = 12 + 2 + (opennova::game_type::is_objective(game_type) ? 16 : 0) + 7 + 1; break;
	}
	if ((flags2 & kFrameFlags2RouteMask) == kFrameFlags2MountedAmmoRoute)
		bytes += hdr.mount_ammo.mount_handle == wire_handle::kInvalid ? 2u : 6u;
	return bytes;
}

} // namespace

// Drain + read-apply the queued C2S 0x0C player uplinks on one connection's transport. Takes the
// whole Connection (not a bare transport) so it can enforce the per-connection owner gate and
// null-checks the transport internally — symmetric with emit_connection_s2c.
void drain_connection_c2s(world::World &world, Connection &conn) {
	if (conn.transport == nullptr) return;
	Datagram dg;
	while (conn.transport->host_recv(dg)) {
		// §5.10 player-input uplink only this increment (other in-match C2S tags TBD).
		if (dg.tag != c2s::ENTITY_UPLINK) continue;

		// 5-byte sub-header [u16 handle][u16 itemTypeId][u8 sub_op], then the 43-B body.
		std::size_t consumed = 0;
		EntityPacketSubHeader hdr;
		if (!decode_entity_packet_sub_header(dg.body.data(), dg.body.size(), hdr, consumed))
			continue;
		if (hdr.sub_op != ENTITY_SUB_OP_EXTENDED) continue; // compact (type 11) = later

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
		// The receive queue remains live during the countdown, but the player
		// state callback does not apply its remote pose/state until the shared
		// pre-round timer clears.
		// [orig: NetPacket_SerializePlayerState @0x4C2010..0x4C2028]
		if (world.preround_delay_seconds != 0) continue;

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
		intent.analog_x = static_cast<int8_t>(up.analog_x); // entity+0x130.. control axes —
		intent.analog_y = static_cast<int8_t>(up.analog_y); // the vehicle motor reads the
		intent.analog_z = static_cast<int8_t>(up.analog_z); // controller's axes [orig: @0x48b783]
		intent.buttons = 0; // extended uplink carries state/anim bytes, not a buttons word
		apply_player_intent(world, intent);

		// Store the sender's 4 requested-interest pairs for the 0x0A priority
		// build's tracked-handle floor [orig: the 0x0C read-apply stores at
		// playerState+94346/+94356 @0x4c09c0 — owner-gated like the intent].
		conn.tracked_handle = {up.priority_handle_0, up.priority_handle_1,
		                       up.priority_handle_2, up.priority_handle_3};
		conn.tracked_score = {int32_t(up.priority_score_0), int32_t(up.priority_score_1),
		                      int32_t(up.priority_score_2), int32_t(up.priority_score_3)};
	}
}

// Serialize the live world into one S2C 0x0A frame for `conn` and host_send it. anchor_for_connection
// is file-static; the per-connection emit body is shared by the legacy listen-server binding and
// npruntime's Server_TickUpdate fan over connection_list.
std::vector<std::vector<uint8_t>> build_water_cross_messages(
        const world::World &world) {
	// One positioned message per crossing recorded this tick. Which of the two
	// sounds rides the message is the crossing entity's own airborne bit, not
	// its family — see world::WaterCrossEvent::airborne for the witness. The
	// name-to-slot binding itself is capture-derived (npwire's kWaterCross*
	// constants). Coordinates are world UNITS (the wire's i16), so the 16.16
	// fixed positions shift down 16 - the same convention the decode documents.
	// [orig: NetPacket_WriteOverlayAction @0x505d50 via
	//  Server_SendOverlayActionToAlive @0x50a1b0]
	std::vector<std::vector<uint8_t>> out;
	for (const world::WaterCrossEvent &ev : world.water_crossings.events) {
		PlaySoundCommand cmd;
		cmd.flag = 1; // positioned
		cmd.sound_name = ev.airborne ? kWaterCrossAirborneEffect : kWaterCrossWadeEffect;
		cmd.has_pos = true;
		cmd.pos_x = static_cast<int16_t>(ev.x >> 16);
		cmd.pos_y = static_cast<int16_t>(ev.y >> 16);
		cmd.pos_z = static_cast<int16_t>(ev.water_z >> 16);
		out.push_back(encode_play_sound(cmd));
	}
	return out;
}

bool emit_connection_s2c(const world::World &w, Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         uint32_t game_type,
                         std::size_t max_frame_body_bytes) {
	if (conn.transport == nullptr) return false;
	const world::Entity *owned = owned_entity_for_emit(w, conn);
	if (owned == nullptr) return false;
	// A pre-World reply can bind only the bare wire handle. Stamp its live
	// allocation before retaining any per-recipient frame state.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 reads the deployed
	// recipient entity before ++playerSlot+100566 @0x517BE8]
	conn.owned_entity_spawn_id = owned->registry_spawn_id;
	const PlayerReplicationState anchor = anchor_for_owned_entity(*owned);

	// Advance the per-connection 0x0A sub-block phase and select this frame's header sub-block
	// [orig: ++playerSlot+100566 then NetPacket_WritePlayerState writes it as flags2, phase&3 =
	// sub-block]. The original free-runs an 8-bit counter, so phase&3 cycles all four sub-blocks
	// (0 weapon / 1 server-status / 2 env / 3 gametype) evenly and phase&0xF==8 emits the mounted-
	// weapon ammo block every 16th frame. First send is phase 1 (server-status), so the load-bearing
	// fall-damage tolerance reaches the client on frame 1 (matches the original preincrement @0x517be8).
	const uint8_t flags2 = ++conn.s2c_phase;

	// Derive every variable-width header field before the shared packet budget is
	// divided. Retail keeps these environment values in native fixed-point globals
	// and quantizes only while writing the frame.
	FrameHeaderState hs;
	hs.flags1 = conn.respawn_pending ? 0x02 : 0x00;
	hs.preround_delay_seconds =
			static_cast<uint8_t>(w.preround_delay_seconds);
	const world::EnvNetworkState &env = w.network_env;
	hs.env.present = true;
	hs.env.fog_dist = static_cast<uint16_t>(env.fog_target_q16 >> 16);
	const uint32_t fog_accel = std::min<uint32_t>(env.fog_accel_clamp, 0x00FF0000u);
	hs.env.fog_accel = static_cast<uint16_t>((fog_accel + 0xFFu) >> 8);
	hs.env.tod_fixed = static_cast<uint16_t>((env.tod_fixed24 + 0x1000u) >> 13);
	hs.env.quake_ticks = static_cast<uint8_t>(std::min<uint32_t>(env.quake_ticks, 0xFFu));
	hs.env.cloud_scroll = static_cast<uint8_t>(
			std::min<uint32_t>(env.cloud_scroll_rate_target >> 10, 0xFFu));
	hs.env.rain_pct = static_cast<uint8_t>(
			std::min<uint32_t>(env.rain_pct_current_q16 >> 8, 0xFFu));
	hs.env.overcast = static_cast<uint8_t>(
			std::min<uint32_t>(env.overcast_blend_q16 >> 8, 0xFFu));
	hs.env.env_param = static_cast<uint8_t>(env.precipitation_kind);
	hs.mount_ammo.present = (flags2 & kFrameFlags2RouteMask) == kFrameFlags2MountedAmmoRoute;
	if (conn.owned_entity.valid()) {
		if (const world::Entity *own = w.registry.get(conn.owned_entity)) {
			// +360 and +368 are zero unless entity Flags bit1 is set; +364 is
			// unconditional. All three dword stores cross this header as low bytes.
			// [orig: NetPacket_WritePlayerState @0x4ff81b]
			hs.respawn_delay_seconds = (own->flags & 2u) != 0
					? static_cast<uint8_t>(conn.respawn_delay_seconds)
					: uint8_t{0};
			hs.downed_revive_seconds = (own->flags & 2u) != 0
					? static_cast<uint8_t>(conn.downed_revive_seconds)
					: uint8_t{0};
			hs.spawn_target_hold_seconds =
					static_cast<uint8_t>(conn.spawn_target_hold_seconds);
			hs.tail_state_byte = static_cast<uint8_t>(own->net_stance_bits & 0x03u);
			if (own->mounted && own->mount_target.valid()) {
				hs.tail_mount_handle = own->mount_target.packed;
				hs.mount_ammo.mount_handle = own->mount_target.packed;
				hs.mount_ammo.has_mount = true;
				if (const world::Entity *mount = w.registry.get(own->mount_target)) {
					if (const world::WeaponSlotState *slot =
							world::resolve_mounted_ammo_slot(w, *mount)) {
						hs.mount_ammo.clip = static_cast<uint16_t>(slot->clip);
						hs.mount_ammo.reserve = static_cast<uint16_t>(slot->reserve);
					}
				}
			}
			hs.tail_health = static_cast<int16_t>(
					own->health > 32767 ? 32767 : (own->health < 0 ? 0 : own->health));
		}
	}

	// Priority + aging + byte-budget selection of this frame's tag-1 records [orig:
	// Server_BuildEntityPriorityList @ 0x50e590 + the serialize_entity_states_to_packet
	// @ 0x50f070 budget loop]. NOT ported: the original writes NO entity records to the
	// LISTEN HOST's OWN local player (@0x50f07e early return; the priority build is also
	// skipped @0x517c1b) — its local client reads process memory. Our serve-and-play local
	// view RENDERS FROM the loopback 0x0A fold (ADR 0011), so the loopback connection gets
	// the full record set; that frame never leaves the process, so retail interop is
	// unaffected (D-NET-140).
	const std::size_t header_bytes = frame_header_bytes(flags2, game_type, hs);
	const std::size_t hard_event_bytes = max_frame_body_bytes == 0
			? std::numeric_limits<std::size_t>::max()
			: (max_frame_body_bytes > header_bytes
					? max_frame_body_bytes - header_bytes
					: 0);
	// Round events FIRST under the shared frame budget [orig: the @0x50f312 interleave
	// serves tag-2 refs inside the SAME @0x50f070 budget loop as the tag-1 records].
	// The first grouped-order port handed rounds only the leftovers — a real-world
	// entity set (players + ~21 vehicles) fills the 600-B budget alone, so tag-2
	// starved to ZERO on the wire and the sweep still advanced the watermark,
	// discarding every round echo (v29; D-NET-154). Rounds are rare and <= 20 B each;
	// entities absorb the remainder — same cap, and the retail decode loop is
	// tag-driven either way. (D-NET-152/154)
	std::vector<RoundEventRecord> rounds =
			select_round_events(
					w, conn, anchor,
					std::size_t(g_entity_send_budget) - header_bytes,
					hard_event_bytes);
	std::size_t rounds_bytes = 0;
	for (const RoundEventRecord &r : rounds)
		rounds_bytes += 1 + 17 + ((r.flags & 0x80) ? 1u : 0u) + ((r.flags & 0x40) ? 2u : 0u);
	const std::vector<GameEntitySnapshot> selected =
			select_frame_entities(
					w, conn, ents, anchor, header_bytes + rounds_bytes,
					max_frame_body_bytes);

	conn.transport->host_send(s2c::PER_FRAME_UPDATE,
	                          build_0a_frame(anchor, selected, flags2, hs, game_type, w.subgoals,
	                                         std::move(rounds)),
	                          /*reliable=*/false);
	return true;
}

} // namespace opennova::netsim
