#include "client_replica_body_arbitration.h"

#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/anim/remote_body_state.h>

namespace opennova::replication {

void apply_record_body_arbitration(ClientEntityState &es, uint8_t decoded,
		uint8_t ratio_byte, bool is_player, bool wire_dead, bool row_was_dead,
		bool respawned_this_record) {
	const auto direct_commit = [&es, decoded, ratio_byte, is_player] {
		es.net_anim_current = static_cast<int16_t>(decoded);
		es.net_anim_pending = 0;
		es.net_anim_pending_boundary = -1;
		// The +0x377 one-shot phase seed rides ONLY the direct leg
		// [orig: @0x4c11a6/@0x4c1198].
		if (is_player) {
			es.net_anim_ratio = ratio_byte;
			es.net_anim_ratio_live = true;
		}
	};
	if (wire_dead) {
		// A dead record on a live row PARKS the byte in deathAnimStateId
		// (+0x2C0) for the mover's death edge and leaves the FSM pair untouched
		// [orig: @0x4c10f1 / @0x4c0509]; the fold stores the park. A dead
		// record on an already-dead row stores current and clears pending, and
		// no phase seed: the dead leg jumps past the +0x377 store
		// [orig: @0x4c1015..0x4c1021 -> @0x4c11d7; infantry @0x4c0635]. The
		// row-dead test is the entity's own Flags & 2 [orig: @0x4c100b;
		// infantry @0x4c04f9].
		if (row_was_dead) {
			es.net_anim_current = static_cast<int16_t>(decoded);
			es.net_anim_pending = 0;
			es.net_anim_pending_boundary = -1;
		}
		return;
	}
	// The respawn edge and a row's first-ever record take the spawn-leg
	// direct commit [orig: @0x4c110f..0x4c1151 / @0x4c05b8..0x4c063b].
	if (respawned_this_record || es.net_anim_current < 0) {
		direct_commit();
		return;
	}
	const auto arrival = anim::body_arrival(es.net_anim_current, decoded,
			world::infantry_anim_flags(es.net_anim_current),
			world::infantry_anim_flags(decoded));
	if (arrival == anim::BodyArrival::keep) return;
	if (arrival == anim::BodyArrival::queue) {
		// The queue classes: the current state plays out; the arrival defers
		// until the channel's completion boundary [orig: @0x4c1169..0x4c1190 /
		// @0x4c060a..0x4c0633]. A REPLACED pending keeps the already-armed
		// boundary — retail's end-notify stays latched on the unchanged
		// current clip across pending retargets.
		if (es.net_anim_pending == 0) es.net_anim_pending_boundary = -1;
		es.net_anim_pending = static_cast<int16_t>(decoded);
		return;
	}
	direct_commit();
}

// The death edge both organic movers run on every machine, the client's remote
// rows included (no authority gate before it): a body whose Health reached zero
// without the dead latch takes deathAnimStateId, or the generic death_pungi
// selection when nothing parked one, or death_drown while afloat; latches Flags
// bit 2, drops bits 0xC0 and the pending state, and consumes the park. The
// scream, the corpse timer, the detach and the drop are the same block's other
// legs. [orig: Entity_UpdateInfantryPlayerBody @0x4b4bf1 gate, @0x4b4c72..
// 0x4b4cdb; Entity_UpdateInfantryAI @0x4b9937 gate, @0x4b9cc9..0x4b9d3e;
// Entity_ComputeAnimSlotIndex(.., 0, 0, 4) @0x4b4c87 / @0x4b9cd6]
int16_t replica_death_edge(ClientEntityState &es) {
	int state = es.net_death_anim != 0
			? es.net_death_anim
			: world::compute_death_anim_state(0, 0, world::death_cause::kGeneric);
	if ((es.rm_entity_flags & world::kEntityFlagDrowning) != 0)
		state = world::anim_state::kDeathDrown; // [orig: @0x4b4c96 / @0x4b9cf9]
	es.rm_entity_flags = (es.rm_entity_flags | world::kEntityFlagDead) &
			~(world::kEntityFlagMounted | world::kEntityFlagAiClimb);
	es.net_death_anim = 0;
	return static_cast<int16_t>(state);
}

} // namespace opennova::replication
