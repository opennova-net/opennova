#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/wire_handle.h>
#include <runtime/world/entity.h>

#include <algorithm>
#include <cstdint>

namespace opennova::replication {
namespace {
uint16_t resolved_spawn_handle(uint16_t packed) {
    const world::EntityHandle handle{packed};
    if (!handle.valid() || handle.pool() >= world::kEntityPoolCount ||
            static_cast<std::size_t>(handle.slot()) >=
                    world::retail_pool_capacity(handle.pool()))
        return world::EntityHandle::kInvalid;
    return packed;
}
}

// The 0x0F repair request is answered by this full single-slot record, not
// another pool-load page. Retail destroys and clears the slot first, including
// when the replacement has the same type. Empty slots still take that path.
// [orig: NapiNPClientMsg_FullEntitySpawn @0x433780]
void ClientReplicaPipeline::apply_full_entity_spawn(const std::vector<uint8_t> &body) {
    FullEntitySpawnRecord rec;
    if (!decode_full_entity_spawn(body.data(), body.size(), rec)) {
        ++malformed_bodies_;
        return;
    }
    if (resolved_spawn_handle(rec.slot_id) == world::EntityHandle::kInvalid) return;
    const uint32_t generation = begin_entity_lifetime(rec.slot_id);
    erase_entity_tree(rec.slot_id);
    ++state_.world_stream_revision;
    if (rec.item_type == 0) {
        state_.mark_changed();
        return;
    }
    // ItemDef's actual serializer remains authoritative. With no catalog
    // installed the full record, unlike a compact, also names the item family.
    if (rec.item_type == 1)
        learned_classes_[rec.item_type_id] = EntityClass::Vehicle;
    else if (rec.item_type == 3)
        learned_classes_[rec.item_type_id] = (rec.minimap_flags & 0x100u) != 0
                ? EntityClass::Player : EntityClass::Infantry;
    ClientEntityState &row = state_.upsert(rec.slot_id);
    row.type_id = rec.item_type_id;
    row.cls = classify(rec.item_type_id);
    row.name = rec.entity_name;
    row.net_id = rec.net_id;
    row.spawn_tag = s2c::FULL_ENTITY_SPAWN;
    row.spawn_revision = generation;
    row.x = rec.pos_x;
    row.y = rec.pos_y;
    row.z = rec.pos_z;
    row.heading_bam = static_cast<int32_t>(uint32_t{rec.heading_hi} << 16);
    row.pitch_bam = static_cast<int32_t>(uint32_t{rec.pitch_hi} << 16);
    row.yaw_byte = static_cast<uint8_t>(rec.heading_hi >> 8);
    row.pitch_byte = static_cast<uint8_t>(rec.pitch_hi >> 8);
    row.heading_known = true;
    row.net_smooth_heading = row.heading_bam;
    row.net_target_heading_bam = row.heading_bam;
    row.net_smooth_target[0] = row.x;
    row.net_smooth_target[1] = row.y;
    row.net_smooth_target[2] = row.z;
    row.net_smooth_pitch = 0;
    row.team = rec.team;
    row.team_known = true;
    row.spawn_entity_flags = rec.minimap_flags;
    row.rm_entity_flags = rec.minimap_flags;
    row.spawn_ref_num = rec.alert_level;
    row.spawn_sub_type = rec.sub_type;
    row.spawn_owner_connection_id = rec.entity_flags;
    row.spawn_player_class = rec.player_class;
    row.spawn_ai_state = rec.ai_state;
    row.spawn_anim_slot = rec.anim_slot;
    row.spawn_byte_154 = rec.unused_byte;
    row.spawn_mount_mask = rec.seat_mask;
    for (std::size_t i = 0; i < 8; ++i)
        row.spawn_mount_handles[i] = resolved_spawn_handle(rec.mount_handles[i]);
    row.spawn_mount_handles[8] = rec.mount_handle_8;
    row.spawn_mount_handles[9] = rec.mount_handle_9;
    row.parent_handle = resolved_spawn_handle(rec.parent_vehicle_handle);
    row.target_handle = resolved_spawn_handle(rec.ground_entity_handle);
    row.carrier_handle = resolved_spawn_handle(rec.parent_entity_handle);
    row.resolved_ground = row.target_handle;
    state_.mark_changed();
    refresh_carried_entities();
}

// Deferred gameplay notifications belong to the current slot lifetime. A later
// destroy/repair must not apply them to the newly materialized replacement.
void ClientReplicaPipeline::discard_entity_notifications(uint16_t handle) {
	pending_entity_deaths_.erase(std::remove_if(pending_entity_deaths_.begin(),
			pending_entity_deaths_.end(), [handle](const EntityDeathRecord &event) {
				return event.entity_handle == handle;
			}), pending_entity_deaths_.end());
	pending_weapon_reloads_.erase(std::remove_if(pending_weapon_reloads_.begin(),
			pending_weapon_reloads_.end(), [handle](const WeaponReload &event) {
				return event.entity_handle == handle;
			}), pending_weapon_reloads_.end());
}

uint32_t ClientReplicaPipeline::begin_entity_lifetime(uint16_t handle) {
	uint32_t &revision = spawn_revisions_[handle];
	if (const ClientEntityState *prior = state_.find(handle))
		if (prior->spawn_revision != 0) revision = prior->spawn_revision;
	if (++revision == 0) ++revision;
	discard_entity_notifications(handle);
	return revision;
}

void ClientReplicaPipeline::erase_entity_tree(uint16_t root_handle) {
	discard_entity_notifications(root_handle);
	// Retail destroys ONE row and DETACHES its dependents: Entity_Destroy
	// walks the occupant + mount handles through the vehicle detach and then
	// memsets only the target entity — a child attached to the removed row
	// survives with its parent link cleared until its own remove arrives.
	// [orig: Entity_Destroy @0x43e810 — occupant detach @0x43e9e9, per-mount
	//  detach loop @0x43ea38..0x43ea59, memset(entity, 0, 0x2B4) @0x43ea70]
	bool detached = false;
	for (ClientEntityState &entity : state_.entities) {
		if (entity.parent_handle != root_handle) continue;
		entity.parent_handle = wire_handle::kInvalid;
		entity.parent_pose_valid = false;
		detached = true;
	}
	const std::size_t before = state_.entities.size();
	state_.entities.erase(
			std::remove_if(state_.entities.begin(), state_.entities.end(),
					[&](const ClientEntityState &entity) {
						return entity.handle == root_handle;
					}),
			state_.entities.end());
	if (detached || state_.entities.size() != before)
		state_.mark_topology_changed();
}

// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 — the body carries RAW pool-0
//  indices, resolved with Pool_GetEntryUnchecked(0, idx), so the wire handle is
//  (0 << 12) | idx]
void ClientReplicaPipeline::destroy_pool0_slot(uint16_t pool0_index) {
	if (wire_handle::pool(pool0_index) != wire_handle::kPoolOrganic) return; // not a pool-0 slot index
	if (state_.find(pool0_index) == nullptr) return;
	erase_entity_tree(pool0_index);
}

// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the non-authority entity team store @0x4319ee]
void ClientReplicaPipeline::apply_team_assign(uint16_t handle, uint8_t team) {
	// Retail's gates: not the 0xFFFF sentinel, and the pool nibble must address one
	// of the five entity pools (@0x431910 header checks).
	const world::EntityHandle h{handle};
	if (!h.valid() || h.pool() >= world::kEntityPoolCount) return;
	ClientEntityState &entity = state_.upsert(handle);
	if (entity.team == team && entity.team_known) return;
	entity.team = team;
	entity.team_known = true;
	state_.mark_changed();
}

} // namespace opennova::replication
