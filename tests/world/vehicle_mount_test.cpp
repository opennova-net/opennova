// The USE-ITEM mount chain + the BMS Player mount triggers + the AI-driver leg
// (the vehicle pass, 2026-07-16 witness):
//  - Entity_ToggleVehicleMount @0x436950 / Entity_TryEnterNearestVehicle @0x4368c0 /
//    Entity_FindNearestSeatOrArmory @0x435d50 / Entity_FindBestSeatSlot @0x4351f0
//  - EventTrigger cat-7 subs 38-41 -> @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0
//  - Entity_UpdateVehiclePhysics @0x48af00: parked stamp @0x48c002-0x48c02d + the
//    AI-driver leg @0x48bc12-0x48c034
//  - the player deploy group stamp @0x519fd0 (commandGroup = 1)
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

VehicleTraits truck_traits() {
    // DTruck1's authored block (items.def id 101294), pre-scaled per
    // ItemDef_ParsePhysicsProperty @0x49d870.
    VehicleTraits t;
    t.physics = 1;
    t.player_speed = 64 * 293;
    t.acceleration = 10 * 4;
    t.deceleration = 20 * 4;
    t.turn_rate = 45 * 192426;
    t.turn_rate2 = 30 * 192426;
    t.player_control = true;
    return t;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    AiSystem sys;
    EntityHandle veh_h, player_h;

    explicit Rig(float player_dx = 2.0f) {
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.ai = &sys;

        Entity veh;
        veh.net_id = 11; // 00TRa's DTruck1 SSN
        veh.bms_id = 11;
        veh.kind = EntityKind::Item;
        veh.has_item_def = true; // pool-1 sources need an ItemDef (retail gate)
        veh.item_id = 1294;
        veh.position = {100.0f, 200.0f, 10.0f};
        veh.yaw = 0;
        veh.health = 2000;
        veh.health_max = 2000;
        veh.alive = true;
        Seat ctrl;
        ctrl.type = SeatType::Controller;
        ctrl.bone_index = 1;
        ctrl.source_name = "ctrlx00";
        ctrl.seat_local = {0.5f, 1.5f, 1.0f};
        veh.seats.push_back(ctrl);
        Seat sit;
        sit.type = SeatType::Passenger;
        sit.bone_index = 2;
        sit.source_name = "sitex00";
        sit.seat_local = {0.0f, -2.0f, 1.2f};
        veh.seats.push_back(sit);
        veh_h = w.registry.spawn(1, veh);

        Entity pl;
        pl.kind = EntityKind::Organic;
        pl.item_id = 5305;
        pl.player_class = 8;
        pl.group_id = 1; // the deploy stamp [orig: @0x519fd0]
        pl.position = {100.0f + player_dx, 200.0f, 10.0f};
        pl.health = 150;
        pl.health_max = 150;
        pl.alive = true;
        player_h = w.registry.spawn(0, pl);
        w.cached.local_player = player_h;
    }
    Entity &veh() { return *w.registry.get(veh_h); }
    Entity &player() { return *w.registry.get(player_h); }
};

struct FakeMountedPoseProvider final : IPoseProvider {
    bool available = true;
    MountedPose pose;

    bool resolve_mounted_pose(World &, const Entity &, const Seat &,
                              MountedPose &out) override {
        if (!available) return false;
        out = pose;
        return true;
    }
};

struct StubCollisionMatrixProvider final : IPoseProvider {
    int calls = 0;

    bool build_section_matrices(World &, EntityHandle, int32_t,
                                const CollisionMatrix &entity_world,
                                const CollisionModel &model,
                                std::vector<CollisionMatrix> &out) override {
        ++calls;
        out.assign(model.sections.size(), entity_world);
        return true;
    }
};

struct HeadingMountedPoseProvider final : IPoseProvider {
    std::vector<int32_t> headings;
    std::vector<int32_t> pitches;

    bool resolve_mounted_pose(World &world, const Entity &carrier, const Seat &,
                              MountedPose &out) override {
        if (world.ai == nullptr || !carrier.primary_weapon_owner.valid()) return false;
        const AiEntity *gunner = world.ai->for_handle(carrier.primary_weapon_owner);
        if (gunner == nullptr) return false;
        headings.push_back(gunner->heading);
        pitches.push_back(gunner->pitch);
        const float heading_steps = static_cast<float>(gunner->heading) / 16777216.0f;
        const float pitch_steps = static_cast<float>(gunner->pitch) / 16777216.0f;
        out.position = {heading_steps, pitch_steps, 41.0f};
        out.yaw = static_cast<int16_t>(10 + std::lround(heading_steps));
        out.pitch = static_cast<int16_t>(20 + std::lround(pitch_steps));
        out.roll = -30;
        return true;
    }

    void clear() {
        headings.clear();
        pitches.clear();
    }
};

void check_pose(const Entity &entity, const MountedPose &expected) {
    CHECK(std::abs(entity.position.x - expected.position.x) < 0.0001f);
    CHECK(std::abs(entity.position.y - expected.position.y) < 0.0001f);
    CHECK(std::abs(entity.position.z - expected.position.z) < 0.0001f);
    CHECK(entity.yaw == expected.yaw);
    CHECK(entity.pitch == expected.pitch);
    CHECK(entity.roll == expected.roll);
}

// Entity_RequestVehicleAttach pre-snaps the requester's Yaw to the chosen seat
// before the authority applies the relationship. UseGun subtracts its authored
// offset. Our split local-player body must update target_heading too; otherwise
// pose_if_mounted immediately restores the pre-attach look and swings the gun/body
// overlay away from the authored neutral pose.
// [orig: Entity_RequestVehicleAttach @0x4364a0, its UseGun leg @0x43656c]
void test_usegun_attach_presnaps_local_look() {
    World w;
    AiSystem ai;
    w.ai = &ai;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);

    Entity gun;
    gun.kind = EntityKind::Item;
    gun.health = 100;
    gun.alive = true;
    gun.yaw = 35;
    Seat usegun;
    usegun.type = SeatType::Gunner;
    usegun.bone_index = 6;
    usegun.source_name = "UseGun";
    usegun.yaw_offset = 12;
    gun.seats.push_back(usegun);
    const EntityHandle gun_h = w.registry.spawn(1, gun);

    Entity player;
    player.kind = EntityKind::Organic;
    player.player_class = 8;
    player.health = 100;
    player.alive = true;
    const EntityHandle player_h = w.registry.spawn(0, player);
    w.cached.local_player = player_h;
    const int ai_index = ai.attach(player_h);
    AiEntity &body = *ai.at(ai_index);
    body.health = 100;
    body.inf.active = true;
    body.inf.is_local_player = true;
    body.heading = bam_heading_from_mission_yaw_deg(160.0);
    body.inf.target_heading = body.heading;
    body.inf.look_pitch = 0x12345678;

    CHECK(entity_process_vehicle_attach(w, player_h, gun_h, 6));
    const int16_t expected_yaw = 23; // vehicle yaw 35 - UseGun offset 12
    const int32_t expected_heading =
            bam_heading_from_mission_yaw_deg(expected_yaw);
    CHECK(w.registry.get(player_h)->yaw == expected_yaw);
    CHECK(body.heading == expected_heading);
    CHECK(body.inf.target_heading == expected_heading);
    CHECK(body.inf.look_pitch == 0x12345678); // request pre-snap is yaw-only

    CHECK(ai.pose_if_mounted(body, w));
    CHECK(body.heading == expected_heading);
    CHECK(body.inf.body_heading == (90 - expected_yaw) * 11930464);
}

// A remote player's LOOK is still independent in either vehicle-control seat.
// The authority motor consumes Entity::yaw as the mouse-steer target, while the
// carried body frame remains owned by the seat. The post-motor refresh must keep
// that split intact for the outgoing snapshot and the next authority tick.
void test_remote_player_control_seat_preserves_wire_look() {
    const SeatType control_types[] = {SeatType::Driver, SeatType::Controller};
    for (const SeatType control_type : control_types) {
        World w;
        AiSystem ai;
        w.ai = &ai;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);

        Entity vehicle;
        vehicle.kind = EntityKind::Item;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.yaw = 0;
        vehicle.health = 100;
        vehicle.alive = true;
        Seat seat;
        seat.type = control_type;
        seat.bone_index = 1;
        seat.seat_local = {1.0f, 2.0f, 3.0f};
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);

        Entity player;
        player.kind = EntityKind::Organic;
        player.player_class = 8;
        player.health = 100;
        player.alive = true;
        const EntityHandle player_h = w.registry.spawn(0, player);
        const int ai_index = ai.attach(player_h);
        AiEntity &body = *ai.at(ai_index);
        body.inf.active = true;
        body.net_is_remote_peer = true;

        CHECK(entity_process_vehicle_attach(w, player_h, vehicle_h, 1));

        const int32_t look_heading = bam_heading_from_mission_yaw_deg(45.0);
        constexpr int32_t look_pitch = 0x06000000;
        body.heading = look_heading;
        body.pitch = look_pitch;
        w.registry.get(player_h)->yaw = 45;

        CHECK(ai.pose_if_mounted(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->yaw == 45);
        CHECK(body.inf.body_heading == 90 * 11930464);

        Entity *live_vehicle = w.registry.get(vehicle_h);
        live_vehicle->position = {40.0f, 50.0f, 60.0f};
        live_vehicle->yaw = 20;
        CHECK(ai.refresh_mounted_pose(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->yaw == 45);
        CHECK(body.inf.body_heading == 70 * 11930464);
        const Entity *mounted = w.registry.get(player_h);
        CHECK(body.pos[0] == to_fixed(mounted->position.x));
        CHECK(body.pos[1] == to_fixed(mounted->position.y));
        CHECK(body.pos[2] == to_fixed(mounted->position.z));
    }
}

