#pragma once

#include "netsim/client_state.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <world/entity_registry.h>

namespace opennova::world {
class World;
}

namespace opennova::netsim {

struct ClientWorldSyncResult {
	std::vector<world::EntityLifetime> spawned;
	std::vector<world::EntityLifetime> updated;
	std::vector<world::EntityLifetime> retired;

	bool changed() const {
		return !spawned.empty() || !updated.empty() || !retired.empty();
	}
};

// Materialize the decoded mission pools a retail client normally allocates
// while consuming S2C 0x10/0x0D/0x20. This deliberately excludes pool 0:
// NovaSimulation owns a separate local-player entity and remote organics stay
// in ClientState/presentation.
class ClientWorldMaterializer {
public:
	ClientWorldSyncResult sync(const ClientState &state, world::World &world);
	world::Entity *owned(world::World &world, world::EntityHandle handle) const;
	const world::Entity *owned(
			const world::World &world, world::EntityHandle handle) const;
	void clear() { materialized_rows_.clear(); }

private:
	struct MaterializedRow {
		uint16_t type_id = 0;
		uint32_t spawn_revision = 0;
		uint64_t registry_spawn_id = 0;
		// A retained 0x0D mountHandles entry initializes each model-defined
		// retail slot once per native lifetime. Live mount updates own it after
		// that point; unset bits allow definitions that arrive later to catch up.
		uint16_t projected_mount_slots = 0;
	};
	std::unordered_map<uint16_t, MaterializedRow> materialized_rows_;
};

} // namespace opennova::netsim
