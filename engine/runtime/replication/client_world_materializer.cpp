#include <runtime/replication/client_world_materializer.h>

#include <formats/mission/authoring.h> // entity_kind_for_item_type: items.def type -> BMS list
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

// Materializes the joiner's streamed pool-1..3 records into the native World
// at their exact packed wire handles (retail's pool<<12|slot ids), mirroring
// the retail client's spawn-from-wire path [orig: NapiNPClientMsg_0x00D
// @ 0x432C40 (mount-occupancy gate @ 0x4330b1, D-NET-53); pool-3 slot keying
// @ 0x425C00, net-re 5.29].

namespace opennova::replication {
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
	if (static_cast<std::size_t>(handle.slot()) >=
			world::retail_pool_capacity(handle.pool()))
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
	seed.display_name = row.display_name; // entity+0xF4
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
	// The streamed dword is the host's one Flags word, which the port splits;
	// each bit lands in the half whose clears own it, as the host homes them:
	// the movers' runtime bits (airborne, afloat, the MoveOrder lights/state
	// bits, the suspension crash) in `flags`, where the client movers clear
	// them, and the matrix bit in engine_flags, where its clears land. Every
	// other bit rides both. [orig: NapiNPClientMsg_0x00D @0x432D7D (the one
	// Flags store); the movers' own Flags writes, e.g. the airborne set/clear
	// @0x483B98 / @0x483D55 and Entity_UpdateVehiclePhysics `or [esi+24h],20h`
	// @0x48BACA]
	constexpr uint32_t kMoverRuntimeBits = world::kEntityFlagInAir | world::kEntityFlagDrowning |
			0x80u | 0x20u | 0x10u | 0x08u;
	seed.flags = row.spawn_entity_flags & ~world::kEntityFlagMatrixBuilt;
	seed.engine_flags = row.spawn_entity_flags & ~kMoverRuntimeBits;
	seed.section_mask = row.spawn_section_mask;
	seed.ammo_count = static_cast<uint8_t>(row.spawn_ammo_count & 0xFFu);
	seed.ref_num = row.spawn_ref_num;
	seed.sub_type = row.spawn_sub_type;
	seed.owner_connection_id = row.spawn_owner_connection_id;
	seed.player_class = row.spawn_player_class;
	if (row.cls == EntityClass::Player || row.cls == EntityClass::Infantry) {
		seed.ai_state = row.spawn_ai_state;
		seed.anim_slot = row.spawn_anim_slot;
	}
	seed.zone_number = static_cast<uint8_t>(row.zone_number_rank & 0x1Fu);
	seed.zone_radius = row.zone_radius;
	seed.bound_radius = static_cast<float>(row.spawn_bound_radius_q16) /
			65536.0f;
	seed.spawn_origin = world::kSpawnOriginNone;
	return seed;
}

bool update_live_row_state(
		const ClientEntityState &row, world::Entity &entity) {
	const world::Entity seed = seed_from(row);
	const bool changed = entity.position.x != seed.position.x ||
			entity.position.y != seed.position.y ||
			entity.position.z != seed.position.z || entity.yaw != seed.yaw ||
			entity.pitch != seed.pitch || entity.roll != seed.roll ||
			entity.team != seed.team;
	if (!changed) return false;
	entity.position = seed.position;
	entity.yaw = seed.yaw;
	entity.pitch = seed.pitch;
	entity.roll = seed.roll;
	entity.team = seed.team;
	return true;
}