// A model-aware host supplies the complete live seat-bone frame at the shared
// World seam. Every attach entry point consumes it immediately, and mounted AI
// consumes a newly evaluated frame on its next tick. A missing/declining host
// retains the portable static seat geometry.
void test_live_pose_provider_and_static_fallback() {
    // EntityCommands mount consumes the host pose on the attach edge.
    {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        FakeMountedPoseProvider provider;
        provider.pose = {{101.25f, -42.5f, 8.75f}, 37, -12, 17};
        w.pose_provider = &provider;

        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.yaw = 90;
        vehicle.pitch = 4;
        vehicle.roll = 5;
        vehicle.health = 100;
        vehicle.alive = true;
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.bone_index = 7;
        seat.source_name = "sitex00";
        seat.seat_local = {2.0f, 3.0f, 4.0f};
        seat.yaw_offset = 10;
        vehicle.seats.push_back(seat);
        w.registry.spawn(1, vehicle);

        Entity occupant;
        occupant.net_id = 100;
        occupant.kind = EntityKind::Organic;
        occupant.health = 100;
        occupant.alive = true;
        const EntityHandle occupant_h = w.registry.spawn(0, occupant);

        CHECK(w.commands.mount(100, 200));
        check_pose(*w.registry.get(occupant_h), provider.pose);
    }

    // The shared attach path consumes the same seam, then AI tick follows a new
    // live position and carries the full orientation into its body frame.
    {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        AiSystem ai;
        w.ai = &ai;
        FakeMountedPoseProvider provider;
        provider.pose = {{11.0f, 12.0f, 13.0f}, 21, -8, 9};
        w.pose_provider = &provider;

        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.health = 100;
        vehicle.alive = true;
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.bone_index = 7;
        seat.source_name = "sitex00";
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);

        Entity occupant;
        occupant.net_id = 100;
        occupant.kind = EntityKind::Organic;
        occupant.health = 100;
        occupant.alive = true;
        const EntityHandle occupant_h = w.registry.spawn(0, occupant);
        const int ai_index = ai.attach(occupant_h);
        AiEntity *body = ai.at(ai_index);
        body->net_id = 100;
        body->inf.active = true;
        body->inf.anim_state = anim_state::kIdleCrouch;

        CHECK(entity_process_vehicle_attach(w, occupant_h, vehicle_h, 7));
        check_pose(*w.registry.get(occupant_h), provider.pose);

        provider.pose = {{31.5f, 32.25f, 33.75f}, 44, 15, -19};
        TickContext ctx{};
        ctx.is_authority = true;
        ai.tick(w, ctx);

        const Entity *mounted = w.registry.get(occupant_h);
        check_pose(*mounted, provider.pose);
        CHECK(body->pos[0] == to_fixed(31.5));
        CHECK(body->pos[1] == to_fixed(32.25));
        CHECK(body->pos[2] == to_fixed(33.75));
        CHECK(body->inf.body_heading == (90 - provider.pose.yaw) * 11930464);
        CHECK(body->body_pitch == bam_from_degrees_wrapped(provider.pose.pitch));
        CHECK(body->roll == bam_from_degrees_wrapped(provider.pose.roll));
    }

    {
        // A non-local Gunner first resolves the existing clamp base, chases its
        // independent look, then resolves the final Hn+1 parent phase. The local
        // path already exposes current input before its single resolve.
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        AiSystem ai;
        w.ai = &ai;
        HeadingMountedPoseProvider provider;
        w.pose_provider = &provider;

        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.yaw = 90;
        vehicle.health = 100;
        vehicle.alive = true;
        vehicle.emplaced_config_valid = true;
        vehicle.emplaced_config = 3; // wide mount: isolate the chase phase from clamp limits
        Seat seat;
        seat.type = SeatType::Gunner;
        seat.bone_index = 7;
        seat.source_name = "UseGun";
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);

        Entity occupant;
        occupant.net_id = 100;
        occupant.kind = EntityKind::Organic;
        occupant.health = 100;
        occupant.alive = true;
        const EntityHandle occupant_h = w.registry.spawn(0, occupant);
        const int ai_index = ai.attach(occupant_h);
        AiEntity *body = ai.at(ai_index);
        body->net_id = 100;
        body->inf.active = true;

        CHECK(entity_process_vehicle_attach(w, occupant_h, vehicle_h, 7));
        provider.clear();
        body->heading = 0;
        body->pitch = 0;
        body->inf.aim_valid = true;
        body->inf.aim_heading = 0x08000000;
        body->inf.aim_pitch = 0x08000000;
        CHECK(ai.pose_if_mounted(*body, w));

        constexpr int32_t kChasedHeading = 0x02000000;
        constexpr int32_t kChasedPitch = 0x01000000;
        CHECK(provider.headings.size() == 2);
        CHECK(provider.pitches.size() == 2);
        if (provider.headings.size() == 2) {
            CHECK(provider.headings[0] == 0);
            CHECK(provider.headings[1] == kChasedHeading);
            CHECK(provider.pitches[0] == 0);
            CHECK(provider.pitches[1] == kChasedPitch);
        }
        const Entity *mounted = w.registry.get(occupant_h);
        CHECK(body->heading == kChasedHeading);
        CHECK(body->pitch == kChasedPitch);
        CHECK(std::abs(mounted->position.x - 2.0f) < 0.0001f);
        CHECK(std::abs(mounted->position.y - 1.0f) < 0.0001f);
        CHECK(std::abs(mounted->position.z - 41.0f) < 0.0001f);
        CHECK(body->pos[0] == to_fixed(2.0));
        CHECK(body->pos[1] == to_fixed(1.0));
        CHECK(mounted->yaw == 87); // independent chased look, not provider body yaw 12
        CHECK(mounted->pitch == 21);
        CHECK(mounted->roll == -30);
        CHECK(body->inf.body_heading == (90 - 12) * 11930464);
        CHECK(body->body_pitch == bam_from_degrees_wrapped(21.0));
        CHECK(body->roll == bam_from_degrees_wrapped(-30.0));

        // A post-carrier refresh reevaluates the seat pose exactly once at the
        // already-chased LOOK. It must not run the full mounted-body phase again:
        // doing so would chase aim a second time and clear animation state.
        provider.clear();
        body->inf.anim_pending = 77;
        body->inf.move_mode = 9;
        body->inf.target_dist = 1234;
        CHECK(ai.refresh_mounted_pose(*body, w));
        CHECK(provider.headings.size() == 1);
        CHECK(provider.pitches.size() == 1);
        if (provider.headings.size() == 1) {
            CHECK(provider.headings[0] == kChasedHeading);
            CHECK(provider.pitches[0] == kChasedPitch);
        }
        CHECK(body->heading == kChasedHeading);
        CHECK(body->pitch == kChasedPitch);
        CHECK(body->inf.anim_pending == 77);
        CHECK(body->inf.move_mode == 9);
        CHECK(body->inf.target_dist == 1234);

        provider.clear();
        body->inf.is_local_player = true;
        body->inf.target_heading = 0x03000000;
        body->inf.look_pitch = 0x02000000;
        CHECK(ai.pose_if_mounted(*body, w));
        CHECK(provider.headings.size() == 1);
        CHECK(provider.pitches.size() == 1);
        if (provider.headings.size() == 1) {
            CHECK(provider.headings[0] == 0x03000000);
            CHECK(provider.pitches[0] == 0x02000000);
        }
        CHECK(body->heading == 0x03000000);
        CHECK(body->pitch == 0x02000000);
        CHECK(std::abs(w.registry.get(occupant_h)->position.x - 3.0f) < 0.0001f);
        CHECK(std::abs(w.registry.get(occupant_h)->position.y - 2.0f) < 0.0001f);

        // Likewise, the local post-prediction pose pass preserves the promoted
        // LOOK and wire/input state instead of consuming a newer input latch.
        provider.clear();
        body->inf.target_heading = 0x09000000;
        body->inf.look_pitch = 0x07000000;
        body->inf.jump_requested = true;
        Entity *mounted_local = w.registry.get(occupant_h);
        mounted_local->net_move_input = 0x5Au;
        mounted_local->net_stance_bits = 0x03u;
        CHECK(ai.refresh_mounted_pose(*body, w));
        CHECK(provider.headings.size() == 1);
        CHECK(provider.pitches.size() == 1);
        if (provider.headings.size() == 1) {
            CHECK(provider.headings[0] == 0x03000000);
            CHECK(provider.pitches[0] == 0x02000000);
        }
        CHECK(body->heading == 0x03000000);
        CHECK(body->pitch == 0x02000000);
        CHECK(body->inf.jump_requested);
        CHECK(mounted_local->net_move_input == 0x5Au);
        CHECK(mounted_local->net_stance_bits == 0x03u);
    }

    // Null and declining providers both preserve the known static fallback.
    {
        World w;
        Entity vehicle;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.yaw = 90;
        vehicle.pitch = 4;
        vehicle.roll = 5;
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.seat_local = {2.0f, 3.0f, 4.0f};
        seat.yaw_offset = 10;
        // The full-frame fallback (yaw 90, pitch 4, roll 5 over seat_local
        // {2,3,4}): Rz(90-yaw)Ry(-pitch)Rx(roll) on the un-swizzled local —
        // was {13, 18, 34} while the fallback was yaw-only (the pre-§6.13
        // stand-in). [orig: @0x4b0c50 over @0x613f40]
        const MountedPose expected = {{12.726887f, 17.658988f, 34.010455f}, 100, 4, 5};

        Entity occupant;
        pose_mounted_occupant(w, occupant, vehicle, seat);
        check_pose(occupant, expected);

        FakeMountedPoseProvider provider;
        provider.available = false;
        provider.pose = {{999.0f, 999.0f, 999.0f}, -1, -2, -3};
        w.pose_provider = &provider;
        occupant = Entity{};
        pose_mounted_occupant(w, occupant, vehicle, seat);
        check_pose(occupant, expected);
    }
}

// The nearest-seat toggle: a free seat within the 4.0 u horizontal gate attaches; out of
// range does nothing. [orig: @0x435d50 gate @0x436123; @0x436950]
void test_toggle_nearest_seat() {
    {
        Rig r(2.0f);
        CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
        CHECK(r.player().mounted);
        CHECK(r.player().mount_target == r.veh_h);
        CHECK((r.player().flags & 0x40u) != 0);
        CHECK((r.player().engine_flags & 0x40u) != 0);
        // Lowest SCORE (horiz + 3D/512) wins [orig: @0x436123]: from +2x the ctrl
        // bone (+0.5,+1.5 local; horiz 2.12) beats the sitex (0,-2; horiz 2.83).
        CHECK(r.player().mount_seat == 0);
    }
    {
        // From -2.5x the SITEX (horiz 3.20) outscores the ctrl (3.35): the scan is
        // score-ranked, not seat-weighted (the deck weights never apply here).
        Rig r(-2.5f);
        CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
        CHECK(r.player().mount_seat == 1);
    }
    {
        Rig r(30.0f); // far outside the 4 u gate
        CHECK(!player_toggle_vehicle_mount(r.w, r.player_h));
        CHECK(!r.player().mounted);
    }
}

// Entity_FindBestSeatSlot walks the requested root and its ground-attached
// children. Control/driver seats belong to the root only; a child UseGun still
// outranks a root passenger. The chosen entity is part of the result because
// Entity_RequestVehicleAttach boards that child, not the root.
// [orig: Entity_FindBestSeatSlot @0x4351F0;
//  Entity_RequestVehicleAttach @0x4364A0]
void test_best_seat_walks_vehicle_children() {
    World w;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 8);

    Entity root;
    root.kind = EntityKind::Item;
    root.health = 100;
    root.alive = true;
    Seat passenger;
    passenger.type = SeatType::Passenger;
    passenger.bone_index = 3;
    root.seats.push_back(passenger);
    const EntityHandle root_h = w.registry.spawn(1, root);

    Entity child;
    child.kind = EntityKind::Item;
    child.health = 100;
    child.alive = true;
    child.ground_target = root_h;
    Seat child_controller;
    child_controller.type = SeatType::Controller;
    child_controller.bone_index = 4;
    child.seats.push_back(child_controller);
    Seat gunner;
    gunner.type = SeatType::Gunner;
    gunner.bone_index = 5;
    child.seats.push_back(gunner);
    const EntityHandle child_h = w.registry.spawn(1, child);

    Entity player;
    player.kind = EntityKind::Organic;
    player.health = 100;
    player.alive = true;
    const EntityHandle player_h = w.registry.spawn(0, player);

    VehicleSeatSelection selected;
    CHECK(find_best_vehicle_seat(w, root_h, player_h, selected));
    CHECK(selected.vehicle == child_h);
    CHECK(selected.seat_index == 1);
    CHECK(selected.type == SeatType::Gunner);
    CHECK(attach_to_vehicle_seat(w, player_h, selected));
    CHECK(w.registry.get(player_h)->mount_target == child_h);
    CHECK(w.registry.get(child_h)->seats[1].occupant == player_h);

    CHECK(entity_detach_from_vehicle(w, player_h));
    Seat controller;
    controller.type = SeatType::Controller;
    controller.bone_index = 6;
    w.registry.get(root_h)->seats.push_back(controller);
    selected = {};
    CHECK(find_best_vehicle_seat(w, root_h, player_h, selected));
    CHECK(selected.vehicle == root_h);
    CHECK(selected.seat_index == 1);
    CHECK(selected.type == SeatType::Controller);

    // Child control seats are never eligible; command 123 also narrows the
    // walk to passenger seats without changing root-before-child weighting.
    w.registry.get(root_h)->seats[1].occupant = EntityHandle::make(0, 3);
    selected = {};
    CHECK(find_best_vehicle_seat(
            w, root_h, player_h, selected,
            SeatSelectionMode::PassengerOnly));
    CHECK(selected.vehicle == root_h);
    CHECK(selected.seat_index == 0);
}

