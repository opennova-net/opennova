// The live placed-device lifecycle a joiner folds: S2C 0x59 spawns or
// updates the pool-1 row a host-side throwable converted into (satchel /
// claymore / AV mine at rest), and S2C 0x12 retires one packed handle. Both
// ride the reliable channel with the host's own loopback excluded (send mask
// 0x90) [orig: Entity_UpdateSatchelPhysics @0x448bd5 / Entity_UpdateClaymorePhysics
//  @0x447b56 -> S2C 0x59 (32 B) -> NapiNPClientMsg_0x059 @0x4228E0 ->
//  Entity_SpawnOrUpdateFromSlotPacket @0x546770; Server_RemoveEntityAndNotify
//  @0x50A270 -> S2C 0x12 [u16 handle] -> NapiNPClientMsg_0x012 @0x425EE0 ->
//  Entity_Destroy @0x43e810].

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/wire_handle.h>
#include <runtime/world/entity.h> // retail_pool_capacity (the handle gate)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::replication {

void ClientReplicaPipeline::apply_deployed_item(
		const std::vector<uint8_t> &body) {
	DeployedItemSpawn spawn;
	size_t consumed = 0;
	if (!decode_deployed_item_spawn(
			body.data(), body.size(), spawn, consumed) ||
			consumed != body.size()) {
		++malformed_bodies_;
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
	const bool owner_team_known = owner != nullptr && owner->team_known;
	const uint8_t owner_team = owner_team_known ? owner->team : uint8_t{0xFF};
	const bool viewer_team_known = viewer != nullptr && viewer->team_known;
	const uint8_t viewer_team = viewer_team_known ? viewer->team : uint8_t{0xFF};
	ClientEntityState *existing = state_.find(spawn.slot_handle);
	// The team-variant pick runs only on the FRESH-SPAWN leg (retail's
	// found/update path never touches the item id) and only when BOTH
	// authored variant ids are nonzero — a one-sided pair shows every
	// client the base item id.
	// [orig: Entity_SpawnOrUpdateFromSlotPacket @0x5469db (found &&
	//  g_LocalPlayerEntity && packet[2] != 0 && packet[3] != 0),
	//  enemy pick @0x5469fb..0x546a08, base id @0x546a11; the found-path
	//  update @0x546828..0x54697a leaves the type alone]
	if (existing != nullptr) {
		selected_type = existing->type_id;
	} else if (owner_team_known && viewer_team_known &&
			spawn.friendly_item_id != 0 && spawn.enemy_item_id != 0) {
		const bool enemy = (mp_attributes_ & 0x8000u) != 0 ||
				owner_team != viewer_team;
		selected_type = enemy ? spawn.enemy_item_id
				      : spawn.friendly_item_id;
	}
	if (selected_type == 0) return;

	const bool type_changed = existing != nullptr &&
			existing->type_id != selected_type;
	const uint32_t next_spawn_revision = existing == nullptr || type_changed ||
			existing->spawn_revision == 0 ? begin_entity_lifetime(spawn.slot_handle)
			: existing->spawn_revision;

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
	if (owner_team_known) {
		row.team = owner_team;
		row.team_known = true;
	} else {
		row.team = 0xFF;
		row.team_known = false;
	}
	++state_.world_stream_revision;
	state_.mark_changed();
}

// S2C 0x12: one packed handle. Retail ignores 0xFFFF and any pool >= 5, then
// destroys that one entity in place, DETACHING (not destroying) anything
// attached to it — the replica mirrors that: the named row goes, children
// keep their rows with the parent link cleared.
// [orig: NapiNPClientMsg_0x012 @0x425EE0 (0xFFFF / pool gates
//  @0x425f05..0x425f36) -> Entity_Destroy @0x43e810]
void ClientReplicaPipeline::apply_entity_remove(
		const std::vector<uint8_t> &body) {
	EntityRemove removal;
	size_t consumed = 0;
	if (!decode_entity_remove(body.data(), body.size(), removal, consumed) ||
			consumed != body.size()) {
		++malformed_bodies_;
		return;
	}
	const std::size_t before = state_.entities.size();
	erase_entity_tree(removal.entity_handle);
	if (state_.entities.size() != before) {
		++state_.world_stream_revision;
		state_.mark_changed();
	}
}

// S2C 0x2F: retail accepts this state writer only for the three flag item ids,
// replaces the entity's low flags byte and position, then applies the two
// relationships as occupantEntity and groundEntity. Attachment itself is
// projected by the native materializer; the canonical replica keeps the raw
// packed handles so render-only embedders see the same state.
// [orig: NapiNPClientMsg_0x02F @0x430E10]
void ClientReplicaPipeline::apply_objective_entity_state(
		const std::vector<uint8_t> &body) {
	ObjectiveEntityState state;
	size_t consumed = 0;
	if (!decode_objective_entity_state(
			body.data(), body.size(), state, consumed) ||
			consumed != body.size()) {
		++malformed_bodies_;
		return;
	}
	ClientEntityState *row = state_.find(state.entity_handle);
	const bool flag_row = row != nullptr && (row->type_id == 4091 || row->type_id == 4093 ||
			row->type_id == 4095);
	// The FlagBall / type-8 carrier latch rides both arms — the authority
	// takes the attach handle without touching the entity [orig: `attachHandle
	// = attachRef` @0x4310f6], a client after its writes — so the listen
	// host's own replica latches too. Its view carries no pool-1 rows; the
	// item-id gate is its server's own (the 0x2F senders walk only the three
	// flag ids).
	// [orig: `g_GameType == 65544 || g_GameType == 8` @0x4310fa..0x431109;
	//  dword_A860C4 = the attach handle's entity, or 0 for 0xFFFF / an
	//  out-of-range pool @0x431139..0x43115a]
	if ((flag_row || (row == nullptr && authority_recipient_)) &&
			(game_type_ == 0x10008u || game_type_ == 8u)) {
		const uint16_t carrier = state.attach_handle;
		state_.flag_carrier_handle =
				(carrier != 0xFFFFu && (carrier & 0xF000u) < 0x5000u) ? carrier : uint16_t{0xFFFF};
	}
	if (!flag_row) return;

	row->x = state.pos_x;
	row->y = state.pos_y;
	row->z = state.pos_z;
	row->state_flags = state.flags_byte;
	row->state_flags_known = true;
	row->spawn_entity_flags =
			(row->spawn_entity_flags & 0xFFFFFF00u) | state.flags_byte;
	row->rm_entity_flags =
			(row->rm_entity_flags & 0xFFFFFF00u) | state.flags_byte;
	row->parent_handle = state.attach_handle;
	row->target_handle = state.ground_handle;
	row->parent_pose_valid = false;
	++row->objective_state_serial;
	++state_.world_stream_revision;
	state_.mark_topology_changed();
}

} // namespace opennova::replication
