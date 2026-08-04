#include "netsim/client_world_materializer.h"

#include <world/angle.h>
#include <world/world.h>

#include <cmath>
#include <unordered_map>

// Materializes the joiner's streamed pool-1..3 records into the native World
// at their exact packed wire handles (retail's pool<<12|slot ids), mirroring
// the retail client's spawn-from-wire path [orig: NapiNPClientMsg_0x00D
// @ 0x432C40 (mount-occupancy gate @ 0x4330b1, D-NET-53); pool-3 slot keying
// @ 0x425C00, net-re 5.29].

namespace opennova::netsim {
namespace {

world::EntityKind kind_for_pool(int pool) {
	switch (pool) {
	case 1: return world::EntityKind::Item;
	case 2: return world::EntityKind::Building;
	case 3: return world::EntityKind::Marker;
	default: return world::EntityKind::Item;
	}
}

bool retail_mount_handle_resolves(
		const world::World &world, uint16_t packed) {
	if (packed == world::EntityHandle::kInvalid) return false;
	const world::EntityHandle handle{packed};
	if (handle.pool() < 0 || handle.pool() >= world::kEntityPoolCount)
		return false;
	const std::size_t retail_capacity = handle.pool() == 3
			? world::kRetailMarkerPoolCapacity
			: world::kRetailActorPoolCapacity;
	if (static_cast<std::size_t>(handle.slot()) >= retail_capacity)
		return false;
	// Retail validates that the fixed pool row can be computed, not that an
	// entity is currently live there. The configured registry capacity is our
	// equivalent storage-row check; do not call registry.get() here.
	return static_cast<std::size_t>(handle.slot()) <
			world.registry.pool_capacity(handle.pool());
}

world::Entity seed_from(const ClientEntityState &row) {
	world::Entity seed;
	const world::EntityHandle handle{row.handle};
	seed.kind = kind_for_pool(handle.pool());
	seed.item_id = row.type_id;
	seed.net_id = row.net_id;
	seed.name = row.name;
	seed.position = {
			static_cast<float>(row.x) / 65536.0f,
			static_cast<float>(row.y) / 65536.0f,
			static_cast<float>(row.z) / 65536.0f};
	seed.spawn_position = seed.position;
	seed.yaw = static_cast<int16_t>(std::lround(
			world::mission_yaw_deg_from_bam_heading(row.heading_bam)));
	seed.pitch = static_cast<int16_t>(std::lround(
			static_cast<double>(row.pitch_bam) * world::kDegreesPerBam));
	seed.roll = static_cast<int16_t>(std::lround(
			static_cast<double>(row.roll_bam) * world::kDegreesPerBam));
	seed.team = row.team_known ? row.team : 0;
	seed.flags = row.spawn_entity_flags;
	seed.engine_flags = row.spawn_entity_flags;
	seed.section_mask = row.spawn_section_mask;
	seed.ammo_count = static_cast<uint8_t>(row.spawn_ammo_count & 0xFFu);
	seed.ref_num = row.spawn_ref_num;
	seed.sub_type = row.spawn_sub_type;
	seed.zone_number = static_cast<uint8_t>(row.zone_number_rank & 0x1Fu);
	seed.zone_radius = row.zone_radius;
	seed.spawn_origin = world::kSpawnOriginNone;
	return seed;
}

} // namespace

world::Entity *ClientWorldMaterializer::owned(
		world::World &world, world::EntityHandle handle) const {
	const auto tracked = materialized_rows_.find(handle.packed);
	if (tracked == materialized_rows_.end()) return nullptr;
	return world.registry.get(world::EntityLifetime{
			handle, tracked->second.registry_spawn_id});
}

const world::Entity *ClientWorldMaterializer::owned(
		const world::World &world, world::EntityHandle handle) const {
	const auto tracked = materialized_rows_.find(handle.packed);
	if (tracked == materialized_rows_.end()) return nullptr;
	return world.registry.get(world::EntityLifetime{
			handle, tracked->second.registry_spawn_id});
}

ClientWorldSyncResult ClientWorldMaterializer::sync(
		const ClientState &state, world::World &world) {
	ClientWorldSyncResult result;
	std::unordered_map<uint16_t, const ClientEntityState *> current;
	current.reserve(state.entities.size());
	for (const ClientEntityState &row : state.entities) {
		const world::EntityHandle handle{row.handle};
		if (!handle.valid() || handle.pool() < 1 || handle.pool() > 3 ||
				row.type_id == 0 || row.spawn_revision == 0)
			continue;
		current.emplace(row.handle, &row);
	}

	for (auto it = materialized_rows_.begin();
			it != materialized_rows_.end();) {
		const auto found = current.find(it->first);
		if (found != current.end() &&
				found->second->type_id == it->second.type_id) {
			++it;
			continue;
		}
		const world::EntityHandle handle{it->first};
		const world::EntityLifetime lifetime{
				handle, it->second.registry_spawn_id};
		if (lifetime.valid()) {
			world.registry.despawn(lifetime);
			result.retired.push_back(lifetime);
		}
		it = materialized_rows_.erase(it);
	}

	for (const auto &[packed, row] : current) {
		auto tracked = materialized_rows_.find(packed);
		const world::EntityHandle handle{packed};
		if (tracked != materialized_rows_.end()) {
			const world::EntityLifetime lifetime{
					handle, tracked->second.registry_spawn_id};
			world::Entity *owned_row = world.registry.get(lifetime);
			if (owned_row != nullptr) {
				if (tracked->second.spawn_revision == row->spawn_revision)
					continue;
				// Each accepted 0x0D/0x10/0x20 load record starts with a
				// retail memset of the complete slot. A new wire generation is
				// therefore a new native lifetime even when item_type is the same;
				// preserving seats, damage, or motor state would resurrect values
				// retail cleared before applying the record.
				if (world.registry.despawn(lifetime))
					result.retired.push_back(lifetime);
				tracked->second.registry_spawn_id = 0;
				tracked->second.spawn_revision = row->spawn_revision;
			} else {
				const bool fresh_generation =
						tracked->second.spawn_revision != row->spawn_revision;
				// The former allocation vanished or the slot now belongs to someone
				// else. Tombstone the consumed wire generation; handle reuse is not
				// permission to resurrect, mutate, or evict another lifetime.
				if (lifetime.valid()) result.retired.push_back(lifetime);
				tracked->second.registry_spawn_id = 0;
				if (!fresh_generation) continue;
				tracked->second.spawn_revision = row->spawn_revision;
				if (world.registry.get(handle) != nullptr) continue;
			}
		} else if (world.registry.get(handle) != nullptr) {
			// A true wire-header world normally starts with empty mission pools,
			// but a conflicting live owner must remain authoritative for its slot.
			materialized_rows_[packed] = {
					row->type_id, row->spawn_revision, 0};
			continue;
		}

		if (world.registry.spawn_at(handle, seed_from(*row)) != handle) continue;
		const world::Entity *spawned = world.registry.get(handle);
		MaterializedRow &claimed = materialized_rows_[packed];
		claimed = {row->type_id, row->spawn_revision,
				spawned != nullptr ? spawned->registry_spawn_id : 0};
		result.spawned.push_back(world::EntityLifetime{
				handle, claimed.registry_spawn_id});
	}

	// The 0x0D parent is a relationship pointer. Resolve it only after every
	// row in this fold has had a chance to occupy its exact slot.
	for (const auto &[packed, row] : current) {
		world::Entity *child = owned(world, world::EntityHandle{packed});
		if (child == nullptr) continue;
		child->emplacement_parent = world::EntityHandle{};
		child->emplacement_parent_spawn_id = 0;
		child->ground_target = world::EntityHandle{};
		if (row->parent_handle == 0xFFFFu) {
			continue;
		}
		const world::EntityHandle parent_handle{row->parent_handle};
		const world::Entity *parent = parent_handle.pool() >= 1 &&
				parent_handle.pool() <= 3
				? owned(world, parent_handle)
				: world.registry.get(parent_handle);
		if (parent == nullptr) continue;
		child->emplacement_parent = parent->handle;
		child->emplacement_parent_spawn_id = parent->registry_spawn_id;
		// For retail NoNetworkCallback attachment rows this relationship is
		// simultaneously the child's groundEntity (+0x28), which the mounted
		// weapon-slot selector follows. `owned` above makes the assignment
		// generation-safe; a same-handle foreign replacement leaves it clear.
		child->ground_target = parent->handle;
	}

	// Model resolution owns which seats exist and their dense gameplay order.
	// Project the decoded retail mountHandles image once for each definition's
	// fixed retail_slot. A later ItemDef/model-table refresh can expose a slot
	// that was unavailable on the first fold, while live attach/detach changes
	// on an already-projected slot remain authoritative.
	for (const auto &[packed, row] : current) {
		const world::EntityHandle handle{packed};
		if (handle.pool() != 1) continue;
		auto tracked = materialized_rows_.find(packed);
		if (tracked == materialized_rows_.end()) continue;
		world::Entity *carrier = owned(world, handle);
		if (carrier == nullptr) continue;
		for (world::Seat &seat : carrier->seats) {
			if (seat.retail_slot >= row->spawn_mount_handles.size()) continue;
			const uint16_t slot_bit =
					static_cast<uint16_t>(1u << seat.retail_slot);
			if ((tracked->second.projected_mount_slots & slot_bit) != 0)
				continue;
			const uint16_t occupant =
					row->spawn_mount_handles[seat.retail_slot];
			// Slots 0..7 pass retail's structural pool-row resolution before
			// storing. Tail slots 8/9 are copied raw by the distinct handler
			// legs, even when their packed value names pool 5 or an OOB row.
			seat.occupant = seat.retail_slot < 8 &&
					!retail_mount_handle_resolves(world, occupant)
					? world::EntityHandle{}
					: world::EntityHandle{occupant};
			tracked->second.projected_mount_slots |= slot_bit;
		}
	}
	return result;
}

} // namespace opennova::netsim
