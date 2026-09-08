// The mounted-vehicle panel feed: the re-rooted vehicle, retail's slot-list
// order, and the per-marker rows (pair placement, occupancy, digits, own seat).
// [orig: Entity_BuildWeaponSlotList @0x434c60; HUD_DrawVehicleHealthBars
//  @0x5a4fd0 — seats @0x5a5112, emplacements @0x5a53b7, driver @0x5a568e]
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/world.h>
#include <runtime/world/local_player.h>

#include <runtime/hud/hud_vehicle_panel.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace opennova::world;
using namespace opennova::def;
namespace hud = opennova::hud;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

Seat make_seat(SeatType type, uint8_t retail_slot, const char *name) {
    Seat s;
    s.type = type;
    s.retail_slot = retail_slot;
    s.bone_index = 1;
    s.source_name = name;
    return s;
}

struct Rig {
    World w;
    EntityHandle veh, gun, driver, passenger, gunner;
    DefVehicleHudBlock block{};
    Rig() {
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);

        Entity v;
        v.kind = EntityKind::Item;
        v.item_id = 1291;
        v.has_item_def = true;
        v.item_attrib = 0x40; // a vehicle
        v.alive = true;
        v.health = 3000;
        v.health_max = 3000;
        v.seats.push_back(make_seat(SeatType::Passenger, 0, "sitex00"));
        v.seats.push_back(make_seat(SeatType::Controller, 8, "ctrlx00"));
        v.seats.push_back(make_seat(SeatType::Passenger, 3, "sitex03"));
        veh = w.registry.spawn(1, v);

        // An attached gun child on gun slot 1 with a UseGun seat.
        Entity g;
        g.kind = EntityKind::Item;
        g.item_id = 1300;
        g.has_item_def = true;
        g.item_attrib = 0x20; // attachable, not a vehicle
        g.alive = true;
        g.emplacement_parent = veh;
        g.emplacement_slot = 1;
        g.seats.push_back(make_seat(SeatType::Gunner, 9, "UseGun"));
        gun = w.registry.spawn(1, g);

        auto organic = [&](int hp) {
            Entity o;
            o.kind = EntityKind::Organic;
            o.item_id = 5305;
            o.alive = true;
            o.health = hp;
            o.health_max = 150;
            return w.registry.spawn(0, o);
        };
        driver = organic(150);
        passenger = organic(40);
        gunner = organic(90);

        std::strcpy(block.sid, "dbuggy1");
        block.driver_x = 10; block.driver_y = 11;
        block.seat_count = 4;
        for (int i = 0; i < 4; ++i) { block.seat_x[i] = 20 + i; block.seat_y[i] = 30 + i; }
        block.emplace_count = 2;
        for (int i = 0; i < 2; ++i) { block.emplace_x[i] = 50 + i; block.emplace_y[i] = 60 + i; }
    }
    Entity &e(EntityHandle h) { return *w.registry.get(h); }
    void seat_occupant(EntityHandle vehicle, int retail_slot, EntityHandle who) {
        for (Seat &s : e(vehicle).seats)
            if (s.retail_slot == retail_slot) s.occupant = who;
        Entity &o = e(who);
        o.mounted = true;
        o.mount_target = vehicle;
    }
};

const hud::HudVehicleSeat *row_at(const std::vector<hud::HudVehicleSeat> &rows,
                                  int x, int y) {
    for (const auto &r : rows)
        if (r.x == x && r.y == y) return &r;
    return nullptr;
}

} // namespace