// The portable whole-registry walk is only for a world that never built the
// collision tables. Once a live table build has happened, the retail BSS-zero
// cadence leaves the first 16 ticks deliberately sliceless.
void test_attach_scan_never_built_fallback_and_initial_empty_slice() {
    {
        Rig r(2.0f);
        CollisionWorld cw;
        r.w.collision = &cw;
        VehicleSeatSelection hit;
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
    }
    {
        // Headless/GDScript fixtures can publish seat metadata without a 3DI
        // collision bound. Before the first candidate-slice epoch, retain the
        // compatibility registry walk rather than treating an absent slice as
        // authoritative.
        Rig r(2.0f);
        CollisionWorld cw;
        r.w.collision = &cw;
        cw.build_tick_tables(r.w);
        CHECK(cw.tick_tables_ready());
        CHECK(!cw.attach_candidate_slices_authoritative());
        VehicleSeatSelection hit;
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
    }
    {
        // A live host may know the seat before the model/collision hull resolves.
        // The short pre-slice epoch retains compatibility discovery; once the
        // 17-tick proximity epoch lands, the seat-only carrier itself is in the
        // bounded slice and no per-frame registry fallback is needed.
        Rig r(2.0f);
        CollisionWorld cw;
        StubCollisionMatrixProvider provider;
        cw.set_pose_provider(&provider);
        r.w.collision = &cw;
        cw.build_tick_tables(r.w);
        CHECK(cw.tick_tables_ready());
        CHECK(!cw.attach_candidate_slices_authoritative());
        VehicleSeatSelection hit;
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
        for (int i = 1; i < 17; ++i) cw.build_tick_tables(r.w);
        CHECK(cw.attach_candidate_slices_authoritative());
        CHECK(cw.candidate_count(r.player_h) == 1);
        hit = VehicleSeatSelection{};
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
        CHECK(provider.calls == 0);
    }
    {
        Rig r(2.0f);
        CollisionWorld cw;
        r.w.collision = &cw;
        r.veh().bound_radius = 3.0f;
        cw.build_tick_tables(r.w);
        VehicleSeatSelection hit;
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        for (int i = 1; i < 17; ++i) cw.build_tick_tables(r.w);
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
    }
}

// A deploy/respawn after a completed proximity epoch rebuilds the new player's
// source slice immediately. Waiting for the normal 17-tick cadence would make
// USE and attach labels disappear just after spawning.
void test_post_epoch_player_spawn_discovers_nearby_seat_immediately() {
    World w;
    AiSystem ai;
    CollisionWorld cw;
    w.ai = &ai;
    w.collision = &cw;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 8);

    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.position = {10.0f, 20.0f, 0.0f};
    vehicle.health = 100;
    vehicle.alive = true;
    vehicle.bound_radius = 3.0f;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.bone_index = 1;
    seat.source_name = "sitex00";
    seat.seat_local = {0.0f, 0.0f, 1.0f};
    vehicle.seats.push_back(seat);
    const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);

    for (int i = 0; i < 17; ++i) cw.build_tick_tables(w);

    PlayerSpawn spawn;
    spawn.position = {12.0f, 20.0f, 0.0f};
    spawn.health = 100;
    const EntityHandle player_h = spawn_player(w, spawn);
    CHECK(player_h.valid());
    const Entity *player = w.registry.get(player_h);
    CHECK(player != nullptr);
    if (player != nullptr) {
        VehicleSeatSelection hit;
        CHECK(find_nearest_free_seat(w, *player, hit, false));
        CHECK(hit.vehicle == vehicle_h);
    }
}

// Registry rewind must replace the candidate arena from the later play epoch.
// Otherwise a restored player keeps scanning the vehicles that were near its
// pre-restore position until the next 17-tick refresh.
void test_restore_refreshes_completed_candidate_epoch() {
    Rig r(2.0f);
    CollisionWorld cw;
    r.w.collision = &cw;
    r.veh().bound_radius = 3.0f;

    Entity other;
    other.kind = EntityKind::Item;
    other.position = {200.0f, 200.0f, 10.0f};
    other.health = 100;
    other.alive = true;
    other.bound_radius = 3.0f;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.bone_index = 1;
    seat.source_name = "sitex00";
    seat.seat_local = {0.0f, 0.0f, 1.0f};
    other.seats.push_back(seat);
    const EntityHandle other_h = r.w.registry.spawn(1, other);

    for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
    const World::Snapshot baseline = r.w.snapshot();

    r.player().position = {202.0f, 200.0f, 10.0f};
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
    VehicleSeatSelection hit;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(hit.vehicle == other_h);

    r.w.restore(baseline);
    hit = VehicleSeatSelection{};
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(hit.vehicle == r.veh_h);
}

// The deck path: standing ON the vehicle (ground_target) picks the BEST seat by weight —
// ctrl/drvr (0x2000) beats sitex (0x200000). [orig: @0x4368cf -> @0x4351f0]
void test_toggle_deck_best_seat() {
    Rig r(30.0f); // out of scan range: only the deck path can mount
    r.player().ground_target = r.veh_h;
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    CHECK(r.player().mount_type == SeatType::Controller);
}

// Toggle while mounted = dismount [orig: @0x4369c7]: the OWN vehicle's seats are
// LOS-blocked by its hull in retail, so the seated scan runs dry — USE exits, it
// never cycles seats (the hull occlusion is a candidate skip until pool-1 collision
// lands, D-AI-11 j). A DIFFERENT vehicle's free seat within reach still swaps
// [orig: @0x4369ac].
void test_toggle_dismount_and_swap() {
    Rig r(2.0f);
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    // Second toggle: the own vehicle's other free seat does NOT swap — USE exits.
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(!r.player().mounted);

    // Remount, then park a SECOND vehicle with a free seat inside the 4 u gate of the
    // seated player: the fresh scan hit re-enters (the vehicle-to-vehicle swap).
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    Entity other;
    other.kind = EntityKind::Item;
    other.item_id = 1294;
    other.position = {103.0f, 200.0f, 10.0f}; // ~1 u from the standing spot at 102
    other.yaw = 0;
    other.health = 2000;
    other.health_max = 2000;
    other.alive = true;
    Seat sit;
    sit.type = SeatType::Passenger;
    sit.bone_index = 2;
    sit.source_name = "sitex00";
    sit.seat_local = {0.0f, 0.5f, 1.0f};
    other.seats.push_back(sit);
    EntityHandle oh = r.w.registry.spawn(1, other);
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    CHECK(r.player().mount_target == oh); // swapped ACROSS vehicles, not out
}

// An enemy occupant rejects the whole vehicle in the scan [orig: Vehicle_HasEnemyOccupant
// @0x4359F0 via @0x435e58].
void test_enemy_occupant_blocks_scan() {
    Rig r(2.0f);
    Entity enemy;
    enemy.kind = EntityKind::Organic;
    enemy.item_id = 5305;
    enemy.team = 2; // player team defaults 0... stamp both sides below
    enemy.health = 150;
    enemy.alive = true;
    EntityHandle eh = r.w.registry.spawn(0, enemy);
    r.player().team = 1;
    Entity *ee = r.w.registry.get(eh);
    ee->mounted = true;
    ee->mount_target = r.veh_h;
    ee->mount_seat = 0;
    ee->mount_type = SeatType::Controller;
    r.veh().seats[0].occupant = eh;

    auto label_count = [&]() {
        std::vector<AttachLabel> labels;
        collect_attach_labels(r.w, r.player(), false, false, labels);
        return labels.size();
    };
    VehicleSeatSelection hit;
    CHECK(!player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(!r.player().mounted);
    CHECK(!find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 0);
    CHECK(!entity_process_vehicle_attach(r.w, r.player_h, r.veh_h, 2));

    // Same-team, dead, or detached riders do not block the vehicle. The occupied
    // seat itself remains hidden independently, leaving the passenger seat.
    ee->team = 1;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->team = 2;
    ee->alive = false;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->alive = true;
    ee->health = 0;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->health = 150;
    CHECK(!find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 0);
    ee->mounted = false;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 1);

    // Retail's hostile-parent scan is pool-0-only. A mounted lookalike in pool 1
    // must not block, even though it carries the same direct mount target.
    Entity pool1_enemy = *ee;
    pool1_enemy.mounted = true;
    pool1_enemy.mount_target = r.veh_h;
    CHECK(r.w.registry.spawn(1, pool1_enemy).valid());
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(label_count() == 1);
}

// The four BMS Player mount triggers [orig: subs 38-41 -> @0x4f10d0/0x4f1260/0x4f1150/
// 0x4f11e0]: attached (any seat) / standing-on / driving (ctrl/drvr) / on-gun (UseGun).
void test_bms_mount_predicates() {
    Rig r(2.0f);
    auto &cmds = r.w.commands;
    CHECK(!cmds.local_player_attached_to_ssn(11));
    CHECK(!cmds.local_player_standing_on_ssn(11));

    // Standing on the deck: sub 39 true, sub 38 false.
    r.player().ground_target = r.veh_h;
    CHECK(cmds.local_player_standing_on_ssn(11));
    CHECK(!cmds.local_player_attached_to_ssn(11));

    // Mounted into the ctrl seat: 38 + 40 true, 41 false.
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h)); // deck path -> ctrl seat
    CHECK(cmds.local_player_attached_to_ssn(11));
    CHECK(cmds.local_player_driving_ssn(11));
    CHECK(!cmds.local_player_on_gun_of_ssn(11));

    // A dead local player reads false on every sub [orig: the Flags & 2 gate].
    r.player().health = 0;
    CHECK(!cmds.local_player_attached_to_ssn(11));
    r.player().health = 150;

    // The carrier chain: a gun CARRIED by SSN 11 counts for sub 38 against 11.
    Entity gun;
    gun.net_id = 500;
    gun.bms_id = 500;
    gun.kind = EntityKind::Item;
    gun.item_id = 1800;
    gun.position = r.veh().position;
    gun.health = 500;
    gun.alive = true;
    gun.primary_weapon.assign(1, 'x');
    r.w.weapons.entries.resize(2);
    r.w.weapons.entries[1].name.assign(1, 'x');
    r.w.weapons.entries[1].clipsize = -1;
    r.w.weapons.entries[1].valid = true;
    r.player().equipped_adm_index = 7;
    r.player().flags |= 0x100u;
    r.player().engine_flags |= 0x100u;
    Seat gseat;
    gseat.type = SeatType::Gunner;
    gseat.bone_index = 1;
    gseat.source_name = "UseGun";
    gun.seats.push_back(gseat);
    EntityHandle gh = r.w.registry.spawn(1, gun);
    r.w.registry.get(gh)->ground_target = r.veh_h; // the gun rides the truck
    entity_detach_from_vehicle(r.w, r.player_h);
    CHECK(entity_process_vehicle_attach(r.w, r.player_h, gh, 1));
    CHECK(r.w.registry.get(r.player_h)->mount_type == SeatType::Gunner);
    CHECK((r.player().flags & 0x40u) == 0);
    CHECK((r.player().engine_flags & 0x40u) == 0);
    CHECK(cmds.local_player_on_gun_of_ssn(500));
    CHECK(cmds.local_player_attached_to_ssn(11)); // via the carrier link
    CHECK(!cmds.local_player_driving_ssn(500));
    CHECK(r.player().equipped_adm_index == 1);
    CHECK(r.player().pre_use_gun_equipped_adm_index == 7);
    CHECK(r.player().use_gun_slot_swapped);
    CHECK(r.w.registry.get(gh)->primary_weapon_owner == r.player_h);
    CHECK(entity_detach_from_vehicle(r.w, r.player_h));
    CHECK(r.player().equipped_adm_index == 7);
    CHECK(!r.player().use_gun_slot_swapped);
    CHECK(!r.w.registry.get(gh)->primary_weapon_owner.valid());
}

