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

EntityClass NetClientView::classify(uint16_t type_id) const {
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

	for (const FrameUpdateRecord &rec : fu.records) {
		if (rec.cls == EntityClass::NoNetworkCallback) {
			continue;
		}
		ClientEntityState &es = state_.upsert(rec.handle);
		es.type_id = rec.type_id;
		es.cls = rec.cls;
		es.seen_this_frame = true;

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor — or, for a CARRIER-LOCAL player record (vehicle/ground handle !=
		// 0xFFFF, D-NET-151), lift the local offset through the carrier's pose from this
		// view's own state [orig: op2 resolves the carrier from g_pool_list and runs
		// Entity_TransformLocalToWorld @0x4c10d4; a carrier with no itemDef DROPS the
		// record and queues a C2S 0x0F entity request — request plumbing an in-process
		// view does not need, so an unknown carrier just skips the position sample].
		// Mounted vehicle-parent records stay a later phase.
		uint16_t cx = 0, cy = 0, cz = 0;
		bool skip_pos = false;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.yaw_byte = rec.player.yaw_byte;
			if (rec.player.carrier_handle != 0xFFFFu) {
				if (const ClientEntityState *carrier =
				            state_.find(rec.player.carrier_handle)) {
					const int32_t carrier_yaw_bam =
							static_cast<int32_t>(uint32_t(carrier->yaw_byte) << 24);
					const WorldPose w = network_transform_local_to_world(
							network_decompress_fixedpoint(cx),
							network_decompress_fixedpoint(cy),
							network_decompress_fixedpoint(cz), carrier->x, carrier->y,
							carrier->z, uint32_t(carrier_yaw_bam), 0u, 0u);
					es.x = w.x;
					es.y = w.y;
					es.z = w.z;
					// world yaw byte = carrier yaw + local yaw (BAM addition holds in
					// the 8-bit ring).
					es.yaw_byte = uint8_t(carrier->yaw_byte + rec.player.yaw_byte);
				}
				skip_pos = true; // carrier form: either applied above or dropped
			}
			break;
		case EntityClass::Vehicle:
			cx = rec.vehicle.pos_x_compressed;
			cy = rec.vehicle.pos_y_compressed;
			cz = rec.vehicle.pos_z_compressed;
			es.yaw_byte = static_cast<uint8_t>(
					static_cast<uint16_t>(rec.vehicle.euler_z) >> 8);
			break;
		case EntityClass::Infantry:
			cx = rec.infantry.pos_x_compressed;
			cy = rec.infantry.pos_y_compressed;
			cz = rec.infantry.pos_z_compressed;
			es.yaw_byte = rec.infantry.yaw_byte;
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

	++state_.frames_applied;
}

} // namespace opennova::netsim
