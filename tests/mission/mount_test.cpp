// Emplacement / mount (Phase 2) tests: the seat model + EntityCommands mount primitives
// (mount/mount_best/dismount/find_best_seat/find_mounted_on), the per-tick AI seat-follow,
// the BMS AttachToEmplaced action path (emits no unported_action), and snapshot/restore
// rewinding the mount. [orig chain: EventAction_Dispatch case 0x25 @0x4542e0 ->
// WacScript_TryMountEntityToVehicle @0x4f70f0 -> Entity_FindBestSeatSlot @0x4351f0.]
#include <cmath>
#include <cstdio>
#include <memory>

#include "mission/event_runtime.h"
#include "mission/bms.h"
#include "world/ai.h"
#include "world/angle.h"
#include "world/vehicle_attach.h"
#include "world/world.h"

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

    int32_t clip_length_ticks(int /*adm_id*/, int /*state_id*/) const override { return -1; }

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
    w.weapons.entries.resize(2);
    w.weapons.entries[1].name.assign(1, 'x');
    w.weapons.entries[1].clipsize = -1;
    w.weapons.entries[1].valid = true;
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

    CHECK(entity_process_vehicle_attach(w, sh, gh, 7));
    CHECK(w.registry.get(sh)->mounted_config_valid);
    CHECK(w.registry.get(sh)->mounted_config == 6);
    CHECK(entity_detach_from_vehicle(w, sh));
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

    CHECK(!entity_process_vehicle_attach(w, sh, gh, 9));
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

    auto ai_fixture = std::make_unique<AiSystem>();
    AiSystem &ai = *ai_fixture;
    w.ai = &ai;
    const int mounted_ai_index = ai.attach(mounted);
    AiEntity *mounted_ai = ai.at(mounted_ai_index);

    CHECK(entity_process_vehicle_attach(w, mounted, target, 7));
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
    CHECK(entity_process_vehicle_attach(w, mounted, replacement_target, 7));
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
            CHECK(w.effects.entries().size() == 1);
            if (w.effects.entries().size() == 1) {
                const auto &started = w.effects.entries()[0];
                CHECK(started.kind == "vehicle_control_started");
                CHECK(started.a == 200);
                CHECK(started.b == 77);
                CHECK(started.c == 0x01000003);
            }

            CHECK(w.commands.dismount(100));
            CHECK(w.effects.entries().size() == 2);
            if (w.effects.entries().size() == 2) {
                const auto &stopped = w.effects.entries()[1];
                CHECK(stopped.kind == "vehicle_control_stopped");
                CHECK(stopped.a == 200);
                CHECK(stopped.b == 77);
                CHECK(stopped.c == 0x01000003);
            }
        }
    }

    // A vehicle can expose both Controller and Driver seats. The +368 claim is
    // single-owner: the second control occupant never claims (no started repeat), the
    // claimant's departure runs the stop leg even while the other controller remains
    // seated, and the survivor does not inherit the claim [orig:
    // Entity_AttachToVehicleSlot @0x4946d0 empty-or-same claim;
    // Entity_DetachFromVehicle @0x4356e9 claimant-only stop leg].
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle vh = w.registry.spawn(1, make_dual_control_vehicle(200));
        const EntityHandle c0 = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        const EntityHandle c1 = w.registry.spawn(0, make_soldier(101, 0.f, 0.f, 0.f));

        CHECK(w.commands.mount(100, 200));
        CHECK(w.commands.mount(101, 200));
        CHECK(w.registry.get(c0)->mount_type == SeatType::Controller);
        CHECK(w.registry.get(c1)->mount_type == SeatType::Driver);
        CHECK(w.registry.get(vh)->seats[0].occupant == c0);
        CHECK(w.registry.get(vh)->seats[1].occupant == c1);
        CHECK(w.effects.entries().size() == 1);
        if (w.effects.entries().size() == 1)
            CHECK(w.effects.entries()[0].kind == "vehicle_control_started");

        CHECK(w.commands.dismount(100)); // the claimant departs -> stop, c1 still seated
        CHECK(w.effects.entries().size() == 2);
        if (w.effects.entries().size() == 2)
            CHECK(w.effects.entries()[1].kind == "vehicle_control_stopped");
        CHECK(w.commands.dismount(101)); // the non-claimant departure is silent
        CHECK(w.effects.entries().size() == 2);
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

        CHECK(entity_process_vehicle_attach(w, sh, vh, 7));
        CHECK(w.effects.entries().size() == 1);
        if (w.effects.entries().size() == 1) {
            const auto &started = w.effects.entries()[0];
            CHECK(started.kind == "vehicle_control_started");
            CHECK(started.a == 200 && started.b == 77 && started.c == 0x01000003);
        }
        CHECK(entity_detach_from_vehicle(w, sh));
        CHECK(w.effects.entries().size() == 2);
        if (w.effects.entries().size() == 2) {
            const auto &stopped = w.effects.entries()[1];
            CHECK(stopped.kind == "vehicle_control_stopped");
            CHECK(stopped.a == 200 && stopped.b == 77 && stopped.c == 0x01000003);
        }
    }

    // The wire path obeys the same single-owner claim for dual control seats.
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        const EntityHandle vh = w.registry.spawn(1, make_dual_control_vehicle(200));
        const EntityHandle c0 = w.registry.spawn(0, make_soldier(100, 0.f, 0.f, 0.f));
        const EntityHandle c1 = w.registry.spawn(0, make_soldier(101, 0.f, 0.f, 0.f));

        CHECK(entity_process_vehicle_attach(w, c0, vh, 7));
        CHECK(entity_process_vehicle_attach(w, c1, vh, 9));
        CHECK(w.effects.entries().size() == 1);
        if (w.effects.entries().size() == 1)
            CHECK(w.effects.entries()[0].kind == "vehicle_control_started");

        CHECK(entity_detach_from_vehicle(w, c0)); // the claimant departs -> stop
        CHECK(w.effects.entries().size() == 2);
        if (w.effects.entries().size() == 2)
            CHECK(w.effects.entries()[1].kind == "vehicle_control_stopped");
        CHECK(entity_detach_from_vehicle(w, c1)); // the non-claimant departure is silent
        CHECK(w.effects.entries().size() == 2);
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
        CHECK(w.effects.entries().empty());
        const auto occupied = std::make_unique<World::Snapshot>(w.snapshot());
        CHECK(w.commands.dismount(100));
        CHECK(w.effects.entries().empty());
        w.restore(*occupied);
        CHECK(w.effects.entries().empty());
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
        w.effects.clear();
        w.registry.despawn(vh);

        CHECK(w.commands.dismount(100));
        CHECK(w.effects.entries().size() == 1);
        if (w.effects.entries().size() == 1) {
            const auto &stopped = w.effects.entries()[0];
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
        AiSystem ai;
        w.ai = &ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;
        ai.at(idx)->inf.clip_phase = 42;
        CHECK(w.commands.mount(100, 200));

        TickContext ctx{};
        ctx.is_authority = true;
        ai.tick(w, ctx);
        AiEntity *ae = ai.at(idx);
        CHECK(ae->pos[0] == to_fixed(10.0));
        CHECK(ae->pos[1] == to_fixed(20.0));
        CHECK(ae->pos[2] == to_fixed(5.0));
        CHECK(ae->inf.anim_state == 67); // anim_emplaced
        CHECK(ae->inf.clip_phase == 0);

        // Move the gun -> the gunner follows next tick.
        w.registry.get(gh)->position = {30.f, 40.f, 5.f};
        ai.tick(w, ctx);
        CHECK(ae->pos[0] == to_fixed(30.0));
        CHECK(ae->pos[1] == to_fixed(40.0));

        // Even with a locomotion target set, a mounted gunner does NOT path-follow.
        ae->brain.f[AiBrain::kOutSpeed] = 9999;
        ae->brain.f[AiBrain::kWorkPosX] = to_fixed(999.0);
        ai.tick(w, ctx);
        CHECK(ae->pos[0] == to_fixed(30.0)); // still seated, not moved toward 999

        // Vehicle gone -> auto-dismount, occupant resumes normal AI.
        w.registry.despawn(gh);
        ai.tick(w, ctx);
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

        AiSystem ai;
        ClipSource clips;
        ai.root_motion = &clips;
        w.ai = &ai;
        const int idx = ai.attach(sh);
        ai.at(idx)->net_id = 100;
        ai.at(idx)->inf.active = true;
        ai.at(idx)->inf.adm_id = 12;

        CHECK(w.commands.mount(100, 200));
        TickContext ctx{};
        ctx.is_authority = true;
        ai.tick(w, ctx);
        CHECK(ai.at(idx)->inf.anim_state == 67); // variant configured, but clip missing -> base

        clips.available_state = 70; // emplaced_4 = base 67 + variant 3
        ai.at(idx)->inf.anim_state = world::anim_state::kIdleCrouch;
        ai.tick(w, ctx);
        CHECK(ai.at(idx)->inf.anim_state == 70);
    }

    // ---- mounted seat orientation feeds the carried body while gunner look stays live ----
    // The seat is authored in mission degrees, while every AiEntity yaw field is engine BAM:
    // heading = (90 - mission yaw) * 11930464. For a gunner, 37 - 11 = 26 degrees,
    // so the witnessed integer convention produces (90 - 26) * 11930464 = 763549696.
    // [orig: seat carry @0x4b654e-0x4b6575; heading multiplier @0x4659fa]
    [] {
        constexpr int32_t kSeatHeading = 763549696;
        constexpr int32_t kSeatPitch = -143165577;
        constexpr int32_t kSeatRoll = 202817900;
        const int32_t kRequestHeading =
                world::bam_heading_from_mission_yaw_deg(26.0);
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

        auto aip = std::make_unique<AiSystem>();
        AiSystem &ai = *aip;
        w.ai = &ai;
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
        ae->heading = kLookHeading;            // first post-attach NPC look update
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
        constexpr int32_t kSeatHeading = 763549696;
        constexpr int32_t kSeatPitch = -143165577;
        constexpr int32_t kSeatRoll = 202817900;
        const int32_t request_heading =
                world::bam_heading_from_mission_yaw_deg(26.0);
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

        auto aip = std::make_unique<AiSystem>();
        AiSystem &ai = *aip;
        w.ai = &ai;
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
        AiSystem ai;
        w.ai = &ai;
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
        ai.tick(w, ctx);
        CHECK(ai.at(idx)->inf.anim_state == 100); // anim_sit_24
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

    // ---- BMS AttachToEmplaced: emits no unported_action + mounts via proximity ----
    {
        auto world_fixture = std::make_unique<World>();
        World &w = *world_fixture;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.spawn(1, make_gun(200, 1.f, 1.f, 0.f, 0));
        w.registry.spawn(0, make_soldier(100, 2.f, 1.f, 0.f)); // within kMountRadius

        bms::Event e{};
        e.flags = bms::EventFlags::None;
        e.trigger_count = 0; // unconditional -> fires
        e.action_index = 0;
        e.action_count = 1;
        bms::Action act{};
        act.action_type = bms::ActionType::AttachToEmplaced;
        act.param1 = 100; // occupant SSN (the only param the action carries)
        mission::BmsEventSystem bms_sys;
        bms_sys.load({e}, {}, {act});
        w.add_system(&bms_sys);
        w.load_systems();
        // A normal event's first processing pass is the 16th tick (quarter-list
        // round-robin; see event_runtime_test).
        for (int i = 0; i < 16; ++i) w.run_logic_tick(true);

        CHECK(w.effects.count("unported_action") == 0);
        CHECK(w.commands.find_mounted_on(200) == 100);
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
        CHECK(w.commands.mount(101, 200));
        const auto occupied = std::make_unique<World::Snapshot>(w.snapshot());
        CHECK(w.commands.dismount(100));
        CHECK(w.commands.dismount(101));

        w.restore(*occupied);
        CHECK(w.registry.get(sh0)->mounted);
        CHECK(w.registry.get(sh1)->mounted);
        CHECK(w.registry.get(vh)->seats[0].occupant == sh0);
        CHECK(w.registry.get(vh)->seats[1].occupant == sh1);
        CHECK(w.effects.entries().size() == 1);
        if (w.effects.entries().size() == 1) {
            const auto &started = w.effects.entries()[0];
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