// Retail's shared EWeap slot resolver uses the live MountSlot+0x5e bit 8 to
// choose between an addeweap child's embedded slot and its groundEntity
// carrier's vehicle slot. The authored G/C flags are capabilities, not this
// mutable route bit. [orig: shared helper @0x5460e0; phase-8 writer @0x4ffe18]
void test_mounted_ammo_slot_route() {
    World w;
    w.registry.configure_pool(1, 8);

    // Retail gates on the item definition and the EWeap attrib before ANY
    // slot resolution — an entity without traits resolves nothing, even on
    // the unredirected route [orig: @0x5460E0 / writer gate @0x4FFE0B].
    Entity ungated;
    ungated.primary_weapon_slot_adm = 3;
    CHECK(resolve_mounted_ammo_slot(w, ungated) == nullptr);

    // With the witnessed gate satisfied, the unredirected route is the
    // entity's own embedded MountSlot; the cross-entity route remains closed
    // until the strict EWeap relationship proof is present.
    Entity direct_only;
    direct_only.primary_weapon_slot_adm = 3;
    direct_only.has_item_def = true;
    direct_only.item_attrib = kItemAttribEweap;
    CHECK(resolve_mounted_ammo_slot(w, direct_only) ==
          &direct_only.primary_weapon_slot);
    direct_only.primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(resolve_mounted_ammo_slot(w, direct_only) == nullptr);

    Entity parent;
    parent.kind = EntityKind::Item;
    parent.has_item_def = true;
    parent.item_type = 1;
    parent.item_attrib = kItemAttribEweap;
    parent.primary_weapon_slot.clip = 41;
    const EntityHandle parent_h = w.registry.spawn(1, parent);
    const uint64_t parent_spawn_id =
            w.registry.get(parent_h)->registry_spawn_id;

    Entity child;
    child.kind = EntityKind::Item;
    child.has_item_def = true;
    child.item_type = 2;
    child.item_attrib = kItemAttribEweap;
    child.emplacement_attachment_flags = 0x02; // designated G capability
    child.emplacement_parent = parent_h;
    child.emplacement_parent_spawn_id = parent_spawn_id;
    child.ground_target = parent_h;
    child.primary_weapon_slot.clip = 7;
    const EntityHandle child_h = w.registry.spawn(1, child);
    Entity *live_child = w.registry.get(child_h);

    CHECK(resolve_mounted_ammo_slot(w, *live_child) ==
          &live_child->primary_weapon_slot);
    live_child->primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(resolve_mounted_ammo_slot(w, *live_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);
    const World &const_world = w;
    const Entity &const_child = *w.registry.get(child_h);
    CHECK(resolve_mounted_ammo_slot(const_world, const_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);

    // The similarly named attachment pointer is not a substitute for retail's
    // groundEntity relationship.
    live_child->ground_target = EntityHandle{};
    CHECK(resolve_mounted_ammo_slot(w, *live_child) == nullptr);
    live_child->ground_target = parent_h;

    // A packed handle may be reused. The child-side generation captured when
    // the relationship was authored must reject the replacement lifetime.
    w.registry.despawn(parent_h);
    Entity replacement = parent;
    CHECK(w.registry.spawn_at(parent_h, replacement) == parent_h);
    CHECK(resolve_mounted_ammo_slot(w, *live_child) == nullptr);
}

void test_prepare_vehicle_weapon_slot_after_armory_load() {
    World w;
    w.weapons.entries.resize(4);
    WeaponTableEntry &weapon = w.weapons.entries[3];
    weapon.name = "WPN_LATE_MOUNT";
    weapon.clipsize = 12;
    weapon.startrounds = 41;
    weapon.valid = true;

    Entity mount;
    mount.primary_weapon = weapon.name;
    CHECK(vehicle_prepare_weapon_slot(w, mount));
    CHECK(mount.primary_weapon_slot_adm == 3);
    CHECK(mount.primary_weapon_slot.clip == 12);
    CHECK(mount.primary_weapon_slot.reserve == 29);

    // Preparing an already-live slot is idempotent: a later route edge must
    // never refund cartridges spent since the first armory resolution.
    mount.primary_weapon_slot.clip = 7;
    mount.primary_weapon_slot.reserve = 23;
    mount.primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(vehicle_prepare_weapon_slot(w, mount));
    CHECK(mount.primary_weapon_slot.clip == 7);
    CHECK(mount.primary_weapon_slot.reserve == 23);
    CHECK(mount.primary_weapon_slot.redirect_to_parent_slot);
}

// A HOST-crewed helicopter's rotor turns from the authority pass: the helo
// mover is the unported residual, but retail runs the part-animation
// accumulator from EVERY mover's tail, the helo mover included, so the pass
// ticks it for a direct-air row at the point that tail would run — and the
// HELO_ROTOR register (the angle's high word) advances while crewed, then
// winds down at the helo machine's 46603/tick after the dismount.
// [orig: the HELO twin @0x48FA70 from the aircraft mover's tail @0x4905A6]
void test_host_crewed_helicopter_rotor_turns() {
    Rig r;
    VehicleTraits t = truck_traits();
    t.family = VehicleFamily::Helicopter;
    t.physics = 0; // a CHel def: no ground selector, no ground mover
    r.w.vehicle_traits.set(r.veh().item_id, t);
    const int ai_idx = r.sys.attach(r.veh_h);
    r.sys.at(ai_idx)->profile.type = 1; // the helo profile class
    CHECK(entity_process_vehicle_attach(r.w, r.player_h, r.veh_h, 1));
    CHECK(r.veh().primary_occupant == r.player_h);
    TickContext ctx{};
    ctx.is_authority = true;
    for (int i = 0; i < 3; ++i) r.sys.tick(r.w, ctx);
    CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull);
    CHECK(part_register(r.veh().veh.part_spin.angle) > 0);
    CHECK(entity_detach_from_vehicle(r.w, r.player_h));
    r.sys.tick(r.w, ctx);
    CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo);
    // A joiner never runs the authority pass for it.
    ctx.is_authority = false;
    const int32_t before = r.veh().veh.part_spin.speed;
    r.sys.tick(r.w, ctx);
    CHECK(r.veh().veh.part_spin.speed == before);
}

// The AI-driver leg: an NPC in the ctrl seat + a staged brain waypoint drives the truck
// toward the node; no controller parks it (SM state 22, no movement).
// [orig: @0x48bc12-0x48c034 / the parked stamp @0x48c002-0x48c02d]
void test_ai_drive_leg() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicle_traits.set(r.veh().item_id, t);

    // Brain for the truck with a live nav waypoint straight ahead (+x).
    const int ai_idx = r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.at(ai_idx);
    ve.pos[0] = 100 << 16;
    ve.pos[1] = 200 << 16;
    ve.pos[2] = 10 << 16;
    ve.heading = bam_heading_from_mission_yaw_deg(90.0); // face +x in mission space
    r.sys.nav.channels.resize(3);
    r.sys.nav.channels[2].count = 1;
    r.sys.nav.channels[2].entries[0] = 0;
    r.sys.nav.nodes.resize(1);
    r.sys.nav.nodes[0] = NavEntry{{2 << 16, 300 << 16, 200 << 16, 10 << 16, 0}};
    AiBrain &b = ve.brain;
    b.f[AiBrain::kCurState] = 16;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 2;
    b.f[AiBrain::kWpNode] = 0;
    b.f[AiBrain::kOutSpeed] = 40 * 293; // the SM mover's out-speed (PatrolSpeed 40)

    // No controller: parked. [orig: @0x48c002-0x48c02d]
    {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), nullptr, t, cmd);
        CHECK(!cmd.ai_drive);
        CHECK(b.f[AiBrain::kCurState] == 22);
        // The pend mirror keeps the SM's transition pass from reverting the stamp
        // to the promoted pending 16 next tick (one state word in the original).
        CHECK(b.f[AiBrain::kPendState] == 22);
        const float x0 = r.veh().position.x;
        tick_vehicle_motor(r.w, r.veh(), t, &cmd);
        CHECK(std::abs(r.veh().position.x - x0) < 0.05f);
    }

    // An NPC controller: the AI leg drives. Refresh the waypoint bearing first.
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    CHECK(r.w.registry.get(nh)->mount_type == SeatType::Controller);
    Entity *ctrl = resolve_vehicle_controller(r.w, r.veh());
    CHECK(ctrl != nullptr);

    const float x0 = r.veh().position.x;
    bool drove = false;
    for (int i = 0; i < 300; ++i) {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
        drove = drove || cmd.ai_drive;
        tick_vehicle_motor(r.w, r.veh(), t, &cmd);
        AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->pos[0] = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->pos[1] = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->pos[2] = static_cast<int32_t>(r.veh().position.z * 65536.0f);
        ve2->heading = r.veh().veh.yaw_bam;
    }
    CHECK(drove);
    CHECK(b.f[AiBrain::kCurState] == 16); // the 22 -> 16 hand-back held
    CHECK(b.f[AiBrain::kPendState] == 16); // both fields hand back (the pend mirror)
    // ~4.8 s of drive: the truck swung from its initial 90-degree heading error onto the
    // +x bearing (a real turn ARC — the speed-coupled steering sweeps y while turning)
    // and covered ground toward the node.
    CHECK(r.veh().position.x - x0 > 4.0f);
    const int32_t heading_err = r.veh().veh.yaw_bam - b.f[AiBrain::kWpBearing];
    CHECK(std::abs(heading_err) < 60000000); // within ~5 deg of the bearing
    CHECK(std::abs(r.veh().position.y - 200.0f) < 30.0f); // the turn arc, not a runaway

    // A DEAD controller counts as no controller (the death->detach chain stand-in,
    // D-AI-11 k): the motor's own resolve must not consume the corpse's input.
    r.w.registry.get(nh)->health = 0;
    r.w.registry.get(nh)->alive = false;
    {
        VehicleDriveCmd cmd; // ai_drive stays false: the staging parks dead drivers
        r.sys.vehicle_ai_drive(r.w, r.veh(), nullptr, t, cmd);
        CHECK(!cmd.ai_drive);
        CHECK(b.f[AiBrain::kCurState] == 22);
        tick_vehicle_motor(r.w, r.veh(), t, &cmd);
        CHECK(r.veh().veh.cmd_speed == 0); // the no-controller hold, not the stale drive
    }
}

