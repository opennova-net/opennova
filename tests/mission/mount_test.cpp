// Emplacement / mount (Phase 2) tests: the seat model + EntityCommands mount primitives
// (mount/use_boarding_target/dismount/find_mounted_on), canonical best-seat selection/attach,
// the per-tick AI seat-follow,
// the BMS AttachToEmplaced action path (emits no unported_action), and snapshot/restore
// rewinding the mount. [orig chain: EventAction_Dispatch case 0x25 @0x4542e0 ->
// WacScript_TryMountEntityToVehicle @0x4f70f0 -> Entity_FindBestSeatSlot @0x4351f0.]
#include <cmath>
#include <cstdio>
#include <memory>

#include <runtime/mission/event_runtime.h>
#include <formats/mission/bms.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

using namespace opennova;
using world::AiBrain;
using world::AiEntity;
using world::AiSystem;
using world::Entity;
using world::EntityHandle;
using world::EntityKind;
using world::IRootMotionSource;
using world::RootMotionFrame;
using world::Seat;
using world::SeatType;
using world::TickContext;
using world::World;
using world::to_fixed;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                 \
    do {                                                                      \
        const double _a = (a);                                                 \
        const double _b = (b);                                                 \
        if (std::fabs(_a - _b) > (eps)) {                                      \
            std::printf("FAIL %s:%d  %s ~= %s  got %.6f expected %.6f\n",      \
                        __FILE__, __LINE__, #a, #b, _a, _b);                  \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static Entity make_gun(uint16_t ssn, float x, float y, float z, int16_t yaw) {
    Entity e;
    e.net_id = ssn;
    e.kind = EntityKind::Item;
    e.position = {x, y, z};
    e.yaw = yaw;
    Seat s;
    s.type = SeatType::Gunner;
    e.seats.push_back(s);
    return e;
}

static Entity make_soldier(uint16_t ssn, float x, float y, float z) {
    Entity e;
    e.net_id = ssn;
    e.kind = EntityKind::Organic;
    e.position = {x, y, z};
    return e;
}

static Entity make_vehicle(uint16_t ssn, SeatType seat_type) {
    Entity e;
    e.net_id = ssn;
    e.bms_id = 77;
    e.spawn_origin = (1u << 24) | 3u;
    e.kind = EntityKind::Item;
    // Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
    e.item_attrib = world::kItemAttribPlayerControl;
    Seat seat;
    seat.type = seat_type;
    e.seats.push_back(seat);
    return e;
}

static Entity make_dual_control_vehicle(uint16_t ssn) {
    Entity e = make_vehicle(ssn, SeatType::Controller);
    e.seats[0].bone_index = 7;
    Seat driver;
    driver.type = SeatType::Driver;
    driver.bone_index = 9;
    e.seats.push_back(driver);
    return e;
}

struct ClipSource : IRootMotionSource {
    int available_state = -1;

    bool has_clip(int /*adm_id*/, int state_id) const override {
        return state_id == available_state;
    }

    int32_t clip_length_ticks(int /*adm_id*/, int /*state_id*/, int /*variant*/) const override { return -1; }

    bool advance(int /*adm_id*/, int /*state_id*/, int32_t &/*phase_ticks*/,
                 RootMotionFrame &/*out*/) override {
        return false;
    }
};

static void test_mount_config_lifecycle() {
    auto world_fixture = std::make_unique<World>();
    World &w = *world_fixture;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(1, 16);
    Entity gun = make_gun(200, 0.f, 0.f, 0.f, 0);
    gun.emplaced_config_valid = true;
    gun.emplaced_config = 0;
    gun.seats[0].bone_index = 7;
    gun.primary_weapon.assign(1, 'x');
    w.tables.weapons.entries.resize(2);
    w.tables.weapons.entries[1].name.assign(1, 'x');
    w.tables.weapons.entries[1].clipsize = -1;
    w.tables.weapons.entries[1].valid = true;
    const EntityHandle gh = w.registry.spawn(1, gun);
    Entity soldier = make_soldier(100, 0.f, 0.f, 0.f);
    soldier.equipped_adm_index = 7;
    soldier.flags |= 0x100u;
    soldier.engine_flags |= 0x100u;
    const EntityHandle sh = w.registry.spawn(0, soldier);

    CHECK(w.commands.mount(100, 200));
    CHECK(w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 0);
    CHECK(w.registry.get(sh)->mount_bone == 7);
    // UseGun is not the generic carried-slot path and never gains Flags 0x40.
    // [orig: Entity_AttachToUseGunSlot @0x546c56-0x546c7c]
    CHECK((w.registry.get(sh)->flags & 0x40u) == 0);
    CHECK((w.registry.get(sh)->engine_flags & 0x40u) == 0);
    CHECK(w.registry.get(sh)->equipped_adm_index == 1);
    CHECK(w.registry.get(sh)->pre_use_gun_equipped_adm_index == 7);
    CHECK(w.registry.get(sh)->use_gun_slot_swapped);
    CHECK(w.registry.get(gh)->primary_weapon_owner == sh);
    CHECK(w.registry.get(gh)->primary_weapon_slot_adm == 1);
    CHECK(w.registry.get(gh)->primary_weapon_slot.clip == -1);
    const auto occupied = std::make_unique<World::Snapshot>(w.snapshot());

    CHECK(w.commands.dismount(100));
    CHECK(!w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 0);
    CHECK(w.registry.get(sh)->mount_bone == 0);
    CHECK((w.registry.get(sh)->flags & 0x40u) == 0);
    CHECK((w.registry.get(sh)->engine_flags & 0x40u) == 0);
    CHECK(w.registry.get(sh)->equipped_adm_index == 7);
    CHECK(!w.registry.get(sh)->use_gun_slot_swapped);
    CHECK(!w.registry.get(gh)->primary_weapon_owner.valid());

    w.restore(*occupied);
    CHECK(w.registry.get(sh)->mounted);
    CHECK(w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 0);
    CHECK(w.registry.get(sh)->mount_bone == 7);
    CHECK(w.registry.get(sh)->equipped_adm_index == 1);
    CHECK(w.registry.get(sh)->pre_use_gun_equipped_adm_index == 7);
    CHECK(w.registry.get(sh)->use_gun_slot_swapped);
    CHECK(w.registry.get(gh)->primary_weapon_owner == sh);
    CHECK(w.registry.get(gh)->emplaced_config_valid);
    CHECK(w.registry.get(gh)->emplaced_config == 0);
}

static void test_wire_mount_config_lifecycle() {
    auto world_fixture = std::make_unique<World>();
    World &w = *world_fixture;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(1, 16);
    Entity gun = make_gun(200, 0.f, 0.f, 0.f, 0);
    gun.emplaced_config_valid = true;
    gun.emplaced_config = 6;
    gun.seats[0].bone_index = 7;
    const EntityHandle gh = w.registry.spawn(1, gun);
    const EntityHandle sh =
            w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

    CHECK(w.vehicles.process_attach(sh, gh, 7));
    CHECK(w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 6);
    CHECK(w.vehicles.detach(sh));
    CHECK(!w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 0);
}

static void test_wire_attach_rejects_unknown_model_bone() {
    // The wire byte is the 1-based index into the model's 48-byte user-point
    // table. Retail first classifies that exact row's name and rejects an
    // unrecognized/out-of-range row; it never substitutes another free seat.
    // [orig: Entity_GetBoneSlotType @0x434ED0; reject @0x435B14]
    auto world_fixture = std::make_unique<World>();
    World &w = *world_fixture;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(1, 16);
    Entity gun = make_gun(200, 0.f, 0.f, 0.f, 0);
    gun.seats[0].bone_index = 7;
    const EntityHandle gh = w.registry.spawn(1, gun);
    const EntityHandle sh =
            w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

    CHECK(!w.vehicles.process_attach(sh, gh, 9));
    CHECK(!w.registry.get(sh)->mounted);
    CHECK(!w.registry.get(gh)->seats[0].occupant.valid());
}

static void test_target_loss_clears_zero_net_id_occupant() {
    // Player entities deliberately share net_id 0 and are distinguished by their
    // pool handles. Auto-dismount must clear the exact mounted occupant, not resolve
    // the first zero-id player through the script-facing SSN table.
    auto world_fixture = std::make_unique<World>();
    World &w = *world_fixture;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(1, 16);

    Entity first_player = make_soldier(0, 0.f, 0.f, 0.f);
    first_player.player_class = 1;
    const EntityHandle first = w.registry.spawn(0, first_player);
    Entity mounted_player = make_soldier(0, 0.f, 0.f, 0.f);
    mounted_player.player_class = 2;
    const EntityHandle mounted = w.registry.spawn(0, mounted_player);

    Entity gun = make_gun(200, 0.f, 0.f, 0.f, 0);
    gun.emplaced_config_valid = true;
    gun.emplaced_config = 6;
    gun.seats[0].bone_index = 7;
    const EntityHandle target = w.registry.spawn(1, gun);

    AiSystem &ai = w.ai;
    const int mounted_ai_index = ai.attach(mounted);
    AiEntity *mounted_ai = ai.at(mounted_ai_index);

    CHECK(w.vehicles.process_attach(mounted, target, 7));
    CHECK(w.registry.get(mounted)->mounted_config_valid);
    w.registry.despawn(target);
    CHECK(!ai.pose_if_mounted(*mounted_ai, w));
    CHECK(!w.registry.get(mounted)->mounted);
    CHECK(!w.registry.get(mounted)->mounted_config_valid);
    CHECK(w.registry.get(mounted)->mounted_config == 0);
    CHECK(!w.registry.get(first)->mounted);

    Entity replacement = make_gun(201, 0.f, 0.f, 0.f, 0);
    replacement.emplaced_config_valid = true;
    replacement.emplaced_config = 6;
    replacement.seats[0].bone_index = 7;
    const EntityHandle replacement_target = w.registry.spawn(1, replacement);
    CHECK(w.vehicles.process_attach(mounted, replacement_target, 7));
    Entity *mounted_entity = w.registry.get(mounted);
    mounted_entity->health = 0;
    mounted_entity->deathtime_ticks = 120;
    mounted_ai->inf.active = true;
    mounted_ai->inf.anim_state = world::anim_state::kIdleCrouch;
    ai.tick_infantry(*mounted_ai, w, 1);
    mounted_entity = w.registry.get(mounted);
    CHECK(mounted_entity != nullptr);
    if (mounted_entity != nullptr) {
        CHECK(!mounted_entity->mounted);
        CHECK(!mounted_entity->mounted_config_valid);
        CHECK(mounted_entity->mounted_config == 0);
    }
    CHECK(!w.registry.get(first)->mounted);
}

int main() {
    // ---- mount sets both sides + poses onto the seat ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16); // organics
        w.registry.configure_pool(1, 16); // items
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 10.f, 20.f, 5.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 100.f, 100.f, 0.f));

        CHECK(w.commands.mount(100, 200));
        Entity *occ = w.registry.get(sh);
        Entity *gun = w.registry.get(gh);
        CHECK(occ->mounted);
        CHECK(occ->mount_target == gh);
        CHECK(occ->mount_seat == 0);
        CHECK(occ->mount_type == SeatType::Gunner);
        CHECK(!occ->mounted_config_valid);
        CHECK(occ->mounted_config == 0);
        CHECK(gun->seats[0].occupant == sh);
        // seat_local default 0 -> occupant snaps to the gun origin.
        CHECK(occ->position.x == 10.f && occ->position.y == 20.f && occ->position.z == 5.f);
        CHECK(w.commands.find_mounted_on(200) == 100);

        // dismount frees the seat + clears the ref.
        CHECK(w.commands.dismount(100));
        CHECK(!occ->mounted);
        CHECK(occ->mount_seat == -1);
        CHECK(!occ->mounted_config_valid);
        CHECK(occ->mounted_config == 0);
        CHECK(!gun->seats[0].occupant.valid());
        CHECK(w.commands.find_mounted_on(200) == 0);
        CHECK(!w.commands.dismount(100)); // not mounted -> false
    }

    // ---- target config validity/value copy onto the active selector ----
    // Explicit zero is a real target phrase_set (+0x86C), distinct from the
    // zero-valued unknown target above. The occupied selector rides snapshots and
    // dismount clears both its validity and value.
    test_mount_config_lifecycle();
    test_wire_mount_config_lifecycle();
    test_wire_attach_rejects_unknown_model_bone();
    test_target_loss_clears_zero_net_id_occupant();

    // ---- only Controller/Driver seats publish the vehicle-control lifecycle ----
    {
        const SeatType control_types[] = {SeatType::Controller, SeatType::Driver};
        for (SeatType control_type : control_types) {
            auto world_fixture = std::make_unique<World>();
            World &w = *world_fixture;
            w.registry.configure_pool(0, 16);
            w.registry.configure_pool(1, 16);
            w.registry.spawn(1, make_vehicle(200, control_type));
            w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

            CHECK(w.commands.mount(100, 200));
            CHECK(w.out.effects.entries().size() == 1);
            if (w.out.effects.entries().size() == 1) {
                const auto &started = w.out.effects.entries()[0];
                CHECK(started.kind == "vehicle_control_started");
                CHECK(started.a == 200);
                CHECK(started.b == 77);
                CHECK(started.c == 0x01000003);
            }

            CHECK(w.commands.dismount(100));
            CHECK(w.out.effects.entries().size() == 2);
            if (w.out.effects.entries().size() == 2) {
                const auto &stopped = w.out.effects.entries()[1];
                CHECK(stopped.kind == "vehicle_control_stopped");
                CHECK(stopped.a == 200);
                CHECK(stopped.b == 77);
                CHECK(stopped.c == 0x01000003);
            }
        }
    }

    // A vehicle can expose both Controller and Driver seats. The +368 claim is
    // single-owner, and a control seat refuses while another entity holds it:
    // the second control occupant is not seated at all (no seat, no parent, no
    // started repeat); after the claimant's departure (the stop) it boards and
    // claims afresh (D-NET-398) [orig: Entity_AttachToVehicleSlot @0x4946d0, the
    // refusals @0x4947bf / @0x4948c5; Entity_DetachFromVehicle @0x4356e9
    // claimant-only stop leg].
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle vh = w.registry.spawn(1, make_dual_control_vehicle(200));
        const EntityHandle c0 = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        const EntityHandle c1 = w.registry.spawn(0, make_soldier(101, 0.f, 0.f, 0.f));

        CHECK(w.commands.mount(100, 200));
        CHECK(!w.commands.mount(101, 200)); // the Driver seat refuses: +368 is c0
        CHECK(w.registry.get(c0)->mount_type == SeatType::Controller);
        CHECK(!w.registry.get(c1)->mounted);
        CHECK(w.registry.get(c1)->mount_type == SeatType::None);
        CHECK(!w.registry.get(c1)->mount_target.valid());
        CHECK(w.registry.get(vh)->seats[0].occupant == c0);
        CHECK(!w.registry.get(vh)->seats[1].occupant.valid());
        CHECK(w.registry.get(vh)->primary_occupant == c0);
        CHECK(w.out.effects.entries().size() == 1);
        if (w.out.effects.entries().size() == 1)
            CHECK(w.out.effects.entries()[0].kind == "vehicle_control_started");

        CHECK(w.commands.dismount(100)); // the claimant departs -> stop
        CHECK(w.out.effects.entries().size() == 2);
        if (w.out.effects.entries().size() == 2)
            CHECK(w.out.effects.entries()[1].kind == "vehicle_control_stopped");
        CHECK(w.commands.mount(101, 200)); // +368 empty: c1 boards and claims
        CHECK(w.registry.get(vh)->primary_occupant == c1);
        CHECK(w.out.effects.entries().size() == 3);
        if (w.out.effects.entries().size() == 3)
            CHECK(w.out.effects.entries()[2].kind == "vehicle_control_started");
    }

    // The accepted wire attach/detach path publishes the same controlling-seat edges.
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity vehicle = make_vehicle(200, SeatType::Driver);
        vehicle.seats[0].bone_index = 7;
        const EntityHandle vh = w.registry.spawn(1, vehicle);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

        CHECK(w.vehicles.process_attach(sh, vh, 7));
        CHECK(w.out.effects.entries().size() == 1);
        if (w.out.effects.entries().size() == 1) {
            const auto &started = w.out.effects.entries()[0];
            CHECK(started.kind == "vehicle_control_started");
            CHECK(started.a == 200 && started.b == 77 && started.c == 0x01000003);
        }
        CHECK(w.vehicles.detach(sh));
        CHECK(w.out.effects.entries().size() == 2);
        if (w.out.effects.entries().size() == 2) {
            const auto &stopped = w.out.effects.entries()[1];
            CHECK(stopped.kind == "vehicle_control_stopped");
            CHECK(stopped.a == 200 && stopped.b == 77 && stopped.c == 0x01000003);
        }
    }

    // The wire path refuses the same way. A rider moving from its sitex seat
    // to the claimed control seat has already left the sitex seat, so it ends
    // detached; a body on foot keeps its stance bits, the success-only clear
    // skipped (D-NET-398). [orig: Entity_ProcessVehicleAttach @0x435bce, the
    // clear @0x435c42 under @0x435c40; Entity_AttachToVehicleSlot @0x4948c5 ->
    // @0x494714]
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity dual = make_dual_control_vehicle(200);
        Seat sit;
        sit.type = SeatType::Passenger;
        sit.bone_index = 11;
        dual.seats.push_back(sit);
        const EntityHandle vh = w.registry.spawn(1, dual);
        const EntityHandle c0 = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        const EntityHandle c1 = w.registry.spawn(0, make_soldier(101, 0.f, 0.f, 0.f));
        const EntityHandle c2 = w.registry.spawn(0, make_soldier(102, 0.f, 0.f, 0.f));

        CHECK(w.vehicles.process_attach(c0, vh, 7));
        w.registry.get(c2)->net_stance_bits = 1; // prone
        CHECK(!w.vehicles.process_attach(c2, vh, 9));
        CHECK(!w.registry.get(c2)->mounted && w.registry.get(c2)->net_stance_bits == 1);
        CHECK(w.vehicles.process_attach(c1, vh, 11));
        CHECK(w.registry.get(c1)->mount_type == SeatType::Passenger);
        // Refused, but the relation changed: c1 left its sitex seat first.
        CHECK(w.vehicles.process_attach(c1, vh, 9));
        CHECK(!w.registry.get(c1)->mounted && !w.registry.get(c1)->mount_target.valid());
        CHECK((w.registry.get(c1)->flags & world::kEntityFlagMounted) == 0);
        CHECK(!w.registry.get(vh)->seats[1].occupant.valid());
        CHECK(!w.registry.get(vh)->seats[2].occupant.valid());
        CHECK(w.registry.get(vh)->primary_occupant == c0);
        CHECK(w.out.effects.entries().size() == 1);
        if (w.out.effects.entries().size() == 1)
            CHECK(w.out.effects.entries()[0].kind == "vehicle_control_started");

        CHECK(w.vehicles.detach(c0)); // the claimant departs -> stop
        CHECK(w.out.effects.entries().size() == 2);
        if (w.out.effects.entries().size() == 2)
            CHECK(w.out.effects.entries()[1].kind == "vehicle_control_stopped");
        CHECK(!w.vehicles.detach(c1)); // c1 left its seat with the refusal
        CHECK(w.out.effects.entries().size() == 2);
    }

    // A passenger is occupancy, not vehicle control; mount, dismount, and restore stay silent.
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.spawn(1, make_vehicle(200, SeatType::Passenger));
        w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        CHECK(w.commands.mount(100, 200));
        CHECK(w.out.effects.entries().empty());
        const auto occupied = std::make_unique<World::Snapshot>(w.snapshot());
        CHECK(w.commands.dismount(100));
        CHECK(w.out.effects.entries().empty());
        w.restore(*occupied);
        CHECK(w.out.effects.entries().empty());
    }

    // Teardown can remove the vehicle before the occupant is detached. The occupant's cached
    // target identity still has to publish the matching stop edge.
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle vh = w.registry.spawn(1, make_vehicle(200, SeatType::Controller));
        w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        CHECK(w.commands.mount(100, 200));
        w.out.effects.clear();
        w.registry.despawn(vh);

        CHECK(w.commands.dismount(100));
        CHECK(w.out.effects.entries().size() == 1);
        if (w.out.effects.entries().size() == 1) {
            const auto &stopped = w.out.effects.entries()[0];
            CHECK(stopped.kind == "vehicle_control_stopped");
            CHECK(stopped.a == 200);
            CHECK(stopped.b == 77);
            CHECK(stopped.c == 0x01000003);
        }
    }

    // ---- edges: full seat, already-mounted, seatless target ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.spawn(1, make_gun(200, 0.f, 0.f, 0.f, 0));
        w.registry.spawn(1, make_gun(201, 50.f, 50.f, 0.f, 0));
        Entity seatless;
        seatless.net_id = 202;
        seatless.kind = EntityKind::Item;
        w.registry.spawn(1, seatless);
        w.registry.spawn(0, make_soldier(100, 1.f, 0.f, 0.f));
        w.registry.spawn(0, make_soldier(101, 2.f, 0.f, 0.f));

        CHECK(w.commands.mount(100, 200));   // takes the only seat
        CHECK(!w.commands.mount(101, 200));  // gun full
        CHECK(!w.commands.mount(100, 201));  // 100 already mounted
        CHECK(!w.commands.mount(101, 202));  // target has no seats
        CHECK(!w.commands.mount(101, 999));  // missing target
        CHECK(w.commands.mount(101, 201));   // a free gun works
    }

    // ---- mounted seat local rotates in the same frame as placed vehicle models ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        // Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
        vehicle.item_attrib = world::kItemAttribPlayerControl;
        vehicle.position = {100.f, 200.f, 7.f};
        vehicle.yaw = -90;
        Seat control;
        control.type = SeatType::Controller;
        control.seat_local = {-0.7148895f, -0.1189880f, 2.1048889f};
        vehicle.seats.push_back(control);
        w.registry.spawn(1, vehicle);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

        CHECK(w.commands.mount(100, 200));
        Entity *occ = w.registry.get(sh);
        CHECK(occ->mounted);
        CHECK_NEAR(occ->position.x, 100.1189880, 0.0001);
        CHECK_NEAR(occ->position.y, 199.2851105, 0.0001);
        CHECK_NEAR(occ->position.z, 9.1048889, 0.0001);
    }

    // ---- AI tick seat-follow: occupant tracks the seat, never path-follows ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 10.f, 20.f, 5.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        AiSystem &ai = w.ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;
        ai.at(idx)->inf.clip_phase = 42;
        CHECK(w.commands.mount(100, 200));

        TickContext ctx{};
        ctx.is_authority = true;
        w.update_all_entities(ctx);
        AiEntity *ae = ai.at(idx);
        CHECK(ae->pos[0] == to_fixed(10.0));
        CHECK(ae->pos[1] == to_fixed(20.0));
        CHECK(ae->pos[2] == to_fixed(5.0));
        CHECK(ae->inf.anim_state == 67); // anim_emplaced request
        CHECK(ae->inf.clip_phase == 42); // data-less body keeps its current channel

        // Move the gun -> the gunner follows next tick.
        w.registry.get(gh)->position = {30.f, 40.f, 5.f};
        w.update_all_entities(ctx);
        CHECK(ae->pos[0] == to_fixed(30.0));
        CHECK(ae->pos[1] == to_fixed(40.0));

        // Even with a locomotion target set, a mounted gunner does NOT path-follow.
        ae->brain.f[AiBrain::kOutSpeed] = 9999;
        ae->brain.f[AiBrain::kWorkPosX] = to_fixed(999.0);
        w.update_all_entities(ctx);
        CHECK(ae->pos[0] == to_fixed(30.0)); // still seated, not moved toward 999

        // Vehicle gone -> auto-dismount, occupant resumes normal AI.
        w.registry.despawn(gh);
        w.update_all_entities(ctx);
        CHECK(!w.registry.get(sh)->mounted);
    }

    // ---- mounted pose selection follows seat context, not just "is mounted" ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 0.f, 0.f, 0.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        w.registry.get(gh)->emplaced_config_valid = true;
        w.registry.get(gh)->emplaced_config = 3;

        AiSystem &ai = w.ai;
        ClipSource clips;
        ai.root_motion = &clips;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.adm_id = 12;

        CHECK(w.commands.mount(100, 200));
        TickContext ctx{};
        ctx.is_authority = true;
        w.update_all_entities(ctx);
        CHECK(ai.at(idx)->inf.anim_state == 67); // variant configured, but clip missing -> base

        clips.available_state = 70; // emplaced_4 = base 67 + variant 3
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;
        w.update_all_entities(ctx);
        CHECK(ai.at(idx)->inf.anim_state == 70);
    }

    // ---- mounted seat orientation feeds the carried body while gunner look stays live ----
    // The unmoved gun still holds its placement angles in the spawn form (low half zero):
    // heading ((90 - 37) << 16) / 360 << 16 = 632291328, which the gunner's 11-degree seat
    // offset turns to 763526440; pitch -12 and roll 17 take the same form. The attach
    // pre-snap faces the gunner along the gun, the gun's heading less its stored yaw word
    // (zero here), so the request heading is the gun's own 632291328.
    // [orig: seat carry @0x4b654e-0x4b6575; Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6;
    //  Entity_RequestVehicleAttach @0x43655F..0x43656E]
    [] {
        constexpr int32_t kSeatHeading = 763526440;
        constexpr int32_t kSeatPitch = -143130624;
        constexpr int32_t kSeatRoll = 202768384;
        constexpr int32_t kRequestHeading = 632291328;
        const int32_t kLookHeading = world::bam_heading_from_mission_yaw_deg(80.0);

        auto wp = std::make_unique<World>();
        World &w = *wp;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity gun = make_gun(200, 10.f, 20.f, 5.f, 37);
        gun.pitch = -12;
        gun.roll = 17;
        gun.seats[0].yaw_offset = 11;
        w.registry.spawn(1, gun);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

        AiSystem &ai = w.ai;
        const int idx = ai.attach(sh);
        AiEntity *ae = ai.at(idx);
        ae->inf.active = true;
        ae->inf.body_heading = 0x11111111;
        ae->inf.leg_yaw[0] = 0x22222222;
        ae->inf.leg_yaw[1] = 0x33333333;
        ae->inf.leg_target[0] = 0x44444444;
        ae->inf.leg_target[1] = 0x55555555;
        ae->heading = kLookHeading;
        ae->body_pitch = 0x12345678;
        ae->roll = 0x23456789;

        CHECK(w.commands.mount(100, 200));
        CHECK(ae->heading == kRequestHeading); // request pre-snap, before live look resumes
        ae->heading = ae->inf.aim_heading = kLookHeading; // live and desired post-attach gaze
        CHECK(ai.pose_if_mounted(*ae, w));
        const Entity *occ = w.registry.get(sh);
        CHECK(occ != nullptr);
        // After Entity_RequestVehicleAttach establishes the seat base, later UseGun
        // look updates remain independent while the body stays in the carried frame.
        // [orig: request pre-snap @0x4364a0; UseGun save/restore @0x546416..0x546664]
        CHECK(occ->yaw == 80);
        CHECK(occ->pitch == -12);
        CHECK(occ->roll == 17);
        CHECK(ae->heading == kLookHeading);
        CHECK(ae->inf.body_heading == kSeatHeading);
        CHECK(ae->inf.leg_yaw[0] == kSeatHeading);
        CHECK(ae->inf.leg_yaw[1] == kSeatHeading);
        CHECK(ae->inf.leg_target[0] == kSeatHeading);
        CHECK(ae->inf.leg_target[1] == kSeatHeading);
        CHECK(ae->body_pitch == kSeatPitch);
        CHECK(ae->roll == kSeatRoll);
    }();

    // ---- a mounted local player's look stays independent of the carried seat body ----
    // Capture the seat frame before replacing Entity.yaw with the rounded player look mirror.
    // The AiEntity camera mirrors retain the full-precision look heading/pitch; the body,
    // both leg chains, body pitch, and roll remain attached to the seat.
    [] {
        constexpr int32_t kSeatHeading = 763526440;
        constexpr int32_t kSeatPitch = -143130624;
        constexpr int32_t kSeatRoll = 202768384;
        constexpr int32_t request_heading = 632291328; // the pre-snap, as above
        const int32_t look_heading = world::bam_heading_from_mission_yaw_deg(137.25);
        constexpr int32_t kLookPitch = 12345678;

        auto wp = std::make_unique<World>();
        World &w = *wp;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity gun = make_gun(200, 10.f, 20.f, 5.f, 37);
        gun.pitch = -12;
        gun.roll = 17;
        gun.seats[0].yaw_offset = 11;
        w.registry.spawn(1, gun);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));

        AiSystem &ai = w.ai;
        const int idx = ai.attach(sh);
        AiEntity *ae = ai.at(idx);
        ae->inf.active = true;
        ae->inf.is_local_player = true;
        ae->inf.target_heading = look_heading;
        ae->inf.look_pitch = kLookPitch;
        ae->inf.body_heading = 0x11111111;
        ae->inf.leg_yaw[0] = 0x22222222;
        ae->inf.leg_yaw[1] = 0x33333333;
        ae->inf.leg_target[0] = 0x44444444;
        ae->inf.leg_target[1] = 0x55555555;
        ae->body_pitch = 0x12345678;
        ae->roll = 0x23456789;

        CHECK(w.commands.mount(100, 200));
        CHECK(ae->heading == request_heading);
        CHECK(ae->inf.target_heading == request_heading);
        ae->inf.target_heading = look_heading; // first post-attach mouse look
        CHECK(ai.pose_if_mounted(*ae, w));
        const Entity *occ = w.registry.get(sh);
        CHECK(occ != nullptr);
        CHECK(occ->yaw == 137); // degree-rounded registry/wire/motor look mirror
        CHECK(occ->pitch == -12);
        CHECK(occ->roll == 17);
        CHECK(ae->heading == look_heading); // full precision, never the seat heading
        CHECK(ae->pitch == kLookPitch);
        CHECK(ae->inf.target_heading == look_heading);
        CHECK(ae->heading != kSeatHeading);
        CHECK(ae->pitch != kSeatPitch);
        CHECK(ae->inf.body_heading == kSeatHeading);
        CHECK(ae->inf.leg_yaw[0] == kSeatHeading);
        CHECK(ae->inf.leg_yaw[1] == kSeatHeading);
        CHECK(ae->inf.leg_target[0] == kSeatHeading);
        CHECK(ae->inf.leg_target[1] == kSeatHeading);
        CHECK(ae->body_pitch == kSeatPitch);
        CHECK(ae->roll == kSeatRoll);
    }();

    // ---- non-gunner seats carry a local facing offset, not just vehicle yaw ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.position = {10.f, 20.f, 5.f};
        vehicle.yaw = 15;
        Seat passenger;
        passenger.type = SeatType::Passenger;
        passenger.yaw_offset = 90;
        passenger.pose_index = 24;
        vehicle.seats.push_back(passenger);
        const EntityHandle vh = w.registry.spawn(1, vehicle);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        AiSystem &ai = w.ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;

        CHECK(w.commands.mount(100, 200));
        Entity *occ = w.registry.get(sh);
        CHECK(occ->mounted);
        CHECK(occ->mount_target == vh);
        CHECK(occ->yaw == 105);

        TickContext ctx{};
        ctx.is_authority = true;
        w.update_all_entities(ctx);
		CHECK(ai.at(idx)->inf.anim_state == 107); // sit_24 selects its stationary driver pose
	}

	// ---- unmounted stance stays under normal infantry logic, not mount pose logic ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        AiSystem ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;
        CHECK(!ai.pose_if_mounted(*ai.at(idx), w));
        CHECK(ai.at(idx)->inf.anim_state == world::anim_state::kIdleCrouch);
    }

    // ---- BMS AttachToEmplaced: the occupant boards the vehicle its AI slot
    // +0x90 (slot[36]) names, however far; an occupant without one boards
    // nothing, even beside a free gun. Emits no unported_action.
    // [orig: EventAction_Dispatch case 37 @0x454989 ->
    //  WacScript_TryMountEntityToVehicle @0x4F70F0] ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        Entity far_gun = make_gun(200, 60.f, 60.f, 0.f, 0);
        far_gun.item_type_index = 1;
        Entity near_gun = make_gun(201, 2.f, 2.f, 0.f, 0);
        near_gun.item_type_index = 1;
        w.registry.spawn(1, far_gun);
        w.registry.spawn(1, near_gun);
        Entity ordered = make_soldier(100, 1.f, 1.f, 0.f);
        ordered.item_type_index = 1;
        Entity idle = make_soldier(101, 1.f, 2.f, 0.f);
        idle.item_type_index = 1;
        const EntityHandle ordered_h = w.registry.spawn(0, ordered);
        const EntityHandle idle_h = w.registry.spawn(0, idle);
        w.ai.attach(ordered_h);
        w.ai.attach(idle_h);
        // The ssn2ssn board order arms slot[36] with the far gun.
        CHECK(w.commands.order_boarding(100, 200));

        bms::Event e{};
        e.flags = bms::EventFlags::None;
        e.trigger_count = 0; // unconditional -> fires
        e.action_index = 0;
        e.action_count = 2;
        bms::Action act{};
        act.action_type = bms::ActionType::AttachToEmplaced;
        act.param1 = 100; // occupant SSN (the only param the action carries)
        bms::Action idle_act = act;
        idle_act.param1 = 101;
        mission::BmsEventSystem bms_sys;
        bms_sys.load({e}, {}, {act, idle_act});
        w.add_system(&bms_sys);
        w.load_systems();
        // A normal event's first processing pass is the 16th tick (quarter-list
        // round-robin; see event_runtime_test).
        for (int i = 0; i < 16; ++i) w.run_logic_tick(true);

        CHECK(w.out.effects.count("unported_action") == 0);
        CHECK(w.commands.find_mounted_on(200) == 100);
        CHECK(w.commands.find_mounted_on(201) == 0);
        CHECK(!w.registry.get(idle_h)->mounted);
    }

    // ---- snapshot/restore rewinds the mount (seats ride the registry value-copy) ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle gh = w.registry.spawn(1, make_gun(200, 1.f, 1.f, 0.f, 0));
        const EntityHandle sh = w.registry.spawn(0, make_soldier(100, 2.f, 1.f, 0.f));

        const auto snap = std::make_unique<World::Snapshot>(w.snapshot()); // pre-mount
        CHECK(w.commands.mount(100, 200));
        CHECK(w.registry.get(sh)->mounted);
        CHECK(w.registry.get(gh)->seats[0].occupant.valid());

        w.restore(*snap);
        CHECK(!w.registry.get(sh)->mounted);            // rewound
        CHECK(!w.registry.get(gh)->seats[0].occupant.valid()); // seat freed
    }

    // Restoring occupied control seats republishes one per-vehicle lifecycle edge so
    // embedders can rebuild effects that were cleared with the transient EffectLog.
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle vh = w.registry.spawn(1, make_dual_control_vehicle(200));
        const EntityHandle sh0 = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        const EntityHandle sh1 = w.registry.spawn(0, make_soldier(101, 0.f, 0.f, 0.f));
        CHECK(w.commands.mount(100, 200));
        CHECK(!w.commands.mount(101, 200)); // the second control seat refuses (D-NET-398)
        const auto occupied = std::make_unique<World::Snapshot>(w.snapshot());
        CHECK(w.commands.dismount(100));

        w.restore(*occupied);
        CHECK(w.registry.get(sh0)->mounted);
        CHECK(!w.registry.get(sh1)->mounted);
        CHECK(w.registry.get(vh)->seats[0].occupant == sh0);
        CHECK(!w.registry.get(vh)->seats[1].occupant.valid());
        CHECK(w.out.effects.entries().size() == 1);
        if (w.out.effects.entries().size() == 1) {
            const auto &started = w.out.effects.entries()[0];
            CHECK(started.kind == "vehicle_control_started");
            CHECK(started.a == 200);
            CHECK(started.b == 77);
            CHECK(started.c == 0x01000003);
        }
    }

    if (failures == 0) std::printf("mount_test: OK\n");
    else std::printf("mount_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
