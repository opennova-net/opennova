#include "client_replica_card.h"

#include <runtime/replication/client_state.h>
#include <runtime/world/angle.h>

namespace opennova::np {

// A pure projection of the retained decoded row — the client entity fields
// the per-record compact applies store as they decode
// [orig: the player apply @0x4c1153; the infantry apply @0x4c0600..0x4c0641;
//  see runtime/replication/client_state.h for the per-field provenance].
ClientReplicaCard client_replica_card(const netsim::ClientState &state,
		uint16_t handle) {
	ClientReplicaCard card;
	for (const netsim::ClientEntityState &es : state.entities) {
		if (es.handle != handle) continue;
		card.valid = true;
		card.handle = static_cast<int32_t>(es.handle);
		card.type_id = static_cast<int32_t>(es.type_id);
		card.cls = static_cast<int32_t>(es.cls);
		card.net_id = static_cast<int32_t>(es.net_id);
		card.name = es.name;
		card.carrier_handle = static_cast<int32_t>(es.carrier_handle);
		card.mount_bone = static_cast<int32_t>(es.mount_bone);
		card.seat_type = static_cast<int32_t>(es.seat_type);
		card.net_seat_valid = es.net_seat_valid;
		card.heading_bam = es.heading_bam;
		card.heading_deg = world::mission_yaw_deg_from_bam_heading(es.heading_bam);
		card.heading_known = es.heading_known;
		card.pitch_bam = es.pitch_bam;
		card.yaw_byte = static_cast<int32_t>(es.yaw_byte);
		card.mission_position = world::Vec3{
				static_cast<float>(es.x) / 65536.0f,
				static_cast<float>(es.y) / 65536.0f,
				static_cast<float>(es.z) / 65536.0f};
		card.anim_state_id = static_cast<int32_t>(es.anim_state_id);
		card.state_flags = static_cast<int32_t>(es.state_flags);
		card.state_flags_known = es.state_flags_known;
		card.team = static_cast<int32_t>(es.team);
		card.compact_revision = static_cast<int64_t>(es.compact_revision);
		card.spawn_revision = static_cast<int64_t>(es.spawn_revision);
		return card;
	}
	return card;
}

} // namespace opennova::np