// The pool-1 avoid BRAKE: a neighbor whose footprint ellipse overlaps ours and
// sits within ~30 deg of dead ahead damps the command speed by the id/frame
// factor ((id + (tick<<8)) & 0x7FFF) + 0x4000 per tick; a neighbor BEHIND does
// not [orig: @0x48bd8f-0x48bf26].
void test_ai_drive_avoid_brake() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicle_traits.set(r.veh().item_id, t);
    const int ai_idx = r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.at(ai_idx);
    ve.pos[0] = 100 << 16;
    ve.pos[1] = 200 << 16;
    ve.pos[2] = 10 << 16;
    r.sys.nav.channels.resize(3);
    r.sys.nav.channels[2].count = 1;
    r.sys.nav.channels[2].entries[0] = 0;
    r.sys.nav.nodes.resize(1);
    r.sys.nav.nodes[0] = NavEntry{{2 << 16, 300 << 16, 200 << 16, 10 << 16, 0}};
    AiBrain &b = ve.brain;
    b.f[AiBrain::kCurState] = 16;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 2;
    b.f[AiBrain::kWpNode] = 0;
    b.f[AiBrain::kOutSpeed] = 40 * 293;
    // Face +x (engine BAM 0) with no bearing error -> the sharp-leg damps stay out.
    r.veh().position = Vec3{100.0f, 200.0f, 10.0f};
    r.veh().veh.yaw_bam = bam_heading_from_mission_yaw_deg(90.0);
    r.veh().veh.yaw_seeded = true;
    ve.heading = r.veh().veh.yaw_bam;
    r.veh().bound_radius = 3.0f;

    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    const EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    Entity *ctrl = resolve_vehicle_controller(r.w, r.veh());
    CHECK(ctrl != nullptr);

    // Baseline: open road, undamped.
    VehicleDriveCmd cmd0;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd0);
    CHECK(cmd0.ai_drive);
    CHECK(cmd0.cmd_speed > 0);

    // A parked prop dead AHEAD (+x), footprints overlapping.
    Entity prop;
    prop.kind = EntityKind::Item;
    prop.item_id = 999;
    prop.position = Vec3{104.0f, 200.0f, 10.0f};
    prop.bound_radius = 3.0f;
    const EntityHandle ph = r.w.registry.spawn(1, prop);

    VehicleDriveCmd cmd1;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd1);
    CHECK(cmd1.ai_drive);
    const int32_t f = static_cast<int32_t>(
            ((static_cast<uint32_t>(r.veh().net_id) +
              (static_cast<uint32_t>(r.w.logic_tick) << 8)) &
             0x7FFFu) +
            0x4000u);
    const int32_t expect = static_cast<int32_t>(
            (static_cast<int64_t>(f) * cmd0.cmd_speed + 0x8000) >> 16);
    CHECK(cmd1.cmd_speed == expect);
    CHECK(cmd1.cmd_speed < cmd0.cmd_speed);

    // The same prop BEHIND: inside reach, but the dead-ahead gate rejects.
    r.w.registry.get(ph)->position = Vec3{96.0f, 200.0f, 10.0f};
    VehicleDriveCmd cmd2;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd2);
    CHECK(cmd2.cmd_speed == cmd0.cmd_speed);
}

// A driverless hull's stuck escalation [orig: AI_CheckVehicleStuckState
// @0x465290]: the count climbs once per parked tick; on the authority every
// 16th count past 32 a live pool-0 body within (radii + 12 u) resets it; past
// 3410 a hull more than 12 u from its spawn anchor is nudged up (slideDecay
// += 1024 per check) until 3720, then killed.
void test_stuck_check() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.sys.is_authority = true;
    r.sys.attach(r.veh_h);
    r.veh().spawn_position = Vec3{0.0f, 0.0f, 10.0f}; // 100+ u from the hull
    r.veh().bound_radius = 3.0f;
    // The rig's player stands 30 u away: outside (3 + 0 + 12) u, no reset.
    for (int i = 0; i < 3410; ++i) {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), nullptr, t, cmd);
        CHECK(!cmd.ai_drive);
    }
    CHECK(r.veh().veh.stuck_ticks == 3410);
    CHECK(r.veh().veh.slide_z == 0);
    CHECK(r.veh().health == 2000);
    // 3411..3720: every 16th count nudges the hull upward.
    int32_t nudges = 0;
    for (int i = 3411; i <= 3720; ++i) {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), nullptr, t, cmd);
        if ((i & 0xF) == 0) ++nudges;
        CHECK(r.veh().veh.slide_z == nudges * 1024);
    }
    CHECK(r.veh().health == 2000);
    // The first checked count past 3720 writes the hull off.
    for (int i = 3721; i <= 3728; ++i) {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), nullptr, t, cmd);
    }
    CHECK(r.veh().veh.stuck_ticks == 3728);
    CHECK(r.veh().health == 0);

    // A hull still AT its spawn anchor is never written off, and a live body
    // nearby resets the count entirely.
    Rig r2(2.0f); // the player 2 u away: inside the reset reach
    r2.w.vehicle_traits.set(r2.veh().item_id, t);
    r2.sys.is_authority = true;
    r2.sys.attach(r2.veh_h);
    r2.player().has_item_def = true; // [orig: the walk's entity[7] gate]
    r2.veh().spawn_position = r2.veh().position;
    r2.veh().bound_radius = 3.0f;
    for (int i = 0; i < 48; ++i) {
        VehicleDriveCmd cmd;
        r2.sys.vehicle_ai_drive(r2.w, r2.veh(), nullptr, t, cmd);
    }
    // Counts 33..48: the check at 48 (> 32, & 0xF == 0) saw the body and reset.
    CHECK(r2.veh().veh.stuck_ticks == 0);
    // Move the body away and let the count run past the kill line: at the
    // anchor, distance <= 12 u -> neither nudge nor kill.
    r2.player().position = Vec3{200.0f, 200.0f, 10.0f};
    for (int i = 0; i < 3800; ++i) {
        VehicleDriveCmd cmd;
        r2.sys.vehicle_ai_drive(r2.w, r2.veh(), nullptr, t, cmd);
    }
    CHECK(r2.veh().veh.stuck_ticks == 3800);
    CHECK(r2.veh().veh.slide_z == 0);
    CHECK(r2.veh().health == 2000);
    // An occupant rests the count.
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    const EntityHandle nh = r2.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r2.w, nh, r2.veh_h, 1));
    VehicleDriveCmd cmd;
    tick_vehicle_motor(r2.w, r2.veh(), t, &cmd);
    CHECK(r2.veh().veh.stuck_ticks == 0);
    // A joiner never runs the escalation (the count still climbs).
    r2.sys.is_authority = false;
    r2.w.registry.get(nh)->health = 0;
    r2.w.registry.get(nh)->alive = false;
    for (int i = 0; i < 3800; ++i) {
        VehicleDriveCmd c2;
        r2.sys.vehicle_ai_drive(r2.w, r2.veh(), nullptr, t, c2);
    }
    CHECK(r2.veh().veh.stuck_ticks == 3800);
    CHECK(r2.veh().health == 2000);
}

// The minAI crew clamp [orig: @0x48bc4e-0x48bc94]: an AI-driven hull that has
// left its spawn anchor with fewer than minAI riders bleeds to criticalHp.
void test_min_ai_crew_clamp() {
    Rig r(30.0f);
    VehicleTraits t = truck_traits();
    t.min_ai = 2;
    t.critical_hp = 300;
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.sys.attach(r.veh_h);
    r.veh().spawn_position = r.veh().position;
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    npc.has_item_def = true;
    const EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    Entity *ctrl = resolve_vehicle_controller(r.w, r.veh());
    CHECK(ctrl != nullptr);
    CHECK(count_mounted_entities(r.w, r.veh()) == 1);

    // At the anchor: no clamp.
    CHECK(vehicle_at_spawn_anchor(r.veh()));
    VehicleDriveCmd cmd;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 2000);
    // 8 u planar is still the anchor (<= 0x80000); 9 u is not.
    r.veh().position = Vec3{108.0f, 200.0f, 10.0f};
    CHECK(vehicle_at_spawn_anchor(r.veh()));
    r.veh().position = Vec3{109.0f, 200.0f, 10.0f};
    CHECK(!vehicle_at_spawn_anchor(r.veh()));
    // The Z delta is HALVED: 15 u straight up still counts as the anchor.
    r.veh().position = Vec3{100.0f, 200.0f, 25.0f};
    CHECK(vehicle_at_spawn_anchor(r.veh()));
    r.veh().position = Vec3{100.0f, 200.0f, 27.0f};
    CHECK(!vehicle_at_spawn_anchor(r.veh()));
    // Off the anchor, one rider of the required two: clamped to critical.
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 300);
    // Health below critical is left alone.
    r.veh().health = 120;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 120);
    // A second rider (a free-standing body grounded on the deck) fills the crew.
    r.veh().health = 2000;
    Entity rider;
    rider.kind = EntityKind::Organic;
    rider.item_id = 2072;
    rider.health = 150;
    rider.alive = true;
    rider.has_item_def = true;
    rider.ground_target = r.veh_h;
    const EntityHandle rh = r.w.registry.spawn(0, rider);
    CHECK(count_mounted_entities(r.w, r.veh()) == 2);
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 2000);
    // A dead rider does not count.
    r.w.registry.get(rh)->flags |= kEntityFlagDead;
    CHECK(count_mounted_entities(r.w, r.veh()) == 1);
    // minAI 1 never clamps.
    t.min_ai = 1;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 2000);
}

// The handbrake latch [orig: @0x48c03a..0x48c095]: the driver's lean-right key
// (vehicle Flags bit 3) with the def's handBrake set forces the command word to
// zero while held; the crashed byte does the same.
void test_handbrake_latch() {
    Rig r(2.0f);
    VehicleTraits t = truck_traits();
    t.hand_brake = 1;
    r.w.vehicle_traits.set(r.veh().item_id, t);
    CHECK(entity_process_vehicle_attach(r.w, r.player_h, r.veh_h, 1));
    Entity &drv = r.player();
    drv.net_move_input = 0x08; // moving forward
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    CHECK(r.veh().veh.handbrake_latched == 0);
    drv.net_move_input = 0x08 | 0x80; // + lean right = handbrake
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK((r.veh().flags & 0x8u) != 0);
    CHECK(r.veh().veh.handbrake_latched == 1);
    CHECK(r.veh().veh.cmd_speed == 0);
    drv.net_move_input = 0x08;
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK(r.veh().veh.handbrake_latched == 0);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // A def without handBrake ignores the key.
    t.hand_brake = 0;
    drv.net_move_input = 0x08 | 0x80;
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK(r.veh().veh.handbrake_latched == 0);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // The crashed byte stops the command regardless.
    r.veh().veh.crashed = 1;
    drv.net_move_input = 0x08;
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK(r.veh().veh.cmd_speed == 0);
    r.veh().veh.crashed = 0;
    // The MoveOrder merge: a nonzero direction with the move bit latches the
    // free-look bit on the occupant's own word [orig: @0x48b847..0x48b897].
    drv.net_move_input = 0x08 | 0x01;
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK((drv.net_move_input & 0x10u) != 0);
    drv.net_move_input = 0x08; // straight: no latch
    tick_vehicle_motor(r.w, r.veh(), t, nullptr);
    CHECK((drv.net_move_input & 0x10u) == 0);
}

