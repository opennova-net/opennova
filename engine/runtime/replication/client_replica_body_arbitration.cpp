#include "client_replica_body_arbitration.h"

#include <runtime/world/infantry.h>

namespace opennova::netsim {

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
		// A dead record on a live row PARKS the byte (retail's +0x2C0 store,
		// consumed by the death dispatch) and leaves the FSM pair untouched
		// [orig: @0x4c10f1 / @0x4c0509]; the raw anim_state_id plus the
		// frozen-row presentation fallback carry the parked byte's visible
		// outcome. A dead record on an already-dead row commits directly
		// [orig: the entity-dead tests @0x4c1109 / @0x4c04f9 -> @0x4c0635].
		if (row_was_dead) direct_commit();
		return;
	}
	// The respawn edge and a row's first-ever record take the spawn-leg
	// direct commit [orig: @0x4c110f..0x4c1151 / @0x4c05b8..0x4c063b].
	if (respawned_this_record || es.net_anim_current < 0) {
		direct_commit();
		return;
	}
	if (static_cast<int16_t>(decoded) == es.net_anim_current) {
		// Same as current: a PURE no-op — an armed pending SURVIVES
		// [orig: @0x4c115f / @0x4c0606].
		return;
	}
	if (world::remote_body_state_defers(
			world::infantry_anim_flags(static_cast<int>(es.net_anim_current)),
			world::infantry_anim_flags(static_cast<int>(decoded)))) {
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

} // namespace opennova::netsim
