// The USE-ITEM mount chain + the BMS Player mount triggers + the AI-driver leg
// (the vehicle pass, 2026-07-16 witness):
//  - Entity_ToggleVehicleMount @0x436950 / Entity_TryEnterNearestVehicle @0x4368c0 /
//    Entity_FindNearestSeatOrArmory @0x435d50 / Entity_FindBestSeatSlot @0x4351f0
//  - EventTrigger cat-7 subs 38-41 -> @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0
//  - Entity_UpdateVehiclePhysics @0x48af00: parked stamp @0x48c002-0x48c02d + the
//    AI-driver leg @0x48bc12-0x48c034
//  - the player deploy group stamp @0x519fd0 (commandGroup = 1)
#include <base/io/bam.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_pose.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/mounted_pose.h>
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
    t.brain_class = VehicleBrainClass::Ground; // ai_function cveh: the vehicle machine
    return t;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    AiSystem &sys = w.ai;
    EntityHandle veh_h, player_h;

    explicit Rig(float player_dx = 2.0f) {
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);

        Entity veh;
        veh.net_id = 11; // 00TRa's DTruck1 SSN
        veh.bms_id = 11;
        veh.kind = EntityKind::Item;
        veh.has_item_def = true; // pool-1 sources need an ItemDef (retail gate)
        veh.item_id = 1294;
        veh.item_type_index = 7; // the def row's ordinal, the script gates' +0x1C
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

// Point a player's view at a world point (the seat point before its +0.1875 u
// scan bias): the USE scan's aim cone is measured from the entity Yaw/Pitch
// [orig: Entity_FindNearestSeatOrArmory @0x4360b6 / @0x4360c6], which a body
// carries in BAM32 and a bare entity as the mission-degree mirror.
void look_at(World &w, EntityHandle player_h, const Vec3 &target) {
	Entity *player = w.registry.get(player_h);
	if (player == nullptr) return;
	const double dx = static_cast<double>(target.x) - player->position.x -
			player->eye_offset_x / 65536.0;
	const double dy = static_cast<double>(target.y) - player->position.y -
			player->eye_offset_y / 65536.0;
	const double dz = static_cast<double>(target.z) + 0.1875 - player->position.z -
			player->eye_offset_z / 65536.0;
	const double heading = std::atan2(dy, dx);
	const double pitch = std::atan2(dz, std::hypot(dx, dy));
	player->yaw = static_cast<int16_t>(std::lround(90.0 - heading * 180.0 / 3.14159265358979323846));
	player->pitch = static_cast<int16_t>(std::lround(pitch * 180.0 / 3.14159265358979323846));
	if (AiEntity *body = w.ai.for_handle(player_h)) {
		body->heading = static_cast<int32_t>(heading * opennova::io::kBamPerRadian);
		body->pitch = static_cast<int32_t>(pitch * opennova::io::kBamPerRadian);
	}
}

MountedPose pose_degrees(Vec3 position, double yaw, double pitch, double roll) {
    return {position, bam_heading_from_mission_yaw_deg(yaw),
            bam_from_degrees_wrapped(pitch), bam_from_degrees_wrapped(roll)};
}

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
        if (!carrier.primary_weapon_owner.valid()) return false;
        const AiEntity *gunner = world.ai.for_handle(carrier.primary_weapon_owner);
        if (gunner == nullptr) return false;
        headings.push_back(gunner->heading);
        pitches.push_back(gunner->pitch);
        const float heading_steps = static_cast<float>(gunner->heading) / 16777216.0f;
        const float pitch_steps = static_cast<float>(gunner->pitch) / 16777216.0f;
        out.position = {heading_steps, pitch_steps, 41.0f};
        out.heading = bam_heading_from_mission_yaw_deg(10 + std::lround(heading_steps));
        out.pitch = bam_from_degrees_wrapped(20 + std::lround(pitch_steps));
        out.roll = bam_from_degrees_wrapped(-30);
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
    CHECK(entity.yaw == std::lround(mission_yaw_deg_from_bam_heading(expected.heading)));
    CHECK(entity.pitch == std::lround(expected.pitch * kDegreesPerBam));
    CHECK(entity.roll == std::lround(expected.roll * kDegreesPerBam));
}

// Entity_RequestVehicleAttach pre-snaps the requester's Yaw to the chosen seat
// before the authority applies the relationship. A UseGun seat faces the
// requester along the gun: the gun's own Yaw less its stored yaw word (+0x322)
// shifted up; the seat userpoint's facing turns only the carried body frame.
// Our split local-player body must update target_heading too; otherwise
// pose_if_mounted immediately restores the pre-attach look.
// [orig: Entity_RequestVehicleAttach @0x4364a0, its UseGun leg
//  @0x43655F..0x43656E (the subtract @0x43656c)]
void test_usegun_attach_presnaps_local_look() {
    World w;
    AiSystem &ai = w.ai;
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
    gun.emplaced_gun_yaw_word = 0x0400; // a previous gunner left it traversed
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

    CHECK(w.vehicles.process_attach(player_h, gun_h, 6));
    const int16_t expected_yaw = 23; // the carried frame: vehicle yaw 35 - UseGun offset 12
    // The unmoved gun's spawn heading ((55 << 16) / 360) << 16 = 656146432 less
    // 0x0400 << 16. [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
    const int32_t expected_heading = 589037568;
    CHECK(w.registry.get(player_h)->yaw == expected_yaw);
    CHECK(body.heading == expected_heading);
    CHECK(body.inf.target_heading == expected_heading);
    CHECK(body.inf.look_pitch == 0x12345678); // request pre-snap is yaw-only

    CHECK(ai.pose_if_mounted(body, w));
    CHECK(body.heading == expected_heading);
    // The unmoved gun's spawn heading ((55 << 16) / 360) << 16 = 656146432, turned by the
    // 12-degree seat offset. [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
    CHECK(body.inf.body_heading == 799312009);
}

