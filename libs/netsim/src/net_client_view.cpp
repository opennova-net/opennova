#include "netsim/net_client_view.h"

#include "netsim/entity_wire_bridge.h" // class_for_type_id (default resolver)
#include "netsim/connection_fan.h"     // kTag0aFrameUpdate

namespace opennova::netsim {

// ---- ClientState lookup -----------------------------------------------------

ClientEntityState *ClientState::find(uint16_t handle) {
	// `entities` is intentionally public decoded state. Callers may clear,
	// reorder, append, or edit it directly, so a separate handle-to-index cache
	// cannot remain valid without changing that API. Keep lookup derived from
	// the authoritative vector.
	for (ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

ClientEntityState &ClientState::upsert(uint16_t handle) {
	if (ClientEntityState *e = find(handle)) return *e;
	ClientEntityState e;
	e.handle = handle;
	entities.push_back(e);
	return entities.back();
}

// ---- NetClientView ----------------------------------------------------------

NetClientView::NetClientView()
		: resolver_([](uint16_t tid) { return class_for_type_id(tid); }) {}

NetClientView::NetClientView(std::function<EntityClass(uint16_t)> resolver)
		: resolver_(std::move(resolver)) {}

void NetClientView::set_item_class_resolver(std::function<EntityClass(uint16_t)> resolver) {
	item_resolver_ = std::move(resolver);
}

EntityClass NetClientView::classify(uint16_t type_id) const {
	// items.def first — the retail client's own dispatch source [orig: itemDef+356
	// @0x50f2e2]. It must outrank the 0x0D pool blanket: pool-1 holds no-callback
	// types too (an `ewep` emplacement), and sizing their header-only records as a
	// vehicle compact desyncs the whole frame after them.
	if (item_resolver_) {
		const EntityClass cls = item_resolver_(type_id);
		if (cls != EntityClass::Unknown) return cls;
	}
	const auto it = learned_classes_.find(type_id);
	if (it != learned_classes_.end()) return it->second;
	return resolver_(type_id);
}

void NetClientView::apply(uint8_t tag, const std::vector<uint8_t> &body) {
	switch (tag) {
	case kTag0aFrameUpdate:
		apply_frame_update(body);
		break;
	case 0x0C: // pool-0 organic spawn batch (§5.23)
		apply_organic_spawn(body);
		break;
	case 0x0D: // pool-1 entity spawn batch (§5.11)
		apply_pool_spawn(body);
		break;
	case 0x10: // pool-2 static entity batch (§5.9)
		apply_static_batch(body);
		break;
	case 0x20: // pool-3 marker/waypoint sync batch (§5.12)
		apply_pool3_batch(body);
		break;
	default:
		// Game-start scalars / world-state-load and other non-entity tags.
		++unknown_tags_;
		break;
	}
}

void NetClientView::pump(ISessionTransport &channel) {
	Datagram dg;
	while (channel.client_recv(dg)) apply(dg.tag, dg.body);
}

namespace {
// The compact coarse heading the present rebuilds: yaw_byte = top 8 bits of the 32-bit
// engine BAM (present does `bam = yaw_byte << 24`). [nova_simulation present.]
inline uint8_t yaw_byte_from_bam(int32_t bam) {
	return static_cast<uint8_t>(static_cast<uint32_t>(bam) >> 24);
}
} // namespace

void NetClientView::apply_organic_spawn(const std::vector<uint8_t> &body) {
	OrganicSpawnBatch batch;
	decode_organic_spawn_batch(body.data(), body.size(), batch); // lenient: apply what decoded
	for (const OrganicSpawnRecord &rec : batch.records) {
		if (!rec.has_body) continue;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		es.type_id = rec.item_type_id;
		es.cls = resolver_(rec.item_type_id);
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.orientation);
		es.pitch_bam = 0; // organic spawn carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
	}
}

void NetClientView::apply_pool_spawn(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	decode_pool_spawn_batch(body.data(), body.size(), batch);
	for (const PoolSpawnRecord &rec : batch.records) {
		// A 0x0D spawn is pool-1 by construction — learn the type's 0x0A replication
		// class so the vehicle compact body decodes for it (see classify()).
		learned_classes_[rec.item_type_id] = EntityClass::Vehicle;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Vehicle;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
	}
}

void NetClientView::apply_static_batch(const std::vector<uint8_t> &body) {
	StaticEntityBatch batch;
	decode_static_entity_batch(body.data(), body.size(), batch);
	// The 0x10 record carries no slot id — the entity's slot is start_index + iteration index.
	for (size_t i = 0; i < batch.records.size(); ++i) {
		const StaticEntityRecord &rec = batch.records[i];
		if (rec.is_empty_slot) continue;
		const uint16_t handle = static_cast<uint16_t>(0x2000u | ((batch.start_index + i) & 0x0FFFu));
		ClientEntityState &es = state_.upsert(handle);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Unknown; // a static has no 0x0A motion class; it never moves
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
	}
}

void NetClientView::apply_pool3_batch(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	decode_pool3_sync_batch(body.data(), body.size(), batch);
	for (const Pool3SyncRecord &rec : batch.records) {
		if (rec.is_empty_slot) continue;
		ClientEntityState &es = state_.upsert(rec.net_handle);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Unknown;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(static_cast<int32_t>(rec.movement_val));
		es.pitch_bam = 0; // pool-3 sync carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
	}
}

void NetClientView::apply_frame_update(const std::vector<uint8_t> &body) {
	FrameUpdate fu;
	// decode_frame_update leaves everything it walked in `fu` even on a short read,
	// so we apply whatever decoded cleanly (out.complete reflects a clean terminator).
	decode_frame_update(body.data(), body.size(),
	                    [this](uint16_t tid) { return classify(tid); }, fu);

	state_.anchor_x = fu.anchor_x;
	state_.anchor_y = fu.anchor_y;
	state_.anchor_z = fu.anchor_z;
	state_.local_health = fu.health;

	for (ClientEntityState &e : state_.entities) e.seen_this_frame = false;
	struct PendingCarrierPose {
		uint16_t child_handle;
		uint16_t carrier_handle;
		uint16_t cx;
		uint16_t cy;
		uint16_t cz;
		uint8_t local_yaw_byte;
	};
	std::vector<PendingCarrierPose> pending_carrier_poses;
	pending_carrier_poses.reserve(fu.records.size());

	for (const FrameUpdateRecord &rec : fu.records) {
		if (rec.cls == EntityClass::NoNetworkCallback) {
			continue;
		}
		ClientEntityState &es = state_.upsert(rec.handle);
		es.type_id = rec.type_id;
		es.cls = rec.cls;
		es.seen_this_frame = true;
		// Every compact record is a complete sample of these organic fields. Clear the
		// normalized row before class-specific assignment so dismounts and class changes
		// cannot retain a stale carrier/bone selector from an earlier frame.
		es.carrier_handle = 0xFFFFu;
		es.mount_bone = 0;
		es.seat_type = 0;
		es.pitch_byte = 0;
		es.aim_yaw_byte = 0;
		es.anim_state_id = 0;
		es.anim_channel_ratio = 0;

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor — or, for a CARRIER-LOCAL player record (vehicle/ground handle !=
		// 0xFFFF, D-NET-151), lift the local offset through the carrier's pose from this
		// view's own state [orig: op2 resolves the carrier from g_pool_list and runs
		// Entity_TransformLocalToWorld @0x4c10d4; a carrier with no itemDef DROPS the
		// record and queues a C2S 0x0F entity request — request plumbing an in-process
		// view does not need, so an unknown carrier just skips the position sample].
		// Carrier-local samples are queued for a second pass after every record has
		// updated the view. Production order is pool-0 child before pool-1 carrier.
		uint16_t cx = 0, cy = 0, cz = 0;
		bool skip_pos = false;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.carrier_handle = rec.player.carrier_handle;
			es.mount_bone = rec.player.vehicle_bone;
			es.seat_type = rec.player.seat_type;
			es.pitch_byte = rec.player.pitch_byte;
			es.anim_state_id = rec.player.anim_state_id;
			es.anim_channel_ratio = rec.player.anim_channel_ratio;
			if (rec.player.carrier_handle != 0xFFFFu) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.player.carrier_handle, cx, cy, cz,
						rec.player.yaw_byte});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.player.yaw_byte;
			}
			break;
		case EntityClass::Vehicle:
			cx = rec.vehicle.pos_x_compressed;
			cy = rec.vehicle.pos_y_compressed;
			cz = rec.vehicle.pos_z_compressed;
			es.yaw_byte = static_cast<uint8_t>(
					static_cast<uint16_t>(rec.vehicle.euler_z) >> 8);
			// Live vehicle compacts omit entity+20/+24. Preserve the last full
			// spawn/dead-pose values until the short dead-pose form carries new
			// signed high words [orig: @0x460d4c/@0x460d52].
			if (rec.vehicle.is_dead_pose) {
				es.pitch_bam = static_cast<int32_t>(rec.vehicle.euler_x) * 65536;
				es.roll_bam = static_cast<int32_t>(rec.vehicle.euler_y) * 65536;
			}
			break;
		case EntityClass::Infantry:
			cx = rec.infantry.pos_x_compressed;
			cy = rec.infantry.pos_y_compressed;
			cz = rec.infantry.pos_z_compressed;
			es.carrier_handle = rec.infantry.vehicle_slot_handle;
			es.mount_bone = rec.infantry.seat_bone_idx;
			es.pitch_byte = rec.infantry.pitch_byte;
			es.aim_yaw_byte = rec.infantry.aim_yaw_byte;
			es.anim_state_id = rec.infantry.anim_byte;
			if (rec.infantry.vehicle_slot_handle != 0xFFFFu) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.infantry.vehicle_slot_handle, cx, cy, cz,
						rec.infantry.yaw_byte});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.infantry.yaw_byte;
			}
			break;
		default:
			break; // unresolved/guided records are not compact motion samples
		}
		if (!skip_pos) {
			es.x = fu.anchor_x + network_decompress_fixedpoint(cx);
			es.y = fu.anchor_y + network_decompress_fixedpoint(cy);
			es.z = fu.anchor_z + network_decompress_fixedpoint(cz);
		}
	}

	// Resolve carrier-local children only after the complete frame has upserted and
	// updated every carrier. If the carrier is genuinely absent, leave the child's
	// prior world pose/yaw untouched, matching the retail record-drop path.
	for (const PendingCarrierPose &pending : pending_carrier_poses) {
		ClientEntityState *child = state_.find(pending.child_handle);
		const ClientEntityState *carrier = state_.find(pending.carrier_handle);
		if (child == nullptr || carrier == nullptr) continue;
		const int32_t carrier_yaw_bam = static_cast<int32_t>(
				uint32_t(carrier->yaw_byte) << 24);
		const WorldPose w = network_transform_local_to_world(
				network_decompress_fixedpoint(pending.cx),
				network_decompress_fixedpoint(pending.cy),
				network_decompress_fixedpoint(pending.cz), carrier->x, carrier->y,
				carrier->z, uint32_t(carrier_yaw_bam), uint32_t(carrier->pitch_bam),
				uint32_t(carrier->roll_bam));
		child->x = w.x;
		child->y = w.y;
		child->z = w.z;
		// World yaw byte = carrier yaw + local yaw; BAM addition holds in the
		// 8-bit ring used by the compact view.
		child->yaw_byte = uint8_t(carrier->yaw_byte + pending.local_yaw_byte);
	}

	++state_.frames_applied;
}

} // namespace opennova::netsim
