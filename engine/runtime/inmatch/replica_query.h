// Read-only queries over the decoded replica (replication::ClientState) that
// the joiner role, its presentation and the shell's weapon leg share.
#pragma once

#include <runtime/mission/promote.h> // ItemSeatSpec (the binding-fed per-type seat table)
#include <runtime/replication/client_state.h>
#include <runtime/mission/seat_spec_extract.h> // item_seat_spec_for_type (the installed-table probe)
#include <runtime/world/entity.h>

#include <cstdint>
#include <vector>

namespace opennova::inmatch {

// The decoded ClientState row for a wire handle, or null. Linear: ClientState
// keys presentation identity by handle and stays small (players + streamed
// movers).
inline const replication::ClientEntityState *client_entity_for_handle(
		const replication::ClientState &state, uint16_t handle) {
	for (const replication::ClientEntityState &entity : state.entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

// The mounted shooter's own vehicle joins the projectile trace exclusion exactly like
// retail's mount rule (Controller/Gunner/Driver seats only — passengers keep clipping
// their ride) — resolved from the wire shooter row's carrier + the binding-fed seat table
// instead of live mount pointers. Returns 0xFFFF when unmounted, passenger-seated, or
// the rows aren't streamed yet. Used for BOTH decoded remote rounds and the joiner's
// own predicted rounds: on a joiner the local ignored-mount leg is dead (wire_projected
// skips the local dynamics table), so this carrier gate is the only surviving exclusion.
// [orig: the ignored-mount select feeding Physics_RaycastAgainstBoneCollision @ 0x4e4cb0
//  via ray[18]]
inline uint16_t wire_carrier_exclusion_for(
		const replication::ClientState &state, uint16_t shooter_handle,
		const std::vector<mission::ItemSeatSpec> &seat_specs) {
	const replication::ClientEntityState *row =
			client_entity_for_handle(state, shooter_handle);
	if (row == nullptr || row->carrier_handle == world::EntityHandle::kInvalid ||
			row->mount_bone == 0)
		return 0xFFFFu;
	const replication::ClientEntityState *carrier =
			client_entity_for_handle(state, row->carrier_handle);
	const mission::ItemSeatSpec *spec = carrier != nullptr
			? mission::item_seat_spec_for_type(seat_specs, carrier->type_id)
			: nullptr;
	if (spec == nullptr) return 0xFFFFu;
	for (const world::Seat &seat : spec->seats) {
		if (seat.bone_index != row->mount_bone) continue;
		if (seat.type == world::SeatType::Controller ||
				seat.type == world::SeatType::Gunner ||
				seat.type == world::SeatType::Driver)
			return row->carrier_handle;
		break;
	}
	return 0xFFFFu;
}

} // namespace opennova::inmatch