int main() {
    // --- the slot list: control seat first, then the gun child, then the
    //     passenger seats by index [orig: @0x434ca9 / @0x434cf4 / @0x434d8d].
    {
        Rig r;
        VehiclePanelSlotList slots;
        CHECK(build_vehicle_panel_slots(r.w, r.veh, slots) == 4);
        CHECK(slots.count == 4 && slots[4].type == 0 && !slots[4].entity.valid());
        CHECK(slots[0].type == 8 && slots[0].entity == r.veh);
        CHECK(slots[1].type == 9 && slots[1].entity == r.gun && slots[1].gun_slot == 1);
        CHECK(slots[2].type == 0);
        CHECK(slots[3].type == 3);
    }

    // Numbered seats use the same root/list, independent of vector order,
    // HUD art availability and proximity. Player-held seats reject requests;
    // the request-side AI exception does not override authority occupancy.
    {
        Rig r;
        Entity &local = r.e(r.passenger);
        local.flags |= kEntityFlagPlayer;
        r.seat_occupant(r.veh, 3, r.passenger);
        VehicleSeatSelection pick;
        CHECK(find_numbered_vehicle_seat(r.w, local, 0, pick));
        CHECK(pick.vehicle == r.veh && pick.seat_index == 1 && pick.type == SeatType::Controller);
        CHECK(find_numbered_vehicle_seat(r.w, local, 1, pick));
        CHECK(pick.vehicle == r.gun && pick.type == SeatType::Gunner);
        CHECK(find_numbered_vehicle_seat(r.w, local, 2, pick));
        CHECK(pick.vehicle == r.veh && pick.seat_index == 0);
        CHECK(!find_numbered_vehicle_seat(r.w, local, 3, pick)); // own player-held seat
        CHECK(!find_numbered_vehicle_seat(r.w, local, -1, pick));
        CHECK(!find_numbered_vehicle_seat(r.w, local, 4, pick));
        CHECK(!find_numbered_vehicle_seat(r.w, local, 10, pick));
        r.seat_occupant(r.veh, 8, r.driver);
        r.e(r.driver).flags |= kEntityFlagPlayer;
        CHECK(!find_numbered_vehicle_seat(r.w, local, 0, pick));
        r.e(r.driver).flags &= ~kEntityFlagPlayer;
        CHECK(find_numbered_vehicle_seat(r.w, local, 0, pick));
        CHECK(!r.w.vehicles.attach_to_seat(local.handle, pick)); // authority still rejects busy
        local.flags &= ~kEntityFlagPlayer;
        CHECK(!find_numbered_vehicle_seat(r.w, local, 0, pick)); // AI cannot request another rider's seat
        local.flags |= kEntityFlagPlayer;
        r.e(r.veh).seats[1].occupant = EntityHandle{0x5000};
        CHECK(!find_numbered_vehicle_seat(r.w, local, 0, pick)); // unresolved retained occupant
        r.e(r.veh).seats[1].occupant = {};
        r.w.cached.local_player = local.handle;
        LocalPlayer controls(r.w);
        controls.weapon.active = true;
        controls.weapon.slot.current = weapon_action::kReload;
        controls.weapon.slot.next = weapon_action::kOverheated;
        CHECK(!controls.find_numbered_seat(0, pick));
        controls.weapon.slot.current = weapon_action::kEmpty;
        CHECK(!controls.find_numbered_seat(0, pick));
        controls.weapon.slot.current = weapon_action::kOverheated;
        CHECK(controls.find_numbered_seat(0, pick));
        controls.weapon.active = false;
        controls.weapon.slot.current = weapon_action::kReload;
        CHECK(controls.find_numbered_seat(0, pick)); // no EquippedSlot
        local.mount_target = r.gun;
        CHECK(controls.find_numbered_seat(0, pick) && pick.vehicle == r.veh);
        r.e(r.veh).seats.erase(r.e(r.veh).seats.begin() + 1);
        CHECK(!controls.find_numbered_seat(0, pick)); // root must author a control bone
    }

    // --- the root: a mounted driver names the vehicle; a gunner on the child
    //     re-roots to the parent; on foot there is no panel.
    {
        Rig r;
        CHECK(!vehicle_panel_root(r.w, r.e(r.driver)).valid());
        r.seat_occupant(r.veh, 8, r.driver);
        CHECK(vehicle_panel_root(r.w, r.e(r.driver)) == r.veh);
        r.seat_occupant(r.gun, 9, r.gunner);
        CHECK(vehicle_panel_root(r.w, r.e(r.gunner)) == r.veh);
    }

    // --- the rows: placement from the authored pairs, occupancy + health from
    //     the live seats, the digits from the list position, own seat marked.
    {
        Rig r;
        r.seat_occupant(r.veh, 8, r.driver);
        r.seat_occupant(r.veh, 3, r.passenger);
        r.seat_occupant(r.gun, 9, r.gunner);
        std::vector<hud::HudVehicleSeat> rows;
        fill_vehicle_panel_seats(r.w, r.veh, r.driver, r.block, rows);
        CHECK(rows.size() == 4);

        const hud::HudVehicleSeat *drv = row_at(rows, 10, 11);
        CHECK(drv != nullptr);
        CHECK(drv && drv->occupied && drv->retail_slot == 8);
        CHECK(drv && drv->own_seat);
        CHECK(drv && drv->label == "1");
        CHECK(drv && drv->health == 150 && drv->max_health == 150);

        // Gun slot 1 -> the second emplace pair, digit 1 + 2 = 3, banded through
        // the emplacement classifier.
        const hud::HudVehicleSeat *g = row_at(rows, 51, 61);
        CHECK(g != nullptr);
        CHECK(g && g->is_emplacement && g->occupied && g->health == 90);
        CHECK(g && g->label == "3");
        CHECK(g && !g->own_seat);

        // Seat 0 is empty: list position 2 -> digit 3; seat 3 occupied at
        // position 3 -> digit 4.
        const hud::HudVehicleSeat *s0 = row_at(rows, 20, 30);
        CHECK(s0 != nullptr);
        CHECK(s0 && !s0->occupied && s0->label == "3" && s0->retail_slot == 0);
        const hud::HudVehicleSeat *s3 = row_at(rows, 23, 33);
        CHECK(s3 != nullptr);
        CHECK(s3 && s3->occupied && s3->health == 40 && s3->label == "4");
    }

    // --- an authored block with fewer pairs than the vehicle offers drops the
    //     unauthored seats rather than placing them at zero.
    {
        Rig r;
        r.block.seat_count = 1; // only seat 0 authored
        r.block.emplace_count = 1; // gun slot 1 unauthored
        std::vector<hud::HudVehicleSeat> rows;
        fill_vehicle_panel_seats(r.w, r.veh, r.driver, r.block, rows);
        CHECK(rows.size() == 2); // driver + seat 0
        CHECK(row_at(rows, 23, 33) == nullptr);
        CHECK(row_at(rows, 51, 61) == nullptr);
    }

    // --- the digit rules [orig: @0x5a5283 / @0x5a5602 / @0x5a57ee].
    CHECK(hud::seat_label_digit(0) == 1);
    CHECK(hud::seat_label_digit(8) == 9);
    CHECK(hud::seat_label_digit(9) == 0);
    CHECK(hud::emplace_label_digit(0) == 2);
    CHECK(hud::kDriverLabelDigit == 1);

    // --- the emplacement classifier: clamped + signed, so a negative ratio
    //     reads BAD where the rider arm reads Good [orig: @0x5a54d3..0x5a54e3].
    CHECK(hud::emplacement_health_band(100, 100) == hud::SeatHealthBand::Good);
    CHECK(hud::emplacement_health_band(500, 100) == hud::SeatHealthBand::Good);
    CHECK(hud::emplacement_health_band(50, 100) == hud::SeatHealthBand::Middle);
    CHECK(hud::emplacement_health_band(-1, 100) == hud::SeatHealthBand::Bad);
    CHECK(hud::seat_health_band(-1, 100) == hud::SeatHealthBand::Good);

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vehicle_panel_feed_test OK\n");
    return 0;
}
