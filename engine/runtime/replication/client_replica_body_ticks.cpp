// The per-tick body accumulators the replica pipeline runs over every decoded
// Player/Infantry row: lean decay, the remote arms-dip integrator, and the
// recoil chase. Split from client_replica_pipeline.cpp (size ratchet — the
// infantry_ladder.cpp precedent).
#include <runtime/replication/client_replica_pipeline.h>

#include <runtime/world/entity.h> // kMoveOrderLean* (the wire move_input bits), SeatType
#include <net/npwire/wire_handle.h>
#include <runtime/world/infantry.h> // anim_state (the prone idle and roll states)
#include <base/io/bam.h>
#include <base/io/rotating_prng.h>

namespace opennova::replication {

void ClientReplicaPipeline::tick_lean_decay() {
	for (ClientEntityState &es : state_.entities) {
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry)
			continue;
		es.lean_angle = io::bam_sub(es.lean_angle,
				io::bam_sar(io::bam_add(es.lean_angle, 8), 4));
	}
}

void ClientReplicaPipeline::tick_lean_ramp() {
	for (ClientEntityState &es : state_.entities)
		if (!es.rm_parked) lean_ramp_row(es);
}

void ClientReplicaPipeline::lean_ramp_row(ClientEntityState &es) {
	// The on-foot ramp is the player body pass's (the NPC pass ramps none).
	if (es.cls != EntityClass::Player) return;
	// A seated body's pass ends at the mounted site, ahead of the on-foot
	// ramp: its passenger keys ramp there, ahead of its eye
	// (tick_seated_body) [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 -
	// the site selector @0x4B637C..0x4B638F, the passenger ramp
	// @0x4B6684..0x4B66C3, the eye @0x4B66D9..0x4B68FC].
	if (es.carrier_handle != wire_handle::kInvalid && es.mount_bone != 0) return;
	// Its gates: a ladder contact or a chute (Flags 0x100020; a remote body's
	// chute bit is the record's, whose low byte bar the dead bit lands on the
	// row's Flags mirror), the prone latch with none of Flags 0x10A000 (a remote body's
	// latch is the record's stance store, net_stance_bits bit 0), and the dead
	// bit, which skips both keys [orig: Entity_UpdateInfantryPlayerBody
	// @0x4B40E0 - `test eax, 100020h` @0x4B7DA2, the prone_local test
	// @0x4B7DA9..0x4B7DAD (its derivation @0x4B416C..0x4B4183), the dead test
	// @0x4B7DAF..0x4B7DB4; NetPacket_SerializePlayerState @0x4C09C0 - the
	// remote Flags mask 0xFD @0x4C1307..0x4C1311, the stance stores
	// @0x4C11FD..0x4C1242].
	if ((es.rm_entity_flags & 0x100020u) != 0u) return;
	if ((es.net_stance_bits & 1u) != 0u && (es.rm_entity_flags & 0x10A000u) == 0u)
		return;
	if ((es.rm_entity_flags & world::kEntityFlagDead) != 0u) return;
	if ((es.move_input & world::Entity::kMoveOrderLeanLeft) != 0)
		es.lean_angle = io::bam_sub(es.lean_angle, 0x3000000);
	if ((es.move_input & world::Entity::kMoveOrderLeanRight) != 0)
		es.lean_angle = io::bam_add(es.lean_angle, 0x3000000);
}

// The remote arms-dip integrator: the exact block AiSystem::infantry_weapon_channel
// runs for authoritative bodies, applied here to wire-decoded peers. The window byte
// decrements in BOTH branches -- twice per tick -- so an 80 stamp dips for 40 ticks.
// [orig: @0x4b5cab..0x4b5ce7]
void ClientReplicaPipeline::tick_arms_dip() {
	for (ClientEntityState &es : state_.entities) {
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry)
			continue;
		if (es.arms_dip_ticks > 0) {
			--es.arms_dip_ticks;                 // [orig: @0x4b5cb5]
			es.pitch_kick_accum = io::bam_sub(
					es.pitch_kick_accum, 0x2800000); // [orig: @0x4b5cb7 += 0xFD800000]
		}
		es.pitch_kick_accum = io::bam_sub(
				es.pitch_kick_accum,
				io::bam_sar(io::bam_add(es.pitch_kick_accum, 4), 3)); // [orig: @0x4b5cc7..0x4b5cd5]
		if (es.arms_dip_ticks > 0) --es.arms_dip_ticks;         // [orig: @0x4b5cdb..0x4b5ce7]
		// A remote player's torso roll, the authority's player-body rule
		// (world::AiSystem::infantry_torso_roll_tick): prone idle decays it a sixteenth
		// toward level, the prone rolls step it 0x4000000, otherwise it chases the Roll
		// word a sixteenth with the lag clamped to +-20 degrees (D-NET-330).
		// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - prone idle 0x30 @0x4b5d05, the
		//  chase's skip of the rolls @0x4b5d20..0x4b5d28, the chase and clamps @0x4b5d4e /
		//  @0x4b5d61; the rolls @0x4b700e / @0x4b701d]
		if (es.cls != EntityClass::Player) continue;
		if (es.rm_state == world::anim_state::kIdleProne) {
			es.torso_roll = io::bam_sub(es.torso_roll, io::bam_sar(io::bam_add(es.torso_roll, 8), 4));
			continue;
		}
		if (es.rm_state == world::anim_state::kRollLeft) {
			es.torso_roll = io::bam_add(es.torso_roll, -0x4000000);
			continue;
		}
		if (es.rm_state == world::anim_state::kRollRight) {
			es.torso_roll = io::bam_add(es.torso_roll, 0x4000000);
			continue;
		}
		es.torso_roll = io::bam_add(es.torso_roll,
				io::bam_sar(io::bam_add(io::bam_sub(es.roll_bam, es.torso_roll), 8), 4));
		const int32_t lag = io::bam_sub(es.torso_roll, es.roll_bam);
		if (lag > 0x0E38E380) es.torso_roll = io::bam_add(es.roll_bam, 0x0E38E380);
		if (lag < -0x0E38E380) es.torso_roll = io::bam_add(es.roll_bam, -0x0E38E380);
	}
}

void ClientReplicaPipeline::tick_recoil() {
	for (ClientEntityState &es : state_.entities) {
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry)
			continue;
		// [orig: PRNG_Next16 @0x6130a0, dword_31BFBB0] The low bit selects the
		// recoil-yaw sign; prng16_ keeps the complete state because decoded rows
		// share one stream rather than owning one generator each. Other
		// process-global retail consumers remain outside this view's bounded
		// call-history seam.
		const int32_t random16 = io::rotating_prng_next16(prng16_); // unconditional [orig: body updater]
		const int32_t step = io::bam_sar(io::bam_add(es.recoil_pitch, 4), 3);
		const int32_t half = io::bam_sar(step, 1);
		es.recoil_pitch = io::bam_sub(es.recoil_pitch, half);
		if (es.recoil_pitch <= 0x300) es.recoil_pitch = 0;
		es.pitch_bam = io::bam_add(es.pitch_bam, io::bam_sar(step, 3));
		es.heading_bam = (random16 & 1) == 0
				? io::bam_add(es.heading_bam, half)
				: io::bam_sub(es.heading_bam, half);
	}
}

} // namespace opennova::replication