// A remote player's LOOK is still independent in either vehicle-control seat.
// The authority motor consumes Entity::yaw as the mouse-steer target, while the
// carried body frame remains owned by the seat. The post-motor refresh must keep
// that split intact for the outgoing snapshot and the next authority tick.
void test_remote_player_control_seat_preserves_wire_look() {
    const SeatType control_types[] = {SeatType::Driver, SeatType::Controller};
    for (const SeatType control_type : control_types) {
        World w;
        AiSystem &ai = w.ai;
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

        CHECK(w.vehicles.process_attach(player_h, vehicle_h, 1));

        const int32_t look_heading = bam_heading_from_mission_yaw_deg(45.0);
        constexpr int32_t look_pitch = 0x06000000;
        body.heading = look_heading;
        body.pitch = look_pitch;
        w.registry.get(player_h)->yaw = 45;

        CHECK(ai.pose_if_mounted(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->yaw == 45);
        CHECK(body.inf.body_heading == 0x40000000); // yaw 0: the spawn form of 90

        Entity *live_vehicle = w.registry.get(vehicle_h);
        live_vehicle->position = {40.0f, 50.0f, 60.0f};
        live_vehicle->yaw = 20;
        CHECK(ai.refresh_mounted_pose(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->yaw == 45);
        CHECK(body.inf.body_heading == 835125248); // ((70 << 16) / 360) << 16
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
        provider.pose = pose_degrees({101.25f, -42.5f, 8.75f}, 37, -12, 17);
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
        AiSystem &ai = w.ai;
        FakeMountedPoseProvider provider;
        provider.pose = pose_degrees({11.0f, 12.0f, 13.0f}, 21, -8, 9);
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

        CHECK(w.vehicles.process_attach(occupant_h, vehicle_h, 7));
        check_pose(*w.registry.get(occupant_h), provider.pose);

        // The carrier changes the BODY frame; idle LOOK remains independent.
        body->heading = body->inf.aim_heading = 0;
        body->pitch = body->inf.aim_pitch = 0;
        provider.pose = pose_degrees({31.5f, 32.25f, 33.75f}, 44, 15, -19);
        TickContext ctx{};
        ctx.is_authority = true;
        w.update_all_entities(ctx);

        const Entity *mounted = w.registry.get(occupant_h);
        MountedPose expected = provider.pose;
        expected.heading = 0; // BAM look 0, while the seated body faces yaw 44.
        check_pose(*mounted, expected);
        CHECK(body->heading == 0 && !body->inf.aim_valid);
        CHECK(body->pos[0] == to_fixed(31.5));
        CHECK(body->pos[1] == to_fixed(32.25));
        CHECK(body->pos[2] == to_fixed(33.75));
        CHECK(body->inf.body_heading == provider.pose.heading);
        CHECK(body->body_pitch == provider.pose.pitch);
        CHECK(body->roll == provider.pose.roll);
    }

    {
        // A non-local Gunner first resolves the existing clamp base, chases its
        // independent look, then resolves the final Hn+1 parent phase. The local
        // path already exposes current input before its single resolve.
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        AiSystem &ai = w.ai;
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

        CHECK(w.vehicles.process_attach(occupant_h, vehicle_h, 7));
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
        CHECK(body->inf.body_heading == bam_heading_from_mission_yaw_deg(12));
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
        const MountedPose expected = pose_degrees({12.726887f, 17.658988f, 34.010455f}, 100, 4, 5);

        Entity occupant;
        w.vehicles.pose_mounted_occupant(occupant, vehicle, seat);
        check_pose(occupant, expected);

        FakeMountedPoseProvider provider;
        provider.available = false;
        provider.pose = pose_degrees({999.0f, 999.0f, 999.0f}, -1, -2, -3);
        w.pose_provider = &provider;
        occupant = Entity{};
        w.vehicles.pose_mounted_occupant(occupant, vehicle, seat);
        check_pose(occupant, expected);
    }
}

// An unarmed player's USE onto a UseGun seat is refused outside a session:
// single player, the in-process listen server included, is outside it; in a
// session the same USE mounts the gun.
// [orig: Entity_AttachToUseGunSlot @0x546b80 -- `cmp g_napi_np_ctx.is_in_session`
//  @0x546BF6, the player bit @0x546BFE, the EquippedSlot test @0x546C07;
//  SinglePlayer_StartMission @0x561AF0 leaves is_in_session clear]
void test_unarmed_usegun_needs_the_session() {
    for (const bool mp : {false, true}) {
        Rig r(2.0f);
        r.veh().seats[0].type = SeatType::Gunner;
        r.veh().primary_weapon = "WPN_USEGUN";
        r.w.tables.weapons.entries.resize(1);
        r.w.tables.weapons.entries[0].name = "WPN_USEGUN";
        r.w.tables.weapons.entries[0].valid = true;
        r.player().flags |= kEntityFlagPlayer;
        r.player().engine_flags |= kEntityFlagPlayer;
        r.w.rules.mp_session = mp;
        LocalPlayer local(r.w);
        CHECK(!local.weapon.active);
        CHECK(local.toggle_mount() == mp);
        CHECK(r.player().mounted == mp);
    }
}

// The nearest-seat toggle: a free seat within the 4.0 u horizontal gate attaches; out of
// range does nothing. [orig: @0x435d50 gate @0x436123; @0x436950]
void test_toggle_nearest_seat() {
    {
        Rig r(2.0f);
        CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(r.player().mounted);
        CHECK(r.player().mount_target == r.veh_h);
        CHECK((r.player().flags & 0x40u) != 0);
        CHECK((r.player().engine_flags & 0x40u) != 0);
        // Facing +y from +2x, only the ctrl bone (+0.5,+1.5 local) sits inside the
        // standing 90 deg aim cone; the sitex (0,-2) is over 100 deg off the view
        // [orig: the cone gate @0x43611f over 0x3FFFFFC0].
        CHECK(r.player().mount_seat == 0);
    }
    {
        // Looking at the SITEX from -2.5x: its aim term is ~0 while the ctrl's
        // 65 deg offset costs 1.5M on the score (dist3d + aim/512), so the sitex
        // wins on score, not seat weight (the deck weights never apply here)
        // [orig: @0x436111].
        Rig r(-2.5f);
        look_at(r.w, r.player_h, entity_local_point_world(r.veh(), r.veh().seats[1].seat_local));
        CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(r.player().mount_seat == 1);
    }
    {
        Rig r(30.0f); // far outside the 4 u gate
        CHECK(!r.w.vehicles.player_toggle_mount(r.player_h));
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
    CHECK(w.vehicles.attach_to_seat(player_h, selected));
    CHECK(w.registry.get(player_h)->mount_target == child_h);
    CHECK(w.registry.get(child_h)->seats[1].occupant == player_h);

    CHECK(w.vehicles.detach(player_h));
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
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
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
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
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
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
        for (int i = 1; i < 17; ++i) cw.build_tick_tables(r.w);
        CHECK(cw.attach_candidate_slices_authoritative());
        CHECK(cw.candidate_count(r.player_h) == 1);
        hit = VehicleSeatSelection{};
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
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
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        for (int i = 1; i < 17; ++i) cw.build_tick_tables(r.w);
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
    }
}

// A deploy/respawn after a completed proximity epoch rebuilds the new player's
// source slice immediately. Waiting for the normal 17-tick cadence would make
// USE and attach labels disappear just after spawning.
void test_post_epoch_player_spawn_discovers_nearby_seat_immediately() {
    World w;
    AiSystem &ai = w.ai;
    CollisionWorld cw;
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
    // The spawn faces +x; the seat is at -x, so look at it (the aim cone).
    look_at(w, player_h, entity_local_point_world(*w.registry.get(vehicle_h), seat.seat_local));
    const Entity *player = w.registry.get(player_h);
    CHECK(player != nullptr);
    if (player != nullptr) {
        VehicleSeatSelection hit;
        CHECK(w.vehicles.find_nearest_free_seat(*player, hit, false));
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
    look_at(r.w, r.player_h, {200.0f, 200.0f, 11.0f}); // the aim cone
    for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
    VehicleSeatSelection hit;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(hit.vehicle == other_h);

    r.w.restore(baseline);
    hit = VehicleSeatSelection{};
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(hit.vehicle == r.veh_h);
}

// The deck path: standing ON the vehicle (ground_target) picks the BEST seat by weight —
// ctrl/drvr (0x2000) beats sitex (0x200000). [orig: @0x4368cf -> @0x4351f0]

void test_queued_player_mount_consumes_teleport_fallback_once() {
    Rig r(30.0f);
    r.sys.attach(r.player_h);
    auto &body = *r.sys.for_handle(r.player_h);
    body.inf.active = true;
    body.inf.adm_id = 1;
    body.inf.is_local_player = true;
    CHECK(r.w.commands.teleport_local_to_ssn(r.veh_h));
    r.player().ground_target = {}; // ground probe cleared the live reference
    r.player().engine_flags |= 0x200u;
    r.sys.tick_infantry(body, r.w, 1);
    CHECK(r.player().mounted && r.player().mount_target == r.veh_h);
    CHECK(r.player().mount_type == SeatType::Controller);
    CHECK(((r.player().flags | r.player().engine_flags) & 0x200u) == 0);
    r.sys.tick_infantry(body, r.w, 2);
    CHECK(r.player().mounted); // no second USE/detach on the next tick

    Rig failed(30.0f);
    failed.sys.attach(failed.player_h);
    auto &failed_body = *failed.sys.for_handle(failed.player_h);
    failed_body.inf.active = true;
    failed_body.inf.adm_id = 1;
    failed_body.inf.is_local_player = true;
    failed.player().engine_flags |= 0x200u;
    failed.sys.tick_infantry(failed_body, failed.w, 1);
    CHECK(!failed.player().mounted);
    CHECK(((failed.player().flags | failed.player().engine_flags) & 0x200u) == 0);
}

void test_vehicle_respawn_clears_matching_teleport_ground_backup() {
    Rig r;
    CHECK(r.w.commands.teleport_local_to_ssn(r.veh_h));
    Entity other;
    other.item_id = 11;
    other.mount_toggle_fallback = r.veh_h; // unmatched ground does not clear
    const auto unmatched = r.w.registry.spawn(0, other);
    other.item_id = 0;
    other.ground_target = r.veh_h; // raw zero type index does not participate
    const auto no_item = r.w.registry.spawn(0, other);
    r.w.vehicles.respawn(r.veh());
    CHECK(!r.player().ground_target.valid());
    CHECK(!r.player().mount_toggle_fallback.valid());
    CHECK(r.w.registry.get(unmatched)->mount_toggle_fallback == r.veh_h);
    CHECK(r.w.registry.get(no_item)->ground_target == r.veh_h);
    CHECK(r.w.registry.get(no_item)->mount_toggle_fallback == r.veh_h);
}

// The toggle's two arms key on the queued Co-op spawn-marker mount latch
// (Flags 0x200) alone: with it set, the best free seat of the groundEntity
// carrier or NOTHING (a full carrier does not fall through to the scan); with
// it clear, the look-cone scan even for a player standing on the carrier's
// deck — he boards the seat he looks at, not the priority seat.
// [orig: Entity_TryEnterNearestVehicle @0x4368CF / @0x4368E0 / @0x436903 /
//  @0x43691A; the latch's writer Server_PositionPlayerForSpawn @0x50D44D]
void test_toggle_deck_best_seat() {
    {
        Rig r(30.0f); // out of scan range: only the queued arm can mount
        r.player().ground_target = r.veh_h;
        r.player().flags |= kEntityFlagQueuedMount;
        CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(r.player().mounted);
        CHECK(r.player().mount_type == SeatType::Controller);
    }
    {
        Rig r(30.0f); // a bare deck stander out of scan range mounts nothing
        r.player().ground_target = r.veh_h;
        CHECK(!r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(!r.player().mounted);
    }
    {
        // A deck stander in reach boards the LOOKED-AT passenger seat, not the
        // controller the priority weights would pick.
        Rig r(-2.5f);
        r.player().ground_target = r.veh_h;
        look_at(r.w, r.player_h, entity_local_point_world(r.veh(), r.veh().seats[1].seat_local));
        CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(r.player().mount_seat == 1);
        CHECK(r.player().mount_type == SeatType::Passenger);
    }
    {
        // The queued arm on a FULL carrier returns nothing and never scans a
        // neighbouring vehicle, even one with a free seat in reach.
        Rig r(2.0f);
        Entity other;
        other.kind = EntityKind::Organic;
        other.item_id = 5305;
        other.player_class = 8;
        other.health = 150;
        other.alive = true;
        const EntityHandle oh1 = r.w.registry.spawn(0, other);
        const EntityHandle oh2 = r.w.registry.spawn(0, other);
        CHECK(r.w.vehicles.process_attach(oh1, r.veh_h, 1));
        CHECK(r.w.vehicles.process_attach(oh2, r.veh_h, 2));
        Entity neighbour = r.veh();
        neighbour.net_id = 12;
        neighbour.bms_id = 12;
        neighbour.position = {102.0f, 201.0f, 10.0f}; // beside the player: seats in reach
        for (Seat &seat : neighbour.seats) seat.occupant = EntityHandle{};
        neighbour.primary_occupant = EntityHandle{};
        const EntityHandle nh = r.w.registry.spawn(1, neighbour);
        r.player().ground_target = r.veh_h;
        r.player().flags |= kEntityFlagQueuedMount;
        look_at(r.w, r.player_h, entity_local_point_world(*r.w.registry.get(nh), r.veh().seats[0].seat_local));
        CHECK(!r.w.vehicles.player_toggle_mount(r.player_h));
        CHECK(!r.player().mounted);
        VehicleSeatSelection preview;
        CHECK(!r.w.vehicles.find_mount_toggle_candidate(r.player(), preview));
        // Without the latch the same press scans and finds the neighbour.
        r.player().flags &= ~kEntityFlagQueuedMount;
        CHECK(r.w.vehicles.find_mount_toggle_candidate(r.player(), preview));
        CHECK(preview.vehicle == nh);
    }
}

CollisionModel make_seat_hull() {
	CollisionModel box;
	const auto plane = [&](int x, int y, int z, int d) {
		CollisionPlane p;
		p.nx = static_cast<int16_t>(x);
		p.ny = static_cast<int16_t>(y);
		p.nz = static_cast<int16_t>(z);
		p.dist = d;
		box.planes.push_back(p);
	};
	plane(16384, 0, 0, -65536);
	plane(-16384, 0, 0, -65536);
	plane(0, 16384, 0, -65536);
	plane(0, -16384, 0, -65536);
	plane(0, 0, 16384, -2 * 65536);
	plane(0, 0, -16384, 0);
	CollisionVolume volume;
	volume.type = 1;
	volume.min_x = volume.min_y = -65536;
	volume.max_x = volume.max_y = 65536;
	volume.min_z = 0;
	volume.max_z = 2 * 65536;
	volume.plane_start = 0;
	volume.plane_count = 6;
	box.volumes.push_back(volume);
	CollisionSection section;
	section.volume_start = 0;
	section.volume_count = 1;
	section.min_x = section.min_y = -65536;
	section.max_x = section.max_y = 65536;
	section.min_z = 0;
	section.max_z = 2 * 65536;
	section.center[2] = 65536;
	section.radius = 2 * 65536;
	box.sections.push_back(section);
	return box;
}

// A mounted USE scans first and swaps onto a free seat only when the rider is
// LOOKING at it: the seated aim cone is 5.0 deg (0x38E38E0 BAM32) against the
// standing 90 deg, so USE dismounts unless a seat sits inside that cone. The
// carrier's own hull never blocks the ray (the USE LOS walker skips the
// endpoint entity and both parent slots), so an interior ray through the real
// hull is clear and the cone alone decides.
// [orig: Entity_ToggleVehicleMount @0x4369ac..0x4369c7 (scan, then
//  TryEnterNearestVehicle / SendDetachPacket); the cone caps @0x435d90 /
//  @0x435d9a and their gate @0x43611f; raycast_against_entity_pool ctx[17..20]
//  skips @0x538832..0x538859]
void test_toggle_dismount_and_swap() {
	Rig r;
	r.veh().item_type = 1;
	r.veh().seats[0].seat_local = { -2, 0, 1 };
	r.veh().seats[1].seat_local = { 1, 0, 1 };
	r.player().position = entity_local_point_world(r.veh(), { -2.5f, 0, 1 });
	CollisionWorld collision;
	StubCollisionMatrixProvider provider;
	collision.set_pose_provider(&provider);
	r.w.collision = &collision;
	collision.assign_entity(r.veh_h, collision.add_model(make_seat_hull()));
	for (int i = 0; i < 17; ++i)
		collision.build_tick_tables(r.w);
	const Vec3 ctrl = entity_local_point_world(r.veh(), r.veh().seats[0].seat_local);
	const Vec3 sit = entity_local_point_world(r.veh(), r.veh().seats[1].seat_local);
	look_at(r.w, r.player_h, ctrl);
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(r.player().mount_type == SeatType::Controller);
	// Seated, looking along the carrier (+y) with the sitex 90 deg to the side:
	// outside the 5 deg cone -> the scan is empty -> dismount.
	r.player().yaw = 0;
	r.player().pitch = 0;
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(!r.player().mounted);
	// Back in the control seat and looking straight at the sitex 3.0 u across
	// the hull box: the ray crosses the real hull, the endpoint/parent skips
	// admit it, the cone holds it -> swap, not dismount.
	look_at(r.w, r.player_h, ctrl);
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(r.player().mount_type == SeatType::Controller);
	look_at(r.w, r.player_h, sit);
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(r.player().mounted && r.player().mount_type == SeatType::Passenger);
	// Looking away from the freed control seat (it is behind the rider now):
	// its yaw offset clamps at +100 deg, far outside the cone -> dismount.
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(!r.player().mounted);

	// Beyond the 4.0 u 3D reach even when looked at: no candidate
	// [orig: @0x436113 dist3d <= 0x40000].
	r.veh().seats[1].seat_local = { 4.5f, 0, 1 };
	look_at(r.w, r.player_h, ctrl);
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(r.player().mount_type == SeatType::Controller);
	look_at(r.w, r.player_h, entity_local_point_world(r.veh(), r.veh().seats[1].seat_local));
	CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
	CHECK(!r.player().mounted);
}

void test_scan_uses_live_eye_offset() {
	Rig r;
	r.player().position = { 0, 0, 0 };
	r.player().yaw = 90; // facing +x, toward the seat (the aim cone)
	r.veh().position = { 5, 1, 0 };
	r.veh().seats.resize(1);
	r.veh().seats[0].seat_local = {};
	VehicleSeatSelection hit;
	CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
	r.player().eye_offset_x = 2 * 65536;
	r.player().eye_offset_y = 65536;
	CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
	CHECK(hit.vehicle == r.veh_h);
}

void test_enemy_on_carried_gun_blocks_root_and_sibling() {
	Rig r;
	r.player().team = 1;
	r.veh().item_type = 1;
	Entity gun;
	gun.kind = EntityKind::Item;
	gun.has_item_def = true;
	gun.item_type = 6;
	gun.item_attrib = kItemAttribEweap;
	gun.ground_target = r.veh_h;
	gun.health = 100;
	gun.alive = true;
	gun.seats.push_back(r.veh().seats[1]);
	const EntityHandle gh = r.w.registry.spawn(1, gun);
	Entity enemy;
	enemy.kind = EntityKind::Organic;
	enemy.health = 100;
	enemy.alive = true;
	enemy.team = 2;
	enemy.mounted = true;
	enemy.mount_target = gh;
	const EntityHandle eh = r.w.registry.spawn(0, enemy);
	VehicleSeatSelection hit;
	CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
	CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
	CHECK(!r.w.vehicles.process_attach(r.player_h, gh, 2));
	r.w.registry.get(eh)->team = 1;
	CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
}

void test_vehicle_admission_stationary_threshold_and_deck() {
	Rig r;
	r.veh().spawn_position = {};
	r.veh().saved_live_pos[0] = 100 * 65536;
	r.veh().saved_live_pos[1] = 200 * 65536;
	CHECK(vehicle_can_enter(r.w, nullptr, r.veh()));
	r.veh().saved_live_pos[0] -= 16;
	CHECK(vehicle_can_enter(r.w, nullptr, r.veh()));
	--r.veh().saved_live_pos[0];
	CHECK(!vehicle_can_enter(r.w, nullptr, r.veh()));
	r.player().ground_target = r.veh_h;
	CHECK(vehicle_can_enter(r.w, &r.player(), r.veh()));
	r.player().ground_target = {};
	r.veh().spawn_position = r.veh().position;
	r.veh().flags |= kEntityFlagInAir;
	CHECK(vehicle_can_enter(r.w, nullptr, r.veh())); // spawn arm precedes airborne
	r.veh().spawn_position = {};
	CHECK(!vehicle_can_enter(r.w, nullptr, r.veh()));
}

struct EntryPointProvider final : IPoseProvider {
	struct Point {
		const char *name;
		Vec3 position;
	};
	std::vector<Point> points;
	bool resolve_named_transform(World &, EntityHandle, const char *name, int32_t out[6]) override {
		for (const auto &point : points) {
			if (std::string(name) != point.name)
				continue;
			out[0] = static_cast<int32_t>(point.position.x * 65536);
			out[1] = static_cast<int32_t>(point.position.y * 65536);
			out[2] = static_cast<int32_t>(point.position.z * 65536);
			out[3] = out[4] = out[5] = 0;
			return true;
		}
		return false;
	}
};

void test_ai_authored_entry_claim_and_stages() {
	Rig r;
	r.w.cached.local_player = {};
	r.veh().item_type = 6;
	r.veh().item_attrib = 0; // entry-walk object; no attach-capable attrib
	EntryPointProvider pose;
	pose.points = { { "E1", { 110, 200, 10 } }, { "E2", { 110, 210, 10 } },
		{ "G2", { 105, 210, 10 } }, { "H2", { 101, 210, 10 } } };
	r.w.pose_provider = &pose;
	Entity other = r.player();
	other.net_id = 89;
	const auto oh = r.w.registry.spawn(0, other);
	auto &ob = *r.sys.at(r.sys.attach(oh));
	ob.slot.f[37] = 125;
	ob.slot.f[38] = 11;
	ob.inf.board_entry_slot = 1;
	r.player().net_id = 88;
	auto &brain = *r.sys.at(r.sys.attach(r.player_h));
	brain.slot.f[37] = 125;
	brain.slot.f[38] = 11;
	const auto move = [&](int x, int y) {
		brain.pos[0] = x * 65536;
		brain.pos[1] = y * 65536;
		brain.pos[2] = 10 * 65536;
		r.player().position = { float(x), float(y), 10 };
	};
	move(100, 210);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_slot == 2);
	CHECK(brain.inf.board_entry_stage == 1);
	CHECK(brain.inf.move_target[0] == 110 * 65536);
	CHECK(brain.inf.arrival_radius == 57344);
	move(110, 210);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 2);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 4);
	CHECK(brain.inf.move_target[0] == 105 * 65536);
	move(105, 210);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 5);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 6);
	CHECK(brain.inf.move_target[0] == 101 * 65536);
	move(101, 210);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 7);
	CHECK(!r.player().mounted);
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.board_entry_stage == 7);
	// UseGun bypasses the staging sequence and admits an EWeap on arrival.
	r.veh().item_attrib = kItemAttribEweap;
	pose.points.push_back({ "UseGun", { 101, 210, 10 } });
	r.sys.infantry_think(brain, r.w);
	CHECK(r.player().mounted);
}

