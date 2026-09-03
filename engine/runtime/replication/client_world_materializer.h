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
class ClientWorldMaterializer {
public:
	ClientWorldSyncResult sync(const ClientState &state, world::World &world);
	// Stamp every materialized pool-1..3 row that still carries no placed
	// identity with one: spawn_origin = (kind, per-kind ordinal) and a
	// nonzero bms_id. Run once the initial world stream's static pools are
	// complete (the pool-0 fence); rows streamed later stay wire-direct like
	// the host's own runtime spawns. Returns how many rows were stamped.
	int assign_placement_origins(world::World &world);
	// Every stamped row, ordered by (kind, index) — the registry rows
	// themselves carry the placed identity (spawn_origin, bms_id) the shell's
	// mission placer keys on, the same batched/placed path the host's own
	// statics take (ADR 0043 slice E10: no record twin of the row).
	std::vector<const world::Entity *> placed_rows(const world::World &world) const;
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
		// none). Deliberately a copy of the registry row's pair (ADR 0043
		// slice E10 kept it): it is the identity's TOMBSTONE — it outlives the
		// row so a vanished stamped slot still retires its id to the shell, and
		// a same-type re-spawn of the slot inherits it even when the previous
		// lifetime is already gone.
		uint32_t spawn_origin = 0xFFFFFFFFu;
		int32_t bms_id = 0;
	};
	std::vector<int32_t> retired_placement_ids_;
	std::unordered_map<uint16_t, MaterializedRow> materialized_rows_;
	// The next spawn_origin index per kind (Marker/Item/Building/Organic).
	int placement_index_next_[4] = {0, 0, 0, 0};
};

} // namespace opennova::replication
