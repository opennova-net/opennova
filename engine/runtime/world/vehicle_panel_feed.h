#pragma once

// THE MOUNTED-VEHICLE PANEL FEED — the live half of hud/hud_vehicle_panel.h:
// which vehicle the panel describes, the seat/weapon slot list retail builds
// for it, and the per-marker rows (position from the VEHICLE_HUD block,
// occupancy + health from the entity, the seat-select digit from the list).
// [orig: the root re-root + the slot list Entity_BuildWeaponSlotList @0x434c60;
//  the three marker arms of HUD_DrawVehicleHealthBars @0x5a4fd0 —
//  seats @0x5a5112..0x5a5364, emplacements @0x5a53b7..0x5a5547,
//  driver @0x5a568e..0x5a5793]
// Witness record: docs/interface/hud-re.md "Mounted-vehicle panel".

#include <runtime/world/entity.h>

#include <formats/def/def.h>
#include <runtime/hud/hud_frame.h>

#include <vector>

namespace opennova::world {

class World; // the class-key must match world.h (MSVC mangles struct/class apart)

// One entry of retail's 10-slot weapon/seat list [orig: Entity_BuildWeaponSlotList
// @0x434c60]: `entity` is the vehicle for seat entries and the attached gun
// child for type 9; `type` is the retail slot type (8 control, 9 gun child,
// 0..7 passenger seat index).
struct VehiclePanelSlot {
    EntityHandle entity;
    int type = 0;
    // For type 9: the gun-slot index (0..3) the child's bone resolved to in the
    // vehicle def's gun-slot table [orig: Entity_GetMountSlotBoneIndex @0x546680
    //  (cached at +0x319; parentDef[0x214 + child slot byte]) vs def+532+i
    //  @0x434d28..0x434d3d].
    int gun_slot = -1;
};
inline constexpr int kVehiclePanelSlotMax = 10;

// The vehicle the panel is drawn for, from the local player's mount. A mount
// on an attached gun child re-roots to the child's parent vehicle
// [orig: `if (!(rootEntity->def+84 & 0x40)) vehicle = rootEntity->parent`
//  @0x434c77..0x434c7e]. Invalid when the player is on foot, when the root
// carries no vehicle def [orig: the def+613 test @0x434c91]. The writer of
// rootEntity @0x27235BC is UNWITNESSABLE (all five xrefs read it; the HUD
// entity-info block is filled from outside the image), so this takes EVERY
// mount — the list builder's own re-root implies as much — and the panel's
// only witnessed gate is the interface-texture test in the element
// [orig: HUD_DrawVehicleHealthBars @0x5a4fd0 draws nothing without it].
EntityHandle vehicle_panel_root(const World &world, const Entity &local);

// Retail's list, in retail's order: the control seat first (type 8), then the
// attached gun children that resolve to a gun slot and offer a UseGun seat
// (type 9), then the present passenger seats by seat index (types 0..7),
// capped at 10 [orig: @0x434ca9 (slot 0 = type 8); the child walk
//  @0x434cf4..0x434d5e; the seat loop @0x434d8d..0x434da8; the cap @0x434dad].
// The child walk follows the parent's attachment list; this walks the registry
// in handle order, which equals attachment order for authored emplacements.
int build_vehicle_panel_slots(const World &world, EntityHandle root,
                              std::vector<VehiclePanelSlot> &out);

// The marker rows for the panel element: one per authored pair the vehicle
// actually offers — passenger seats from the block's `seats` pairs (by retail
// slot 0..7), the control seat from the `driver` pair, and each attached gun
// child from the `emplace` pair of its gun slot. Occupancy and the rider's
// health come from the live seat occupant; the digit from the slot list;
// `own_seat` marks the seat the local player sits in.
void fill_vehicle_panel_seats(const World &world, EntityHandle root,
                              EntityHandle local, const DefVehicleHudBlock &block,
                              std::vector<hud::HudVehicleSeat> &out);


// The vehicle the local player rides, re-rooted from an attached gun child to
// its parent (vehicle_panel_root): shown + the root's items.def id the shell
// joins to its VEHICLE_HUD block. One value the embedder fills; its Godot
// record wraps it by value.
struct VehiclePanelRoot {
    bool shown = false;
    int32_t item_id = 0;
};

} // namespace opennova::world