void test_ai_boarding_moving_and_destroyed_carrier_admission() {
	Rig r;
	r.w.cached.local_player = {};
	r.player().net_id = 88;
	r.veh().item_type = 1;
	r.veh().item_attrib = kItemAttribPlayerControl;
	r.veh().flags |= kEntityFlagInAir;
	r.veh().spawn_position = { 0, 0, 0 };
	auto &brain = *r.sys.at(r.sys.attach(r.player_h));
	brain.slot.f[37] = 125;
	brain.slot.f[38] = 11;
	brain.pos[0] = 100 * 65536;
	brain.pos[1] = 200 * 65536;
	brain.pos[2] = 10 * 65536;
	r.player().spawn_position = { 100, 200, 10 };
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.move_mode == 0);
	CHECK(!r.player().mounted);
	CHECK(r.player().health > 0);
	r.veh().health = 0;
	r.veh().last_attacker = r.veh_h;
	r.sys.infantry_think(brain, r.w);
	CHECK(r.player().health > 0); // dead carrier, walker still at spawn
	brain.pos[0] += 9 * 65536;
	r.sys.infantry_think(brain, r.w);
	CHECK(r.player().health == 0);
	CHECK(r.player().last_attacker == r.veh_h);
}

void test_seated_board_any_upgrades_on_64_tick_phase() {
	Rig r;
	r.w.cached.local_player = {};
	r.player().player_class = 0;
	r.player().net_id = 0;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
	const int index = r.sys.attach(r.player_h);
	AiEntity &body = *r.sys.at(index);
	body.inf.active = true;
	body.inf.is_local_player = false;
	body.slot.f[37] = 125;
	body.slot.f[38] = r.veh().net_id;
	body.slot.f[36] = int32_t(r.veh_h.packed) + 1;
	r.sys.is_authority = true;
	r.sys.tick_infantry(body, r.w, 16);
	CHECK(r.player().mount_type == SeatType::Passenger);
	r.sys.tick_infantry(body, r.w, 64);
	CHECK(r.player().mount_type == SeatType::Controller);
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
    // The free sitex sits over 100 deg off the rig's +y view: look at it.
    look_at(r.w, r.player_h, entity_local_point_world(r.veh(), r.veh().seats[1].seat_local));

    auto label_count = [&]() {
        std::vector<AttachLabel> labels;
        r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
        return labels.size();
    };
    VehicleSeatSelection hit;
    CHECK(!r.w.vehicles.player_toggle_mount(r.player_h));
    CHECK(!r.player().mounted);
    CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 0);
    CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));

    // Same-team, dead, or detached riders do not block the vehicle. The occupied
    // seat itself remains hidden independently, leaving the passenger seat.
    ee->team = 1;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->team = 2;
    ee->alive = false;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->alive = true;
    ee->health = 0;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->health = 150;
    CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 0);
    ee->mounted = false;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);

    // Retail's hostile-parent scan is pool-0-only. A mounted lookalike in pool 1
    // must not block, even though it carries the same direct mount target.
    Entity pool1_enemy = *ee;
    pool1_enemy.mounted = true;
    pool1_enemy.mount_target = r.veh_h;
    CHECK(r.w.registry.spawn(1, pool1_enemy).valid());
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
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

    // Mounted into the ctrl seat: 38 + 40 true, 41 false. The queued-mount
    // latch takes the FindBestSeatSlot arm (a bare deck stander would scan).
    r.player().flags |= kEntityFlagQueuedMount;
    CHECK(r.w.vehicles.player_toggle_mount(r.player_h)); // queued arm -> ctrl seat
    r.player().flags &= ~kEntityFlagQueuedMount;
    CHECK(cmds.local_player_attached_to_ssn(11));
    CHECK(cmds.local_player_driving_ssn(11));
    CHECK(!cmds.local_player_on_gun_of_ssn(11));

    // A dead local player reads false on every sub [orig: the Flags & 2 gate].
    r.player().health = 0;
    CHECK(cmds.local_player_attached_to_ssn(11)); // health alone is not the flag
    r.player().flags |= kEntityFlagDead;
    CHECK(!cmds.local_player_attached_to_ssn(11));
    r.player().flags &= ~kEntityFlagDead;
    r.player().health = 150;

    // The carrier chain: a gun CARRIED by SSN 11 counts for sub 38 against 11.
    Entity gun;
    gun.net_id = 500;
    gun.bms_id = 500;
    gun.kind = EntityKind::Item;
    gun.item_id = 1800;
    gun.item_type_index = 7;
    gun.position = r.veh().position;
    gun.health = 500;
    gun.alive = true;
    gun.primary_weapon.assign(1, 'x');
    r.w.tables.weapons.entries.resize(2);
    r.w.tables.weapons.entries[1].name.assign(1, 'x');
    r.w.tables.weapons.entries[1].clipsize = -1;
    r.w.tables.weapons.entries[1].valid = true;
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
    r.w.vehicles.detach(r.player_h);
    CHECK(r.w.vehicles.process_attach(r.player_h, gh, 1));
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
    // PLYRONSSN reads groundEntity, not parentEntity. Both org1 and org2
    // refresh that relationship on a mounted update, even when no floor ray
    // can reach the carrier (07TR's cannon and 03TR's airborne minigun).
    auto &body = *r.sys.at(r.sys.attach(r.player_h));
    body.inf.active = true;
    body.health = 150;
    for (bool local : {false, true}) {
        body.inf.is_local_player = local;
        r.player().ground_target = {};
        CHECK(!cmds.local_player_standing_on_ssn(11));
        CHECK(r.sys.pose_if_mounted(body, r.w));
        CHECK(r.player().ground_target == gh);
        CHECK(cmds.local_player_standing_on_ssn(11));
    }

    CHECK(r.w.vehicles.detach(r.player_h));
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
    CHECK(w.vehicles.resolve_mounted_ammo_slot(ungated) == nullptr);

    // With the witnessed gate satisfied, the unredirected route is the
    // entity's own embedded MountSlot; the cross-entity route remains closed
    // until the strict EWeap relationship proof is present.
    Entity direct_only;
    direct_only.primary_weapon_slot_adm = 3;
    direct_only.has_item_def = true;
    direct_only.item_attrib = kItemAttribEweap;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(direct_only) ==
          &direct_only.primary_weapon_slot);
    direct_only.primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(direct_only) == nullptr);

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

    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) ==
          &live_child->primary_weapon_slot);
    live_child->primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);
    const World &const_world = w;
    const Entity &const_child = *w.registry.get(child_h);
    CHECK(const_world.vehicles.resolve_mounted_ammo_slot(const_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);

    // The similarly named attachment pointer is not a substitute for retail's
    // groundEntity relationship.
    live_child->ground_target = EntityHandle{};
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) == nullptr);
    live_child->ground_target = parent_h;

    // A packed handle may be reused. The child-side generation captured when
    // the relationship was authored must reject the replacement lifetime.
    w.registry.despawn(parent_h);
    Entity replacement = parent;
    CHECK(w.registry.spawn_at(parent_h, replacement) == parent_h);
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) == nullptr);
}

void test_prepare_vehicle_weapon_slot_after_armory_load() {
    World w;
    w.tables.weapons.entries.resize(4);
    WeaponTableEntry &weapon = w.tables.weapons.entries[3];
    weapon.name = "WPN_LATE_MOUNT";
    weapon.clipsize = 12;
    weapon.startrounds = 41;
    weapon.valid = true;

    Entity mount;
    mount.primary_weapon = weapon.name;
    CHECK(w.vehicles.prepare_weapon_slot(mount));
    CHECK(mount.primary_weapon_slot_adm == 3);
    CHECK(mount.primary_weapon_slot.clip == 12);
    CHECK(mount.primary_weapon_slot.reserve == 29);

    // Preparing an already-live slot is idempotent: a later route edge must
    // never refund cartridges spent since the first armory resolution.
    mount.primary_weapon_slot.clip = 7;
    mount.primary_weapon_slot.reserve = 23;
    mount.primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(w.vehicles.prepare_weapon_slot(mount));
    CHECK(mount.primary_weapon_slot.clip == 7);
    CHECK(mount.primary_weapon_slot.reserve == 23);
    CHECK(mount.primary_weapon_slot.redirect_to_parent_slot);
}

// An emplacement's slot starts its optic at the WeaponSlot_InitFromDef seed,
// never at the lazy max: a slot initialized without an owner entity skips the
// class-6 sniper lock, so even a sniper-class gunner's cannon starts at the
// floor. [orig: WeaponSlot_InitFromEntityDef entityPtr 0 @0x546706;
//  WeaponSlot_InitFromDef @0x53EEF7 / @0x53EF2D..0x53EF44]
void test_prepare_vehicle_weapon_slot_seeds_the_zoom() {
    World w;
    w.tables.weapons.entries.resize(3);
    WeaponTableEntry &cannon = w.tables.weapons.entries[1];
    cannon.name = "WPN_M1TURRET";
    cannon.category = 3; // a Primary category must still not lock an ownerless slot
    cannon.scope_max_mag = 10;
    cannon.scope_initial_mag = 2; // JOX 'scope_max_mag 10 2'
    cannon.valid = true;
    WeaponTableEntry &rcws = w.tables.weapons.entries[2];
    rcws.name = "WPN_STRYKERTURRET";
    rcws.category = 11;
    rcws.scope_max_mag = 12; // JOTAC 'SCOPE_MAX_MAG 12' / 'SCOPE_MIN_MAG 1'
    rcws.scope_min_mag = 1;
    rcws.valid = true;
    w.rules.allow_sniper_scope_zoom = false;

    Entity gun;
    gun.primary_weapon = cannon.name;
    CHECK(w.vehicles.prepare_weapon_slot(gun));
    CHECK(gun.primary_weapon_slot.scope_zoom == 2);
    // A later prepare of the same live slot keeps the stepped zoom.
    gun.primary_weapon_slot.scope_zoom = 6;
    CHECK(w.vehicles.prepare_weapon_slot(gun));
    CHECK(gun.primary_weapon_slot.scope_zoom == 6);
    Entity roof;
    roof.primary_weapon = rcws.name;
    CHECK(w.vehicles.prepare_weapon_slot(roof));
    CHECK(roof.primary_weapon_slot.scope_zoom == 1);
}

// A body claims the vehicle under it after its update: the first vehicle on
// its ground chain (a seated body re-reads its parent every tick) takes the
// body's team when it is player-controllable and the body is seated or
// nobody holds it; every fourth tick the vehicle's contact solve is woken. A
// busted or spawn-point vehicle keeps its team but is still woken.
// [orig: Entity_UpdateAllEntities -- Entity_FindChildByDefType @0x4C2484,
//  the attrib/Flags gates @0x4C2494..0x4C24C1, the seated test @0x4C24C7,
//  the team copy @0x4C259E..0x4C25A4, `test tick,3` @0x4C25CE -> Entity_WakeContactSolve
//  @0x459290]
void test_seated_body_claims_its_vehicle() {
    for (const bool spawn_point : {false, true}) {
        Rig r;
        r.veh().item_type = 1;
        r.veh().item_attrib |= kItemAttribPlayerControl;
        if (spawn_point) r.veh().item_attrib |= kItemAttribSpawnPoint;
        r.veh().team = 2;
        r.player().team = 1;
        AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
        body.inf.active = true;
        body.inf.is_local_player = true;
        body.health = r.player().health;
        body.team = 1;
        CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
        TickContext ctx{};
        ctx.world = &r.w;
        ctx.is_authority = true;
        for (uint32_t t = 1; t <= 4; ++t) {
            ctx.logic_tick = t;
            r.w.logic_tick = t;
            r.w.update_all_entities(ctx);
            CHECK(r.player().ground_target == r.veh_h);
            CHECK(r.veh().team == (spawn_point ? 2 : 1));
            // Only tick 4 of these wakes the vehicle.
            CHECK(((r.veh().flags & 0x40u) != 0) == (t == 4));
        }
        CHECK(r.veh().veh.contact_wake_tick == 4);
    }
}

