// The live placed-device lifecycle a joiner folds: S2C 0x59 spawns or
// updates the pool-1 row a host-side throwable converted into (satchel /
// claymore / AV mine at rest), and S2C 0x12 retires one packed handle. Both
// ride the reliable channel with the host's own loopback excluded (send mask
// 0x90) [orig: Entity_UpdateSatchelPhysics @0x448bd5 / Entity_UpdateClaymorePhysics
//  @0x447b56 -> S2C 0x59 (32 B) -> NapiNPClientMsg_0x059 @0x4228E0 ->
//  Entity_SpawnOrUpdateFromSlotPacket @0x546770; Server_RemoveEntityAndNotify
//  @0x50A270 -> S2C 0x12 [u16 handle] -> NapiNPClientMsg_0x012 @0x425EE0 ->
//  Entity_Destroy @0x43e810].

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>
#include <npwire/wire_handle.h>
#include <world/entity.h> // retail_pool_capacity (the handle gate)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

void ClientReplicaPipeline::apply_deployed_item(
		const std::vector<uint8_t> &body) {
	DeployedItemSpawn spawn;
	size_t consumed = 0;
	if (!decode_deployed_item_spawn(
			body.data(), body.size(), spawn, consumed) ||
			consumed != body.size()) {
		++unknown_tags_;
		return;
	}

	const world::EntityHandle handle{spawn.slot_handle};
	if (!handle.valid() || handle.pool() != 1 ||
			static_cast<std::size_t>(handle.slot()) >=
					world::retail_pool_capacity(1))
		return;

	uint16_t selected_type = spawn.item_id;
	const ClientEntityState *owner = state_.find(spawn.owner_handle);
	const ClientEntityState *viewer = state_.find(viewer_handle_);
	if (owner != nullptr && viewer != nullptr && owner->team_known &&
			viewer->team_known) {
		const bool enemy = (mp_attributes_ & 0x8000u) != 0 ||
				owner->team != viewer->team;
		const uint16_t variant = enemy
				? spawn.enemy_item_id
				: spawn.friendly_item_id;
		if (variant != 0) selected_type = variant;
	}
	if (selected_type == 0) return;

	ClientEntityState *existing = state_.find(spawn.slot_handle);
	const bool type_changed = existing != nullptr &&
			existing->type_id != selected_type;
	uint32_t next_spawn_revision = 1;
	if (existing != nullptr) {
		next_spawn_revision = existing->spawn_revision;
		if (type_changed || next_spawn_revision == 0) {
			++next_spawn_revision;
			if (next_spawn_revision == 0) next_spawn_revision = 1;
		}
	}

	ClientEntityState &row = state_.upsert(spawn.slot_handle);
	if (type_changed) state_.mark_topology_changed();
	if (existing == nullptr || type_changed) {
		row = ClientEntityState{};
		row.handle = spawn.slot_handle;
		row.type_id = selected_type;
		row.cls = classify(selected_type);
		row.net_id = 0;
		row.spawn_revision = next_spawn_revision;
	} else if (row.spawn_revision == 0) {
		row.spawn_revision = next_spawn_revision;
	}

	row.spawn_tag = s2c::DEPLOYED_ITEM;
	row.x = spawn.pos_x;
	row.y = spawn.pos_y;
	row.z = spawn.pos_z;
	// The three angle words are the high halves of entity+16/+20/+24 in that
	// order — the eulerZ/eulerX/eulerY triple every spawn record carries
	// (yaw heading, pitch, roll), NOT an x/y/z-named pitch/roll/yaw pack
	// [orig: Entity_SpawnOrUpdateFromSlotPacket @0x5468cb..0x5468df stores
	//  packet[12..14] << 16 into +16/+20/+24; the emitters read the entity's
	//  +18/+22/+26 words (HIWORD of Yaw/Pitch/Roll) into record words
	//  12/13/14, Entity_UpdateSatchelPhysics @0x448aeb..0x448b09 and
	//  Entity_UpdateClaymorePhysics @0x447a6c..0x447a8a].
	row.heading_bam = static_cast<int32_t>(
			static_cast<uint32_t>(spawn.angle_x) << 16);
	row.pitch_bam = static_cast<int32_t>(
			static_cast<uint32_t>(spawn.angle_y) << 16);
	row.roll_bam = static_cast<int32_t>(
			static_cast<uint32_t>(spawn.angle_z) << 16);
	row.net_smooth_heading = row.heading_bam;
	row.net_target_heading_bam = row.heading_bam;
	// The compact coarse heading the present rebuilds: the top 8 BAM bits.
	row.yaw_byte = static_cast<uint8_t>(
			static_cast<uint32_t>(row.heading_bam) >> 24);
	row.heading_known = true;
	// 0x59's parent is the structural support/groundEntity used when the
	// placed row follows a carrier. It is distinct from 0x0D's occupant
	// back-reference, which lives in parent_handle.
	row.parent_handle = wire_handle::kInvalid;
	row.target_handle = spawn.parent_handle;
	row.parent_pose_valid = false;
	if (owner != nullptr && owner->team_known) {
		row.team = owner->team;
		row.team_known = true;
	} else {
		row.team = 0xFF;
		row.team_known = false;
	}
	++state_.world_stream_revision;
	state_.mark_changed();
}

// S2C 0x12: one packed handle. Retail ignores 0xFFFF and any pool >= 5, then
// destroys the entity in place; the replica retires the whole attachment tree
// rooted at that handle so a carried child never outlives its parent row.
// [orig: NapiNPClientMsg_0x012 @0x425EE0 (0xFFFF / pool gates
//  @0x425f05..0x425f36) -> Entity_Destroy @0x43e810]
void ClientReplicaPipeline::apply_entity_remove(
		const std::vector<uint8_t> &body) {
	EntityRemove removal;
	size_t consumed = 0;
	if (!decode_entity_remove(body.data(), body.size(), removal, consumed) ||
			consumed != body.size()) {
		++unknown_tags_;
		return;
	}
	const std::size_t before = state_.entities.size();
	erase_entity_tree(removal.entity_handle);
	if (state_.entities.size() != before) {
		++state_.world_stream_revision;
		state_.mark_changed();
	}
}

} // namespace opennova::netsim