// The ground twin of the boarders hold [orig: @0x48bf6f-0x48bff9]: an AI
// driver holds at cmd 0 while a body walks over to a free seat.
void test_ground_waits_for_boarders() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicle_traits.set(r.veh().item_id, t);
    r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.for_handle(r.veh_h);
    ve.brain.f[AiBrain::kOutSpeed] = 40 * 293;
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    const EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    Entity *ctrl = resolve_vehicle_controller(r.w, r.veh());
    CHECK(ctrl != nullptr);
    VehicleDriveCmd cmd0;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd0);
    CHECK(cmd0.ai_drive);
    CHECK(cmd0.cmd_speed > 0);
    // A walker running the board order at this truck: full stop.
    Entity walker;
    walker.kind = EntityKind::Organic;
    walker.item_id = 2072;
    walker.health = 150;
    walker.alive = true;
    walker.has_item_def = true;
    const EntityHandle wh = r.w.registry.spawn(0, walker);
    AiEntity &wb = *r.sys.at(r.sys.attach(wh));
    wb.brain.f[37] = 125;
    wb.brain.f[38] = static_cast<int32_t>(r.veh().net_id);
    VehicleDriveCmd cmd1;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd1);
    CHECK(cmd1.ai_drive);
    CHECK(cmd1.cmd_speed == 0);
    CHECK(cmd1.steer_target_bam ==
          bam_heading_from_mission_yaw_deg(static_cast<double>(r.veh().yaw)));
}

// A helicopter rig for the AI flight legs: a CHel row with a helo profile, an
// NPC in the control seat, the rotor at full speed (a cold rotor parks every
// command for the ~18 s spool-up), and a one-node route.
struct HeloRig {
    // A flat sea-level field: the hull starts 10 u up, so the ground legs and
    // the boxless terrain clamp both have a ground to read.
    std::vector<uint16_t> heightmap = std::vector<uint16_t>(64 * 64, 0);
    std::vector<int> sector_grid = std::vector<int>(256, 1);
    opennova::terrain::TerrainHeightField field;
    Rig r;
    VehicleTraits t;
    AiEntity *ve = nullptr;
    Entity *ctrl = nullptr;
    explicit HeloRig(bool rotor_full = true) : r(30.0f) {
        field.heightmap = heightmap.data();
        field.dim = 64;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
        r.w.terrain = &field;
        t = truck_traits();
        t.family = VehicleFamily::Helicopter;
        t.physics = 0;
        t.acceleration = 512;
        t.turn_rate = 0x600000;
        t.climb_speed = 50 * 293;
        t.critical_hp = 300;
        t.critical_drain = 40;
        t.non_critical_regen = 10;
        r.w.vehicle_traits.set(r.veh().item_id, t);
        r.sys.is_authority = true;
        const int ai_idx = r.sys.attach(r.veh_h);
        ve = r.sys.at(ai_idx);
        ve->profile.type = 1; // the helo profile class
        ve->pos[0] = to_fixed(r.veh().position.x);
        ve->pos[1] = to_fixed(r.veh().position.y);
        ve->pos[2] = to_fixed(r.veh().position.z);
        r.veh().spawn_position = r.veh().position;
        r.veh().bound_radius = 6.0f;
        if (rotor_full) rotor_spawn_full(r.veh().veh.part_spin);
        Entity npc;
        npc.kind = EntityKind::Organic;
        npc.item_id = 2072;
        npc.health = 150;
        npc.alive = true;
        npc.team = 1;
        npc.has_item_def = true;
        const EntityHandle nh = r.w.registry.spawn(0, npc);
        CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
        ctrl = resolve_vehicle_controller(r.w, r.veh());
        CHECK(ctrl != nullptr);
    }
    void route_to(int32_t x, int32_t y, int32_t z) {
        r.sys.nav.channels.resize(3);
        r.sys.nav.channels[2].count = 1;
        r.sys.nav.channels[2].entries[0] = 0;
        r.sys.nav.nodes.resize(1);
        r.sys.nav.nodes[0] = NavEntry{{2 << 16, x << 16, y << 16, z << 16, 0}};
        AiBrain &b = ve->brain;
        b.f[AiBrain::kCurState] = 16;
        b.f[AiBrain::kPendState] = 16;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 2;
        b.f[AiBrain::kWpNode] = 0;
        b.f[AiBrain::kWpResolved] = 0;
        b.f[AiBrain::kOutSpeed] = 40 * 293;
    }
    void tick(int n) {
        TickContext ctx{};
        ctx.is_authority = true;
        for (int i = 0; i < n; ++i) {
            r.sys.tick(r.w, ctx);
            ++r.w.logic_tick;
        }
    }
};

// The AI flight block [orig: @0x491672..0x491998]: a crewed, routed helicopter
// with its rotor up lifts toward the node's altitude and closes on it; with no
// route it sits on its skids; with a cold rotor every command parks.
void test_helo_ai_flight() {
    {
        HeloRig h;
        h.route_to(300, 200, 40); // 200 u east, 30 u up
        const float z0 = h.r.veh().position.z;
        const float x0 = h.r.veh().position.x;
        h.tick(400);
        CHECK(h.r.veh().veh.net_engine_on);
        CHECK(h.r.veh().position.z > z0 + 5.0f);
        CHECK(h.r.veh().position.x > x0 + 5.0f);
        CHECK(h.r.veh().position.x < 300.0f);
        // Steer chases the node's bearing (+x = mission yaw 90).
        const int32_t err = h.r.veh().veh.yaw_bam - bam_heading_from_mission_yaw_deg(90.0);
        CHECK(std::abs(err) < 120000000);
    }
    {
        HeloRig h; // no route: the parked altitude target holds -> it settles
        h.ve->brain.f[AiBrain::kCurState] = 16;
        h.ve->brain.f[AiBrain::kPendState] = 16;
        const float z0 = h.r.veh().position.z;
        h.tick(200);
        CHECK(h.r.veh().veh.cmd_speed == 0);
        CHECK(h.r.veh().position.z <= z0 + 1.0f);
        CHECK(!h.r.veh().veh.net_engine_on);
    }
    {
        HeloRig h(/*rotor_full=*/false); // cold rotor: the LABEL_328 park
        h.route_to(300, 200, 40);
        const float z0 = h.r.veh().position.z;
        h.tick(200);
        CHECK(h.r.veh().veh.part_spin.speed < kRotorSpeedMax);
        CHECK(h.r.veh().veh.cmd_speed == 0);
        CHECK(!h.r.veh().veh.net_engine_on);
        CHECK(h.r.veh().position.z <= z0 + 1.0f);
    }
}

// The aircraft authority health machine [orig: @0x4903F0..0x490480 regen/burn,
// @0x492637..0x49266f the crash drain]: above criticalHp the hull regens on the
// 64-tick cadence; at or below it burns criticalDrain per cadence and, airborne
// with a target above ground, spins its yaw; past 100 deg of roll it bleeds 200
// a tick.
void test_helo_authority_health() {
    HeloRig h;
    h.route_to(300, 200, 40);
    h.r.veh().health = 1000;
    h.tick(128);
    CHECK(h.r.veh().health == 1000 + 2 * 10); // two cadence hits of regen
    // Burning: at critical the drain replaces the regen and the yaw spins.
    h.r.veh().health = 300;
    const int32_t yaw0 = h.r.veh().veh.yaw_bam;
    h.tick(64);
    CHECK(h.r.veh().health == 300 - 40);
    CHECK(h.r.veh().veh.yaw_bam != yaw0);
    // The crash drain: past 100 deg of roll, 200 a tick to zero.
    h.r.veh().health = 500;
    h.r.veh().veh.air_roll_bam = 0x50000000; // ~112 deg
    h.tick(2);
    CHECK(h.r.veh().health == 100);
    h.tick(1);
    CHECK(h.r.veh().health == 0);
}

// The redirect order reaches the BRAIN (mode/list/node + budget) and the BMS speed
// commands write kSpeedA/kSpeedB at the witnessed x65536/225 scale.
// [orig: Entity_SetWaypointByTeam @0x43cdb4; Entity_ApplyCommand @0x43ab60 0x1D/0x1E ->
//  AI_HandleCommand @0x465770 0xA/0xB]
void test_redirect_and_speed_commands() {
    Rig r(30.0f);
    r.veh().group_id = 3; // 00TRa's truck group
    const int ai_idx = r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.at(ai_idx);
    ve.pos[0] = 100 << 16;
    ve.pos[1] = 200 << 16;
    ve.net_id = 11;
    r.sys.nav.channels.resize(3);
    r.sys.nav.channels[2].count = 2;
    r.sys.nav.channels[2].entries[0] = 0;
    r.sys.nav.channels[2].entries[1] = 1;
    r.sys.nav.nodes.resize(2);
    r.sys.nav.nodes[0] = NavEntry{{1 << 16, 500 << 16, 200 << 16, 10 << 16, 0}};
    r.sys.nav.nodes[1] = NavEntry{{1 << 16, 150 << 16, 200 << 16, 10 << 16, 0}};

    // A stale spawn command in the SLOT must be overwritten by the redirect —
    // the 00TRg debarked-crew freeze: slot[37]=125 short-circuits the infantry
    // think forever if the redirect only writes the brain registers.
    ve.slot.f[37] = 125;
    CHECK(r.w.commands.group_to_waypoint(3, 2) == 1); // RedirectGroupTo(3, list 2)
    AiBrain &b = ve.brain;
    CHECK(b.f[AiBrain::kWpType] == 1);
    CHECK(b.f[AiBrain::kWpChannel] == 2);
    // Entry 1 (x=150) is nearest to x=100 — distinct from entry 0, which is also
    // the scan's fallback initializer, so a broken nearest-node scan fails here.
    CHECK(b.f[AiBrain::kWpNode] == 1);
    CHECK(r.veh().wp_number == 1);
    // The SLOT half of the witnessed redirect block [orig: Entity_SetWaypointByTeam
    // @0x43cdb4 — aiComp+140=1, +148=list, +152=node, carrier cleared]: the
    // infantry think navigates from these, not the brain registers.
    CHECK(ve.slot.f[35] == 1);
    CHECK(ve.slot.f[37] == 2);
    CHECK(ve.slot.f[38] == 1);
    CHECK(ve.slot.f[36] == 0);

    // BMS RedirectGroupTo carries an explicit node in param3. It must not be
    // replaced by the nearest-node sentinel used by the two-argument WAC form.
    CHECK(r.w.commands.group_to_waypoint(3, 2, 0) == 1);
    CHECK(b.f[AiBrain::kWpNode] == 0);
    CHECK(r.veh().wp_number == 0);

    // Speed commands retain retail's two-stage path: Entity_ApplyCommand queues
    // event 10/11 and AI_HandleCommand performs the conversion at dispatch.
    b.f[AiBrain::kCurState] = kAiGroundFollowWp;
    b.f[AiBrain::kPendState] = kAiGroundFollowWp;
    CHECK(r.w.commands.apply_group_ai_command(3, 30, 40, 0, 0) == 1);
    CHECK(b.f[AiBrain::kSpeedB] == 0);
    CHECK(r.sys.events.count() == 1);
    r.sys.events.process_timed(r.sys, r.w);
    CHECK(b.f[AiBrain::kSpeedB] == 11650);
    CHECK(r.w.commands.apply_group_ai_command(3, 29, 55, 0, 0) == 1);
    CHECK(b.f[AiBrain::kSpeedA] == 0);
    CHECK(r.sys.events.count() == 1);
    r.sys.events.process_timed(r.sys, r.w);
    CHECK(b.f[AiBrain::kSpeedA] == 16019); // trunc(55 * 1000 * 4.4444446e-6 * 65536)

    // A mounted NON-player in the group auto-detaches on redirect [orig: @0x43cdb4].
    Entity npc;
    npc.net_id = 900;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.group_id = 3;
    EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    CHECK(r.w.registry.get(nh)->mounted);
    r.w.commands.group_to_waypoint(3, 2);
    CHECK(!r.w.registry.get(nh)->mounted);

    // The budget's bearing error is a WRAPPING 32-bit sub [orig: a plain x86 sub,
    // @0x48bc9a-cf]: heading just short of +half-turn, node bearing just past
    // -half-turn = a ~0.1 deg true error, not ~360 deg. The unwrapped form cast a
    // NEGATIVE budget here, inverting the delta clamp.
    r.sys.nav.nodes[0] =
            NavEntry{{1 << 16, -300 * 65536, static_cast<int32_t>(199.5 * 65536),
                      10 << 16, 0}};
    ve.heading = 2147000000; // the +pi side of the seam
    r.sys.apply_route_order(ve, 2, 0);
    CHECK(b.f[AiBrain::kAnimFlag] >= 0);
    CHECK(b.f[AiBrain::kAnimFlag] < (1 << 22)); // the short-way error stays small
}