// A same-team body standing on a vehicle skips the hold scan outside a
// session. Single player (the in-process listen server) is outside it, so
// the body's berserk bit reaches the vehicle although a rider
// holds it; in a non-co-op session the rider's hold blocks the copy.
// [orig: Entity_UpdateAllEntities -- `cmp g_napi_np_ctx.is_in_session,0`
//  @0x4C24EA, `test g_GameType,10000h` @0x4C24F7, the seat words
//  @0x4C2507..0x4C251F, the berserk sync @0x4C25B8..0x4C25C7;
//  SinglePlayer_StartMission @0x561AF0 leaves is_in_session clear]
void test_same_team_hold_scan_follows_the_session() {
    for (const bool mp : {false, true}) {
        Rig r;
        r.w.rules.mp_session = mp;
        r.veh().item_type = 1;
        r.veh().item_attrib |= kItemAttribPlayerControl;
        r.veh().team = 1;
        r.player().team = 1;
        Entity rider;
        rider.kind = EntityKind::Organic;
        rider.item_id = 2072;
        rider.health = 150;
        rider.alive = true;
        rider.team = 1;
        const EntityHandle rider_h = r.w.registry.spawn(0, rider);
        CHECK(r.w.vehicles.process_attach(rider_h, r.veh_h, 2)); // the sitex seat
        r.sys.attach(r.veh_h);
        r.sys.attach(r.player_h);
        AiEntity &body = *r.sys.for_handle(r.player_h);
        body.inf.active = true;
        body.inf.is_local_player = true;
        body.health = r.player().health;
        body.team = 1;
        body.slot.f[AiSlot::kBehaviorFlags] |= 0x200;
        r.player().ground_target = r.veh_h; // standing on it, not seated
        TickContext ctx{};
        ctx.world = &r.w;
        ctx.is_authority = true;
        ctx.logic_tick = 1;
        r.w.logic_tick = 1;
        r.w.update_all_entities(ctx);
        const AiEntity *vehicle_brain = r.sys.for_handle(r.veh_h);
        CHECK(vehicle_brain != nullptr);
        if (vehicle_brain == nullptr) continue;
        const bool copied = (vehicle_brain->slot.f[AiSlot::kBehaviorFlags] & 0x200) != 0;
        CHECK(copied == !mp);
    }
}

// The SP lose epilog runs a reduced entity update: every pool-1 row's pose is
// copied into its saved pose, only the row a player drives is visited, and the
// update is not counted; the pool-0 walk still runs (it wakes the driven
// vehicle on tick 8). Once the epilog lifts, the full update visits and counts.
// [orig: Entity_UpdateAllEntities -- `cmp g_epilog_screen_active,0`
//  @0x4C211D, the walk @0x4C239A..0x4C2408, the tail @0x4C2624..0x4C2639]
void test_epilog_entity_update() {
    Rig r;
    r.veh().item_type = 1;
    r.veh().item_attrib |= kItemAttribPlayerControl;
    Entity prop;
    prop.kind = EntityKind::Item;
    prop.has_item_def = true;
    prop.item_id = 999;
    prop.position = {50.0f, 60.0f, 7.0f};
    prop.health = 100;
    prop.alive = true;
    const EntityHandle prop_h = r.w.registry.spawn(1, prop);
    CHECK(prop_h.valid());
    r.player().flags |= kEntityFlagPlayer;
    r.player().engine_flags |= kEntityFlagPlayer;
    AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
    body.inf.active = true;
    body.inf.is_local_player = true;
    body.health = r.player().health;
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1)); // the ctrlx seat
    CHECK(r.veh().primary_occupant == r.player_h);

    r.w.rules.mp_session = false;
    CHECK(r.w.match.finish(2, r.w));
    r.w.round_end_tick = 0;
    r.w.logic_tick = 8;
    CHECK(r.w.epilog_screen_active());
    TickContext ctx{};
    ctx.world = &r.w;
    ctx.is_authority = true;
    ctx.logic_tick = 8;
    r.w.update_all_entities(ctx);
    const Entity &kept = *r.w.registry.get(prop_h);
    CHECK(r.veh().pool1_visited);
    CHECK(!kept.pool1_visited);
    CHECK(kept.saved_live_valid);
    CHECK(kept.saved_live_pos[0] == to_fixed(50.0f));
    CHECK(kept.saved_live_pos[1] == to_fixed(60.0f));
    CHECK(kept.saved_live_pos[2] == to_fixed(7.0f));
    CHECK(((r.veh().flags & 0x40u) != 0) && r.veh().veh.contact_wake_tick == 8);
    CHECK(r.w.entity_update_counter == 0);

    r.w.rules.mp_session = true;
    CHECK(!r.w.epilog_screen_active());
    r.w.update_all_entities(ctx);
    CHECK(r.w.registry.get(prop_h)->pool1_visited);
    CHECK(r.w.entity_update_counter == 1);
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
    t.brain_class = VehicleBrainClass::Air; // ai_function CHel: the air machine
    t.physics = 0; // a CHel def: no ground selector, no ground mover
    r.w.vehicles.traits.set(r.veh().item_id, t);
    const int ai_idx = r.sys.attach(r.veh_h);
    r.sys.at(ai_idx)->profile.type = 1; // the helo profile class
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    CHECK(r.veh().primary_occupant == r.player_h);
    TickContext ctx{};
    ctx.is_authority = true;
    for (int i = 0; i < 3; ++i) r.w.update_all_entities(ctx);
    CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull);
    CHECK(part_register(r.veh().veh.part_spin.angle) > 0);
    CHECK(r.w.vehicles.detach(r.player_h));
    r.w.update_all_entities(ctx);
    CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo);
    // A joiner never runs the authority pass for it.
    ctx.is_authority = false;
    const int32_t before = r.veh().veh.part_spin.speed;
    r.w.update_all_entities(ctx);
    CHECK(r.veh().veh.part_spin.speed == before);
}

// The current-state drive stamp must not consume the pending death callback.
// [orig: Entity_UpdateVehiclePhysics @0x48AF00; EntityAI_ProcessAirStateMachine @0x4581B0]
void test_vehicle_pending_death_survives_drive_tick() {
	for (const auto family : { VehicleFamily::Ground, VehicleFamily::Watercraft }) {
		for (bool occupied : { false, true }) {
			Rig r;
			auto traits = truck_traits();
			traits.family = family;
			r.w.vehicles.traits.set(r.veh().item_id, traits);
			ItemDeathTraits death;
			death.unit_type = 1;
			death.has_husk = true;
			death.husk_model_loaded = true;
			r.w.tables.item_death_traits.set(r.veh().item_id, death);
			auto &ai = *r.sys.at(r.sys.attach(r.veh_h));
			ai.brain.f[AiBrain::kCurState] = 22;
			ai.brain.f[AiBrain::kPendState] = 21;
			ai.brain.f[AiBrain::kStep] = 1;
			if (occupied)
				CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
			r.veh().health = 0;
			TickContext ctx{};
			ctx.is_authority = true;
			for (int i = 0; i < 4; ++i) {
				r.w.update_all_entities(ctx);
				++r.w.logic_tick;
			}
			CHECK((r.veh().engine_flags & kEntityFlagHusk) != 0);
			CHECK(ai.brain.f[AiBrain::kCurState] == 23);
			CHECK(!r.veh().alive);
			CHECK(r.veh().team == 0);
		}
	}
}

// The AI-driver leg: an NPC in the ctrl seat + a staged brain waypoint drives the truck
// toward the node; no controller parks it (SM state 22, no movement).
// [orig: @0x48bc12-0x48c034 / the parked stamp @0x48c002-0x48c02d]
void test_ai_drive_leg() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);

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
	b.f[AiBrain::kPendState] = 16;
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
		CHECK(b.f[AiBrain::kPendState] == 16); // pending callback state is untouched
		const float x0 = r.veh().position.x;
		r.w.vehicles.tick_motor(r.veh(), t, &cmd);
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
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    CHECK(r.w.registry.get(nh)->mount_type == SeatType::Controller);
    Entity *ctrl = r.w.vehicles.resolve_controller(r.veh());
    CHECK(ctrl != nullptr);

    const float x0 = r.veh().position.x;
    bool drove = false;
    for (int i = 0; i < 300; ++i) {
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
        drove = drove || cmd.ai_drive;
        r.w.vehicles.tick_motor(r.veh(), t, &cmd);
        AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->pos[0] = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->pos[1] = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->pos[2] = static_cast<int32_t>(r.veh().position.z * 65536.0f);
        ve2->heading = r.veh().veh.yaw_bam;
    }
    CHECK(drove);
    CHECK(b.f[AiBrain::kCurState] == 16); // the 22 -> 16 hand-back held
	CHECK(b.f[AiBrain::kPendState] == 16); // the pending state was never overwritten
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
        r.w.vehicles.tick_motor(r.veh(), t, &cmd);
        CHECK(r.veh().veh.cmd_speed == 0); // the no-controller hold, not the stale drive
    }
}

// The pool-1 avoid BRAKE: a neighbor whose footprint ellipse overlaps ours and
// sits within ~30 deg of dead ahead damps the command speed by the id/counter
// factor ((id + (counter<<8)) & 0x7FFF) + 0x4000 per tick, the counter being
// the entity-update counter; a neighbor BEHIND does not
// [orig: @0x48bd8f-0x48bf26; Entity_UpdateVehiclePhysics @0x48BF26].
void test_ai_drive_avoid_brake() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);
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
	b.f[AiBrain::kPendState] = 16;
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
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    Entity *ctrl = r.w.vehicles.resolve_controller(r.veh());
    CHECK(ctrl != nullptr);

    // Baseline: open road, undamped.
    VehicleDriveCmd cmd0;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd0);
    CHECK(cmd0.ai_drive);
    CHECK(cmd0.cmd_speed > 0);

    // A parked prop dead AHEAD (+x), footprints overlapping. The ground walk
    // admits any nonzero ItemTypeIndex [orig: Entity_UpdateVehiclePhysics
    // `cmp dword ptr [edi+1Ch],0` @0x48BDD9].
    Entity prop;
    prop.kind = EntityKind::Item;
    prop.item_id = 999;
    prop.item_type_index = 7;
    prop.position = Vec3{104.0f, 200.0f, 10.0f};
    prop.bound_radius = 3.0f;
    const EntityHandle ph = r.w.registry.spawn(1, prop);

    r.w.entity_update_counter = 5; // differs from the tick (0)
    VehicleDriveCmd cmd1;
    r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd1);
    CHECK(cmd1.ai_drive);
    const int32_t f = static_cast<int32_t>(
            ((static_cast<uint32_t>(r.veh().net_id) +
              (r.w.entity_update_counter << 8)) &
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

// Quantized footprint cosine and truncating FPATAN through the complete AI
// command staging, shared by ground/boat. Bounds and positions are exact Q16.
// [orig: ground @0x48BE86..0x48BF06; boat @0x48E577..0x48E756]
void test_ai_drive_avoid_quantized_footprints() {
    struct Case { int32_t x, y, other_yaw; bool brakes; };
    const Case cases[] = {
        // distance 554569; retail threshold 554643, full-angle cosine 554566.
        {546144, 96300, 1193046471, true},
        // Half-bin at 90 degrees: thresholds 557056 -> 558061.
        {557500, 0, -1071644673, false},
        {557500, 0, -1071644672, true},
        {557500, 0, -1071644671, true},
        // Half-bin at zero: thresholds 720896 -> 720892.
        {720894, 0, -2145386497, true},
        {720894, 0, -2145386496, false},
        {720894, 0, -2145386495, false},
        // Bearing -2028195171.7175 truncates to -2028195171. Rounding to
        // -2028195172 selects the next bin and falsely brakes: distance
        // 554644 exceeds the true threshold 554643 (rounded threshold 555648).
        {546220, 96300, -952356196, false},
    };
    for (bool boat : {false, true}) {
        Rig r(30.0f);
        VehicleTraits traits = truck_traits();
        traits.player_speed = traits.water_speed = 65536;
        r.w.vehicles.traits.set(r.veh().item_id, traits);
        r.veh().position = {0.0f, 0.0f, 0.0f};
        r.veh().bound_radius = 5.0f; // 327680 Q16
        r.veh().veh.yaw_bam = 0;
        r.veh().veh.yaw_seeded = true;
        const int ai_index = r.sys.attach(r.veh_h);
        AiBrain &brain = r.sys.at(ai_index)->brain;
        brain.f[AiBrain::kCurState] = 16;
        brain.f[AiBrain::kPendState] = 16;
        brain.f[AiBrain::kOutSpeed] = 65536;
        brain.f[AiBrain::kWpType] = 0;
        brain.f[AiBrain::kWpBearing] = 0;
        brain.f[AiBrain::kAnimFlag] = 0;
        Entity driver;
        driver.kind = EntityKind::Organic;
        driver.item_id = 2072;
        driver.health = 150;
        driver.alive = true;
        const EntityHandle driver_handle = r.w.registry.spawn(0, driver);
        CHECK(r.w.vehicles.process_attach(driver_handle, r.veh_h, 1));
        const Entity *controller = r.w.vehicles.resolve_controller(r.veh());
        CHECK(controller != nullptr);
        const auto speed = [&]() {
            VehicleDriveCmd command;
            if (boat) r.sys.watercraft_ai_drive(r.w, r.veh(), controller, traits, command);
            else r.sys.vehicle_ai_drive(r.w, r.veh(), controller, traits, command);
            CHECK(command.ai_drive);
            return command.cmd_speed;
        };
        CHECK(speed() == 65536);
        // ItemTypeIndex 1 passes both walks: the ground walk skips index 0, the
        // boat walk admits index 1 only [orig: Entity_UpdateVehiclePhysics
        // @0x48BDD9; Entity_UpdateWatercraftPhysics @0x48E5C5].
        Entity obstacle;
        obstacle.kind = EntityKind::Item;
        obstacle.item_id = 999;
        obstacle.item_type_index = 1;
        obstacle.bound_radius = 5.0f;
        obstacle.veh.yaw_seeded = true;
        const EntityHandle other = r.w.registry.spawn(1, obstacle);
        for (const Case &c : cases) {
            Entity &neighbor = *r.w.registry.get(other);
            neighbor.position = {static_cast<float>(c.x) / 65536.0f,
                                 static_cast<float>(c.y) / 65536.0f, 0.0f};
            neighbor.veh.yaw_bam = c.other_yaw;
            // net id 11, frame 0: one brake maps 65536 to 0x4000+11.
            CHECK(speed() == (c.brakes ? 16395 : 65536));
        }
        Entity &neighbor = *r.w.registry.get(other);
        neighbor.position = {546144.0f / 65536.0f, 96300.0f / 65536.0f, 0.0f};
        neighbor.veh.yaw_bam = 1193046471;
        const EntityHandle second = r.w.registry.spawn(1, neighbor);
        CHECK(speed() == 4102); // both overlapping neighbors compound the brake
        neighbor.ground_target = r.veh_h;
        CHECK(speed() == 16395); // neighbor carried by self is excluded
        neighbor.ground_target = {};
        r.veh().ground_target = other;
        CHECK(speed() == 16395); // self carried by neighbor is excluded too
        r.w.registry.get(second)->ground_target = r.veh_h;
        CHECK(speed() == 65536); // both carrier links excluded, no false brake
    }
}

// A driverless hull's stuck escalation [orig: AI_CheckVehicleStuckState
// @0x465290]: the count climbs once per parked tick; on the authority every
// 16th count past 32 a live pool-0 body within (radii + 12 u) resets it; past
// 3410 a hull more than 12 u from its spawn anchor is nudged up (slideDecay
// += 1024 per check) until 3720, then killed.
void test_stuck_check() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);
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
    r2.w.vehicles.traits.set(r2.veh().item_id, t);
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
    CHECK(r2.w.vehicles.process_attach(nh, r2.veh_h, 1));
    VehicleDriveCmd cmd;
    r2.w.vehicles.tick_motor(r2.veh(), t, &cmd);
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
    r.w.vehicles.traits.set(r.veh().item_id, t);
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
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    Entity *ctrl = r.w.vehicles.resolve_controller(r.veh());
    CHECK(ctrl != nullptr);
    CHECK(count_mounted_entities(r.w, r.veh()) == 1);

    // At the anchor: no clamp.
	CHECK(vehicle_at_spawn_anchor(r.w, r.veh()));
	VehicleDriveCmd cmd;
	r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
    CHECK(r.veh().health == 2000);
    // 8 u planar is still the anchor (<= 0x80000); 9 u is not.
    r.veh().position = Vec3{108.0f, 200.0f, 10.0f};
	CHECK(vehicle_at_spawn_anchor(r.w, r.veh()));
	r.veh().position = Vec3{ 109.0f, 200.0f, 10.0f };
	CHECK(!vehicle_at_spawn_anchor(r.w, r.veh()));
	// The Z delta is HALVED: 15 u straight up still counts as the anchor.
	r.veh().position = Vec3{100.0f, 200.0f, 25.0f};
	CHECK(vehicle_at_spawn_anchor(r.w, r.veh()));
	r.veh().position = Vec3{ 100.0f, 200.0f, 27.0f };
	CHECK(!vehicle_at_spawn_anchor(r.w, r.veh()));
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
    r.w.vehicles.traits.set(r.veh().item_id, t);
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    Entity &drv = r.player();
    drv.net_move_input = 0x08; // moving forward
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    CHECK(r.veh().veh.handbrake_latched == 0);
    drv.net_move_input = 0x08 | 0x80; // + lean right = handbrake
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK((r.veh().flags & 0x8u) != 0);
    CHECK(r.veh().veh.handbrake_latched == 1);
    CHECK(r.veh().veh.cmd_speed == 0);
    drv.net_move_input = 0x08;
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK(r.veh().veh.handbrake_latched == 0);
    // The old latch still selects analog input on the release tick; digital
    // forward resumes on the following visit. [orig: @0x48B9D2..0x48B9E5]
    CHECK(r.veh().veh.cmd_speed == 0);
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // A def without handBrake ignores the key.
    t.hand_brake = 0;
    drv.net_move_input = 0x08 | 0x80;
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK(r.veh().veh.handbrake_latched == 0);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // The crashed byte stops the command regardless.
    r.veh().veh.crashed = 1;
    drv.net_move_input = 0x08;
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK(r.veh().veh.cmd_speed == 0);
    r.veh().veh.crashed = 0;
    // The MoveOrder merge: a nonzero direction with the move bit latches the
    // free-look bit on the occupant's own word [orig: @0x48b847..0x48b897].
    drv.net_move_input = 0x08 | 0x01;
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK((drv.net_move_input & 0x10u) != 0);
    drv.net_move_input = 0x08; // straight: no latch
    r.w.vehicles.tick_motor(r.veh(), t, nullptr);
    CHECK((drv.net_move_input & 0x10u) == 0);
}

