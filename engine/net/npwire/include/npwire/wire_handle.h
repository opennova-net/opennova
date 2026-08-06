#pragma once

// The wire entity-handle bit layout: the original entity pool rides the high
// nibble and the pool slot the low 12 bits — handle = pool << 12 | slot.
// This is the native home for that decode (godot/engine/world/wire_handle.gd is
// the GDScript twin). engine/runtime/world's EntityHandle carries the same packing for
// the sim-side registry — world stays net-agnostic, so the two are pinned
// against each other where both are visible (engine/net/netsim, entity_wire_bridge.cpp).
// [orig: return value of EntityPool_FindByNetId @ 0x4f0a20; 0xFFFF == not found]
//
// Live pools 0..4 = organics / items / buildings / markers / effects
// [orig: the g_pool_list walk bound @0x431910; pool taxonomy per the
// EntityPool_Allocate capacity stores @0x442168 (D-NET-207)].
//
// The batch-end / validity idiom every spawn-batch decoder shares: a slot id of
// 0xFFFF — or any id whose pool nibble is past the live pools — ends the batch
// or denotes "no entity". Retail phrases it `(id & 0xF000) >= 0x5000`
// [orig: NapiNPClientMsg_0x00D @ 0x432C40 sentinel test].

#include <cstdint>

namespace opennova::wire_handle {

inline constexpr int kPoolShift = 12;
inline constexpr uint16_t kPoolMask = 0xF;
inline constexpr uint16_t kSlotMask = 0xFFF;
inline constexpr uint16_t kInvalid = 0xFFFF;
inline constexpr int kPoolCount = 5;

// Pool indices (the g_pool_list order; capacities witnessed per D-NET-207).
inline constexpr int kPoolOrganic = 0;
inline constexpr int kPoolItem = 1;
inline constexpr int kPoolBuilding = 2;
inline constexpr int kPoolMarker = 3;
inline constexpr int kPoolEffects = 4;

constexpr int pool(uint16_t handle) {
	return (handle >> kPoolShift) & kPoolMask;
}
constexpr int slot(uint16_t handle) {
	return handle & kSlotMask;
}
constexpr uint16_t make(int pool_index, int slot_index) {
	return static_cast<uint16_t>(((pool_index & kPoolMask) << kPoolShift) |
	                             (slot_index & kSlotMask));
}

// True when a spawn-batch slot id ends the batch (or a handle denotes no
// entity): 0xFFFF, or a pool nibble past the live pools.
constexpr bool is_batch_end_sentinel(uint16_t slot_id) {
	return slot_id == kInvalid || pool(slot_id) >= kPoolCount;
}

// True when a compact record's parent/vehicle handle denotes a real mount —
// the complement of the sentinel test on parent handles.
constexpr bool is_real_mount_parent(uint16_t parent) {
	return !is_batch_end_sentinel(parent);
}

static_assert((kPoolCount << kPoolShift) == 0x5000,
              "the witnessed batch-end test is (id & 0xF000) >= 0x5000");
static_assert(make(2, 0x123) == 0x2123);
static_assert(pool(0x2123) == 2 && slot(0x2123) == 0x123);
static_assert(is_batch_end_sentinel(0xFFFF));
static_assert(is_batch_end_sentinel(0x5000));
static_assert(!is_batch_end_sentinel(0x4FFF));
static_assert(!is_real_mount_parent(0xFFFF) && is_real_mount_parent(0x2001));

}  // namespace opennova::wire_handle