// The SP drive input mirror: a mounted LOCAL player's live move bits reach the wire
// input fields the motor consumes (pose_if_mounted mirrors them while tick_infantry is
// skipped), and the occupant leg drives the vehicle from them.
// [orig: one entity struct — MoveOrder feeds Entity_UpdateVehiclePhysics directly]
void test_local_player_drive_mirror() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicle_traits.set(r.veh().item_id, t);

    // The local player as an infantry-active AI entity in the ctrl seat.
    const int ai_idx = r.sys.attach(r.player_h);
    AiEntity &pe = *r.sys.at(ai_idx);
    pe.inf.active = true;
    pe.inf.is_local_player = true;
    pe.health = 150;
    r.player().ground_target = r.veh_h;
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h)); // deck path -> ctrl seat
    CHECK(is_vehicle_control_seat(r.player().mount_type));

    // Live input: forward held, looking along +x (mission yaw 90).
    pe.inf.player_moving = true;
    pe.inf.player_move_dir_index = 0;
    pe.inf.target_heading = bam_heading_from_mission_yaw_deg(90.0);

    const float x0 = r.veh().position.x;
    for (int i = 0; i < 124; ++i) {
        CHECK(r.sys.pose_if_mounted(pe, r.w)); // the mirror + seat carry
        tick_vehicle_motor(r.w, r.veh(), t);   // occupant leg reads the mirrored input
    }
    CHECK((r.player().net_move_input & 0x08u) != 0); // moving bit mirrored
    CHECK(r.veh().position.x - x0 > 1.0f);           // the truck drove
    // The seat carry kept the driver aboard.
    const float dx = r.player().position.x - r.veh().position.x;
    const float dy = r.player().position.y - r.veh().position.y;
    CHECK(std::sqrt(dx * dx + dy * dy) < 4.0f);

    // The seated LOOK stays mouse-instant at FULL precision on the AiEntity mirrors
    // the camera reads — a mid-ride look change lands the same tick, both axes
    // (tick_infantry's own mirrors are skipped for the whole ride)
    // [orig: Input_HandleActionBinding_0 @0x4e1330 -> entity+0x10/+0x14].
    pe.inf.target_heading = bam_heading_from_mission_yaw_deg(137.25);
    pe.inf.look_pitch = 12345678;
    CHECK(r.sys.pose_if_mounted(pe, r.w));
    CHECK(pe.heading == pe.inf.target_heading); // full precision, not the deg roundtrip
    CHECK(pe.pitch == 12345678);
}

// spawn_player stamps commandGroup 1 [orig: the deploy leg @0x519fd0] — 00TRa's tour
// dialogs ("group 1 enters area X") track the player through it.
void test_player_spawn_group() {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    AiSystem sys;
    w.registry.configure_pool(0, 8);
    w.ai = &sys;
    PlayerSpawn s;
    s.net_id = 0xFFF0;
    s.position = {10.0f, 10.0f, 5.0f};
    s.team = 1;
    s.health = 150;
    s.player_class = 8;
    const EntityHandle h = spawn_player(w, s);
    CHECK(h.valid());
    const Entity *e = w.registry.get(h);
    CHECK(e != nullptr && e->group_id == 1);
}

// The floating attach-label list [orig: draw_vehicle_seat_and_armory_labels @0x5a3290
// selection half]: free seats within the 4.0 u 3D radius label, occupied seats never
// label, the scan winner alone carries `nearest`, and a ready weapon limits labels to
// the nearest entity [orig: @0x5a3354].
void test_attach_labels_seats() {
    Rig r(2.0f);
    std::vector<AttachLabel> labels;
    collect_attach_labels(r.w, r.player(), /*armory_mode=*/false, /*can_fire=*/false, labels);
    CHECK(labels.size() == 2); // both free seats are inside 4.0 u
    int nearest_count = 0;
    for (const AttachLabel &l : labels) {
        CHECK(l.entity == r.veh_h);
        CHECK(!l.armory);
        CHECK(l.world_pos.z > r.veh().position.z); // the +0.1875 u lift applied
        if (l.nearest) ++nearest_count;
    }
    CHECK(nearest_count == 1); // exactly the scan winner [orig: the nearest compare @0x5a3640]

    // Occupied seats never label [orig: mountHandles != 0xFFFF skip @0x5a348f].
    r.veh().seats[1].occupant = r.player_h;
    labels.clear();
    collect_attach_labels(r.w, r.player(), false, false, labels);
    CHECK(labels.size() == 1);
    CHECK(labels[0].seat_index == 0);

    // Out of the 4.0 u label radius -> the gate empties the list via the nearest scan
    // [orig: the Entity_FindNearestSeatOrArmory bracket @0x5a32e2].
    Rig far(30.0f);
    labels.clear();
    collect_attach_labels(far.w, far.player(), false, false, labels);
    CHECK(labels.empty());
}

// can_fire keeps only the nearest ENTITY's labels: a second in-range vehicle labels only
// when the player cannot fire [orig: !Player_CanFireWeapon() || entity == nearest @0x5a3354].
void test_attach_labels_can_fire_gate() {
    Rig r(1.0f);
    Entity veh2;
    veh2.net_id = 12;
    veh2.kind = EntityKind::Item;
    veh2.item_id = 1295;
    veh2.position = {103.0f, 200.0f, 10.0f}; // ~2 u from the player at 101,200
    veh2.health = 500;
    veh2.health_max = 500;
    veh2.alive = true;
    Seat s;
    s.type = SeatType::Passenger;
    s.bone_index = 1;
    s.source_name = "sitex00";
    s.seat_local = {0.0f, 0.0f, 0.5f};
    veh2.seats.push_back(s);
    const EntityHandle veh2_h = r.w.registry.spawn(1, veh2);

    std::vector<AttachLabel> all;
    collect_attach_labels(r.w, r.player(), false, /*can_fire=*/false, all);
    bool saw_veh2 = false;
    for (const AttachLabel &l : all) saw_veh2 = saw_veh2 || l.entity == veh2_h;
    CHECK(all.size() >= 3); // both trucks' free seats
    CHECK(saw_veh2);

    std::vector<AttachLabel> armed;
    collect_attach_labels(r.w, r.player(), false, /*can_fire=*/true, armed);
    CHECK(!armed.empty());
    EntityHandle only = armed[0].entity;
    for (const AttachLabel &l : armed) CHECK(l.entity == only); // one entity's labels
}

// Armory mode: "armory*" points of Armory-attrib items label (SeatType::ArmoryPoint),
// seats do not; the same scan math picks the nearest [orig: the armory legs @0x4361ee /
// @0x5a36f5; searchMode = Flags & 0x400000 @0x5a32c4].
void test_attach_labels_armory_mode() {
    Rig r(2.0f);
    Entity crate;
    crate.net_id = 21;
    crate.kind = EntityKind::Item;
    crate.item_id = 1125; // "Armory Version #1"
    crate.position = {99.0f, 199.0f, 10.0f};
    crate.health = 100;
    crate.health_max = 100;
    crate.alive = true;
    crate.armory_points.push_back({0.0f, 0.0f, 1.0f});
    const EntityHandle crate_h = r.w.registry.spawn(1, crate);

    std::vector<AttachLabel> labels;
    collect_attach_labels(r.w, r.player(), /*armory_mode=*/true, false, labels);
    CHECK(labels.size() == 1); // the truck's seats do NOT label in armory mode
    CHECK(labels[0].entity == crate_h);
    CHECK(labels[0].armory);
    CHECK(labels[0].type == SeatType::ArmoryPoint);
    CHECK(labels[0].nearest);

    // Seat mode ignores armory points.
    labels.clear();
    collect_attach_labels(r.w, r.player(), false, false, labels);
    for (const AttachLabel &l : labels) CHECK(!l.armory);
}

// The HUD calls collect_attach_labels every render frame. One gather may walk many
// proximity candidates, but enemy occupancy is a world property and must be indexed
// with one registry pass rather than recomputed independently for every candidate.
void test_attach_labels_build_enemy_occupancy_once() {
    World w;
    CollisionWorld collision;
    w.collision = &collision;
    w.registry.configure_pool(0, 128);
    w.registry.configure_pool(1, 64);

    Entity player_seed;
    player_seed.kind = EntityKind::Organic;
    player_seed.position = {100.0f, 200.0f, 10.0f};
    player_seed.health = 100;
    player_seed.alive = true;
    player_seed.team = 1;
    const EntityHandle player_h = w.registry.spawn(0, player_seed);
    CHECK(player_h.valid());

    EntityHandle enemy_occupied_vehicle;
    for (int i = 0; i < 32; ++i) {
        Entity vehicle;
        vehicle.kind = EntityKind::Item;
        vehicle.position = {
            100.1f + 0.05f * static_cast<float>(i), 200.0f, 10.0f};
        vehicle.health = 100;
        vehicle.alive = true;
        vehicle.bound_radius = 1.0f;
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.bone_index = 1;
        seat.source_name = "sitex00";
        seat.seat_local = {0.0f, 0.0f, 1.0f};
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);
        CHECK(vehicle_h.valid());
        if (i == 31) enemy_occupied_vehicle = vehicle_h;
    }

    for (int i = 0; i < 64; ++i) {
        Entity organic;
        organic.kind = EntityKind::Organic;
        organic.position = {120.0f + static_cast<float>(i), 200.0f, 10.0f};
        organic.health = 100;
        organic.alive = true;
        organic.team = i == 63 ? 2 : 1;
        organic.mounted = i == 63;
        organic.mount_target = i == 63 ? enemy_occupied_vehicle : EntityHandle{};
        CHECK(w.registry.spawn(0, organic).valid());
    }

    for (int i = 0; i < 17; ++i) collision.build_tick_tables(w);
    CHECK(collision.attach_candidate_slices_authoritative());
    CHECK(collision.candidate_count(player_h) == 32);

    const Entity *player = w.registry.get(player_h);
    CHECK(player != nullptr);
    if (player == nullptr) return;
    AttachLabelScanStats stats;
    std::vector<AttachLabel> labels;
    collect_attach_labels(w, *player, false, false, labels, &stats);
    CHECK(labels.size() == 31);
    for (const AttachLabel &label : labels)
        CHECK(label.entity != enemy_occupied_vehicle);
    CHECK(stats.enemy_occupancy_registry_passes == 1);
}

} // namespace