// A truck/tank brain with a live AI driver in the control seat and a route
// node straight ahead, for the drive-leg ownership tests below.
struct DriveRig {
    Rig r{30.0f};
    VehicleTraits t = truck_traits();
    EntityHandle driver;
    explicit DriveRig(VehicleFamily family) {
        t.family = family;
        r.w.vehicles.traits.set(r.veh().item_id, t);
        AiEntity &ve = *r.sys.at(r.sys.attach(r.veh_h));
        ve.pos[0] = 100 << 16;
        ve.pos[1] = 200 << 16;
        ve.pos[2] = 10 << 16;
        r.sys.nav.channels.resize(3);
        r.sys.nav.channels[2].count = 1;
        r.sys.nav.channels[2].entries[0] = 0;
        r.sys.nav.nodes.resize(1);
        r.sys.nav.nodes[0] = NavEntry{{2 << 16, 300 << 16, 200 << 16, 10 << 16, 0}};
        AiBrain &b = ve.brain;
        b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 16;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 2;
        b.f[AiBrain::kOutSpeed] = 40 * 293;
        r.veh().veh.yaw_seeded = true;
        r.veh().veh.yaw_bam = 0; // facing the node
        Entity npc;
        npc.kind = EntityKind::Organic;
        npc.item_id = 2072;
        npc.health = 150;
        npc.alive = true;
        driver = r.w.registry.spawn(0, npc);
        CHECK(r.w.vehicles.process_attach(driver, r.veh_h, 1));
    }
    AiEntity &brain() { return *r.sys.for_handle(r.veh_h); }
    void motor_pass() { r.w.vehicles.update_motor(r.veh(), true); }
};

// Zero health alone does not park: until the dying state's death transforms
// raise the dead bit, the hull keeps its AI driver's leg (route command, no 22
// stamp); the dead bit then parks it.
// [orig: ctan @0x48954B..0x489560; cveh @0x48B972..0x48B987]
void test_zero_health_hull_keeps_its_driver_leg() {
    for (VehicleFamily family : {VehicleFamily::Ground, VehicleFamily::Tank}) {
        DriveRig d(family);
        d.r.veh().health = 0;
        d.r.veh().alive = false;
        Entity *ctrl = d.r.w.vehicles.resolve_controller(d.r.veh());
        CHECK(ctrl != nullptr);
        VehicleDriveCmd cmd;
        d.r.sys.vehicle_ai_drive(d.r.w, d.r.veh(), ctrl, d.t, cmd);
        CHECK(cmd.ai_drive && cmd.cmd_speed == 40 * 293);
        CHECK(d.brain().brain.f[AiBrain::kCurState] == 16);
        d.r.w.vehicles.tick_motor(d.r.veh(), d.t, &cmd);
        CHECK(d.r.veh().veh.cmd_speed == 40 * 293);
        d.r.veh().flags |= kEntityFlagDead;
        VehicleDriveCmd parked;
        d.r.sys.vehicle_ai_drive(d.r.w, d.r.veh(), ctrl, d.t, parked);
        CHECK(!parked.ai_drive);
        CHECK(d.brain().brain.f[AiBrain::kCurState] == 22);
        d.r.w.vehicles.tick_motor(d.r.veh(), d.t, &parked);
        CHECK(d.r.veh().veh.cmd_speed == 0);
    }
}

// A PLAYER driver whose eye sits under the water plane hands a ground or tank
// hull to the AI leg, as the boat does: the brain's 22 becomes 16 and the command
// is the brain's out-speed, not the player's last input.
// [orig: cveh @0x48B9A0..0x48B9AC -> @0x48BC12; ctan @0x489579..0x489585
//  -> @0x4897DB]
void test_submerged_player_driver_takes_the_ai_leg() {
    for (VehicleFamily family : {VehicleFamily::Ground, VehicleFamily::Tank}) {
        Rig r(2.0f);
        VehicleTraits t = truck_traits();
        t.family = family;
        r.w.vehicles.traits.set(r.veh().item_id, t);
        r.sys.attach(r.veh_h);
        CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
        AiEntity &ve = *r.sys.for_handle(r.veh_h);
        ve.brain.f[AiBrain::kCurState] = ve.brain.f[AiBrain::kPendState] = 22;
        ve.brain.f[AiBrain::kOutSpeed] = 0; // the brain holds its hull still
        r.veh().veh.cmd_speed = 12345;      // the player's last command
        Entity &pl = r.player();
        pl.eye_offset_z = 65536;
        r.w.env.water_z = to_fixed(pl.position.z) + (2 << 16);
        r.w.vehicles.update_motor(r.veh(), true);
        CHECK(r.sys.for_handle(r.veh_h)->brain.f[AiBrain::kCurState] == 16);
        CHECK(r.veh().veh.cmd_speed == 0);
    }
}

// The mover's command registers are the brain's own words 132/136/137 in retail
// (one dword each): after the motor pass the brain reads the mover's steer target
// and command back, over any state-machine write. The dying tick's zeroing of
// the commanded speed reaches the mover register the same way.
// [orig: ctan [ebx+210h] @0x489952 / [ebx+220h] @0x489811 with ebx = entity+0x64
//  @0x488ACA; AI_UpdatePatrolBehavior reads [esi+210h] @0x457DD4;
//  AI_TickState_VehicleDying `mov [esi+220h],ebx` @0x467D83]
void test_mover_command_registers_are_the_brain_words() {
    for (VehicleFamily family : {VehicleFamily::Ground, VehicleFamily::Tank}) {
        DriveRig d(family);
        d.brain().brain.f[AiBrain::kWorkHeading] = 777; // a state-machine write
        d.motor_pass();
        const AiBrain &b = d.brain().brain;
        CHECK(b.f[AiBrain::kWorkHeading] == d.r.veh().veh.steer_target_bam);
        CHECK(b.f[AiBrain::kWorkHeading] != 777);
        CHECK(b.f[136] == d.r.veh().veh.cmd_speed && b.f[136] != 0);
        CHECK(b.f[137] == d.r.veh().veh.steer_ramp_bam);

        // The dying tick, still moving: its [136] zero is the mover's command.
        AiEntity &ve = d.brain();
        ve.brain.f[AiBrain::kCurState] = 21;
        d.r.veh().veh.vel_x = 5000;           // >= 1057 and > 1024 per tick: moving
        d.r.veh().veh.cmd_speed = 5000;
        AiThinkCtx ctx{&d.r.sys, &ve, &d.r.w, nullptr};
        d.r.sys.row(21).tick(ctx);
        CHECK(ve.brain.f[136] == 0);
        CHECK(d.r.veh().veh.cmd_speed == 0);
    }
}

