#pragma once

#include <mission/promote.h>
#include <netsim/client_state.h>
#include <world/weapon_table.h>

#include <vector>

namespace godot {

// Optional definition-side enrichment for the client-replica projection. The
// wire state is sufficient for identity, pose, lifecycle, animation, and the
// unmounted aim overlay. Seat definitions add mounted overlay/emplacement
// controls; weapon.def adds the peer's exact upper-body hold state.
struct ClientReplicaPresentContext {
	const std::vector<opennova::mission::ItemSeatSpec> *item_seat_specs = nullptr;
	const opennova::world::WeaponTable *weapons = nullptr;
	bool project_remote_appearance = true;
	// D-NET-209 dual-publish (the rollback seam): true presents an ARMED row's
	// simulation-arbitrated channel directly (the host-loopback tuple,
	// remote_request 0 — the model-side remote FSM is bypassed); false keeps
	// the legacy remote-request publish (wire byte + pulse re-arbitrated at
	// the model). The per-record arbitration runs in the fold either way.
	bool remote_body_native_publish = true;
};

// Initialize one NovaSimulation::PF_* row, including every non-zero sentinel.
void initialize_client_replica_present_row(float *row);

// Project the role-independent client replica fields into one initialized
// NovaSimulation::PF_* row. Authoritative/local role adapters may enrich or
// override the result afterwards, but they do not define another wire model.
void project_client_replica_present_row(
		float *row,
		const opennova::netsim::ClientEntityState &entity,
		const opennova::netsim::ClientState &state,
		const ClientReplicaPresentContext &context = {});

} // namespace godot
