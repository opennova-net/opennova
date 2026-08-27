// The per-tick body accumulators the replica pipeline runs over every decoded
// Player/Infantry row: lean decay, the remote arms-dip integrator, and the
// recoil chase. Split from client_replica_pipeline.cpp (size ratchet — the
// infantry_ladder.cpp precedent).
#include <net/netsim/client_replica_pipeline.h>

#include <runtime/world/entity.h> // kMoveOrderLean* (the wire move_input bits)
#include <base/io/bam.h>

namespace opennova::netsim {

namespace {
// [orig: PRNG_Next16 @0x6130a0, dword_31BFBB0] The low bit selects the
// recoil-yaw sign; preserve the complete state because decoded rows share one
// stream rather than owning one generator each. Other process-global retail
// consumers remain outside this view's bounded call-history seam.
inline int32_t prng_next16(uint32_t &state) {
	const uint32_t rol11 = (state << 11) | (state >> 21);
	uint32_t next = state + rol11;
	next = ((next << 4) | (next >> 28)) ^ 1u;
	state = next;
	return static_cast<int32_t>(next);
}
} // namespace

void ClientReplicaPipeline::tick_lean() {
	for (ClientEntityState &es : state_.entities) {
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry)
			continue;
		es.lean_angle = io::bam_sub(es.lean_angle,
				io::bam_sar(io::bam_add(es.lean_angle, 8), 4));
		if ((es.move_input & world::Entity::kMoveOrderLeanLeft) != 0)
			es.lean_angle = io::bam_sub(es.lean_angle, 0x3000000);
		if ((es.move_input & world::Entity::kMoveOrderLeanRight) != 0)
			es.lean_angle = io::bam_add(es.lean_angle, 0x3000000);
	}
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
	}
}

void ClientReplicaPipeline::tick_recoil() {
	for (ClientEntityState &es : state_.entities) {
		if (es.cls != EntityClass::Player && es.cls != EntityClass::Infantry)
			continue;
		const int32_t random16 = prng_next16(prng16_); // unconditional [orig: body updater]
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

} // namespace opennova::netsim