// The ground twin of the boarders hold [orig: @0x48bf6f-0x48bff9]: an AI
// driver holds at cmd 0 while a body walks over to a free seat.
void test_ground_waits_for_boarders() {
    Rig r(30.0f);
	r.veh().spawn_position = r.veh().position; // stationary at its authored anchor
	const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);
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
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    Entity *ctrl = r.w.vehicles.resolve_controller(r.veh());
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
    walker.item_type_index = 7;
    const EntityHandle wh = r.w.registry.spawn(0, walker);
    r.sys.attach(wh);
    // The real board order (SSNtoSSN) stores the command in the walker's SLOT,
    // where the hold reads it [orig: WacCmd_SsnToSsn @0x4F73E4..0x4F73F7; the
    // reader cveh @0x48BFC1..0x48BFDA].
    CHECK(r.w.commands.order_boarding(wh, r.veh_h));
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
        r.w.tables.terrain = &field;
        t = truck_traits();
        t.family = VehicleFamily::Helicopter;
        t.brain_class = VehicleBrainClass::Air; // ai_function CHel: the air machine
        t.physics = 0;
        t.acceleration = 512;
        t.turn_rate = 0x600000;
        t.climb_speed = 50 * 293;
        t.critical_hp = 300;
        t.critical_drain = 40;
        t.non_critical_regen = 10;
        r.w.vehicles.traits.set(r.veh().item_id, t);
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
        CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
        ctrl = r.w.vehicles.resolve_controller(r.veh());
        CHECK(ctrl != nullptr);
    }
    void route_to(int32_t x, int32_t y, int32_t z) {
        r.sys.nav.channels.resize(3);
        r.sys.nav.channels[2].count = 1;
        r.sys.nav.channels[2].entries[0] = 0;
        r.sys.nav.nodes.resize(1);
        r.sys.nav.nodes[0] = NavEntry{{2 << 16, x << 16, y << 16, z << 16, 0}};
        AiBrain &b = ve->brain;
		b.f[AiBrain::kCurState] = 7;
		b.f[AiBrain::kPendState] = 7;
		b.f[AiBrain::kWpType] = 1;
		b.f[AiBrain::kWpChannel] = 2;
        b.f[AiBrain::kWpNode] = 0;
        b.f[AiBrain::kWpResolved] = 0;
		b.f[AiBrain::kSpeedA] = b.f[AiBrain::kSpeedB] = 40 * 293;
		b.f[AiBrain::kOutSpeed] = 40 * 293;
		b.f[AiBrain::kUseWaypointZones] = 1;
		ve->profile.patrol_climb = ve->profile.field220 = t.climb_speed;
	}
	void tick(int n) {
        TickContext ctx{};
        ctx.is_authority = true;
        for (int i = 0; i < n; ++i) {
            r.w.update_all_entities(ctx);
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
		h.tick(100);
		// The helicopter has begun turning toward its route after 100 ticks;
		// its authored turn rate does not complete a quarter turn that quickly.
		const int32_t err = h.r.veh().veh.yaw_bam - bam_heading_from_mission_yaw_deg(90.0);
		CHECK(std::abs(err) < 0x40000000);
		h.tick(400);
		CHECK(h.r.veh().veh.net_engine_on);
        CHECK(h.r.veh().position.z > z0 + 5.0f);
        CHECK(h.r.veh().position.x > x0 + 5.0f);
		// The node is never reached: this rig orbits it, by the witnessed math.
		// The SM's out-speed seeds the forward command verbatim ([544] = brain[128]
		// @0x4915a9) and that command is a TILT, not an airspeed: the tilt block
		// pushes the pitch rate by cmd << 11 capped at acceleration << 12
		// (@0x491d41..0x491d67; this rig's 512 -> 0x200000 BAM/tick), the aero
		// block only counters it with (along - cmd) << 4 / << 2 once the airspeed
		// exceeds the command (@0x491f30..0x491fa5), and against the 1019/1024
		// drag (@0x492146..0x492187) the 1169 * sin(pitch) thrust (@0x492018)
		// settles the airspeed near 1.1 u/tick, six times the 0.18 u/tick route
		// speed (40 km/h * 293). The yaw servo is clamped at itemDef+0x924
		// (@0x491cd6..0x491cf8; 0x600000 = 0.53 deg/tick), so the turn radius is
		// ~117 u and the closest approach is 55.7 u (t=350), outside the node's
		// 2 u radius: AI_UpdateMovementTarget's arrival, wrap and speed*step
		// halving arms (@0x460ea6, @0x460f04, @0x460f8e) never fire and
		// brain[128] stays 11720. The hull crosses x = 300 near t = 305 and the
		// same arithmetic puts it here at t = 500 (the tolerance covers last-bit
		// libm differences in the trig lanes, not a behavior band). The pre-port
		// `x < 300` held only while state 7 had no tick handler and the rig's
		// unseeded SM speed left the command at the 132-count creep seed
		// (@0x49181b..0x491834). The brain's route bearing now refreshes only
		// on its think visits (every brain[7] pool-1 visits, Entity_UpdatePool1Slot
		// @0x4b8e1b / @0x4b8ea0), so the orbit's exact 414.44 pin retired with
		// the every-tick think; the band brackets the same crossing-and-orbit.
		CHECK(h.r.veh().position.x > 380.0f && h.r.veh().position.x < 450.0f);
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

// The group redirect reaches a pool-1 BRAIN (mode/list/node) without the pool-0
// leg's resets or budget seed, and the BMS speed commands write kSpeedA/kSpeedB at
// the witnessed x65536/225 scale.
// [orig: Entity_SetWaypointByTeam @0x43CD20 (pool 1 @0x43CE5A..0x43CF01);
//  Entity_ApplyCommand @0x43ab60 0x1D/0x1E -> AI_HandleCommand @0x465770 0xA/0xB]
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
    // The pool-1 leg carries no cooldown/carrier reset and no waypoint refresh:
    // the vehicle keeps its current leg's budget and bearing.
    ve.slot.f[36] = 77;
    ve.inf.wait_cooldown = 5;
    ve.brain.f[AiBrain::kAnimFlag] = 1234;
    ve.brain.f[AiBrain::kWpBearing] = 4321;
    CHECK(r.w.commands.group_to_waypoint(3, 2) == 1); // RedirectGroupTo(3, list 2)
    AiBrain &b = ve.brain;
    CHECK(b.f[AiBrain::kWpType] == 1);
    CHECK(b.f[AiBrain::kWpChannel] == 2);
    // Entry 1 (x=150) is nearest to x=100 — distinct from entry 0, which is also
    // the scan's fallback initializer, so a broken nearest-node scan fails here.
    CHECK(b.f[AiBrain::kWpNode] == 1);
    CHECK(r.veh().wp_number == 1);
    CHECK(b.f[AiBrain::kAnimFlag] == 1234 && b.f[AiBrain::kWpBearing] == 4321);
    // The SLOT half of the witnessed redirect block [orig: Entity_SetWaypointByTeam
    // @0x43CE91..0x43CECF — aiComp+140=1, +148=list, +152=node]: the infantry
    // think navigates from these, not the brain registers.
    CHECK(ve.slot.f[35] == 1);
    CHECK(ve.slot.f[37] == 2);
    CHECK(ve.slot.f[38] == 1);
    CHECK(ve.slot.f[36] == 77 && ve.inf.wait_cooldown == 5);

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

    // A mounted NON-player slot holder in the group auto-detaches on redirect
    // [orig: @0x43CD86..0x43CD98]; a body without a slot is skipped outright
    // [orig: @0x43CD7E].
    Entity npc;
    npc.net_id = 900;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    npc.group_id = 3;
    EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    CHECK(r.w.registry.get(nh)->mounted);
    r.w.commands.group_to_waypoint(3, 2);
    CHECK(r.w.registry.get(nh)->mounted);
    r.sys.attach(nh);
    r.w.commands.group_to_waypoint(3, 2);
    CHECK(!r.w.registry.get(nh)->mounted);

    // The WAC SSNtoWP and the pool-1 leg of RedirectSingleTo DO refresh the
    // waypoint and seed the budget: (|Yaw - bearing| / denom) << 5, the idiv
    // FIRST. The bearing error is a WRAPPING 32-bit sub: heading just short of
    // +half-turn, node bearing just past -half-turn = a ~0.1 deg true error, not
    // ~360 deg. [orig: WacCmd_SsnToWp @0x4F1D9F..0x4F1DB9; Entity_SetWaypointForTeam
    //  @0x43DE70..0x43DE89]
    r.sys.nav.nodes[0] =
            NavEntry{{1 << 16, -300 * 65536, static_cast<int32_t>(199.5 * 65536),
                      10 << 16, 0}};
    // Node 1 moves far off so node 0 is the nearest for the WAC form.
    r.sys.nav.nodes[1] = NavEntry{{1 << 16, 5000 << 16, 200 << 16, 10 << 16, 0}};
    AiEntity &truck = *r.sys.for_handle(r.veh_h);
    truck.heading = 2147000000; // the +pi side of the seam
    CHECK(r.w.commands.set_ssn_waypoint(r.veh_h, 2));
    CHECK(truck.brain.f[AiBrain::kWpNode] == 0);
    CHECK(truck.brain.f[AiBrain::kAnimFlag] >= 0);
    CHECK(truck.brain.f[AiBrain::kAnimFlag] < (1 << 22)); // the short-way error stays small
    const int32_t denom = (truck.brain.f[AiBrain::kStoredKeyTime] >> 15) + 32;
    const int32_t err = std::abs(static_cast<int32_t>(static_cast<uint32_t>(truck.heading) -
                                 static_cast<uint32_t>(truck.brain.f[AiBrain::kWpBearing])));
    CHECK(truck.brain.f[AiBrain::kAnimFlag] == (err / denom) * 32);
    truck.brain.f[AiBrain::kAnimFlag] = 0;
    CHECK(r.w.commands.redirect_ssn_to_waypoint(11, 2, 0) == 1); // the truck's SSN
    CHECK(truck.brain.f[AiBrain::kAnimFlag] == (err / denom) * 32);
}

// The per-leg turn budget of the ground/tank drive leg divides BEFORE the x32:
// (|Yaw - bearing| / ((brain[35] >> 15) + 32)) << 5. A multiply-first form rounds
// differently whenever the error is not a multiple of the divisor.
// [orig: cveh `cdq; idiv edi; shl eax,5` @0x48BCD3..0x48BCD6; ctan @0x48989C..0x48989F]
void test_ai_drive_budget_divides_first() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);
    AiEntity &ve = *r.sys.at(r.sys.attach(r.veh_h));
    ve.pos[0] = 100 << 16;
    ve.pos[1] = 200 << 16;
    ve.pos[2] = 10 << 16;
    r.sys.nav.channels.resize(3);
    r.sys.nav.channels[2].count = 1;
    r.sys.nav.channels[2].entries[0] = 0;
    r.sys.nav.nodes.resize(1);
    // A node up and to the left: a bearing error that is not a multiple of 33.
    r.sys.nav.nodes[0] = NavEntry{{1 << 16, 130 << 16, 290 << 16, 10 << 16, 0}};
    AiBrain &b = ve.brain;
    b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState] = 16;
    b.f[AiBrain::kWpType] = 1;
    b.f[AiBrain::kWpChannel] = 2;
    b.f[AiBrain::kWpNode] = 0;
    b.f[AiBrain::kStoredKeyTime] = 1 << 15; // denominator 33
    b.f[AiBrain::kOutSpeed] = 40 * 293;
    r.veh().veh.yaw_seeded = true;
    r.veh().veh.yaw_bam = 0;
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.item_id = 2072;
    npc.health = 150;
    npc.alive = true;
    const EntityHandle nh = r.w.registry.spawn(0, npc);
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    VehicleDriveCmd cmd;
    r.sys.vehicle_ai_drive(r.w, r.veh(), r.w.vehicles.resolve_controller(r.veh()), t, cmd);
    CHECK(cmd.ai_drive);
    const int32_t err = std::abs(b.f[AiBrain::kWpBearing]);
    CHECK(err % 33 != 0);
    CHECK(b.f[AiBrain::kAnimFlag] == (err / 33) * 32);
    CHECK(b.f[AiBrain::kAnimFlag] != static_cast<int32_t>(32LL * err / 33));
}

// The SP drive input mirror: a mounted LOCAL player's live move bits reach the wire
// input fields the motor consumes (pose_if_mounted mirrors them while tick_infantry is
// skipped), and the occupant leg drives the vehicle from them.
// [orig: one entity struct — MoveOrder feeds Entity_UpdateVehiclePhysics directly]
void test_local_player_drive_mirror() {
    Rig r(30.0f);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);

    // The local player as an infantry-active AI entity in the ctrl seat.
    const int ai_idx = r.sys.attach(r.player_h);
    AiEntity &pe = *r.sys.at(ai_idx);
    pe.inf.active = true;
    pe.inf.is_local_player = true;
    pe.health = 150;
    r.player().ground_target = r.veh_h;
    r.player().flags |= kEntityFlagQueuedMount; // the queued arm picks the ctrl seat
    CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
    r.player().flags &= ~kEntityFlagQueuedMount;
    CHECK(is_vehicle_control_seat(r.player().mount_type));

    // Live input: forward held, looking along +x (mission yaw 90).
    pe.inf.player_moving = true;
    pe.inf.player_move_dir_index = 0;
    pe.inf.target_heading = bam_heading_from_mission_yaw_deg(90.0);

    const float x0 = r.veh().position.x;
    for (int i = 0; i < 124; ++i) {
        CHECK(r.sys.pose_if_mounted(pe, r.w)); // the mirror + seat carry
        r.w.vehicles.tick_motor(r.veh(), t);   // occupant leg reads the mirrored input
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

// Both mounted body paths use the same ordered sit_24 selection without a
// clip-presence gate. Exercise it through the mount and body-pose API.
void test_driver_animation() {
	for (bool local : { false, true }) {
		Rig r(30.0f);
		r.w.vehicles.traits.set(r.veh().item_id, truck_traits());
		r.veh().seats[0].pose_index = 24;
		const int idx = r.sys.attach(r.player_h);
		auto &body = *r.sys.at(idx);
		body.inf.active = true;
		body.inf.is_local_player = local;
		body.health = 150;
		r.player().ground_target = r.veh_h;
		// Out of scan reach (30 u): the queued-mount latch takes the
		// FindBestSeatSlot(groundEntity) arm into the ctrl seat [orig:
		// Entity_TryEnterNearestVehicle @0x4368CF -> @0x4368E0].
		r.player().flags |= kEntityFlagQueuedMount;
		CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
		r.player().flags &= ~kEntityFlagQueuedMount;
		r.veh().veh.yaw_seeded = true;
		struct Case {
			int32_t roll, speed;
			int state;
		};
		const Case cases[] = { { -71582785, 200, 110 }, { -71582784, 200, 100 },
			{ 71582785, 200, 109 }, { 71582784, 200, 100 }, { 71582785, -200, 108 },
			{ 71582785, -199, 107 }, { -71582785, 199, 107 }, { 0, 200, 100 } };
		for (const auto &c : cases) {
			r.veh().veh.air_roll_bam = c.roll;
			r.veh().veh.speed = c.speed;
			CHECK(r.sys.pose_if_mounted(body, r.w));
			CHECK(body.inf.anim_state == c.state);
			CHECK(body.inf.anim_pending == 0);
		}
		// An unseeded hull reads its placement roll in the spawn form: 180 degrees is
		// 0x80000000, below -6 degrees (sit_24_right); 180 x 11930464 = 0x7FFFFF80
		// would pick sit_24_left.
		// [orig: Entity_SpawnFromBMSRecord @0x40EB89..0x40EBA6; the roll cmps
		//  Entity_UpdateInfantryPlayerBody @0x4B65D0 / Entity_UpdateInfantryAI @0x4BEE3D]
		r.veh().veh.yaw_seeded = false;
		r.veh().roll = 180;
		r.veh().veh.speed = 200;
		CHECK(r.sys.pose_if_mounted(body, r.w));
		CHECK(body.inf.anim_state == 110);
		r.veh().veh.yaw_seeded = true;
		r.veh().seats[0].pose_index = 23;
		r.veh().veh.speed = 0;
		CHECK(r.sys.pose_if_mounted(body, r.w));
		CHECK(body.inf.anim_state == 99);
	}
}

// spawn_player stamps commandGroup 1 [orig: the deploy leg @0x519fd0] — 00TRa's tour
// dialogs ("group 1 enters area X") track the player through it.
void test_player_spawn_group() {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    AiSystem &sys = w.ai;
    w.registry.configure_pool(0, 8);
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
    r.w.vehicles.collect_attach_labels(r.player(), /*armory_mode=*/false, /*can_fire=*/false, labels);
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
    r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
    CHECK(labels.size() == 1);
    CHECK(labels[0].seat_index == 0);

    // Out of the 4.0 u label radius -> the gate empties the list via the nearest scan
    // [orig: the Entity_FindNearestSeatOrArmory bracket @0x5a32e2].
    Rig far(30.0f);
    labels.clear();
    far.w.vehicles.collect_attach_labels(far.player(), false, false, labels);
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
    r.w.vehicles.collect_attach_labels(r.player(), false, /*can_fire=*/false, all);
    bool saw_veh2 = false;
    for (const AttachLabel &l : all) saw_veh2 = saw_veh2 || l.entity == veh2_h;
    CHECK(all.size() >= 3); // both trucks' free seats
    CHECK(saw_veh2);

    std::vector<AttachLabel> armed;
    r.w.vehicles.collect_attach_labels(r.player(), false, /*can_fire=*/true, armed);
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
    look_at(r.w, r.player_h, {99.0f, 199.0f, 11.0f}); // the aim cone

    std::vector<AttachLabel> labels;
    r.w.vehicles.collect_attach_labels(r.player(), /*armory_mode=*/true, false, labels);
    CHECK(labels.size() == 1); // the truck's seats do NOT label in armory mode
    CHECK(labels[0].entity == crate_h);
    CHECK(labels[0].armory);
    CHECK(labels[0].type == SeatType::ArmoryPoint);
    CHECK(labels[0].nearest);

    // Seat mode ignores armory points.
    labels.clear();
    r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
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
    player_seed.yaw = 90; // facing +x, down the row of seats (the aim cone)
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
    w.vehicles.collect_attach_labels(*player, false, false, labels, &stats);
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
	// A four-wheel hull resting on a flat ten-unit road. The old fixture
	// omitted model geometry and only exercised the retired center sphere.
	std::vector<uint16_t> heights(16 * 16, 10 * 256);
	std::vector<int> road_sectors(256, 1);
	opennova::terrain::TerrainHeightField terrain;
	terrain.heightmap = heights.data();
	terrain.dim = 16;
	terrain.layout.sector_grid = road_sectors.data();
	r.w.tables.terrain = &terrain;
	t.box_x_lo = t.foot_x_lo = -2 * 65536;
	t.box_x_hi = t.foot_x_hi = 2 * 65536;
	t.box_y_lo = t.foot_y_lo = -65536;
	t.box_y_hi = t.foot_y_hi = 65536;
	t.box_z_lo = 0;
	t.box_z_hi = 2 * 65536;
	t.max_slope = 50 * 11930464;
	t.slip_slope = 35 * 11930464;
	r.veh().bound_radius = 3.0f;
	t.torque = 2; // sev-3 decay = speed - (speed >> 4) per contact tick
	r.w.vehicles.traits.set(r.veh().item_id, t);

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
	b.f[AiBrain::kPendState] = 16;
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
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    Entity *ctrl = r.w.vehicles.resolve_controller(r.veh());
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
			const int32_t probes[1][3] = { { probe_pos[0], probe_pos[1], probe_pos[2] + 65536 } };
			const int32_t radii[1] = { 98304 };
			for (int axis = 0; axis < 3; ++axis)
				r.veh().saved_live_pos[axis] = probe_prev[axis];
			r.veh().saved_live_valid = true;
			VehicleProbeForce force[1];
			EntityHandle hit;
			const int sev = cw.resolve_vehicle_probes(
					r.w, r.veh(), probe_pos, probes, radii, 1, 2500000, 3400000, force, hit);
			CHECK(sev == 3);
			CHECK(force[0].fx < 0);
			CHECK(force[0].fy == 0);
			CHECK(hit == wh);
		}
		r.veh().position = {100.0f, 200.0f, 10.0f};
    }

    for (int i = 0; i < 950; ++i) {
        cw.build_tick_tables(r.w); // self-gated to the 17-tick cadence
        VehicleDriveCmd cmd;
        r.sys.vehicle_ai_drive(r.w, r.veh(), ctrl, t, cmd);
		stamp_saved_live_pose(r.veh());
		r.w.vehicles.tick_motor(r.veh(), t, &cmd);
		AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->pos[0] = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->pos[1] = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->pos[2] = static_cast<int32_t>(r.veh().position.z * 65536.0f);
        ve2->heading = r.veh().veh.yaw_bam;
    }
	// The wall's near face is x=148; the front wheel/hull probes hold the truck
	// outside it. Without the contact leg the truck ends far past 150.
	CHECK(r.veh().position.x > 110.0f); // it drove
	CHECK(r.veh().position.x < 149.0f);  // and stopped at the wall
    CHECK(std::abs(r.veh().veh.speed) < 40 * 293 / 2); // the contact decay bit

}

