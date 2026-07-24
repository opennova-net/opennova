// The USE-ITEM mount chain + the BMS Player mount triggers + the AI-driver leg
// (the vehicle pass, 2026-07-16 witness):
//  - Entity_ToggleVehicleMount @0x436950 / Entity_TryEnterNearestVehicle @0x4368c0 /
//    Entity_FindNearestSeatOrArmory @0x435d50 / Entity_FindBestSeatSlot @0x4351f0
//  - EventTrigger cat-7 subs 38-41 -> @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0
//  - Entity_UpdateVehiclePhysics @0x48af00: parked stamp @0x48c002-0x48c02d + the
//    AI-driver leg @0x48bc12-0x48c034
//  - the player deploy group stamp @0x519fd0 (commandGroup = 1)
#include "world/ai.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/entity.h"
#include "world/player_spawn.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

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

struct FakeMountedPoseProvider final : IMountedPoseProvider {
    bool available = true;
    MountedPose pose;

    bool resolve_mounted_pose(World &, const Entity &, const Seat &,
                              MountedPose &out) override {
        if (!available) return false;
        out = pose;
        return true;
    }
};

struct StubCollisionMatrixProvider final : ICollisionSectionMatrixProvider {
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

struct HeadingMountedPoseProvider final : IMountedPoseProvider {
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
// [orig: Entity_RequestVehicleAttach @0x4364a0, UseGun @0x43656c]
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

// A model-aware host supplies the complete live seat-bone frame at the shared
// World seam. Every attach entry point consumes it immediately, and mounted AI
// consumes a newly evaluated frame on its next tick. A missing/declining host
// retains the portable static seat geometry.
void test_live_mounted_pose_provider_and_static_fallback() {
    // EntityCommands mount consumes the host pose on the attach edge.
    {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        FakeMountedPoseProvider provider;
        provider.pose = {{101.25f, -42.5f, 8.75f}, 37, -12, 17};
        w.mounted_pose_provider = &provider;

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
        w.mounted_pose_provider = &provider;

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
        w.mounted_pose_provider = &provider;

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
        const MountedPose expected = {{13.0f, 18.0f, 34.0f}, 100, 4, 5};

        Entity occupant;
        pose_mounted_occupant(w, occupant, vehicle, seat);
        check_pose(occupant, expected);

        FakeMountedPoseProvider provider;
        provider.available = false;
        provider.pose = {{999.0f, 999.0f, 999.0f}, -1, -2, -3};
        w.mounted_pose_provider = &provider;
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

// The portable whole-registry walk is only for a world that never built the
// collision tables. Once a live table build has happened, the retail BSS-zero
// cadence leaves the first 16 ticks deliberately sliceless.
void test_attach_scan_never_built_fallback_and_initial_empty_slice() {
    {
        Rig r(2.0f);
        CollisionWorld cw;
        r.w.collision = &cw;
        NearestSeatHit hit;
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
        NearestSeatHit hit;
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
        cw.set_section_matrix_provider(&provider);
        r.w.collision = &cw;
        cw.build_tick_tables(r.w);
        CHECK(cw.tick_tables_ready());
        CHECK(!cw.attach_candidate_slices_authoritative());
        NearestSeatHit hit;
        CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
        for (int i = 1; i < 17; ++i) cw.build_tick_tables(r.w);
        CHECK(cw.attach_candidate_slices_authoritative());
        CHECK(cw.candidate_count(r.player_h) == 1);
        hit = NearestSeatHit{};
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
        NearestSeatHit hit;
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
        NearestSeatHit hit;
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
    NearestSeatHit hit;
    CHECK(find_nearest_free_seat(r.w, r.player(), hit, false));
    CHECK(hit.vehicle == other_h);

    r.w.restore(baseline);
    hit = NearestSeatHit{};
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
    NearestSeatHit hit;
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

    CHECK(r.w.commands.group_to_waypoint(3, 2) == 1); // RedirectGroupTo(3, list 2)
    AiBrain &b = ve.brain;
    CHECK(b.f[AiBrain::kWpType] == 1);
    CHECK(b.f[AiBrain::kWpChannel] == 2);
    // Entry 1 (x=150) is nearest to x=100 — distinct from entry 0, which is also
    // the scan's fallback initializer, so a broken nearest-node scan fails here.
    CHECK(b.f[AiBrain::kWpNode] == 1);
    CHECK(r.veh().wp_number == 1);

    // BMS RedirectGroupTo carries an explicit node in param3. It must not be
    // replaced by the nearest-node sentinel used by the two-argument WAC form.
    CHECK(r.w.commands.group_to_waypoint(3, 2, 0) == 1);
    CHECK(b.f[AiBrain::kWpNode] == 0);
    CHECK(r.veh().wp_number == 0);

    // PatrolSpeed 40 -> kSpeedB = trunc(40 * 65536/225) = 11650. CombatSpeed -> kSpeedA.
    CHECK(r.w.commands.apply_group_ai_command(3, 30, 40, 0, 0) == 1);
    CHECK(b.f[AiBrain::kSpeedB] == 11650);
    CHECK(r.w.commands.apply_group_ai_command(3, 29, 55, 0, 0) == 1);
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

int main() {
    test_usegun_attach_presnaps_local_look();
    test_live_mounted_pose_provider_and_static_fallback();
    test_toggle_nearest_seat();
    test_attach_scan_never_built_fallback_and_initial_empty_slice();
    test_post_epoch_player_spawn_discovers_nearby_seat_immediately();
    test_restore_refreshes_completed_candidate_epoch();
    test_toggle_deck_best_seat();
    test_toggle_dismount_and_swap();
    test_enemy_occupant_blocks_scan();
    test_bms_mount_predicates();
    test_ai_drive_leg();
    test_ai_drive_avoid_brake();
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
