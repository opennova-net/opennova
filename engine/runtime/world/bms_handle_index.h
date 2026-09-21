#pragma once

// Authored BMS id -> registry handle, rebuilt on the registry's spawn serial
// (retail's placement slot carries the entity pointer from registration; this
// lookup is what that identity stands in for).

#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <unordered_map>

namespace opennova::world {

struct BmsHandleIndex {
	std::unordered_map<int, EntityHandle> handles;
	uint64_t serial = 0;
	const World *world = nullptr;

	// The handle carrying `bms_id`, invalid when none does. A despawned row's
	// slot may have been reused, so the occupant must still carry the id.
	EntityHandle resolve(const World &w, int bms_id) {
		if (bms_id <= 0) return EntityHandle{};
		const uint64_t spawn_serial = w.registry.spawn_serial();
		if (world != &w || serial != spawn_serial) {
			handles.clear();
			w.registry.for_each([&](const Entity &e) {
				if (e.bms_id > 0 && handles.find(e.bms_id) == handles.end()) handles[e.bms_id] = e.handle;
			});
			world = &w;
			serial = spawn_serial;
		}
		const auto found = handles.find(bms_id);
		if (found == handles.end()) return EntityHandle{};
		const Entity *e = w.registry.get(found->second);
		return e != nullptr && e->bms_id == bms_id ? found->second : EntityHandle{};
	}
};

} // namespace opennova::world