// The hull-vs-world contact stops a driving vehicle at a building wall instead of
// passing through, and the contact decays speed by the def torque shift.
// [orig: Entity_CheckCollisionState @0x462a30 via the physics @0x47cb8c; severity
//  decay @0x47cc13-0x47ccc1]
void test_vehicle_hull_stops_at_building() {
    Rig r(30.0f);
    VehicleTraits t = truck_traits();
    t.torque = 2; // sev-3 decay = speed - (speed >> 4) per contact tick
    r.w.vehicle_traits.set(r.veh().item_id, t);

    CollisionWorld cw;
    r.sys.collision = &cw;

    // A building wall across the truck's path at x=150 (the truck faces +x from
    // x=100): 2u half-depth, 25u half-width (the spin-up arc drifts ~13u north), 5u tall.
    Entity wall;
    wall.kind = EntityKind::Building;
    wall.position = {150.0f, 200.0f, 10.0f};
    // Mission yaw 90 = engine heading 0 = identity section matrix (the matrix
    // bakes the 90-minus-yaw mission->engine convention): the box's thin local-x
    // axis lies along mission X — a wall square across the drive line.
    wall.yaw = 90;
    wall.alive = true;
    wall.health = 30000;
    r.w.registry.configure_pool(2, 4);
    EntityHandle wh = r.w.registry.spawn(2, wall);
    CollisionModel box;
    {
        auto plane = [&](int nx, int ny, int nz, double d) {
            CollisionPlane p;
            p.nx = static_cast<int16_t>(nx);
            p.ny = static_cast<int16_t>(ny);
            p.nz = static_cast<int16_t>(nz);
            p.dist = static_cast<int32_t>(d * 65536.0);
            box.planes.push_back(p);
        };
        plane(16384, 0, 0, -2.0);
        plane(-16384, 0, 0, -2.0);
        plane(0, 16384, 0, -25.0);
        plane(0, -16384, 0, -25.0);
        plane(0, 0, 16384, -5.0);
        plane(0, 0, -16384, 0.0);
        CollisionVolume v;
        v.type = 1; // solid
        v.min_x = static_cast<int32_t>(-2.0 * 65536);
        v.max_x = static_cast<int32_t>(2.0 * 65536);
        v.min_y = static_cast<int32_t>(-25.0 * 65536);
        v.max_y = static_cast<int32_t>(25.0 * 65536);
        v.min_z = 0;
        v.max_z = static_cast<int32_t>(5.0 * 65536);
        v.plane_start = 0;
        v.plane_count = 6;
        box.volumes.push_back(v);
        CollisionSection s;
        s.volume_start = 0;
        s.volume_count = 1;
        // The section AABB/bound the host fills from COBJ.
        s.min_x = v.min_x;
        s.max_x = v.max_x;
        s.min_y = v.min_y;
        s.max_y = v.max_y;
        s.min_z = v.min_z;
        s.max_z = v.max_z;
        s.center[2] = static_cast<int32_t>(2.5 * 65536);
        s.radius = static_cast<int32_t>(26.0 * 65536);
        box.sections.push_back(s);
    }
    cw.assign_entity(wh, cw.add_model(std::move(box)));

    // NPC driver + a route node BEYOND the wall: without the hull contact the truck
    // drives straight through x=150.
    const int ai_idx = r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.at(ai_idx);
    ve.pos[0] = 100 << 16;
    ve.pos[1] = 200 << 16;
    ve.pos[2] = 10 << 16;
    ve.heading = bam_heading_from_mission_yaw_deg(90.0); // face +x
    r.sys.nav.channels.resize(3);
    r.sys.nav.channels[2].count = 1;
    r.sys.nav.channels[2].entries[0] = 0;
    r.sys.nav.nodes.resize(1);
    r.sys.nav.nodes[0] = NavEntry{{2 << 16, 300 << 16, 200 << 16, 10 << 16, 0}};
    AiBrain &b = ve.brain;
    b.f[AiBrain::kCurState] = 16;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 2;
    b.f[AiBrain::kWpNode] = 0;
    b.f[AiBrain::kOutSpeed] = 40 * 293;

    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.team = 1;
    EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(entity_process_vehicle_attach(r.w, nh, r.veh_h, 1));
    Entity *ctrl = resolve_vehicle_controller(r.w, r.veh());
    CHECK(ctrl != nullptr);

    // Unit probe: with tables built and the hull point INSIDE the wall face, the
    // query must report the wall-severity push.
    {
        const double cases[3][2] = {{146.8, 200.0}, {146.8, 212.6}, {148.5, 212.6}};
        for (auto &c : cases) {
            r.veh().position = {static_cast<float>(c[0]), static_cast<float>(c[1]), 10.0f};
            for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
            const int32_t probe_pos[3] = {static_cast<int32_t>(c[0] * 65536),
                                          static_cast<int32_t>(c[1] * 65536), 10 << 16};
            const int32_t probe_prev[3] = {static_cast<int32_t>((c[0] - 0.2) * 65536),
                                           static_cast<int32_t>(c[1] * 65536), 10 << 16};
            int32_t pf[2];
            const int sev = cw.resolve_vehicle_hull(r.w, r.veh_h, probe_pos, probe_prev, pf);
            CHECK(sev == 3);  // a wall contact is the full-force class
            CHECK(pf[0] < 0); // pushed back out along -x
            CHECK(pf[1] == 0);
        }
        r.veh().position = {100.0f, 200.0f, 10.0f};
    }

    for (int i = 0; i < 950; ++i) {
        cw.build_tick_tables(r.w); // self-gated to the 17-tick cadence
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
        tick_vehicle_motor(r.w, r.veh(), t, &cmd);
        AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->pos[0] = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->pos[1] = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->pos[2] = static_cast<int32_t>(r.veh().position.z * 65536.0f);
        ve2->heading = r.veh().veh.yaw_bam;
    }
    // The wall's near face is x=148; the hull point (1.5u radius) holds the truck
    // outside it. Without the contact leg the truck ends far past 150.
    CHECK(r.veh().position.x > 110.0f);  // it drove
    CHECK(r.veh().position.x < 149.0f);  // and stopped at the wall
    CHECK(std::abs(r.veh().veh.speed) < 40 * 293 / 2); // the contact decay bit

}

// The seat/exit frame is the collision frame: entity_local_point_world must
// (a) reduce to the proven 2D -yaw rotate for flat carriers, and (b) match an
// independent double-precision Rz(heading)Ry(-pitch)Rx(roll) for rolled ones —
// the frame the collision shell is posed with (target_view). A yaw-only seat
// frame under a rolled carrier is the witnessed 00TRg dismount 4-pin
// (AI-PARITY-CONCEPT §6.13). [orig: @0x4b0c50 over @0x613f40]
void test_seat_frame_matches_collision_frame() {
    Entity veh{};
    veh.position = Vec3{100.0f, 200.0f, 30.0f};
    const Vec3 L{1.05f, -3.60f, 2.54f}; // DTruck1 sitex00d seat_local

    // (a) flat carrier: bit-compatible with the legacy 2D rotate.
    for (float yaw : {0.0f, 37.0f, 145.0f, 270.0f}) {
        veh.yaw = static_cast<int16_t>(yaw);
        veh.pitch = 0;
        veh.roll = 0;
        const Vec3 got = entity_local_point_world(veh, L);
        const double a = -static_cast<double>(veh.yaw) * 3.14159265358979323846 / 180.0;
        const double ca = std::cos(a), sa = std::sin(a);
        CHECK(std::fabs(got.x - (veh.position.x + (L.x * ca - L.y * sa))) < 1e-3);
        CHECK(std::fabs(got.y - (veh.position.y + (L.x * sa + L.y * ca))) < 1e-3);
        CHECK(std::fabs(got.z - (veh.position.z + L.z)) < 1e-3);
    }

    // (b) rolled/pitched carrier vs an independent double-precision euler
    // composition of the SAME convention the collision matrix builder uses:
    // Rz(heading) * Ry(-pitch) * Rx(roll) over the RAW model point (seat_local
    // is pre-swizzled, raw = (L.y, -L.x, L.z)), heading = bam(90 - yaw).
    veh.yaw = 20;
    veh.pitch = -6;
    veh.roll = 16;
    const Vec3 got = entity_local_point_world(veh, L);
    const double d2r = 3.14159265358979323846 / 180.0;
    const double h = (90.0 - static_cast<double>(veh.yaw)) * d2r;
    const double pt = -static_cast<double>(veh.pitch) * d2r;
    const double rl = static_cast<double>(veh.roll) * d2r;
    // raw model frame (un-swizzle), then Rx(roll)
    double v0[3] = {L.y, -L.x, L.z};
    double v1[3] = {v0[0], v0[1] * std::cos(rl) - v0[2] * std::sin(rl),
                    v0[1] * std::sin(rl) + v0[2] * std::cos(rl)};
    // Ry(pt) (the -pitch already folded into pt)
    double v2[3] = {v1[0] * std::cos(pt) + v1[2] * std::sin(pt), v1[1],
                    -v1[0] * std::sin(pt) + v1[2] * std::cos(pt)};
    // Rz(h)
    double v3[3] = {v2[0] * std::cos(h) - v2[1] * std::sin(h),
                    v2[0] * std::sin(h) + v2[1] * std::cos(h), v2[2]};
    CHECK(std::fabs(got.x - (veh.position.x + v3[0])) < 0.001);
    CHECK(std::fabs(got.y - (veh.position.y + v3[1])) < 0.001);
    CHECK(std::fabs(got.z - (veh.position.z + v3[2])) < 0.001);
    // The property the 4-pin hinged on: a rolled carrier MOVES a laterally
    // offset point's world z off the flat-frame value (pure roll 25 on a
    // 1.5-u lateral offset shifts z by 1.5*sin(25) ~= 0.63).
    veh.yaw = 0;
    veh.pitch = 0;
    veh.roll = 25;
    const Vec3 lat = entity_local_point_world(veh, Vec3{1.5f, 0.0f, 0.0f});
    CHECK(std::fabs(lat.z - veh.position.z) > 0.5);
}

int main() {
    test_seat_frame_matches_collision_frame();
    test_usegun_attach_presnaps_local_look();
    test_host_crewed_helicopter_rotor_turns();
    test_remote_player_control_seat_preserves_wire_look();
    test_live_pose_provider_and_static_fallback();
    test_toggle_nearest_seat();
    test_best_seat_walks_vehicle_children();
    test_attach_scan_never_built_fallback_and_initial_empty_slice();
    test_post_epoch_player_spawn_discovers_nearby_seat_immediately();
    test_restore_refreshes_completed_candidate_epoch();
    test_toggle_deck_best_seat();
    test_toggle_dismount_and_swap();
    test_enemy_occupant_blocks_scan();
    test_bms_mount_predicates();
    test_mounted_ammo_slot_route();
    test_prepare_vehicle_weapon_slot_after_armory_load();
    test_ai_drive_leg();
    test_ai_drive_avoid_brake();
    test_stuck_check();
    test_min_ai_crew_clamp();
    test_handbrake_latch();
    test_ground_waits_for_boarders();
    test_helo_ai_flight();
    test_helo_authority_health();
    test_redirect_and_speed_commands();
    test_local_player_drive_mirror();
    test_player_spawn_group();
    test_attach_labels_seats();
    test_attach_labels_can_fire_gate();
    test_attach_labels_armory_mode();
    test_attach_labels_build_enemy_occupancy_once();
    test_vehicle_hull_stops_at_building();
    if (failures == 0) std::printf("vehicle_mount_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
