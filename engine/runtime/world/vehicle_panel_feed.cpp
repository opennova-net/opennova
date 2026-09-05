#include <runtime/world/vehicle_panel_feed.h>

#include <runtime/world/world.h>

#include <runtime/hud/hud_vehicle_panel.h>

#include <cstdio>

using namespace opennova::def;

namespace opennova::world {

namespace {

// ItemDefAttrib bits the list builder tests [orig: def+84 & 0x40 vehicle
// @0x434c77/@0x434c91; def+84 & 0x20 attachable child @0x434d18].
constexpr uint32_t kAttribVehicle = 0x40u;
constexpr uint32_t kAttribAttachable = 0x20u;

const Seat *seat_by_retail_slot(const Entity &e, int retail_slot) {
    for (const Seat &s : e.seats) {
        if (s.type == SeatType::None) continue;
        if (s.retail_slot == retail_slot) return &s;
    }
    return nullptr;
}

// The gun child's one UseGun seat [orig: the child def+614 test @0x434d44 —
// the seat-bone table's slot 9].
const Seat *gun_seat(const Entity &child) {
    for (const Seat &s : child.seats) {
        if (s.type == SeatType::Gunner) return &s;
    }
    return nullptr;
}

} // namespace

EntityHandle vehicle_panel_root(const World &world, const Entity &local) {
    if (!local.mounted || !local.mount_target.valid()) return EntityHandle{};
    const Entity *root = world.registry.get(local.mount_target);
    if (root == nullptr) return EntityHandle{};
    // [orig: @0x434c77 — a non-vehicle root (an attached gun child) re-roots
    //  to its parent]
    if ((root->item_attrib & kAttribVehicle) == 0) {
        if (!root->emplacement_parent.valid()) return EntityHandle{};
        root = world.registry.get(root->emplacement_parent);
        if (root == nullptr) return EntityHandle{};
    }
    // [orig: @0x434c91 — no vehicle def]. Every mount qualifies: the
    // rootEntity writer is unwitnessable (see the header), so no seat-type
    // gate is imposed here — the panel's gate is its texture test.
    if (!root->has_item_def || (root->item_attrib & kAttribVehicle) == 0)
        return EntityHandle{};
    return root->handle;
}

int build_vehicle_panel_slots(const World &world, EntityHandle root_h,
                              std::vector<VehiclePanelSlot> &out) {
    out.clear();
    const Entity *root = world.registry.get(root_h);
    if (root == nullptr) return 0;
    if ((root->item_attrib & kAttribVehicle) == 0) return 0;
    // Slot 0: the vehicle's control seat [orig: @0x434ca9..0x434cab].
    out.push_back(VehiclePanelSlot{root_h, 8, -1});
    // The attached gun children [orig: the child walk @0x434cf4..0x434d5e —
    // live (+28), Flags bit 1 clear, a def with attrib 0x20 and not 0x40,
    // parent == vehicle, bone in the gun-slot table, a UseGun seat].
    world.registry.for_each([&](const Entity &child) {
        if (static_cast<int>(out.size()) >= kVehiclePanelSlotMax) return;
        if (child.emplacement_parent != root_h) return;
        if (!child.alive || (child.flags & 1u) != 0) return;
        if (!child.has_item_def) return;
        if ((child.item_attrib & kAttribAttachable) == 0 ||
            (child.item_attrib & kAttribVehicle) != 0)
            return;
        // The gun-slot index: retail matches the child's cached mount-slot bone
        // [orig: Entity_GetMountSlotBoneIndex @0x546680 — +0x319 cache,
        //  parentDef[0x214 + (char)child[0x214]]] against the table
        // def+532..+535 @0x434d28..0x434d3d, and needs childDef+0x266 != 0 (a
        // gun seat); the child's authored addeweap slot IS that index.
        if (child.emplacement_slot >= DEF_VEHICLE_HUD_MAX_EMPLACE) return;
        if (gun_seat(child) == nullptr) return;
        out.push_back(VehiclePanelSlot{child.handle, 9,
                static_cast<int>(child.emplacement_slot)});
    });
    // The passenger seats by index [orig: @0x434d8d..0x434da8].
    for (int s = 0; s < 8; ++s) {
        if (static_cast<int>(out.size()) >= kVehiclePanelSlotMax) break;
        if (seat_by_retail_slot(*root, s) == nullptr) continue;
        out.push_back(VehiclePanelSlot{root_h, s, -1});
    }
    return static_cast<int>(out.size());
}

void fill_vehicle_panel_seats(const World &world, EntityHandle root_h,
                              EntityHandle local, const DefVehicleHudBlock &block,
                              std::vector<hud::HudVehicleSeat> &out,
                              const VehicleOccupancySource *source) {
    out.clear();
    const Entity *root = world.registry.get(root_h);
    if (root == nullptr) return;
    std::vector<VehiclePanelSlot> slots;
    build_vehicle_panel_slots(world, root_h, slots);

    const auto occupant_row = [&](const Entity &carrier, const Seat *seat, hud::HudVehicleSeat &row) {
        if (seat == nullptr) return;
        const auto occupancy = vehicle_seat_occupancy(world, carrier, *seat, local, source);
        row.occupied = occupancy.occupied && occupancy.rider_resolved;
        row.own_seat = row.occupied && occupancy.own_seat;
        row.health = occupancy.health;
        row.max_health = occupancy.max_health;
    };
    char label[8];

    for (size_t pos = 0; pos < slots.size(); ++pos) {
        const VehiclePanelSlot &slot = slots[pos];
        hud::HudVehicleSeat row;
        if (slot.type == 8) {
            // The driver pair [orig: @0x5a568e..0x5a5793, digit 1 @0x5a57ee].
            row.x = block.driver_x;
            row.y = block.driver_y;
            row.retail_slot = 8;
            std::snprintf(label, sizeof(label), "%1d", hud::kDriverLabelDigit);
            occupant_row(*root, seat_by_retail_slot(*root, 8), row);
        } else if (slot.type == 9) {
            // The emplacement pair of the child's gun slot
            // [orig: @0x5a53b7..0x5a5547, digit i + 2 @0x5a5602].
            if (slot.gun_slot < 0 || slot.gun_slot >= block.emplace_count) continue;
            const Entity *child = world.registry.get(slot.entity);
            if (child == nullptr) continue;
            row.x = block.emplace_x[slot.gun_slot];
            row.y = block.emplace_y[slot.gun_slot];
            row.retail_slot = -1;
            row.is_emplacement = true;
            std::snprintf(label, sizeof(label), "%1d",
                    hud::emplace_label_digit(slot.gun_slot));
            occupant_row(*child, gun_seat(*child), row);
        } else {
            // A passenger seat pair [orig: @0x5a5112..0x5a5364, digit
            //  (listPos + 1) % 10 @0x5a5283].
            if (slot.type < 0 || slot.type >= block.seat_count) continue;
            row.x = block.seat_x[slot.type];
            row.y = block.seat_y[slot.type];
            row.retail_slot = slot.type;
            std::snprintf(label, sizeof(label), "%1d",
                    hud::seat_label_digit(static_cast<int>(pos)));
            occupant_row(*root, seat_by_retail_slot(*root, slot.type), row);
        }
        row.label = label;
        out.push_back(row);
    }
}

} // namespace opennova::world
