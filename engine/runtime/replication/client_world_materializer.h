#pragma once

#include <runtime/replication/client_state.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <runtime/world/entity_registry.h>

namespace opennova::world {
class World;
}

namespace opennova::replication {

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
// Simulation owns a separate local-player entity and remote organics stay
// in ClientState/presentation.
// One streamed pool-1..3 row's placed identity, in the shape the shell's
// mission placer consumes (a BMS entity record): the same batched/placed
// path the host's own statics take.
struct StreamedPlacementRecord {
	int kind = 0;         // world::EntityKind value (Marker 0, Item 1, Building 2)
	int index = 0;        // per-kind ordinal = spawn_origin index
	int32_t bms_id = 0;   // the synthetic placed id (nonzero, unique per handle)
	int item_id = 0;
	float x = 0.0f, y = 0.0f, z = 0.0f; // mission space
	int pitch = 0, yaw = 0, roll = 0;   // integer degrees, as a record stores them
	int team = 0;
	int group = 0;
	uint32_t bms_attributes = 0; // Reflective/NoShadow/Indestructible bits
};

class ClientWorldMaterializer {
public:
	ClientWorldSyncResult sync(const ClientState &state, world::World &world);
	// Stamp every materialized pool-1..3 row that still carries no placed
	// identity with one: spawn_origin = (kind, per-kind ordinal) and a
	// nonzero bms_id. Run once the initial world stream's static pools are
	// complete (the pool-0 fence); rows streamed later stay wire-direct like
	// the host's own runtime spawns. Returns how many rows were stamped.
	int assign_placement_origins(world::World &world);
	// Every stamped row, ordered by (kind, index) — the shell places them
	// through its mission placer.
	std::vector<StreamedPlacementRecord> placement_records(
			const world::World &world) const;
	// Placed identities whose slot was retired or re-typed since the last
	// take: the shell hides their placed representation (a re-typed slot's
	// new occupant is wire-direct). A same-type re-spawn of a stamped slot
	// keeps its identity instead, so its placed node keeps drawing it.
	std::vector<int32_t> take_retired_placement_ids();
	world::Entity *owned(world::World &world, world::EntityHandle handle) const;
	const world::Entity *owned(
			const world::World &world, world::EntityHandle handle) const;
	void clear() {
		materialized_rows_.clear();
		retired_placement_ids_.clear();
		for (int &next : placement_index_next_) next = 0;
	}

private:
	struct MaterializedRow {
		uint16_t type_id = 0;
		uint32_t spawn_revision = 0;
		uint64_t registry_spawn_id = 0;
		// A retained 0x0D mountHandles entry initializes each model-defined
		// retail slot once per native lifetime. Live mount updates own it after
		// that point; unset bits allow definitions that arrive later to catch up.
		uint16_t projected_mount_slots = 0;
		// The placed identity stamped at the fence (kSpawnOriginNone / 0 =
		// none), carried across same-type re-spawns of this slot.
		uint32_t spawn_origin = 0xFFFFFFFFu;
		int32_t bms_id = 0;
	};
	std::vector<int32_t> retired_placement_ids_;
	std::unordered_map<uint16_t, MaterializedRow> materialized_rows_;
	// The next spawn_origin index per kind (Marker/Item/Building/Organic).
	int placement_index_next_[4] = {0, 0, 0, 0};
};

} // namespace opennova::replication
