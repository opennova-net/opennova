// The joiner's decoded per-entity replica card (ADR 0042 d5): exactly what
// the wire carried and the client fold retained for one handle, before
// presentation — the ClientState twin of the registry half of
// world/inspect.h's EntityCard. NET level because the decoded replica exists
// only on the wire stack; the world card never includes it.
#pragma once

#include <cstdint>
#include <string>

#include <runtime/world/geom.h>

namespace opennova::replication {
struct ClientState;
}

namespace opennova::inmatch {

struct ClientReplicaCard {
	bool valid = false;
	int32_t handle = 0;
	int32_t type_id = 0;
	int32_t cls = 0;
	int32_t net_id = 0;
	std::string name;
	int32_t carrier_handle = 0;
	int32_t mount_bone = 0;
	int32_t seat_type = 0;
	bool net_seat_valid = false;
	int32_t heading_bam = 0;
	double heading_deg = 0.0;
	bool heading_known = false;
	int32_t pitch_bam = 0;
	int32_t yaw_byte = 0;
	world::Vec3 mission_position{}; // decompressed 16.16 -> world units
	int32_t anim_state_id = 0;
	int32_t state_flags = 0;
	bool state_flags_known = false;
	int32_t team = 0;
	int64_t compact_revision = 0;
	int64_t spawn_revision = 0;
};

// The card for one wire handle out of the retained decoded state; !valid when
// the handle has no row (or the caller is not running a client view).
ClientReplicaCard client_replica_card(const replication::ClientState &state,
		uint16_t handle);

} // namespace opennova::inmatch
