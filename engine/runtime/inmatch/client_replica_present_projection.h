#pragma once

#include <runtime/mission/promote.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/weapon_table.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::inmatch {

// The carried objects the decoded state links, keyed by carrier handle: the
// runtime type of every flag row (4091/4093/4095, the only ids the S2C 0x2F
// state writer accepts) whose attach relation names a carrier. The retail
// client attaches exactly these through that message.
// [orig: NapiNPClientMsg_0x02F @0x430E10 — the flag-id gate @0x430f03..0x430f19,
//  the attach call @0x43106f]
std::unordered_map<uint16_t, uint16_t> carried_object_types_by_carrier(
		const replication::ClientState &state);

// Optional definition-side enrichment for the client-replica projection. The
// wire state is sufficient for identity, pose, lifecycle, animation, and the
// unmounted aim overlay. Seat definitions add mounted overlay/emplacement
// controls; weapon.def adds the peer's exact upper-body hold state; the carry
// relation (carried_object_types_by_carrier) adds a carrier's carried object.
struct ClientReplicaPresentContext {
	const std::vector<mission::ItemSeatSpec> *item_seat_specs = nullptr;
	const world::WeaponTable *weapons = nullptr;
	bool project_remote_appearance = true;
	const std::unordered_map<uint16_t, uint16_t> *carried_types = nullptr;
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