// The seat/exit frame is the collision frame: entity_local_point_world must
// (a) reduce to the proven 2D -yaw rotate for flat carriers, and (b) match an
// independent double-precision Rz(heading)Ry(-pitch)Rx(roll) for rolled ones —
// the frame the collision shell is posed with (target_view). A yaw-only seat
// frame under a rolled carrier is the witnessed 00TRg dismount 4-pin
// (AI-PARITY-CONCEPT §6.13). [orig: @0x4b0c50 over @0x613f40]
// A live helicopter carries sub-degree BAM angles even while its authored
// integer-degree mirrors stay zero. Both the passenger seat and the model's
// addeweap anchor must move with that live frame, as the rendered hull does.
void test_seat_and_emplacement_keep_subdegree_carrier_pose() {
    using namespace opennova::threedi;
    Entity carrier{};
    carrier.position = {100.0f, 200.0f, 30.0f};
    carrier.veh.yaw_seeded = true;
    carrier.veh.yaw_bam = bam_heading_from_mission_yaw_deg(0.25);
    carrier.veh.air_pitch_bam = bam_from_degrees_wrapped(0.25);
    carrier.veh.air_roll_bam = bam_from_degrees_wrapped(-0.25);
    const int32_t raw[3] = {4 * 65536, 65536, -2 * 65536};
    int32_t expected[3];
    entity_placement_matrix(carrier).transform_point(raw, expected);
    const Vec3 seat_local{-1.0f, 4.0f, -2.0f};
    const Vec3 seat_world = entity_local_point_world(carrier, seat_local);
    CHECK(std::abs(seat_world.x - from_fixed(expected[0])) < 0.001);
    CHECK(std::abs(seat_world.y - from_fixed(expected[1])) < 0.001);
    CHECK(std::abs(seat_world.z - from_fixed(expected[2])) < 0.001);

    ThreediRenderObject part{};
    ThreediLod lod{};
    lod.render_objects = &part;
    lod.render_object_count = 1;
    ThreediUserPoint point{};
    point.x = raw[0]; point.y = raw[1]; point.z = raw[2];
    point.rot_x = 65536;
    Threedi3di3 model{};
    model.lods = &lod;
    model.lod_count = 1;
    model.user_points = &point;
    model.user_point_count = 1;
    Seat anchor{};
    anchor.type = SeatType::Gunner;
    anchor.bone_index = 1;
    anchor.attachment_frame = true;
    MountedPose pose;
    CHECK(resolve_model_mounted_pose(model, carrier, anchor, nullptr, 0, pose));
    CHECK(std::abs(pose.position.x - from_fixed(expected[0])) < 0.001);
    CHECK(std::abs(pose.position.y - from_fixed(expected[1])) < 0.001);
    CHECK(std::abs(pose.position.z - from_fixed(expected[2])) < 0.001);
    // The bone resolver must retain the same fractional attitude as its point.
    CHECK(std::abs(mission_yaw_deg_from_bam_heading(pose.heading) - 0.25) < 0.001);
    CHECK(std::abs(pose.pitch * kDegreesPerBam - 0.25) < 0.001);
    CHECK(std::abs(pose.roll * kDegreesPerBam + 0.25) < 0.001);
    // A point without an authored direction falls back to the same full frame.
    point.rot_x = 0;
    CHECK(resolve_model_mounted_pose(model, carrier, anchor, nullptr, 0, pose));
    CHECK(std::abs(mission_yaw_deg_from_bam_heading(pose.heading) - 0.25) < 0.001);
    CHECK(std::abs(pose.pitch * kDegreesPerBam - 0.25) < 0.001);
    CHECK(std::abs(pose.roll * kDegreesPerBam + 0.25) < 0.001);

    World w;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    carrier.health = 100;
    carrier.alive = true;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.seat_local = seat_local;
    carrier.seats.push_back(seat);
    const EntityHandle carrier_h = w.registry.spawn(1, carrier);
    Entity occupant;
    occupant.health = 100;
    occupant.alive = true;
    occupant.mounted = true;
    occupant.mount_target = carrier_h;
    occupant.mount_seat = 0;
    occupant.mount_type = SeatType::Passenger;
    const EntityHandle occupant_h = w.registry.spawn(0, occupant);
    AiEntity *body = w.ai.at(w.ai.attach(occupant_h));
    body->inf.active = true;
    body->inf.is_local_player = true;
    body->heading = bam_heading_from_mission_yaw_deg(33.125);
    body->pitch = bam_from_degrees_wrapped(-6.25);
    const int32_t look_heading = body->heading, look_pitch = body->pitch;
    CHECK(w.ai.refresh_mounted_pose(*body, w));
    CHECK(body->inf.body_heading == carrier.veh.yaw_bam);
    CHECK(body->inf.leg_yaw[0] == carrier.veh.yaw_bam);
    CHECK(body->body_pitch == carrier.veh.air_pitch_bam);
    CHECK(body->roll == carrier.veh.air_roll_bam);
    CHECK(body->heading == look_heading && body->pitch == look_pitch);
}

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

static void test_script_remove_releases_carrier_and_occupant_ownership() {
    {
        Rig r;
        r.veh().item_type = 1;
        r.player().item_type = 3;
        Entity passenger = r.player();
        passenger.player_class = 0;
        passenger.net_id = 42;
        const EntityHandle passenger_h = r.w.registry.spawn(0, passenger);
        CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
        CHECK(r.w.vehicles.process_attach(passenger_h, r.veh_h, 2));
        r.sys.attach(r.veh_h);
        r.sys.attach(r.player_h);
        r.sys.attach(passenger_h);
        AiEntity &observer = *r.sys.for_handle(passenger_h);
        observer.inf.active = true;
        observer.inf.combat_target = r.veh_h;
        observer.slot.f[3] = r.veh_h.packed + 1;
        CollisionWorld collision;
        r.w.collision = &collision;
        const int model = collision.add_model(CollisionModel{});
        collision.assign_entity(r.veh_h, model, r.veh().registry_spawn_id);
        CHECK(collision.has_instance(r.veh_h));
        CHECK(r.w.out.scars.ring_for(r.veh_h, r.veh().registry_spawn_id) != nullptr);
        CHECK(r.w.out.scars.leased_count() == 1);
        r.veh().death_effect_active[1] = 1;
        CHECK(r.w.commands.remove_ssn(r.veh_h));
        CHECK(r.w.registry.get(r.veh_h) == nullptr && r.sys.for_handle(r.veh_h) == nullptr);
        CHECK(!r.player().mounted && !r.player().mount_target.valid());
        CHECK(!r.w.registry.get(passenger_h)->mounted);
        CHECK(!observer.inf.combat_target.valid() && observer.slot.f[3] == 0);
        CHECK(!collision.has_instance(r.veh_h) && r.w.out.scars.leased_count() == 0);
        CHECK(r.w.out.destruction.effects.size() == 4);
        for (const auto &effect : r.w.out.destruction.effects)
            CHECK(effect.release && effect.family == 2 && effect.attach_wire_handle == r.veh_h.packed);
        CHECK(!r.w.commands.remove_ssn(r.veh_h));
        r.w.collision = nullptr;
    }
    {
        Rig r;
        r.player().item_type = 3;
        CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
        r.sys.attach(r.player_h);
        CHECK(r.veh().primary_occupant == r.player_h);
        CHECK(r.w.commands.remove_ssn(r.player_h));
        CHECK(!r.veh().primary_occupant.valid());
        CHECK(!r.veh().seats[0].occupant.valid());
        CHECK(r.sys.for_handle(r.player_h) == nullptr);
    }
}

// USE and its label must address the same live bone that seats the rider.
// A tank turret can turn the seat away from its model's rest position.
static void test_use_scan_and_label_follow_live_seat_pose() {
    Rig r;
    r.veh().seats.resize(1);
    r.veh().seats[0].type = SeatType::Gunner;
    r.veh().seats[0].seat_local = {20.0f, 0.0f, 0.0f};
    FakeMountedPoseProvider provider;
    provider.pose = pose_degrees({102.0f, 201.0f, 10.5f}, 0, 0, 0);
    r.w.pose_provider = &provider;
    look_at(r.w, r.player_h, provider.pose.position);
    VehicleSeatSelection hit;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(hit.vehicle == r.veh_h && hit.seat_index == 0);
    std::vector<AttachLabel> labels;
    r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
    CHECK(labels.size() == 1);
    if (!labels.empty()) {
        CHECK(std::abs(labels[0].world_pos.x - provider.pose.position.x) < 1e-5f);
        CHECK(std::abs(labels[0].world_pos.y - provider.pose.position.y) < 1e-5f);
        CHECK(std::abs(labels[0].world_pos.z - provider.pose.position.z - 0.1875f) < 1e-5f);
    }
    provider.available = false;
    CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    r.w.pose_provider = nullptr;
}

