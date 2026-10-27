// A joiner's world is header-only (every join opens the host's S2C 0x0B header alone,
// D-NET-194): a decoded pool 1..3 row's native twin is the row the joiner's materializer
// spawned for it, the one lifetime its per-row reads take (replica_row_entity). A rig that
// wants a twin with fields of its own decodes the row as a load record would (its type,
// class, position and a spawn revision), lets the materializer's fold spawn the twin at the
// pool's first free slot, and lays its seed over that lifetime. A seed's carrier
// (emplacement_parent) rides the decoded row's target, so a later fold links it again; its
// seats' occupants ride the row's mountHandles image.
// Header-only test infrastructure.
#pragma once

#include <runtime/replication/client_state.h>
#include <runtime/replication/client_world_materializer.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace test_common {

// The twin at one exact handle (the null when the slot is taken or the fold spawns none).
inline opennova::world::EntityHandle materialize_twin_at(
		opennova::replication::ClientWorldMaterializer &materializer,
		opennova::replication::ClientState &state, opennova::world::World &w,
		opennova::world::EntityHandle handle, const opennova::world::Entity &seed,
		opennova::EntityClass cls) {
	namespace ow = opennova::world;
	if (!handle.valid() || w.registry.get(handle) != nullptr) return {};
	opennova::replication::ClientEntityState &row = state.upsert(handle.packed);
	row.type_id = static_cast<uint16_t>(seed.item_id);
	row.cls = cls;
	const auto q = seed.position.q16();
	row.x = q.x;
	row.y = q.y;
	row.z = q.z;
	if (seed.emplacement_parent.valid()) row.target_handle = seed.emplacement_parent.packed;
	if (row.spawn_revision == 0) row.spawn_revision = 1;
	materializer.sync(state, w);
	ow::Entity *twin = materializer.owned(w, handle);
	if (twin == nullptr) return {};
	const uint64_t lifetime = twin->registry_spawn_id;
	*twin = seed;
	twin->handle = handle;
	twin->registry_spawn_id = lifetime;
	twin->facial_slot = 0;
	twin->facial_checked = false;
	// The record's mountHandles image carries the seed's occupants, and a second fold
	// projects it onto the seed's seats once, as the first fold after a model's seats exist
	// does; later attaches own the seats.
	for (const ow::Seat &seat : seed.seats)
		if (seat.retail_slot < row.spawn_mount_handles.size())
			row.spawn_mount_handles[seat.retail_slot] =
					seat.occupant.valid() ? seat.occupant.packed : uint16_t{0xFFFF};
	materializer.sync(state, w);
	twin->emplacement_parent = seed.emplacement_parent;
	twin->emplacement_parent_spawn_id = seed.emplacement_parent_spawn_id;
	return handle;
}

// The twin at the pool's first slot with neither a native row nor a decoded one.
inline opennova::world::EntityHandle materialize_twin(
		opennova::replication::ClientWorldMaterializer &materializer,
		opennova::replication::ClientState &state, opennova::world::World &w, int pool,
		const opennova::world::Entity &seed, opennova::EntityClass cls) {
	namespace ow = opennova::world;
	for (size_t s = 0; s < w.registry.pool_capacity(pool); ++s) {
		const ow::EntityHandle slot = ow::EntityHandle::make(pool, static_cast<int>(s));
		if (w.registry.get(slot) == nullptr && state.find(slot.packed) == nullptr)
			return materialize_twin_at(materializer, state, w, slot, seed, cls);
	}
	return {};
}

} // namespace test_common
