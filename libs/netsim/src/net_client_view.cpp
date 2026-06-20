#include "netsim/net_client_view.h"

#include "netsim/entity_wire_bridge.h" // class_for_type_id (default resolver)
#include "netsim/net_system.h"         // kTag0aFrameUpdate

namespace opennova::netsim {

// ---- ClientState lookup -----------------------------------------------------

ClientEntityState *ClientState::find(uint16_t handle) {
	for (ClientEntityState &e : entities) {
		if (e.handle == handle) return &e;
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

void NetClientView::pump(LoopbackChannel &channel) {
	Datagram dg;
	while (channel.client_recv(dg)) {
		switch (dg.tag) {
		case kTag0aFrameUpdate:
			apply_frame_update(dg.body);
			break;
		default:
			// Phase 1 only reconstructs the per-frame 0x0A world reference; spawn /
			// pool / event tags arrive in later phases.
			++unknown_tags_;
			break;
		}
	}
}

void NetClientView::apply_frame_update(const std::vector<uint8_t> &body) {
	FrameUpdate fu;
	// decode_frame_update leaves everything it walked in `fu` even on a short read,
	// so we apply whatever decoded cleanly (out.complete reflects a clean terminator).
	decode_frame_update(body.data(), body.size(), resolver_, fu);

	state_.anchor_x = fu.anchor_x;
	state_.anchor_y = fu.anchor_y;
	state_.anchor_z = fu.anchor_z;
	state_.local_health = fu.health;

	for (ClientEntityState &e : state_.entities) e.seen_this_frame = false;

	for (const FrameUpdateRecord &rec : fu.records) {
		ClientEntityState &es = state_.upsert(rec.handle);
		es.type_id = rec.type_id;
		es.cls = rec.cls;
		es.seen_this_frame = true;

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor. Phase 1 handles the unmounted case (vehicle/parent handle ==
		// 0xFFFF); mounted records ride a parent-local frame and are resolved in a
		// later phase.
		uint16_t cx = 0, cy = 0, cz = 0;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.yaw_byte = rec.player.yaw_byte;
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
			break; // Guided / Unknown never decode into a compact (fail-closed)
		}
		es.x = fu.anchor_x + network_decompress_fixedpoint(cx);
		es.y = fu.anchor_y + network_decompress_fixedpoint(cy);
		es.z = fu.anchor_z + network_decompress_fixedpoint(cz);
	}

	++state_.frames_applied;
}

} // namespace opennova::netsim
