#pragma once

#include <mission/promote.h>
#include <netsim/client_state.h>
#include <world/weapon_table.h>

#include <vector>

namespace opennova::np {

// Optional definition-side enrichment for the client-replica projection. The
// wire state is sufficient for identity, pose, lifecycle, animation, and the
// unmounted aim overlay. Seat definitions add mounted overlay/emplacement
// controls; weapon.def adds the peer's exact upper-body hold state.
struct ClientReplicaPresentContext {
	const std::vector<mission::ItemSeatSpec> *item_seat_specs = nullptr;
	const world::WeaponTable *weapons = nullptr;
	bool project_remote_appearance = true;
};

// Initialize one world::PF_* row, including every non-zero sentinel.
void initialize_client_replica_present_row(float *row);

// Project the role-independent client replica fields into one initialized
// world::PF_* row. Authoritative/local role adapters may enrich or override
// the result afterwards, but they do not define another wire model.
void project_client_replica_present_row(
		float *row,
		const netsim::ClientEntityState &entity,
		const netsim::ClientState &state,
		const ClientReplicaPresentContext &context = {});

} // namespace opennova::np
