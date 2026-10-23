#pragma once

#include <runtime/replication/client_state.h>

#include <cstdint>

// Internal (src-local) seam: the organic replica bodies' leg phases (client_replica_legs.cpp),
// for the body passes in client_replica_motion.cpp.

namespace opennova::replication {

// The player body's view phase on a decoded row (the contract in the TU). `yaw_clamp_skipped`:
// the mount's config is 3, or the ground entity is a vehicle in its crash state;
// `pitch_clamp_skipped`: the mount is a vehicle in its crash state; `living_mount`: the row is
// alive and seated on a carrier row.
void row_org2_view_phase(ClientEntityState &es, uint32_t key, bool yaw_clamp_skipped,
		bool pitch_clamp_skipped, bool living_mount);

// The org1 body's leg chase on a free-standing row, after its body chase (the contract in the
// TU).
void row_org1_legs(ClientEntityState &es, uint32_t key, uint32_t def_attrib, int current_state);

} // namespace opennova::replication