// The drop's carrier words: a native carrier's own (a materialized pool-1..3
// carrier, or the client's own player); a destroyed person carrier's last
// pose, which the destroy left on the flag's row; else the carrier's decoded
// row (a remote person lives in the replica only).
bool drop_carrier_pose_for(const ClientState &state, const world::World &world,
		const world::Entity *native, uint16_t carrier, const ClientEntityState &flag_row,
		world::DropCarrierPose &out) {
	if (native != nullptr) {
		out = world::drop_carrier_pose(world, *native);
		return true;
	}
	out = world::DropCarrierPose{};
	int32_t x = 0, y = 0, z = 0;
	if (flag_row.objective_drop_serial != 0 &&
			flag_row.objective_drop_serial == flag_row.objective_state_serial) {
		x = flag_row.objective_drop_x;
		y = flag_row.objective_drop_y;
		z = flag_row.objective_drop_z;
		out.heading_bam = flag_row.objective_drop_heading_bam;
		out.pitch_bam = flag_row.objective_drop_pitch_bam;
	} else if (const ClientEntityState *row = state.find(carrier)) {
		x = row->x;
		y = row->y;
		z = row->z;
		out.heading_bam = row->heading_bam;
		out.pitch_bam = row->pitch_bam;
	} else {
		return false;
	}
	out.position = {static_cast<float>(x) / 65536.0f, static_cast<float>(y) / 65536.0f,
			static_cast<float>(z) / 65536.0f};
	return true;
}

} // namespace

// The retail client renders the pools it built from the 0x10/0x0D/0x20 load
// batches through the same sector collectors the host uses [orig:
// Terrain_CollectVisibleEntitiesForTerrain @0x5c8c60; Terrain_RenderSectorModels
// @0x5c5d30] — there is no per-role render path. The placed identity stamped
// here is the shell's key into its one placed/batched presenter, so a joiner's
// statics draw exactly as the host's do. Kind follows the row's item def the
// way the host's own placement does: the BMS list a record of that items.def
// type sits in (authoring::entity_kind_for_item_type — building and
// decoration defs are Buildings, every other pool-1/2 def an Item), pool 3
// rows are markers. It never follows the streamed Flags bit 0x20000: retail
// sets that bit every tick a mover rebuilds the entity matrix, so a live
// vehicle streams it [orig: Entity_InitFromModel @0x40e0d4..0x40e105 (types
// 2/5/6 at init); Entity_UpdateVehiclePhysics @0x48D451 and every mover tail].
// A row whose def did not resolve keeps its pool's kind. The order (pool 2,
// pool 1, pool 3, slot ascending) is the witnessed initial-state order.
int ClientWorldMaterializer::assign_placement_origins(world::World &world) {
	std::vector<uint16_t> handles;
	handles.reserve(materialized_rows_.size());
	for (const auto &row : materialized_rows_) handles.push_back(row.first);
	const auto pool_rank = [](uint16_t packed) {
		switch (world::EntityHandle{packed}.pool()) {
		case 2: return 0;
		case 1: return 1;
		default: return 2;
		}
	};
	std::sort(handles.begin(), handles.end(), [&](uint16_t a, uint16_t b) {
		const int ra = pool_rank(a);
		const int rb = pool_rank(b);
		if (ra != rb) return ra < rb;
		return world::EntityHandle{a}.slot() < world::EntityHandle{b}.slot();
	});
	int stamped = 0;
	for (const uint16_t packed : handles) {
		const world::EntityHandle handle{packed};
		if (handle.pool() < 1 || handle.pool() > 3) continue;
		world::Entity *entity = owned(world, handle);
		if (entity == nullptr || entity->spawn_origin != world::kSpawnOriginNone)
			continue;
		world::EntityKind kind = world::EntityKind::Item;
		if (handle.pool() == 3)
			kind = world::EntityKind::Marker;
		else if (!entity->has_item_def)
			kind = kind_for_pool(handle.pool());
		else if (mission::authoring::entity_kind_for_item_type(entity->item_type) ==
				mission::EntityKind::Building)
			kind = world::EntityKind::Building;
		int &next = placement_index_next_[static_cast<int>(kind)];
		entity->spawn_origin = world::spawn_origin_pack(
				static_cast<uint32_t>(kind), static_cast<uint32_t>(next));
		++next;
		// Nonzero and unique per packed handle; a joiner has no file ids to
		// collide with (its pool-0 organics and runtime spawns keep 0).
		entity->bms_id = static_cast<int32_t>(packed) + 1;
		MaterializedRow &tracked = materialized_rows_[packed];
		tracked.spawn_origin = entity->spawn_origin;
		tracked.bms_id = entity->bms_id;
		++stamped;
	}
	return stamped;
}

