// The ClientState lookups and the per-row pulse drain, split out of
// client_replica_pipeline.cpp (the size ratchet); the state's own witness map
// is in client_state.h. The handle lookup is the (pool << 12) | slot resolve
// every S2C entity handler runs before touching a row
// [orig: e.g. NapiNPClientMsg_EntityDeath @0x42eb50 — pool nibble < 5, slot <
//  the pool's capacity, base + slot * stride].
#include "netsim/client_state.h"

namespace opennova::netsim {

// ---- ClientState lookup -----------------------------------------------------

ClientEntityState *ClientState::find(uint16_t handle) {
	// `entities` is intentionally public decoded state. Callers may clear,
	// reorder, append, or edit it directly, so a separate handle-to-index cache
	// cannot remain valid without changing that API. Keep lookup derived from
	// the authoritative vector.
	for (ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

const ClientEntityState *ClientState::find(uint16_t handle) const {
	for (const ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

ClientEntityState &ClientState::upsert(uint16_t handle) {
	if (ClientEntityState *e = find(handle)) return *e;
	ClientEntityState e;
	e.handle = handle;
	entities.push_back(e);
	mark_topology_changed();
	return entities.back();
}

void ClientState::clear_anim_pulses() {
	for (ClientEntityState &e : entities) e.anim_state_pulse = -1;
}

} // namespace opennova::netsim