// USE and its labels score every seat kind at its live bone through the
// carrier's attachment build, not only a UseGun seat. The provider answers
// that attachment form and declines a plain rider pose for a non-gunner seat.
// [orig: Entity_FindNearestSeatOrArmory seat kinds @0x435F6C..0x435FDF ->
//  build_bone_attachment_matrix @0x435FFA; labels @0x5A3553]
struct AttachmentFormProvider final : IPoseProvider {
    MountedPose pose;
    bool resolve_mounted_pose(World &, const Entity &, const Seat &seat,
                              MountedPose &out) override {
        if (seat.type != SeatType::Gunner || !seat.attachment_frame) return false;
        out = pose;
        out.position.z += static_cast<float>(seat.bone_index);
        return true;
    }
};
static void test_use_scan_poses_every_seat_kind_live() {
    Rig r;
    AttachmentFormProvider provider;
    provider.pose = pose_degrees({101.0f, 201.0f, 9.0f}, 0, 0, 0);
    r.w.pose_provider = &provider;
    std::vector<AttachLabel> labels;
    r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
    CHECK(labels.size() == 2);
    for (const AttachLabel &label : labels) {
        const Seat &seat = r.veh().seats[static_cast<size_t>(label.seat_index)];
        CHECK(std::abs(label.world_pos.x - 101.0f) < 1e-5f);
        CHECK(std::abs(label.world_pos.y - 201.0f) < 1e-5f);
        CHECK(std::abs(label.world_pos.z -
                (9.0f + static_cast<float>(seat.bone_index) + 0.1875f)) < 1e-5f);
    }
    r.w.pose_provider = nullptr;
}

// The claimant leaving a PlayerControl vehicle cuts the running action of the
// vehicle's weapon slot short: a nonzero counter drops to 7 ticks. An idle
// slot, a passenger leaving and a vehicle without the attrib keep theirs.
// [orig: Entity_DetachFromVehicle @0x4356E3..0x4356FF]
static void test_claimant_detach_cuts_vehicle_slot_action() {
    Rig r;
    r.veh().item_attrib |= kItemAttribPlayerControl;
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    CHECK(r.veh().primary_occupant == r.player_h);
    r.veh().primary_weapon_slot.counter = 40;
    CHECK(r.w.vehicles.detach(r.player_h));
    CHECK(r.veh().primary_weapon_slot.counter == 7);

    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    r.veh().primary_weapon_slot.counter = 0;
    CHECK(r.w.vehicles.detach(r.player_h));
    CHECK(r.veh().primary_weapon_slot.counter == 0);

    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
    CHECK(!r.veh().primary_occupant.valid());
    r.veh().primary_weapon_slot.counter = 40;
    CHECK(r.w.vehicles.detach(r.player_h));
    CHECK(r.veh().primary_weapon_slot.counter == 40);

    r.veh().item_attrib &= ~kItemAttribPlayerControl;
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    CHECK(r.veh().primary_occupant == r.player_h);
    CHECK(r.w.vehicles.detach(r.player_h));
    CHECK(r.veh().primary_weapon_slot.counter == 40);
}

// A non-player leaving any seat zeroes its AdmDef byte; a player-classified
// rider keeps its own. [orig: Entity_DetachFromVehicle Flags 0x100 test
// @0x43568D, EquippedSlot @0x435696, the byte @0x43569C]
static void test_npc_detach_zeroes_the_equipped_byte() {
    Rig r;
    Entity npc;
    npc.net_id = 40;
    npc.kind = EntityKind::Organic;
    npc.position = r.player().position;
    npc.health = 100;
    npc.alive = true;
    const EntityHandle npc_h = r.w.registry.spawn(0, npc);
    for (const uint8_t bone : {uint8_t{1}, uint8_t{2}}) { // ctrlx00, sitex00
        CHECK(r.w.vehicles.process_attach(npc_h, r.veh_h, bone));
        r.w.registry.get(npc_h)->equipped_adm_index = 7;
        CHECK(r.w.vehicles.detach(npc_h));
        CHECK(r.w.registry.get(npc_h)->equipped_adm_index == 0);
    }
    r.player().flags |= kEntityFlagPlayer;
    r.player().engine_flags |= kEntityFlagPlayer;
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
    r.player().equipped_adm_index = 7;
    CHECK(r.w.vehicles.detach(r.player_h));
    CHECK(r.player().equipped_adm_index == 7);
}

// An NPC's ordinary seat resolves through the carrier's seat bone. When that
// lookup fails, the authority kills a rider boarding-ordered (123..125) at the
// carrier or at the carrier's ground link, crediting the carrier's last
// attacker, and detaches every such rider; a client keeps it seated. The
// UseGun seat and a def-less carrier never ask.
// [orig: Entity_UpdateInfantryAI @0x4BED72..0x4BED7C, @0x4BEE93..0x4BEEE4]
static void test_npc_seat_bone_failure_kills_boarders_and_detaches() {
    struct SeatBoneProvider final : IPoseProvider {
        bool resolves = false;
        int queries = 0;
        int last_bone = -1;
        bool resolve_seat_bone(World &, const Entity &, int bone_index) override {
            ++queries;
            last_bone = bone_index;
            return resolves;
        }
    };
    struct Outcome {
        bool killed = false;
        bool credited = false;
        bool detached = false;
        int queries = 0;
        int bone = -1;
    };
    const auto run = [](int32_t order, int32_t target_ssn, bool authority, SeatType type,
                        bool resolves, bool item_def) {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        SeatBoneProvider provider;
        provider.resolves = resolves;
        w.pose_provider = &provider;
        Entity attacker;
        attacker.net_id = 400;
        attacker.kind = EntityKind::Organic;
        attacker.health = 100;
        attacker.alive = true;
        const EntityHandle attacker_h = w.registry.spawn(0, attacker);
        Entity deck;
        deck.net_id = 300;
        deck.kind = EntityKind::Item;
        deck.health = 100;
        deck.alive = true;
        const EntityHandle deck_h = w.registry.spawn(1, deck);
        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.health = 100;
        vehicle.alive = true;
        vehicle.has_item_def = item_def;
        vehicle.ground_target = deck_h;
        vehicle.last_attacker = attacker_h;
        Seat seat;
        seat.type = type;
        seat.bone_index = 7;
        seat.source_name = type == SeatType::Gunner ? "UseGun" : "sitex00";
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);
        Entity occupant;
        occupant.net_id = 100;
        occupant.kind = EntityKind::Organic;
        occupant.health = 100;
        occupant.alive = true;
        const EntityHandle occupant_h = w.registry.spawn(0, occupant);
        AiEntity *body = w.ai.at(w.ai.attach(occupant_h));
        body->net_id = 100;
        body->health = 100;
        body->inf.active = true;
        body->slot.f[37] = order;
        body->slot.f[38] = target_ssn;
        CHECK(w.vehicles.process_attach(occupant_h, vehicle_h, 7));
        w.ai.is_authority = authority;
        w.ai.tick_infantry(*body, w, 1);
        const Entity *occ = w.registry.get(occupant_h);
        Outcome out;
        out.killed = occ->health == 0 && body->health == 0;
        out.credited = occ->last_attacker == attacker_h;
        out.detached = !occ->mounted && !occ->ground_target.valid();
        out.queries = provider.queries;
        out.bone = provider.last_bone;
        return out;
    };

    // A board order at the carrier: killed with the carrier's credit, detached.
    Outcome o = run(kCommandAttachPassengerOnly, 200, true, SeatType::Passenger, false, true);
    CHECK(o.queries == 1 && o.bone == 7);
    CHECK(o.killed && o.credited && o.detached);
    // At the carrier's ground link, and each of the three board orders.
    o = run(kCommandAttachSkipController, 300, true, SeatType::Passenger, false, true);
    CHECK(o.killed && o.credited && o.detached);
    o = run(kCommandAttachAnySeat, 200, true, SeatType::Driver, false, true);
    CHECK(o.killed && o.credited && o.detached);
    // No board order, or one aimed elsewhere: detached alive.
    o = run(0, 200, true, SeatType::Passenger, false, true);
    CHECK(!o.killed && !o.credited && o.detached);
    o = run(kCommandAttachPassengerOnly, 999, true, SeatType::Passenger, false, true);
    CHECK(!o.killed && o.detached);
    // A client neither kills nor detaches.
    o = run(kCommandAttachPassengerOnly, 200, false, SeatType::Passenger, false, true);
    CHECK(!o.killed && !o.detached);
    // A resolving bone keeps the rider seated.
    o = run(kCommandAttachPassengerOnly, 200, true, SeatType::Passenger, true, true);
    CHECK(o.queries == 1 && !o.killed && !o.detached);
    // The UseGun seat and a def-less carrier never ask.
    o = run(kCommandAttachPassengerOnly, 200, true, SeatType::Gunner, false, true);
    CHECK(o.queries == 0 && !o.killed && !o.detached);
    o = run(kCommandAttachPassengerOnly, 200, true, SeatType::Passenger, false, false);
    CHECK(o.queries == 0 && !o.killed && !o.detached);
}

// The model-aware provider answers the lookup from the carrier's attached
// model: bone 0 and a bone past the model's userpoint count fail, a model
// without bounding volumes fails, the husk bit swaps in the husk model when
// one is attached, a carrier without a model fails, and a world without
// collision has no model data to decline with.
// [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50]
static void test_entity_pose_seat_bone_lookup() {
    World w;
    w.registry.configure_pool(1, 4);
    CollisionWorld collision;
    w.collision = &collision;
    EntityPoseProvider poses;
    opennova::threedi::ThreediUserPoint points[3] = {};
    opennova::threedi::ThreediCollisionModel block = {};
    block.model_data.num_bounding_volumes = 1;
    opennova::threedi::Threedi3di3 intact = {};
    intact.user_points = points;
    intact.user_point_count = 3;
    intact.collision = &block;
    opennova::threedi::Threedi3di3 husk = intact;
    husk.user_point_count = 1;
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.net_id = 5;
    const EntityHandle carrier_h = w.registry.spawn(1, seed);
    seed.net_id = 6;
    const EntityHandle bare_h = w.registry.spawn(1, seed);
    const int32_t intact_id = collision.add_model(CollisionModel{});
    const int32_t husk_id = collision.add_model(CollisionModel{});
    collision.assign_entity(carrier_h, intact_id);
    collision.assign_entity_husk(carrier_h, husk_id);
    poses.register_userpoint_model(intact_id,
            std::make_shared<opennova::threedi::Threedi3di3>(intact));
    poses.register_userpoint_model(husk_id,
            std::make_shared<opennova::threedi::Threedi3di3>(husk));
    Entity &carrier = *w.registry.get(carrier_h);
    CHECK(poses.resolve_seat_bone(w, carrier, 1));
    CHECK(poses.resolve_seat_bone(w, carrier, 3));
    CHECK(!poses.resolve_seat_bone(w, carrier, 0));
    CHECK(!poses.resolve_seat_bone(w, carrier, 4));
    carrier.engine_flags |= kEntityFlagHusk;
    CHECK(poses.resolve_seat_bone(w, carrier, 1));
    CHECK(!poses.resolve_seat_bone(w, carrier, 3)); // the husk authors one bone
    carrier.engine_flags &= ~kEntityFlagHusk;
    block.model_data.num_bounding_volumes = 0;
    CHECK(!poses.resolve_seat_bone(w, carrier, 1));
    block.model_data.num_bounding_volumes = 1;
    CHECK(!poses.resolve_seat_bone(w, *w.registry.get(bare_h), 1));
    w.collision = nullptr;
    CHECK(poses.resolve_seat_bone(w, *w.registry.get(bare_h), 1));
}

int main() {
    test_use_scan_poses_every_seat_kind_live();
    test_claimant_detach_cuts_vehicle_slot_action();
    test_npc_detach_zeroes_the_equipped_byte();
    test_use_scan_and_label_follow_live_seat_pose();
    test_seat_and_emplacement_keep_subdegree_carrier_pose();
    test_script_remove_releases_carrier_and_occupant_ownership();
	test_vehicle_pending_death_survives_drive_tick();
	test_seat_frame_matches_collision_frame();
	test_usegun_attach_presnaps_local_look();
    test_host_crewed_helicopter_rotor_turns();
    test_seated_body_claims_its_vehicle();
    test_epilog_entity_update();
    test_same_team_hold_scan_follows_the_session();
    test_remote_player_control_seat_preserves_wire_look();
    test_live_pose_provider_and_static_fallback();
    test_npc_seat_bone_failure_kills_boarders_and_detaches();
    test_entity_pose_seat_bone_lookup();
    test_toggle_nearest_seat();
    test_unarmed_usegun_needs_the_session();
    test_best_seat_walks_vehicle_children();
    test_attach_scan_never_built_fallback_and_initial_empty_slice();
    test_post_epoch_player_spawn_discovers_nearby_seat_immediately();
    test_restore_refreshes_completed_candidate_epoch();
    test_toggle_deck_best_seat();
    test_queued_player_mount_consumes_teleport_fallback_once();
    test_vehicle_respawn_clears_matching_teleport_ground_backup();
    test_toggle_dismount_and_swap();
	test_scan_uses_live_eye_offset();
	test_enemy_on_carried_gun_blocks_root_and_sibling();
	test_vehicle_admission_stationary_threshold_and_deck();
	test_seated_board_any_upgrades_on_64_tick_phase();
	test_ai_authored_entry_claim_and_stages();
	test_ai_boarding_moving_and_destroyed_carrier_admission();
	test_enemy_occupant_blocks_scan();
    test_bms_mount_predicates();
    test_mounted_ammo_slot_route();
    test_prepare_vehicle_weapon_slot_after_armory_load();
    test_prepare_vehicle_weapon_slot_seeds_the_zoom();
    test_ai_drive_leg();
    test_ai_drive_avoid_brake();
    test_ai_drive_avoid_quantized_footprints();
    test_stuck_check();
    test_min_ai_crew_clamp();
    test_handbrake_latch();
    test_ground_waits_for_boarders();
    test_zero_health_hull_keeps_its_driver_leg();
    test_submerged_player_driver_takes_the_ai_leg();
    test_mover_command_registers_are_the_brain_words();
    test_helo_ai_flight();
    test_helo_authority_health();
    test_redirect_and_speed_commands();
    test_ai_drive_budget_divides_first();
    test_local_player_drive_mirror();
	test_driver_animation();
	test_player_spawn_group();
	test_attach_labels_seats();
    test_attach_labels_can_fire_gate();
    test_attach_labels_armory_mode();
    test_attach_labels_build_enemy_occupancy_once();
    test_vehicle_hull_stops_at_building();
    if (failures == 0) std::printf("vehicle_mount_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