std::vector<int32_t> ClientWorldMaterializer::take_retired_placement_ids() {
	std::vector<int32_t> out;
	out.swap(retired_placement_ids_);
	return out;
}

std::vector<const world::Entity *> ClientWorldMaterializer::placed_rows(
		const world::World &world) const {
	std::vector<const world::Entity *> out;
	out.reserve(materialized_rows_.size());
	for (const auto &row : materialized_rows_) {
		const world::EntityHandle handle{row.first};
		const world::Entity *entity = owned(world, handle);
		if (entity == nullptr || entity->spawn_origin == world::kSpawnOriginNone)
			continue;
		out.push_back(entity);
	}
	std::sort(out.begin(), out.end(),
			[](const world::Entity *a, const world::Entity *b) {
				const int ka = world::spawn_origin_kind(a->spawn_origin);
				const int kb = world::spawn_origin_kind(b->spawn_origin);
				if (ka != kb) return ka < kb;
				return world::spawn_origin_index(a->spawn_origin) <
						world::spawn_origin_index(b->spawn_origin);
			});
	return out;
}

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

// [orig: Entity_LandmineThink @ 0x441A40] Pool 0 lives in decoded state on a
// joiner; omit its predicted local body, which the native registry supplies.
void ClientWorldMaterializer::fill_minefield_actors(const ClientState &state,
        uint16_t self_handle, std::vector<world::MinefieldActor> &out) {
    out.clear();
    for (const ClientEntityState &row : state.entities) {
        if ((row.handle >> 12) != 0 || row.handle == self_handle) continue;
        const uint32_t flags = row.state_flags_known ? row.state_flags : row.spawn_entity_flags;
        out.push_back({row.handle, row.type_id, flags,
                static_cast<uint32_t>(row.move_input) |
                    (static_cast<uint32_t>(row.net_stance_bits) << 8),
                {row.x, row.y, row.z}});
    }
}

// A flag row's carry state, taken from a new S2C 0x2F state (or a fresh
// native lifetime's load record) only: its flags byte and position, its
// ground entity, then the occupant legs. A flag that loses its occupant is
// dropped off it by the drop's own legs over the occupant's words, which
// install the local fall; between states that fall and the ride after it
// own the flag's pose. The flag's parent field is occupantEntity, not the
// static-load emplacement metadata: the carrier's mountedChild inverse stays
// in lockstep.
// [orig: NapiNPClientMsg_0x02F @0x430E10 — the flags/position stores
//  @0x430F5F..0x430F74, groundEntity @0x430FBD, the old occupant's drop
//  @0x43105C / @0x4310DC (Entity_DropCarriedObject), the attach @0x43106F /
//  @0x4310C7 (Entity_AttachCarriedObject), the moved flag's proximity
//  refresh @0x4310EC]
// A carry relationship's entity: a materialized pool-1..3 row; in pool 0
// only the client's own player, through its wire handle (its native row need
// not sit at that slot); any other pool-0 handle is a replica-only person
// and never names a native row.
world::Entity *ClientWorldMaterializer::resolve_carrier(
		world::World &world, uint16_t packed) const {
	if (packed == world::EntityHandle::kInvalid) return nullptr;
	const world::EntityHandle handle{packed};
	if (handle.pool() >= 1 && handle.pool() <= 3) return owned(world, handle);
	if (packed == local_wire_handle_) return world.registry.get(local_player_);
	return nullptr;
}

