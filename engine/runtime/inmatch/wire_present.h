#pragma once

// Wire-direct presentation policy: the projection from a runtime wire handle
// to the BMS entity-kind family presentation consumers gate on. Runtime
// handles encode the original entity pool in their high nibble; that pool —
// not a decoded PF_KIND, which a joiner intentionally lacks (-1) — drives the
// retail item-effect gates. [orig: pools 0/1/2/3 = organic/item/building/
// marker, the g_pool_list walk bound @0x431910; kind families per
// Entity_SpawnFromBMSRecord's record taxonomy]

#include <cstdint>

#include <runtime/mission/placement_traits.h>
#include <net/npwire/wire_handle.h>

namespace opennova::inmatch {

// The BMS entity kind a wire-direct row presents as, or -1 for pools with no
// BMS family (effects and beyond).
constexpr int mission_kind_for_wire_handle(uint16_t handle) {
	switch (wire_handle::pool(handle)) {
		case wire_handle::kPoolOrganic:
			return mission::kEntityKindOrganic;
		case wire_handle::kPoolItem:
			return mission::kEntityKindItem;
		case wire_handle::kPoolBuilding:
			return mission::kEntityKindBuilding;
		case wire_handle::kPoolMarker:
			return mission::kEntityKindMarker;
		default:
			return -1;
	}
}

static_assert(mission_kind_for_wire_handle(0x0001) == 3);  // organic pool
static_assert(mission_kind_for_wire_handle(0x1001) == 1);  // item pool
static_assert(mission_kind_for_wire_handle(0x2001) == 2);  // building pool
static_assert(mission_kind_for_wire_handle(0x3001) == 0);  // marker pool
static_assert(mission_kind_for_wire_handle(0x4001) == -1);  // effects pool


// --- wire-direct presentation witness ledger --------------------------------
// Retail behaviors the shell's wire present pass mirrors; the addresses live
// HERE so binding comments reference them by name:
//  - the held weapon draws RIGID at the attach transform, so a model-space
//    userpoint just rides that transform (the adm-arm fire-effect anchor);
//    the deepest fallback is the ENTITY ORIGIN, never the wire fire position
//    (which is the shooter's eye)
//    [orig: the rigid weapon draw @0x4e3d71; the userpoint fallback
//    @0x401867..0x401887]
//  - the third-person gun model resolves off the body's equipped ADM; the
//    sim folds the draw gate in (a hidden or unarmed body reports ADM 0)
//    [orig: the model resolve off the equipped ADM row]

}  // namespace opennova::inmatch
