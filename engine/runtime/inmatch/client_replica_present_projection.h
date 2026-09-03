#pragma once

#include <runtime/mission/promote.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/weapon_table.h>

#include <vector>

namespace opennova::inmatch {

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
// world::PF_* row. Authoritative/local role bindings may enrich or override
// the result afterwards, but they do not define another wire model.
void project_client_replica_present_row(
		float *row,
		const replication::ClientEntityState &entity,
		const replication::ClientState &state,
		const ClientReplicaPresentContext &context = {});

} // namespace opennova::inmatch