void ClientWorldMaterializer::apply_objective_state(const ClientState &state,
		world::World &world, uint16_t packed, const ClientEntityState &row,
		world::Entity &child) {
	MaterializedRow &tracked = materialized_rows_[packed];
	const bool fresh = tracked.objective_lifetime != child.registry_spawn_id;
	if (!fresh && tracked.objective_state_serial == row.objective_state_serial)
		return;
	if (fresh) tracked.objective_parent = world::EntityHandle::kInvalid;
	tracked.objective_lifetime = child.registry_spawn_id;
	tracked.objective_state_serial = row.objective_state_serial;
	const auto resolve = [&](uint16_t packed_handle) {
		return resolve_carrier(world, packed_handle);
	};

	const world::Vec3 position{static_cast<float>(row.x) / 65536.0f,
			static_cast<float>(row.y) / 65536.0f,
			static_cast<float>(row.z) / 65536.0f};
	const bool position_changed = child.position.x != position.x ||
			child.position.y != position.y || child.position.z != position.z;
	child.flags = (child.flags & 0xFFFFFF00u) | (row.spawn_entity_flags & 0xFFu);
	child.position = position;
	const world::Entity *ground = resolve(row.target_handle);
	child.ground_target = ground != nullptr ? ground->handle : world::EntityHandle{};

	const uint16_t previous = tracked.objective_parent;
	const uint16_t next = row.parent_handle;
	if (previous != world::EntityHandle::kInvalid && previous != next) {
		world::Entity *old_carrier = resolve(previous);
		if (old_carrier != nullptr && old_carrier->mounted_child == child.handle)
			old_carrier->mounted_child = world::EntityHandle{};
		child.primary_occupant = world::EntityHandle{};
		world::DropCarrierPose pose;
		if (drop_carrier_pose_for(state, world, old_carrier, previous, row, pose))
			world::drop_object_from_carrier(world, child, pose);
	}
	if (next != world::EntityHandle::kInvalid) {
		if (world::Entity *carrier = resolve(next)) {
			child.primary_occupant = carrier->handle;
			carrier->mounted_child = child.handle;
		}
		// The attach hides the flag and puts back the def's own update
		// callback. [orig: Entity_AttachCarriedObject @0x43c14a, @0x43c191]
		child.flags |= world::kEntityFlagCarried;
		child.drop_motion = world::DropMotion::None;
	}
	tracked.objective_parent = next;
	// A moved flag re-runs its proximity/blink query at the new position.
	// [orig: @0x4310EC -> Entity_BuildProximityList]
	if (position_changed && world.collision != nullptr)
		world.collision->refresh_blink(world, child);
}

// The client's 0x0D / 0x10 / 0x18 handlers put a row on its refNum's group
// list when its def is not a person's and its refNum is nonzero: the list the
// 0x12 destroy walks (ClientReplicaPipeline::erase_entity_tree) and the
// vehicle death's EWeap release reads (VehicleSystem::cleanup_destroyed_ref_group).
// The twin carries that membership. Its def resolves after the row lands (the
// item-traits sweep), so the join is taken once per lifetime, on the first
// fold that finds the def; a row whose def never resolves joins nothing, as in
// the 0x12 walk.
// [orig: the def type != 3 and refNum != 0 tests and the DynArray_AddOrFind
//  join in NapiNPClientMsg_0x00D @0x433381..0x4333AD, NapiNPClientMsg_0x010
//  @0x433684..0x4336B0 and NapiNPClientMsg_FullEntitySpawn @0x433E27..0x433E52]
void ClientWorldMaterializer::join_reference_group(uint16_t packed, world::Entity &child) {
	const auto tracked = materialized_rows_.find(packed);
	if (tracked == materialized_rows_.end() || !child.has_item_def ||
			tracked->second.ref_join_lifetime == child.registry_spawn_id)
		return;
	tracked->second.ref_join_lifetime = child.registry_spawn_id;
	child.ref_group_member = child.ref_num != 0 && child.item_type != 3;
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
		if (it->second.bms_id != 0)
			retired_placement_ids_.push_back(it->second.bms_id);
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
				if (tracked->second.spawn_revision == row->spawn_revision) {
					// A flag's live state lands per S2C 0x2F state in the link
					// pass below (apply_objective_state).
					if (row->spawn_tag == s2c::DEPLOYED_ITEM &&
							update_live_row_state(*row, *owned_row))
						result.updated.push_back(lifetime);
					continue;
				}
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
		world::Entity *spawned = world.registry.get(handle);
		MaterializedRow &claimed = materialized_rows_[packed];
		// A same-type re-spawn of a stamped slot (a new wire generation at
		// the same handle) is the same placed object to the shell: carry the
		// identity onto the fresh lifetime so its placed node keeps drawing it.
		const bool keep_identity = claimed.type_id == row->type_id &&
				claimed.bms_id != 0;
		const uint32_t keep_origin = claimed.spawn_origin;
		const int32_t keep_bms = claimed.bms_id;
		claimed = {row->type_id, row->spawn_revision,
				spawned != nullptr ? spawned->registry_spawn_id : 0};
		if (keep_identity && spawned != nullptr) {
			spawned->spawn_origin = keep_origin;
			spawned->bms_id = keep_bms;
			claimed.spawn_origin = keep_origin;
			claimed.bms_id = keep_bms;
		}
		result.spawned.push_back(world::EntityLifetime{
				handle, claimed.registry_spawn_id});
	}

	// The 0x0D parent and target are TWO relationship pointers: parent is the
	// occupant/driver back-ref for an occupied mount (+368), target is the
	// structural carrier the child rides (groundEntity/+40) — an occupied boat
	// gun's target is the DRIVING hull. Resolve both only after every row in
	// this fold has had a chance to occupy its exact slot.
	// [orig: NapiNPClientMsg_0x00D @0x432C40 — parent → occupantEntity store
	//  @0x433289, target → groundEntity resolve @0x4332bc, store @0x4332d7; both
	//  resolved via the pool<<12|slot walk with 0xFFFF / pool<5 / capacity
	//  guards]
	for (const auto &[packed, row] : current) {
		world::Entity *child = owned(world, world::EntityHandle{packed});
		if (child == nullptr) continue;
		join_reference_group(packed, *child);
		if (is_carry_objective(row->type_id)) {
			apply_objective_state(state, world, packed, *row, *child);
			continue;
		}
		child->emplacement_parent = world::EntityHandle{};
		child->emplacement_parent_spawn_id = 0;
		child->ground_target = world::EntityHandle{};
		// Only materialized pool-1..3 lifetimes resolve. A pool-0 handle is a wire
		// identity with no native row here: the joiner's own body L need not sit
		// at its wire slot, and a remote slot equal to L's handle must not name
		// it (a retail host's 0x0D can carry parent 0x0000, its own player).
		const auto resolve = [&](uint16_t packed_handle) -> const world::Entity * {
			const world::EntityHandle handle{packed_handle};
			if (packed_handle == 0xFFFFu || handle.pool() < 1 || handle.pool() > 3)
				return nullptr;
			return owned(world, handle); // generation-safe: a foreign reuse stays clear
		};
		if (const world::Entity *target = resolve(row->target_handle))
			child->ground_target = target->handle;
		// The structural carrier a child rides is its TARGET; a parent names it
		// only when no target is streamed and it is not the pool-0 occupant
		// back-ref. Classes with their own compact motion never take one (the
		// ClientState recompose's rule, D-NET-195).
		const bool compact_class = row->cls == EntityClass::Player ||
				row->cls == EntityClass::Infantry || row->cls == EntityClass::Vehicle ||
				row->cls == EntityClass::Guided;
		if (!compact_class) {
			const world::Entity *carrier = resolve(persistent_carrier_handle(*row));
			if (carrier != nullptr) {
				child->emplacement_parent = carrier->handle;
				child->emplacement_parent_spawn_id = carrier->registry_spawn_id;
			}
		}
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

} // namespace opennova::replication
