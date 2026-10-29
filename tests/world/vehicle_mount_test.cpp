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
#include <runtime/world/destruction.h> // kAmmoKzOrganicBlast
#include <runtime/world/entity.h>
#include <runtime/world/entity_pose.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/replication/entity_wire_bridge.h> // snapshot_of
#include <runtime/world/local_player.h>
#include <runtime/world/player_loadout.h> // local_loadout_rebuild
#include <runtime/world/player_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/mount_controls.h> // emplaced_gun_frame_heading
#include <runtime/world/mounted_pose.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>
#include <runtime/world/infantry_internal.h> // infantry_death_edge

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "channel_clip_source.h"
#include "../common/hull_brain.h"

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

// A body holding its animation channel that plays nothing, for a rig with no clip source.
test_world::ChannelClipSource channel_only;

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    AiSystem &sys = w.ai;
    EntityHandle veh_h, player_h;

    explicit Rig(float player_dx = 2.0f) {
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        sys.root_motion = &channel_only; // the bodies hold their channels

        Entity veh;
        veh.net_id = 11; // 00TRa's DTruck1 SSN
        veh.bms_id = 11;
        veh.kind = EntityKind::Item;
        veh.has_item_def = true; // pool-1 sources need an ItemDef (retail gate)
        veh.has_graphic_model = true;
        // The model's userpoints are the seat bones below (the entry admission
        // refuses a model holding none) [orig: Entity_CanEnterVehicle @0x43549C..0x4354A3].
        veh.graphic_model_userpoints = 2;
        // Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
        veh.item_attrib = kItemAttribPlayerControl;
        veh.item_id = 1294;
        veh.item_type_index = 7; // the def row's ordinal, the script gates' +0x1C
        veh.position = {100.0f, 200.0f, 10.0f};
        veh.heading = placed_heading_bam(0);
        veh.health = 2000;
        veh.health_max = 2000;
        veh.alive = true;
        Seat ctrl;
        ctrl.type = SeatType::Controller;
        ctrl.bone_index = 1;
        ctrl.source_name = "ctrlx00";
        ctrl.seat_local = {0.5f, 1.5f, 1.0f};
        ctrl.bone_point[0] = 98304;
        ctrl.bone_point[1] = -32768;
        ctrl.bone_point[2] = 65536;
        veh.seats.push_back(ctrl);
        Seat sit;
        sit.type = SeatType::Passenger;
        sit.bone_index = 2;
        sit.source_name = "sitex00";
        sit.seat_local = {0.0f, -2.0f, 1.2f};
        sit.bone_point[0] = -131072;
        sit.bone_point[1] = 0;
        sit.bone_point[2] = 78643;
        veh.seats.push_back(sit);
        veh_h = w.registry.spawn(1, veh);

        Entity pl;
        pl.kind = EntityKind::Organic;
        pl.item_type_index = 3; // the def row's ordinal: the +0x1C slice gate
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
	player->heading = placed_heading_bam(static_cast<int16_t>(std::lround(90.0 - heading * 180.0 / 3.14159265358979323846)));
	player->pitch_bam = spawn_angle_bam(static_cast<int>(std::lround(pitch * 180.0 / 3.14159265358979323846)));
	if (AiEntity *body = w.ai.for_handle(player_h)) {
		body->heading = static_cast<int32_t>(heading * opennova::io::kBamPerRadian);
		body->pitch = static_cast<int32_t>(pitch * opennova::io::kBamPerRadian);
	}
}

MountedPose pose_degrees(Vec3 position, double yaw, double pitch, double roll) {
    MountedPose pose;
    pose.position = position;
    pose.heading = bam_heading_from_mission_yaw_deg(yaw);
    pose.pitch = bam_from_degrees_wrapped(pitch);
    pose.roll = bam_from_degrees_wrapped(roll);
    return pose;
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

// An organic occupant's words after a seat pose: the seat's Position, Yaw and Roll
// words, its Pitch word left as the look made it (D-NET-923).
// [orig: Entity_UpdateInfantryAI - Roll @0x4BEDD7, the bone pitch into +0x90
//  @0x4BEDDA; Entity_AttachToVehicleSlot @0x494752..0x494775 (no angle)]
MountedPose organic_words(MountedPose seat, int32_t look_pitch) {
    seat.pitch = look_pitch;
    return seat;
}

void check_pose(const Entity &entity, const MountedPose &expected) {
    CHECK(std::abs(entity.position.x - expected.position.x) < 0.0001f);
    CHECK(std::abs(entity.position.y - expected.position.y) < 0.0001f);
    CHECK(std::abs(entity.position.z - expected.position.z) < 0.0001f);
    CHECK(mission_yaw_whole_deg(entity.heading) == std::lround(mission_yaw_deg_from_bam_heading(expected.heading)));
    CHECK(entity.pitch_bam == expected.pitch); // the live words (D-NET-923)
    CHECK(entity.roll_bam == expected.roll);
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
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.has_graphic_model = true;
    gun.health = 100;
    gun.alive = true;
    gun.heading = placed_heading_bam(35);
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

    CHECK(w.vehicles.request_attach(player_h, {gun_h, 0, SeatType::Gunner}));
    // The unmoved gun's spawn heading ((55 << 16) / 360) << 16 = 656146432 less
    // 0x0400 << 16. [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
    const int32_t expected_heading = 589037568;
    // The attach writes no Yaw word: the entity's stays the request's snap, the
    // gun frame (D-NET-923); the UseGun carry restores the Yaw it saved at its
    // entry. [orig: Entity_AttachToUseGunSlot @0x546B80 - the stores
    //  @0x546C5C..0x546C7C, no Yaw; Entity_AttachToBoneAndUpdateTransform @0x5463D0 -
    //  the Yaw restore @0x546661]
    CHECK(w.registry.get(player_h)->heading == expected_heading);
    CHECK(body.heading == expected_heading);
    CHECK(body.inf.target_heading == expected_heading);
    CHECK(body.inf.look_pitch == 0x12345678); // request pre-snap is yaw-only

    CHECK(ai.pose_if_mounted(body, w));
    CHECK(body.heading == expected_heading);
    // The unmoved gun's spawn heading ((55 << 16) / 360) << 16 = 656146432, turned by the
    // 12-degree seat offset. [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
    CHECK(body.inf.body_heading == 799312009);
}

// The attach yaw snap is the request's alone (D-NET-421). A request turns the
// requester to the seat whenever the carrier's bone lookup answers, ahead of
// every gate of the process, so a request the process refuses has turned the
// body all the same; the process itself (the C2S 0x26 handler, the vehicle
// spawn, a client's decoded relation) writes no yaw.
// [orig: Entity_RequestVehicleAttach @0x4364A0 -- the lookup @0x436540..0x43654a,
//  the stores @0x43655F..0x4365CE, then the process @0x4365E4; the 0x26 handler
//  NapiNPServerMsg_HandleVehicleAttach @0x5023ED / @0x502418 and a client's
//  Entity_TryAttachOrDetach @0x4366E8 reach Entity_ProcessVehicleAttach directly;
//  the AI board's request Entity_UpdateInfantryAI @0x4BBDF2 (EntityCommands::mount); the
//  enemy refusal Entity_ProcessVehicleAttach @0x435b40]
void test_attach_yaw_snaps_at_the_request_alone() {
	constexpr int32_t kStart = 0x10000000;
	struct Npc {
		EntityHandle handle;
		AiEntity *body = nullptr;
	};
	const auto stage = [&](Rig &r) {
		for (Seat &seat : r.veh().seats) seat.yaw_offset = 30;
		Entity npc;
		npc.kind = EntityKind::Organic;
		npc.net_id = 50;
		npc.health = 100;
		npc.alive = true;
		npc.team = 1;
		npc.position = r.player().position;
		Npc out;
		out.handle = r.w.registry.spawn(0, npc);
		out.body = r.sys.at(r.sys.attach(out.handle));
		out.body->inf.active = true;
		out.body->health = 100;
		out.body->heading = kStart;
		return out;
	};
	// Every seat here turns 30 degrees off the carrier's frame.
	const auto seat_heading = [](const Rig &r) {
		return opennova::io::bam_sub(emplaced_gun_frame_heading(
				*r.w.registry.get(r.veh_h)), bam_from_degrees_wrapped(30));
	};
	{
		// The AI board's mount is a request: a free seat turns and seats the body.
		Rig r;
		const Npc npc = stage(r);
		CHECK(r.w.commands.mount(50, 11));
		CHECK(r.w.registry.get(npc.handle)->mount_target == r.veh_h);
		CHECK(npc.body->heading == seat_heading(r));
	}
	{
		// (a) The process refuses a carrier an enemy rides, after the request
		// turned the body.
		Rig r;
		const Npc npc = stage(r);
		Entity enemy;
		enemy.kind = EntityKind::Organic;
		enemy.has_item_def = true;
		enemy.health = 100;
		enemy.alive = true;
		enemy.team = 2;
		enemy.mounted = true;
		enemy.mount_target = r.veh_h;
		r.w.registry.spawn(0, enemy);
		CHECK(!r.w.commands.mount(50, 11));
		CHECK(!r.w.registry.get(npc.handle)->mounted);
		CHECK(npc.body->heading == seat_heading(r));
	}
	{
		// The request writes no yaw where the carrier's bone lookup fails.
		struct NoSeatBone final : IPoseProvider {
			bool resolve_seat_bone(World &, const Entity &, int) override { return false; }
		} no_bone;
		Rig r;
		const Npc npc = stage(r);
		r.w.pose_provider = &no_bone;
		CHECK(r.w.commands.mount(50, 11));
		CHECK(npc.body->heading == kStart);
	}
	{
		// (b) The C2S 0x26 process seats the body and turns nothing; another
		// entity's success leaves the local player's look alone.
		Rig r;
		const Npc npc = stage(r);
		LocalPlayer local(r.w);
		r.w.local_player_state = &local;
		local.input.look_heading = 0x0F000000;
		CHECK(r.w.vehicles.process_attach(npc.handle, r.veh_h, 2));
		CHECK(r.w.registry.get(npc.handle)->mount_target == r.veh_h);
		CHECK(npc.body->heading == kStart);
		CHECK(local.input.look_heading == 0x0F000000);
		r.w.local_player_state = nullptr;
	}
	{
		// (c) A client's decoded relation seats its own player and turns
		// nothing, but the process's success copies the player's Yaw into its
		// look: a look the Yaw does not carry is overwritten, in the input and
		// the body's look target alike (D-NET-447).
		// [orig: Entity_ProcessVehicleAttach -- the local gate @0x435c4c..0x435c52,
		//  `mov ecx, [edi+10h]` @0x435c5e, g_LocalPlayerLookYaw @0x435c61]
		constexpr int32_t kStale = 0x0F000000;
		Rig r;
		AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
		body.inf.active = true;
		body.inf.is_local_player = true;
		body.health = 150;
		body.heading = kStart;
		body.inf.target_heading = kStale;
		LocalPlayer local(r.w);
		r.w.local_player_state = &local;
		local.input.look_heading = kStale;
		r.veh().seats[1].yaw_offset = 30;
		CHECK(r.w.vehicles.apply_confirmed_mount(r.player_h, r.veh_h, 2));
		CHECK(r.player().mount_target == r.veh_h);
		CHECK(body.heading == kStart);
		CHECK(body.inf.target_heading == kStart);
		CHECK(local.input.look_heading == kStart);
		r.w.local_player_state = nullptr;
	}
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
        vehicle.has_item_def = true; // seats need the def (D-NET-422)
        vehicle.has_graphic_model = true;
        // Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
        vehicle.item_attrib = kItemAttribPlayerControl;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.heading = placed_heading_bam(0);
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
        w.registry.get(player_h)->heading = placed_heading_bam(45);

        CHECK(ai.pose_if_mounted(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->heading == placed_heading_bam(45));
        CHECK(body.inf.body_heading == 0x40000000); // yaw 0: the spawn form of 90

        Entity *live_vehicle = w.registry.get(vehicle_h);
        live_vehicle->position = {40.0f, 50.0f, 60.0f};
        live_vehicle->heading = placed_heading_bam(20);
        CHECK(ai.refresh_mounted_pose(body, w));
        CHECK(body.heading == look_heading);
        CHECK(body.pitch == look_pitch);
        CHECK(w.registry.get(player_h)->heading == placed_heading_bam(45));
        CHECK(body.inf.body_heading == 835125248); // ((70 << 16) / 360) << 16
        const Entity *mounted = w.registry.get(player_h);
        CHECK(body.self->position.x.q16 == to_fixed(mounted->position.x));
        CHECK(body.self->position.y.q16 == to_fixed(mounted->position.y));
        CHECK(body.self->position.z.q16 == to_fixed(mounted->position.z));
    }
}

// A remote player in a passenger seat keeps its wire look in its Yaw word and
// its motor heading while the seat owns the body frame: the organic seat carry
// writes the bone yaw into bodyHeading and the legs alone, and the remote chase
// owns +0x10 (FOUND V2). [orig: Entity_UpdateInfantryPlayerBody - the remote chase
//  @0x4B4361, the seat carry @0x4B6510..0x4B6575 (+0x8C / +0x2E8 / +0x2E4
//  @0x4B654E..0x4B655A, no +0x10)]
void test_remote_passenger_keeps_its_wire_look() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    AiSystem &ai = w.ai;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);

    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.has_item_def = true; // seats need the def (D-NET-422)
    vehicle.has_graphic_model = true;
    vehicle.position = {10.0f, 20.0f, 30.0f};
    vehicle.heading = placed_heading_bam(0);
    vehicle.health = 100;
    vehicle.alive = true;
    Seat seat;
    seat.type = SeatType::Passenger;
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
    w.registry.get(player_h)->heading = placed_heading_bam(45);

    CHECK(ai.pose_if_mounted(body, w));
    CHECK(body.heading == look_heading);
    CHECK(body.pitch == look_pitch);
    CHECK(w.registry.get(player_h)->heading == placed_heading_bam(45));
    CHECK(body.inf.body_heading == 0x40000000); // yaw 0: the spawn form of 90

    Entity *live_vehicle = w.registry.get(vehicle_h);
    live_vehicle->heading = placed_heading_bam(20);
    CHECK(ai.refresh_mounted_pose(body, w));
    CHECK(body.heading == look_heading);
    CHECK(body.pitch == look_pitch);
    CHECK(w.registry.get(player_h)->heading == placed_heading_bam(45));
    CHECK(body.inf.body_heading == 835125248); // ((70 << 16) / 360) << 16
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
        vehicle.has_item_def = true; // seats need the def (D-NET-422)
        vehicle.has_graphic_model = true;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.heading = placed_heading_bam(90);
        vehicle.pitch_bam = spawn_angle_bam(4);
        vehicle.roll_bam = spawn_angle_bam(5);
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
        // A bodiless organic keeps its own Pitch word, 0.
        check_pose(*w.registry.get(occupant_h), organic_words(provider.pose, 0));
    }

    // The shared attach path consumes the same seam, then AI tick follows a new
    // live position and carries the full orientation into its body frame.
    {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        AiSystem &ai = w.ai;
        ai.root_motion = &channel_only; // the occupant holds its channel
        FakeMountedPoseProvider provider;
        provider.pose = pose_degrees({11.0f, 12.0f, 13.0f}, 21, -8, 9);
        w.pose_provider = &provider;

        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.has_item_def = true; // seats need the def (D-NET-422)
        vehicle.has_graphic_model = true;
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
        // The attach stores no Position: the bodied occupant keeps its words
        // until its motor's seat block poses it on the next pass (below); ours'
        // attach-edge pose gives it the seat's Roll word, and its Yaw and Pitch
        // words stay its own (the attach writes no angle; an organic's seat carry
        // writes no Yaw word, FOUND V2). [orig: Entity_AttachToVehicleSlot
        // @0x4946D0 - the stores @0x494752..0x494775; Entity_UpdateInfantryPlayerBody
        // - the seat carry @0x4B6510..0x4B6575]
        {
            const Entity &attached = *w.registry.get(occupant_h);
            CHECK(attached.position.x.q16 == 0 && attached.position.y.q16 == 0 &&
                  attached.position.z.q16 == 0);
            CHECK(attached.heading == Entity{}.heading); // the occupant's own word
            CHECK(attached.pitch_bam == 0); // the occupant's own word (D-NET-923)
            CHECK(attached.roll_bam == provider.pose.roll);
        }

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
        // The look's Pitch word, 0, while the body pitch takes the seat's 15.
        check_pose(*mounted, organic_words(expected, 0));
        CHECK(body->heading == 0 && !body->inf.aim_valid);
        CHECK(body->self->position.x.q16 == to_fixed(31.5));
        CHECK(body->self->position.y.q16 == to_fixed(32.25));
        CHECK(body->self->position.z.q16 == to_fixed(33.75));
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
        vehicle.has_item_def = true; // seats need the def (D-NET-422)
        vehicle.has_graphic_model = true;
        vehicle.heading = placed_heading_bam(90);
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
        CHECK(body->self->position.x.q16 == to_fixed(2.0));
        CHECK(body->self->position.y.q16 == to_fixed(1.0));
        CHECK(mounted->heading == kChasedHeading); // the Yaw word is the independent chased look, not the provider body yaw 12
        // The Pitch word is the chased look the attach restores; the seat bone's
        // 21-degree pitch is the body pitch's, and its Roll word as delivered is
        // the occupant's (D-NET-923). [orig: Entity_AttachToBoneAndUpdateTransform
        //  @0x5463D0 - the bone Roll @0x546632, the Pitch word restored @0x546664;
        //  Entity_UpdateInfantryAI's look chase @0x4BEF97]
        CHECK(mounted->pitch_bam == kChasedPitch);
        CHECK(mounted->roll_bam == bam_from_degrees_wrapped(-30.0));
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
        vehicle.heading = placed_heading_bam(90);
        vehicle.pitch_bam = spawn_angle_bam(4);
        vehicle.roll_bam = spawn_angle_bam(5);
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.seat_local = {2.0f, 3.0f, 4.0f};
        seat.yaw_offset = 10;
        // The full-frame fallback (yaw 90, pitch 4, roll 5 over seat_local
        // {2,3,4}): Rz(90-yaw)Ry(-pitch)Rx(roll) on the un-swizzled local —
        // was {13, 18, 34} while the fallback was yaw-only (the pre-§6.13
        // stand-in). [orig: @0x4b0c50 over @0x613f40]
        MountedPose expected = pose_degrees({12.726887f, 17.658988f, 34.010455f}, 100, 4, 5);
        // A model-less carrier has no retail counterpart (the bone transform
        // returns 0 without a graphic model @0x4B0C66..0x4B0C74): ours' root
        // frame composes no bone turn, so the seat's Roll word is the carrier's
        // own, the spawn form of 5 (0x038E0000), and the organic's Pitch word
        // stays its own, 0 (D-NET-923). [orig: Entity_SpawnFromBMSRecord
        //  @0x40EB89..0x40EBA6]
        expected.pitch = 0;
        expected.roll = vehicle.roll_bam;
        CHECK(expected.roll == 0x038E0000);

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

// An unarmed player's UseGun attach is refused outside a session: single
// player, the in-process listen server included, is outside it; in a session
// the same USE mounts the gun. The refusal is the attach's, which the
// authority's process calls with forceAttach 0 after the request turned the
// body and after the process left the seat the body rode: a refused USE has
// turned the body, and a seated passenger who picks the UseGun ends unmounted.
// [orig: Entity_RequestVehicleAttach @0x43655F..0x4365CE (the turn);
//  Entity_ProcessVehicleAttach -- the detach @0x435bce, forceAttach 0
//  @0x435C9C; Entity_AttachToUseGunSlot @0x546b80 -- `cmp
//  g_NapiNPCtx.is_in_session` @0x546BF6, the player bit @0x546BFE, the
//  EquippedSlot test @0x546C07; SinglePlayer_StartMission @0x561AF0 leaves
//  is_in_session clear]
// The rig's seat 0 as a UseGun carrying WPN_USEGUN, and the player's bit.
void arm_usegun_seat(Rig &r) {
    r.veh().seats[0].type = SeatType::Gunner;
    r.veh().primary_weapon = "WPN_USEGUN";
    r.w.tables.weapons.entries.resize(1);
    r.w.tables.weapons.entries[0].name = "WPN_USEGUN";
    r.w.tables.weapons.entries[0].valid = true;
    r.player().flags |= kEntityFlagPlayer;
    r.player().engine_flags |= kEntityFlagPlayer;
    // The gun's class init binds its slot at the spawn; no mount binds it
    // [orig: WeaponSlot_InitFromEntityDef @0x5466C0].
    r.w.vehicles.init_weapon_slot_from_entity_def(r.veh());
}

// Arms the local player the way the boot's Player_InitPlayer leg does: the
// spawn kit's select stores the equipped slot, and no shell install has run,
// so the weapon FSM stays inactive. [orig: Player_InitPlayer @0x4E15F0 --
// Player_SelectWeaponSlot @0x4E1995, its EquippedSlot store @0x4DD704]
void arm_at_boot(Rig &r, LocalPlayer &local) {
    WeaponTable &t = r.w.tables.weapons;
    if (t.ammo_class_names.empty()) {
        t.ammo_class_names.emplace_back("");
        t.ammo_class_caps.push_back(0);
    }
    WeaponTableEntry m4;
    m4.name = "WPN_M4AUTO";
    m4.valid = true;
    m4.category = 3;
    m4.clipsize = 30;
    m4.startrounds = 210;
    m4.maxclips = 10;
    m4.ammo_class_count = 1;
    m4.weapon_class_slot = 1;
    m4.ammo_class_id = 0;
    t.entries.push_back(m4);
    local.loadout.spawn_kit() = {WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
    local_loadout_rebuild(r.w, local.loadout, local.weapon, local.inventory,
            local.inventory_valid, /*select_spawn_default=*/true);
}

void test_unarmed_usegun_needs_the_session() {
    constexpr int32_t kStale = 0x0F000000;
    const auto arm_gun = [](Rig &r) { arm_usegun_seat(r); };
    for (const bool mp : {false, true}) {
        Rig r(2.0f);
        arm_gun(r);
        r.w.rules.mp_session = mp;
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        local.input.look_heading = kStale;
        CHECK(!local.equipped_slot_held());
        CHECK(local.toggle_mount() == mp);
        CHECK(r.player().mounted == mp);
        // The request turned the look to the gun either way.
        CHECK(local.input.look_heading != kStale);
        r.w.local_player_state = nullptr;
    }
    {
        // Seated as a passenger, the unarmed player picks the UseGun: the
        // process leaves the passenger seat, then the attach refuses.
        Rig r(2.0f);
        arm_gun(r);
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        CHECK(r.w.vehicles.request_attach(r.player_h, {r.veh_h, 1, SeatType::Passenger}));
        CHECK(r.player().mounted && r.player().mount_seat == 1);
        CHECK(r.w.vehicles.request_attach(r.player_h, {r.veh_h, 0, SeatType::Gunner}));
        CHECK(!r.player().mounted);
        CHECK(!r.veh().seats[0].occupant.valid() && !r.veh().seats[1].occupant.valid());
        // Armed, the same pick seats the gunner.
        arm_at_boot(r, local);
        CHECK(local.equipped_slot_held() && !local.weapon.active);
        CHECK(r.w.vehicles.request_attach(r.player_h, {r.veh_h, 0, SeatType::Gunner}));
        CHECK(r.player().mounted && r.player().mount_seat == 0);
        r.w.local_player_state = nullptr;
    }
}

// A UseGun round trip leaves the personal bucket rifle reading its own bucket: the
// pump that stages the mount reads the rifle's bucket through the gunner seat (the
// gun's clip), and the switch back reads it again off the seat the player no longer
// holds, as every retail reader of a bucket weapon's rounds reads through the entity's
// seat at the moment it reads (the GUT's simulation_test
// test_local_usegun_switches_viewmodel_and_borrows_parent_weapon_slot).
// [orig: WeaponSlot_GetSharedLoadedRounds @0x5405F0 - seat 3 the carrier's clip @0x540611..0x54061A, else
//  the entity's bucket @0x540657; HUD_BuildEntityInfo @0x4B85C7..0x4B85E1;
//  WeaponSlot_CanFire @0x541C79]
void test_usegun_round_trip_reads_the_personal_bucket() {
    Rig r(2.0f);
    arm_usegun_seat(r);
    WeaponTable &t = r.w.tables.weapons;
    t.ammo_class_names = {"CLASS_70MM", "CLASS_556MM"};
    t.ammo_class_caps = {100, 1000};
    const auto bake_switches = [](WeaponFsmDef &d) {
        for (int a = 0; a < weapon_action::kCount; ++a) d.actions[a].id = a;
        d.actions[weapon_action::kSwitchFrom].delay_start = 2;
        d.actions[weapon_action::kSwitchFrom].delay_end = 2;
        d.actions[weapon_action::kSwitchTo].delay_start = 2;
        d.actions[weapon_action::kSwitchTo].delay_end = 2;
    };
    WeaponTableEntry &gun = t.entries[0];
    gun.category = 9;
    gun.clipsize = 8;
    gun.startrounds = 8;
    gun.flags = weapon_flag::kEmplaced;
    gun.ammo_class_count = 1;
    gun.ammo_class_id = 0;
    bake_switches(gun.action_fsm);
    WeaponTableEntry m4;
    m4.name = "WPN_M4AUTO";
    m4.valid = true;
    m4.category = 3;
    m4.clipsize = 30;
    m4.startrounds = 300;
    m4.maxclips = 10;
    m4.ammo_class_count = 1;
    m4.weapon_class_slot = 1;
    m4.ammo_class_id = 1;
    m4.ammo_bucket = 1;
    bake_switches(m4.action_fsm);
    t.entries.push_back(m4);
    LocalPlayer local(r.w);
    r.w.local_player_state = &local;
    local.loadout.spawn_kit() = {WeaponKitEntry{"WPN_M4AUTO", -1, -1, -1}};
    local_loadout_rebuild(r.w, local.loadout, local.weapon, local.inventory,
            local.inventory_valid, /*select_spawn_default=*/true);
    r.veh().item_attrib |= kItemAttribEweap; // the UseGun's own slot
    CHECK(r.w.vehicles.init_weapon_slot_from_entity_def(r.veh()));
    local.inventory.shared_clips[1] = 30;
    // The shell's installs, as the switch events ask for them.
    const auto install = [&](const char *name, int32_t clipsize, bool preserve) {
        WeaponInstallData data;
        data.name = name;
        data.clipsize = clipsize;
        data.table_baked = true;
        local_weapon_install(r.w, local.weapon, data, preserve, false, &local.inventory,
                local.view);
    };
    install("WPN_M4AUTO", 30, false);
    const auto pump = [&]() {
        ++r.w.logic_tick;
        local.pump_local_weapon();
    };
    for (int i = 0; i < 8; ++i) pump();
    CHECK(active_local_weapon_slot(r.w, local.weapon)->clip == 30);

    CHECK(local.toggle_mount());
    CHECK(r.player().mount_type == SeatType::Gunner && r.player().use_gun_slot_swapped);
    for (int i = 0; i < 8 && !local.weapon.usegun_slot_active; ++i) pump();
    CHECK(local.weapon.usegun_slot_active);
    install("WPN_USEGUN", 8, true);
    CHECK(active_local_weapon_slot(r.w, local.weapon)->clip == 8);
    for (int i = 0; i < 8; ++i) pump();

    CHECK(local.toggle_mount());
    CHECK(!r.player().mounted);
    for (int i = 0; i < 8 && local.weapon.usegun_slot_active; ++i) pump();
    CHECK(!local.weapon.usegun_slot_active);
    install("WPN_M4AUTO", 30, true);
    CHECK(active_local_weapon_slot(r.w, local.weapon)->clip == 30);
    CHECK(local.inventory.shared_clips[1] == 30);
    r.w.local_player_state = nullptr;
}

// The gate reads the local player's EquippedSlot as it stands, not the shell's
// weapon install: a single player whose spawn select equipped a slot at the
// boot, ahead of the mission start's WAC pass and of any shell install, is
// admitted to a UseGun by the script mount; an unarmed one is refused; and an
// unarmed one still holding a borrowed carrier slot (a seat-2/3 detach keeps
// it until the queued switch back commits) is admitted.
// [orig: Player_InitPlayer, the call Game_StartMission @0x525BBC ->
//  Player_SelectWeaponSlot @0x4E1995 (EquippedSlot @0x4DD704), ahead of
//  WacScript_InitAndLoad @0x525CB3; WacScript_TryMountEntityToVehicle @0x4F7186;
//  Entity_AttachToUseGunSlot @0x546C07; Entity_DetachFromVehicle @0x43565F]
void test_script_mount_reads_the_boot_equipped_slot() {
    for (int arm = 0; arm < 3; ++arm) {
        Rig r(2.0f);
        arm_usegun_seat(r);
        r.veh().seats.resize(1); // the UseGun alone
        // A script names the player body by its DcbId, 10000 + its slot byte 0
        // (entity_dcb_id, D-NET-899), never by its record word.
        r.player().net_id = 55;
        r.player().item_type_index = 9;
        AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
        body.inf.active = true;
        body.health = 150;
        body.slot.f[36] = int32_t(r.veh_h.packed) + 1;
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        if (arm == 1) arm_at_boot(r, local);
        if (arm == 2) local.weapon.usegun_slot_active = true;
        CHECK(!local.weapon.active);
        CHECK(local.equipped_slot_held() == (arm != 0));
        CHECK(r.w.commands.use_boarding_target(10000) == (arm != 0));
        CHECK(r.player().mounted == (arm != 0));
        r.w.local_player_state = nullptr;
    }
}

// The binoculars toggle survives a mount change: only the camera reset clears
// it, which the local player's ctrlx / drvrx attach and its UseGun attach run,
// the UseGun's at the attach whatever the gun's slot. A sitex attach, a detach
// and a designated-G route switch keep the toggle raised (D-WPN-49).
// [orig: g_BinocularsToggle's writers -- the reset's @0x4DE2B3, whose only
//  callers are Entity_AttachToVehicleSlot @0x4947ae / @0x4948b4 / @0x494959,
//  Player_SwitchToWeaponByHandle @0x4e01fc and Entity_AttachToUseGunSlot
//  @0x546ba4; none in Entity_DetachFromVehicle @0x4355F0 or
//  Entity_ProcessVehicleAttach @0x435AA0; the route leg stores EquippedSlot
//  alone, NetPacket_SerializePlayerState @0x4c139d / @0x4c13f8]
void test_binoculars_survive_all_but_the_camera_reset() {
    {
        // A sitex attach, then a detach: the toggle stays raised.
        Rig r(-2.5f);
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        look_at(r.w, r.player_h, entity_local_point_world(r.veh(), r.veh().seats[1].seat_local));
        local.view.binoculars_requested = true;
        CHECK(local.toggle_mount());
        CHECK(r.player().mounted && r.player().mount_type == SeatType::Passenger);
        CHECK(local.view.binoculars_requested);
        // Seated, looking away from the ctrlx seat: no candidate, so the USE detaches.
        const Vec3 at = r.player().position;
        const Vec3 ctrl = entity_local_point_world(r.veh(), r.veh().seats[0].seat_local);
        look_at(r.w, r.player_h, {2 * at.x - ctrl.x, 2 * at.y - ctrl.y, at.z});
        CHECK(local.toggle_mount());
        CHECK(!r.player().mounted);
        CHECK(local.view.binoculars_requested);
        r.w.local_player_state = nullptr;
    }
    {
        // A ctrlx attach resets the camera, the toggle with it.
        Rig r(2.0f);
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        local.view.binoculars_requested = true;
        CHECK(local.toggle_mount());
        CHECK(r.player().mount_type == SeatType::Controller);
        CHECK(!local.view.binoculars_requested);
        r.w.local_player_state = nullptr;
    }
    {
        // A UseGun attach resets the camera at the attach, even on a gun whose
        // slot the weapon staging cannot resolve (a carrier without EWeap).
        Rig r(2.0f);
        arm_usegun_seat(r);
        r.w.rules.mp_session = true; // an unarmed player boards in a session
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        local.view.binoculars_requested = true;
        local.weapon.nvg_scope_restore = true;
        r.w.weather.core.scalar_channels.camera_fov_target_fp = 20 << 16;
        CHECK(local.toggle_mount());
        CHECK(r.player().mount_type == SeatType::Gunner);
        CHECK(!local.view.binoculars_requested && !local.weapon.nvg_scope_restore);
        CHECK(r.w.weather.core.scalar_channels.camera_fov_target_fp == (80 << 16));
        r.w.local_player_state = nullptr;
    }
    {
        // On a designated-G gun, a route switch to the carrier's slot resets nothing.
        Rig r(2.0f);
        arm_usegun_seat(r);
        r.w.rules.mp_session = true;
        WeaponTableEntry hull;
        hull.name = "WPN_HULL";
        hull.valid = true;
        r.w.tables.weapons.entries.push_back(hull);
        Entity carrier;
        carrier.kind = EntityKind::Item;
        carrier.has_item_def = true;
        carrier.has_graphic_model = true;
        carrier.item_type = 1;
        carrier.item_attrib = kItemAttribEweap;
        carrier.primary_weapon = "WPN_HULL";
        carrier.position = {200.0f, 200.0f, 10.0f};
        const EntityHandle carrier_h = r.w.registry.spawn(1, carrier);
        CHECK(r.w.vehicles.init_weapon_slot_from_entity_def(*r.w.registry.get(carrier_h)));
        // An emplaced gun's def, as the shipped ones: EWeap without PlayerControl, which
        // would keep the gun on its own slot whatever the route bit [orig:
        // Entity_GetWeaponSlots @0x5460E0 - `test al, 40h` @0x5460F6].
        r.veh().item_type = 2;
        r.veh().item_attrib = kItemAttribEweap;
        r.veh().emplacement_attachment_flags = 0x02;
        r.veh().emplacement_parent = carrier_h;
        r.veh().emplacement_parent_spawn_id = r.w.registry.get(carrier_h)->registry_spawn_id;
        r.veh().ground_target = carrier_h;
        LocalPlayer local(r.w);
        r.w.local_player_state = &local;
        CHECK(local.toggle_mount());
        CHECK(r.player().mount_type == SeatType::Gunner && r.player().use_gun_slot_swapped);
        local.view.binoculars_requested = true;
        local.weapon.nvg_scope_restore = true;
        r.w.weather.core.scalar_channels.camera_fov_target_fp = 20 << 16;
        MountSlotSelectRequest req;
        req.applies = true;
        req.use_parent_slot = true;
        req.mount = r.veh_h;
        req.parent = carrier_h;
        // The toggle moves the route bit only, never the AdmDef byte +0x2B0
        // [orig: NapiNPServerMsg_HandleWeaponToggle @0x511B2C / @0x511B38].
        const uint8_t adm_before = r.player().equipped_adm_index;
        const size_t events_before = local.weapon.events.size();
        local_player_apply_mount_slot_select(r.w, local.weapon, req);
        CHECK(r.veh().primary_weapon_slot.redirect_to_parent_slot);
        CHECK(r.player().equipped_adm_index == adm_before);
        // The route switch stores the EquippedSlot and switches nothing: no switch
        // staged or queued, the borrow names the carrier's slot and def at once, and
        // the first-person model follows its def (D-WPN-52) [orig:
        // NapiNPServerMsg_HandleWeaponToggle @0x511B2C..0x511B38;
        // WeaponAction_ProcessAllEntities @0x5426AD; Player_RenderFirstPersonViewModel
        // @0x4DED60].
        CHECK(local.weapon.usegun_switch == LocalUseGunSwitch::kNone);
        CHECK(local.weapon.usegun_switch_action == -1);
        CHECK(local.weapon.usegun_slot_active && local.weapon.usegun_mount == r.veh_h);
        CHECK(local.weapon.usegun_weapon_adm ==
                r.w.registry.get(carrier_h)->primary_weapon_slot_adm);
        CHECK(local.weapon.events.size() == events_before + 1 &&
                local.weapon.events.back().switch_to_weapon == "WPN_HULL");
        // A fire on the route names the carrier slot's def, for the round's adm and the
        // C2S 0x06 off7 alike, while the AdmDef byte keeps its word (this unarmed
        // player's attach took the direct leg, which stamps none: D-WPN-52).
        // [orig: WeaponAction_Fire @0x542B10 - the slot's def @0x542C1C..0x542C2F, the
        //  +0x2B0 fallback for a def-less slot @0x542C35..0x542C3F, pushed @0x542C55]
        const uint8_t hull_adm =
                static_cast<uint8_t>(r.w.tables.weapons.index_of("WPN_HULL"));
        CHECK(local_weapon_fire_adm(r.w, local.weapon, r.player()) == hull_adm);
        CHECK(r.player().equipped_adm_index != hull_adm);
        CHECK(local.view.binoculars_requested && local.weapon.nvg_scope_restore);
        CHECK(r.w.weather.core.scalar_channels.camera_fov_target_fp == (20 << 16));
        r.w.local_player_state = nullptr;
    }
}

// Action 6 on L's parent (D-NET-860): a mountable gun with the G capability owns the
// action; the route bit set selects its own slot, clear the groundEntity's when that is a
// type-1 EWeap, else the action ends with nothing sent and no scope toggle. A parent without
// the G capability falls to the scope toggle. Neither the UseGun slot swap nor the addeweap
// parent identity is read. [orig: Input_HandleActionBinding_0 case 6 - L's parent @0x4E04A0,
// @0x4E04AC, +0x326 & 2 @0x4E04B5 (the scope toggle @0x4E0528), the route bit @0x4E04BE, the
// groundEntity gates @0x4E04FC..0x4E051F (the end @0x4E13F5), the C2S 0x16 @0x4E04F0]
void test_action6_reads_ls_parent_with_retails_gates() {
    Rig r(2.0f);
    arm_usegun_seat(r);
    r.w.rules.mp_session = true;
    WeaponTableEntry hull;
    hull.name = "WPN_HULL";
    hull.valid = true;
    r.w.tables.weapons.entries.push_back(hull);
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.has_item_def = true;
    carrier.has_graphic_model = true;
    carrier.item_type = 1;
    carrier.item_attrib = kItemAttribEweap;
    carrier.primary_weapon = "WPN_HULL";
    carrier.position = {200.0f, 200.0f, 10.0f};
    const EntityHandle carrier_h = r.w.registry.spawn(1, carrier);
    r.veh().item_type = 2;
    r.veh().item_attrib |= kItemAttribEweap;
    r.veh().emplacement_attachment_flags = 0x02;
    r.veh().ground_target = carrier_h; // no addeweap metadata: the identity is no gate
    LocalPlayer local(r.w);
    r.w.local_player_state = &local;
    CHECK(local.toggle_mount());
    CHECK(r.player().mounted && r.player().mount_target == r.veh_h);
    local.weapon.active = true;
    r.player().use_gun_slot_swapped = false; // not read

    MountSlotSelectRequest req;
    // The route bit clear: the groundEntity's slot.
    r.veh().primary_weapon_slot.redirect_to_parent_slot = false;
    CHECK(local_player_mount_slot_select(r.w, local.weapon, req));
    CHECK(req.applies && req.use_parent_slot && req.mount == r.veh_h && req.parent == carrier_h);
    // The route bit set: the gun's own slot.
    r.veh().primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(local_player_mount_slot_select(r.w, local.weapon, req));
    CHECK(req.applies && !req.use_parent_slot && req.mount == r.veh_h);
    // The bit clear onto a groundEntity that is no type-1 EWeap: the gun keeps the action,
    // nothing is selected and no scope toggle follows.
    r.veh().primary_weapon_slot.redirect_to_parent_slot = false;
    r.w.registry.get(carrier_h)->item_type = 3;
    CHECK(local_player_mount_slot_select(r.w, local.weapon, req));
    CHECK(!req.applies);
    r.veh().ground_target = EntityHandle{};
    CHECK(local_player_mount_slot_select(r.w, local.weapon, req));
    CHECK(!req.applies);
    // A parent without the G capability falls to the scope toggle.
    r.veh().emplacement_attachment_flags = 0;
    CHECK(!local_player_mount_slot_select(r.w, local.weapon, req));
    CHECK(!req.applies);
    r.w.local_player_state = nullptr;
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
    root.has_item_def = true; // seats need the def (D-NET-422)
    root.has_graphic_model = true;
    root.health = 100;
    root.alive = true;
    Seat passenger;
    passenger.type = SeatType::Passenger;
    passenger.bone_index = 3;
    root.seats.push_back(passenger);
    const EntityHandle root_h = w.registry.spawn(1, root);

    Entity child;
    child.kind = EntityKind::Item;
    child.has_item_def = true; // seats need the def (D-NET-422)
    child.has_graphic_model = true;
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
    // walk to passenger seats without changing root/child weighting.
    w.registry.get(root_h)->seats[1].occupant = EntityHandle::make(0, 3);
    selected = {};
    CHECK(find_best_vehicle_seat(
            w, root_h, player_h, selected,
            SeatSelectionMode::PassengerOnly));
    CHECK(selected.vehicle == root_h);
    CHECK(selected.seat_index == 0);
}

// The root's proximity slice bounds the seat search; equal-weight child guns
// precede the root, and the live ground link is rechecked without rebuilding.
// [orig: Entity_FindBestSeatSlot @ 0x4351F0, slice/root/filter/tie @ 0x43521E / @ 0x435228 / @ 0x435289 / @ 0x4352BC / @ 0x435417]
void test_best_seat_uses_proximity_order() {
    World w;
    CollisionWorld collision;
    w.collision = &collision;
    w.registry.configure_pool(1, 9600); // revx02 allocation, only four live rows
    Entity root;
    root.kind = EntityKind::Item;
    root.has_item_def = root.has_graphic_model = true;
    root.item_type_index = 1;
    root.item_type = 1;
    Seat gun;
    gun.type = SeatType::Gunner;
    gun.bone_index = 1;
    root.seats.push_back(gun);
    const EntityHandle rh = w.registry.spawn(1, root);
    Entity child = root;
    child.ground_target = rh;
    child.position = {1000.0f, 0.0f, 0.0f};
    const EntityHandle distant = w.registry.spawn(1, child);
    child.position = EntityPosition{}; // a bare {} is ambiguous to GCC (Vec3 / EntityPosition)
    const EntityHandle near = w.registry.spawn(1, child);
    const EntityHandle second = w.registry.spawn(1, child);
    collision.build_initial_tables(w);
    VehicleSeatSelection selected;
    CHECK(find_best_vehicle_seat(w, rh, {}, selected));
    CHECK(selected.vehicle == near); // distant child excluded; root loses equal weight
    w.registry.get(near)->ground_target = {};
    CHECK(find_best_vehicle_seat(w, rh, {}, selected));
    CHECK(selected.vehicle == second); // list retained, live link changed
    w.registry.get(second)->ground_target = {};
    CHECK(find_best_vehicle_seat(w, rh, {}, selected));
    CHECK(selected.vehicle == rh);
    collision.forget_candidate_slices();
    CHECK(find_best_vehicle_seat(w, rh, {}, selected));
    CHECK(selected.vehicle == rh); // authoritative missing slice does not scan all pools
    CHECK(w.registry.get(distant)->ground_target == rh);
}

// D-NET-557 / D-NET-580: the walk's per-entry gate, the entry admission and the
// attach's carrier gate read the one Flags dword's dead bit (either portable
// view) and the def, never Health: an entry at Health <= 0 whose dead bit is
// still clear (the window before its class edge) offers its seats and takes a
// rider, and a dead bit held in engine_flags alone refuses them.
// [orig: Entity_FindBestSeatSlot @0x4352C2..0x4352CD, @0x4352D3..0x4352D7;
//  Entity_CanEnterVehicle @0x435485, @0x435492; Entity_ProcessVehicleAttach @0x435AF9]
void test_seat_gates_read_the_dead_bit_alone() {
    Rig r;
    r.veh().spawn_position = r.veh().position; // the admission's spawn arm
    VehicleSeatSelection selected;
    r.veh().health = 0;
    r.veh().alive = false;
    CHECK(find_best_vehicle_seat(r.w, r.veh_h, r.player_h, selected));
    CHECK(selected.vehicle == r.veh_h);
    CHECK(vehicle_can_enter(r.w, nullptr, r.veh()));
    // The attach's carrier gate is the same dead bit [orig: Entity_ProcessVehicleAttach @0x435AF9].
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
    CHECK(r.w.vehicles.detach(r.player_h));
    // So is the 0x0A apply's confirmed mount [orig: Entity_TryAttachOrDetach @0x4366E8].
    CHECK(r.w.vehicles.apply_confirmed_mount(r.player_h, r.veh_h, 2));
    CHECK(r.player().mounted && r.player().mount_target == r.veh_h);
    CHECK(r.w.vehicles.detach(r.player_h));
    r.veh().health = 2000;
    r.veh().alive = true;
    r.veh().engine_flags |= kEntityFlagDead;
    CHECK(!find_best_vehicle_seat(r.w, r.veh_h, r.player_h, selected));
    CHECK(!vehicle_can_enter(r.w, nullptr, r.veh()));
    CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
    (void)r.w.vehicles.detach(r.player_h); // whatever the line above did
    CHECK(!r.w.vehicles.apply_confirmed_mount(r.player_h, r.veh_h, 2));
    CHECK(!r.player().mounted);
    // A child carries its own gate: a zero-health child still offers its seat
    // beside a root whose own seats are full.
    r.veh().engine_flags &= ~kEntityFlagDead;
    r.veh().seats[0].occupant = EntityHandle::make(0, 9);
    r.veh().seats[1].occupant = EntityHandle::make(0, 10);
    Entity child;
    child.kind = EntityKind::Item;
    child.has_item_def = true;
    child.has_graphic_model = true;
    child.health = 0;
    child.alive = false;
    child.ground_target = r.veh_h;
    Seat gunner;
    gunner.type = SeatType::Gunner;
    gunner.bone_index = 5;
    child.seats.push_back(gunner);
    const EntityHandle child_h = r.w.registry.spawn(1, child);
    CHECK(find_best_vehicle_seat(r.w, r.veh_h, r.player_h, selected));
    CHECK(selected.vehicle == child_h);
    r.w.registry.get(child_h)->engine_flags |= kEntityFlagDead;
    CHECK(!find_best_vehicle_seat(r.w, r.veh_h, r.player_h, selected));
}

// The USE scan rejects a candidate on its dead bit alone (either view), never its
// Health or the alive latch, and a type-6 child's PlayerControl carrier the same
// way (D-NET-433's scan site). [orig: Entity_FindNearestSeatOrArmory @0x435D50 - the
// candidate @0x435E2B, the carrier @0x435E8B; HUD_DrawVehicleSeatAndArmoryLabels
// @0x5A336F / @0x5A33AF]
void test_use_scan_reads_the_dead_bit_alone() {
    {
        Rig r;
        VehicleSeatSelection hit;
        r.veh().health = 0;
        r.veh().alive = false;
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        CHECK(hit.vehicle == r.veh_h);
        r.veh().health = 2000;
        r.veh().alive = true;
        r.veh().engine_flags |= kEntityFlagDead;
        CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        r.veh().engine_flags &= ~kEntityFlagDead;
        r.veh().flags |= kEntityFlagDead;
        CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    }
    {
        // A type-6 child on the carrier: the carrier's dead bit rejects it, its
        // Health does not.
        Rig r;
        r.veh().seats.clear(); // the child's seat is the only one
        Entity child;
        child.kind = EntityKind::Item;
        child.has_item_def = true;
        child.has_graphic_model = true;
        child.item_type = 6;
        child.health = 100;
        child.alive = true;
        child.position = r.veh().position;
        child.ground_target = r.veh_h;
        Seat gunner;
        gunner.type = SeatType::Gunner;
        gunner.bone_index = 5;
        gunner.seat_local = {0.5f, 1.5f, 1.0f}; // where the rig's ctrlx seat sat
        child.seats.push_back(gunner);
        const EntityHandle child_h = r.w.registry.spawn(1, child);
        VehicleSeatSelection hit;
        r.veh().health = 0;
        r.veh().alive = false;
        CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
        CHECK(hit.vehicle == child_h);
        r.veh().engine_flags |= kEntityFlagDead;
        CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    }
}

// The label pass's carrier block has no type test, where the scan's asks only of
// a type-6 candidate on a PlayerControl carrier: a vehicle parked on a destroyed
// PlayerControl carrier keeps its seat in the scan and loses its labels, and an
// EWeap gun on a vehicle carrier without PlayerControl is hidden by an enemy aboard
// that carrier, which the scan does not ask. A candidate's own enemy occupant is
// asked of a vehicle (def type 1) alone.
// [orig: Entity_FindNearestSeatOrArmory @0x435E58..0x435E6C, @0x435E72..0x435E97;
//  HUD_DrawVehicleSeatAndArmoryLabels @0x5A3379..0x5A3395, @0x5A339B..0x5A33D9]
void test_label_pass_carrier_block_has_no_type_test() {
	const auto labels_of = [](Rig &r) {
		std::vector<AttachLabel> labels;
		r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
		return labels;
	};
	{
		Rig r;
		r.veh().seats.clear(); // the parked vehicle's seat is the only one
		Entity ride;
		ride.kind = EntityKind::Item;
		ride.has_item_def = true;
		ride.has_graphic_model = true;
		ride.item_type = 1;
		ride.health = 500;
		ride.alive = true;
		ride.position = r.veh().position;
		ride.ground_target = r.veh_h;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 2;
		sit.seat_local = {0.5f, 1.5f, 1.0f}; // where the rig's ctrlx seat sat
		ride.seats.push_back(sit);
		const EntityHandle ride_h = r.w.registry.spawn(1, ride);
		VehicleSeatSelection hit;
		CHECK(labels_of(r).size() == 1);
		r.veh().engine_flags |= kEntityFlagDead;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == ride_h);
		CHECK(labels_of(r).empty());
		// Without PlayerControl the dead carrier hides nothing.
		r.veh().item_attrib = 0;
		CHECK(labels_of(r).size() == 1);
	}
	{
		Rig r;
		r.player().team = 1;
		r.veh().item_type = 1;
		r.veh().item_attrib = 0; // a vehicle carrier without PlayerControl
		Entity gun;
		gun.kind = EntityKind::Item;
		gun.has_item_def = true;
		gun.has_graphic_model = true;
		gun.item_type = 6;
		gun.item_attrib = kItemAttribEweap;
		gun.health = 100;
		gun.alive = true;
		gun.position = r.veh().position;
		gun.ground_target = r.veh_h;
		Seat gunner;
		gunner.type = SeatType::Gunner;
		gunner.bone_index = 5;
		gunner.seat_local = {0.5f, 1.5f, 1.0f};
		gun.seats.push_back(gunner);
		const EntityHandle gun_h = r.w.registry.spawn(1, gun);
		Entity enemy;
		enemy.kind = EntityKind::Organic;
		enemy.has_item_def = true;
		enemy.health = 100;
		enemy.alive = true;
		enemy.team = 2;
		enemy.mounted = true;
		enemy.mount_target = r.veh_h;
		enemy.mount_type = SeatType::Passenger;
		const EntityHandle eh = r.w.registry.spawn(0, enemy);
		r.veh().seats[1].occupant = eh;
		VehicleSeatSelection hit;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == gun_h);
		CHECK(labels_of(r).empty());
		r.w.registry.get(eh)->team = 1;
		bool gun_labelled = false;
		for (const AttachLabel &label : labels_of(r))
			gun_labelled = gun_labelled || label.entity == gun_h;
		CHECK(gun_labelled);
	}
	{
		// A two-seat object (def type 6, no EWeap) with an enemy on one seat
		// still offers the other: only a vehicle asks for its enemy occupant.
		Rig r;
		r.player().team = 1;
		r.veh().item_type = 6;
		r.veh().item_attrib = 0;
		r.veh().seats[0].type = SeatType::Passenger;
		Entity enemy;
		enemy.kind = EntityKind::Organic;
		enemy.has_item_def = true;
		enemy.health = 100;
		enemy.alive = true;
		enemy.team = 2;
		enemy.mounted = true;
		enemy.mount_target = r.veh_h;
		enemy.mount_type = SeatType::Passenger;
		const EntityHandle eh = r.w.registry.spawn(0, enemy);
		r.veh().seats[1].occupant = eh;
		VehicleSeatSelection hit;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == r.veh_h && hit.seat_index == 0);
		CHECK(labels_of(r).size() == 1);
		r.veh().item_type = 1;
		CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
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
    vehicle.item_type_index = 7; // the def row's ordinal: the +0x1C slice gate
    vehicle.has_item_def = true; // seats need the def (D-NET-422)
    vehicle.has_graphic_model = true;
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

    // The player def row's ordinal the spawn stamps (the +0x1C slice gate).
    w.tables.player.item_type_index = 3;
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
    other.item_type_index = 7; // the def row's ordinal: the +0x1C slice gate
    other.has_item_def = true; // seats need the def (D-NET-422)
    other.has_graphic_model = true;
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
//  @0x435d9a and their gate @0x43611f; Physics_RaycastAgainstEntityPool ctx[17..20]
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
	r.player().heading = placed_heading_bam(0);
	r.player().pitch_bam = 0;
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
	r.player().heading = placed_heading_bam(90); // facing +x, toward the seat (the aim cone)
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
	gun.has_graphic_model = true;
	gun.item_type = 6;
	gun.item_attrib = kItemAttribEweap;
	gun.ground_target = r.veh_h;
	gun.health = 100;
	gun.alive = true;
	gun.seats.push_back(r.veh().seats[1]);
	const EntityHandle gh = r.w.registry.spawn(1, gun);
	Entity enemy;
	enemy.kind = EntityKind::Organic;
	enemy.has_item_def = true;
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

// A carrier with no ItemDef takes no rider (D-NET-422). The process refuses
// every seat kind of it, as it refuses a dead carrier or requester, before
// anything: a requester riding elsewhere keeps its seat, where a refused
// control seat used to leave it detached. The USE scan, the labels and the
// best-seat search offer none of its seats.
// [orig: Entity_ProcessVehicleAttach @0x435ae5..0x435b01 (the ItemDef
//  @0x435aed..0x435af1), ahead of the detach @0x435bce;
//  Entity_FindNearestSeatOrArmory @0x435e35..0x435e3a;
//  HUD_DrawVehicleSeatAndArmoryLabels @0x5a335a..0x5a335f;
//  Entity_FindBestSeatSlot @0x4352d3..0x4352d7]
void test_carrier_without_a_def_takes_no_rider() {
	{
		Rig r;
		Seat gun;
		gun.type = SeatType::Gunner;
		gun.bone_index = 3;
		gun.source_name = "UseGun";
		gun.seat_local = {0.0f, 0.0f, 2.0f};
		r.veh().seats.push_back(gun);
		Entity ride;
		ride.kind = EntityKind::Item;
		ride.has_item_def = true;
		ride.has_graphic_model = true;
		ride.position = {140.0f, 200.0f, 10.0f};
		ride.health = 500;
		ride.health_max = 500;
		ride.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 2;
		sit.source_name = "sitex00";
		ride.seats.push_back(sit);
		const EntityHandle ride_h = r.w.registry.spawn(1, ride);
		CHECK(r.w.vehicles.process_attach(r.player_h, ride_h, 2));
		const auto still_riding = [&]() {
			const Entity &p = r.player();
			CHECK(p.mounted && p.mount_target == ride_h && p.mount_seat == 0);
			CHECK(r.w.registry.get(ride_h)->seats[0].occupant == r.player_h);
			for (const Seat &s : r.veh().seats) CHECK(!s.occupant.valid());
			CHECK(!r.veh().primary_occupant.valid());
			CHECK(r.w.out.tip_events.empty()); // no boarding, no detach
		};
		r.w.out.tip_events.clear();
		r.veh().has_item_def = false;
		for (const uint8_t bone : {uint8_t(2), uint8_t(3), uint8_t(1)}) { // sitex, UseGun, ctrlx
			CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, bone));
			still_riding();
			CHECK(!r.w.vehicles.apply_confirmed_mount(r.player_h, r.veh_h, bone));
			still_riding();
		}
		// The request paths (the USE scan, the numbered seat, the script mount,
		// the AI board) reach the same process.
		CHECK(!r.w.vehicles.attach_to_seat(r.player_h, {r.veh_h, 1, SeatType::Passenger}));
		still_riding();

		// A dead carrier or a dead requester: Flags & 2 alone refuses.
		r.veh().has_item_def = true;
		r.veh().has_graphic_model = true;
		r.veh().flags |= kEntityFlagDead;
		CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
		still_riding();
		r.veh().flags &= ~kEntityFlagDead;
		r.player().flags |= kEntityFlagDead;
		CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
		still_riding();
		r.player().flags &= ~kEntityFlagDead;
		CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
		CHECK(r.player().mount_target == r.veh_h && r.veh().seats[1].occupant == r.player_h);
		CHECK(!r.w.registry.get(ride_h)->seats[0].occupant.valid());
	}
	{
		// On foot facing +y from +2x, the truck's control seat inside the cone
		// (test_toggle_nearest_seat), and a def-less carrier's sitex seat dead
		// ahead at 1.5 u, which would outscore it: neither the scan nor the
		// labels offer it, and the best-seat search finds no seat on it.
		Rig r(2.0f);
		Entity bare;
		bare.kind = EntityKind::Item;
		bare.position = {102.0f, 201.5f, 10.0f};
		bare.health = 500;
		bare.health_max = 500;
		bare.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		sit.source_name = "sitex00";
		sit.seat_local = {0.0f, 0.0f, -0.1875f}; // eye level once the scan's bias lands
		bare.seats.push_back(sit);
		const EntityHandle bare_h = r.w.registry.spawn(1, bare);
		VehicleSeatSelection hit;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == r.veh_h && hit.seat_index == 0);
		std::vector<AttachLabel> labels;
		r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
		CHECK(labels.size() == 2);
		for (const AttachLabel &l : labels) CHECK(l.entity == r.veh_h);
		CHECK(!find_best_vehicle_seat(r.w, bare_h, r.player_h, hit));
		// With its def the same seat wins the scan.
		r.w.registry.get(bare_h)->has_item_def = true;
		r.w.registry.get(bare_h)->has_graphic_model = true;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == bare_h);
		CHECK(find_best_vehicle_seat(r.w, bare_h, r.player_h, hit));
		r.w.registry.get(bare_h)->has_item_def = false;
		CHECK(r.w.vehicles.player_toggle_mount(r.player_h));
		CHECK(r.player().mount_target == r.veh_h);
	}
	{
		// The best-seat search passes over a def-less child whose UseGun seat
		// outweighs the root's sitex seat (0x20000 against 0x200000).
		World w;
		w.registry.configure_pool(0, 4);
		w.registry.configure_pool(1, 4);
		Entity root;
		root.kind = EntityKind::Item;
		root.has_item_def = true;
		root.has_graphic_model = true;
		root.health = 100;
		root.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		root.seats.push_back(sit);
		const EntityHandle root_h = w.registry.spawn(1, root);
		Entity child;
		child.kind = EntityKind::Item;
		child.health = 100;
		child.alive = true;
		child.ground_target = root_h;
		Seat gun;
		gun.type = SeatType::Gunner;
		gun.bone_index = 2;
		child.seats.push_back(gun);
		const EntityHandle child_h = w.registry.spawn(1, child);
		Entity player;
		player.kind = EntityKind::Organic;
		player.health = 100;
		player.alive = true;
		const EntityHandle player_h = w.registry.spawn(0, player);
		VehicleSeatSelection pick;
		CHECK(find_best_vehicle_seat(w, root_h, player_h, pick));
		CHECK(pick.vehicle == root_h && pick.type == SeatType::Passenger);
		w.registry.get(child_h)->has_item_def = true;
		w.registry.get(child_h)->has_graphic_model = true;
		CHECK(find_best_vehicle_seat(w, root_h, player_h, pick));
		CHECK(pick.vehicle == child_h && pick.type == SeatType::Gunner);
	}
}

// D-NET-507: the label pass measures the UNLIFTED seat point's words from the
// player's Position words, the x87 length truncated, against 0x40000; only the
// LOS point is lifted 0x3000. A seat 0x3FFFF words out on the player's level
// labels (its lifted point lies past 0x40000) and one 0x40000 out does not.
// [orig: HUD_DrawVehicleSeatAndArmoryLabels - the lift `lea esi, [edx+3000h]`
//  @0x5a357f, the deltas @0x5a359a..0x5a35a0, the length @0x5a35cf..0x5a35e6,
//  `cmp eax, 40000h; jge` @0x5a35eb..0x5a35f0]
void test_label_distance_takes_the_unlifted_words() {
	for (const int32_t reach : {0x3FFFF, 0x40000}) {
		Rig r(2.0f);
		const Entity &pl = r.player();
		Entity bare;
		bare.kind = EntityKind::Item;
		bare.has_item_def = true;
		bare.has_graphic_model = true;
		bare.position.set_q16({pl.position.x.q16 + reach, pl.position.y.q16, pl.position.z.q16});
		bare.heading = 0; // the identity frame: the seat point is the row's words
		bare.health = 500;
		bare.health_max = 500;
		bare.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		sit.source_name = "sitex00";
		sit.seat_local = {0.0f, 0.0f, 0.0f};
		bare.seats.push_back(sit);
		const EntityHandle bare_h = r.w.registry.spawn(1, bare);
		std::vector<AttachLabel> labels;
		r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
		bool saw = false;
		for (const AttachLabel &l : labels) saw = saw || l.entity == bare_h;
		CHECK(saw == (reach == 0x3FFFF));
	}
}

// A candidate with its def but no graphic model (+0x30; a BMS marker, which
// the mission start's model pass gives none, keeps its type's seat rows)
// offers nothing: the use-key scan and the label pass skip it, so does the
// best-seat search's per-entry walk, and the admission refuses such a carrier
// before its walk. [orig: Entity_FindNearestSeatOrArmory @0x435E40..0x435E45;
// HUD_DrawVehicleSeatAndArmoryLabels @0x5A3365..0x5A3369;
// Entity_FindBestSeatSlot @0x4352DD..0x4352DF; Entity_CanEnterVehicle
// @0x435498..0x43549A; Game_ReloadEntityModelsAndCallbacks @0x522830]
void test_carrier_without_a_model_takes_no_rider() {
	{
		// The def-less test's geometry: the modelless carrier's sitex seat dead
		// ahead at 1.5 u would outscore the truck's control seat.
		Rig r(2.0f);
		Entity bare;
		bare.kind = EntityKind::Item;
		bare.has_item_def = true;
		bare.position = {102.0f, 201.5f, 10.0f};
		bare.health = 500;
		bare.health_max = 500;
		bare.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		sit.source_name = "sitex00";
		sit.seat_local = {0.0f, 0.0f, -0.1875f};
		bare.seats.push_back(sit);
		const EntityHandle bare_h = r.w.registry.spawn(1, bare);
		VehicleSeatSelection hit;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == r.veh_h && hit.seat_index == 0);
		std::vector<AttachLabel> labels;
		r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
		CHECK(labels.size() == 2);
		for (const AttachLabel &l : labels) CHECK(l.entity == r.veh_h);
		CHECK(!find_best_vehicle_seat(r.w, bare_h, r.player_h, hit));
		// With its model the same seat wins the scan and labels.
		r.w.registry.get(bare_h)->has_graphic_model = true;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
		CHECK(hit.vehicle == bare_h);
		labels.clear();
		r.w.vehicles.collect_attach_labels(r.player(), false, false, labels);
		bool saw_bare = false;
		for (const AttachLabel &l : labels) saw_bare = saw_bare || l.entity == bare_h;
		CHECK(saw_bare);
		CHECK(find_best_vehicle_seat(r.w, bare_h, r.player_h, hit));
	}
	{
		// An Armory crate with its def and an armory point but no model: armory
		// mode finds nothing.
		Rig r(2.0f);
		Entity crate;
		crate.kind = EntityKind::Item;
		crate.has_item_def = true;
		crate.item_id = 1125;
		crate.position = {99.0f, 199.0f, 10.0f};
		crate.health = 100;
		crate.health_max = 100;
		crate.alive = true;
		crate.armory_points.push_back({0.0f, 0.0f, 1.0f});
		const EntityHandle crate_h = r.w.registry.spawn(1, crate);
		look_at(r.w, r.player_h, {99.0f, 199.0f, 11.0f});
		VehicleSeatSelection hit;
		CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, true));
		std::vector<AttachLabel> labels;
		r.w.vehicles.collect_attach_labels(r.player(), true, false, labels);
		CHECK(labels.empty());
		r.w.registry.get(crate_h)->has_graphic_model = true;
		CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, true));
		CHECK(hit.vehicle == crate_h && hit.type == SeatType::ArmoryPoint);
	}
	{
		// The best-seat search passes over a modelless child whose UseGun seat
		// outweighs the root's sitex seat.
		World w;
		w.registry.configure_pool(0, 4);
		w.registry.configure_pool(1, 4);
		Entity root;
		root.kind = EntityKind::Item;
		root.has_item_def = true;
		root.has_graphic_model = true;
		root.health = 100;
		root.alive = true;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		root.seats.push_back(sit);
		const EntityHandle root_h = w.registry.spawn(1, root);
		Entity child;
		child.kind = EntityKind::Item;
		child.has_item_def = true;
		child.health = 100;
		child.alive = true;
		child.ground_target = root_h;
		Seat gun;
		gun.type = SeatType::Gunner;
		gun.bone_index = 2;
		child.seats.push_back(gun);
		const EntityHandle child_h = w.registry.spawn(1, child);
		Entity player;
		player.kind = EntityKind::Organic;
		player.health = 100;
		player.alive = true;
		const EntityHandle player_h = w.registry.spawn(0, player);
		VehicleSeatSelection pick;
		CHECK(find_best_vehicle_seat(w, root_h, player_h, pick));
		CHECK(pick.vehicle == root_h && pick.type == SeatType::Passenger);
		w.registry.get(child_h)->has_graphic_model = true;
		CHECK(find_best_vehicle_seat(w, root_h, player_h, pick));
		CHECK(pick.vehicle == child_h && pick.type == SeatType::Gunner);
	}
	{
		// The admission: a modelless carrier at its spawn anchor, whose
		// ground-attached child has its def, its model and a seat, is refused
		// before the walk would find the child's seat.
		Rig r;
		r.veh().spawn_position = r.veh().position;
		Entity child;
		child.kind = EntityKind::Item;
		child.has_item_def = true;
		child.has_graphic_model = true;
		child.health = 100;
		child.alive = true;
		child.ground_target = r.veh_h;
		Seat sit;
		sit.type = SeatType::Passenger;
		sit.bone_index = 1;
		child.seats.push_back(sit);
		(void)r.w.registry.spawn(1, child);
		CHECK(vehicle_can_enter(r.w, nullptr, r.veh()));
		// A carrier whose model holds no userpoints is refused the same way,
		// whatever its child carries (D-NET-587) [orig: Entity_CanEnterVehicle
		// @0x435480 (the count +0xBC @0x43549C..0x4354A3, ahead of the walk @0x4354FB)].
		r.veh().graphic_model_userpoints = 0;
		CHECK(!vehicle_can_enter(r.w, nullptr, r.veh()));
		r.veh().graphic_model_userpoints = 2;
		r.veh().has_graphic_model = false;
		CHECK(!vehicle_can_enter(r.w, nullptr, r.veh()));
	}
}

void test_vehicle_admission_stationary_threshold_and_deck() {
	Rig r;
	r.veh().spawn_position = Vec3{};
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
	r.veh().spawn_position = Vec3{};
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

// The entry claim's walk reads a neighbour's dead bit alone (D-NET-433): a
// claimant at Health 0 with the bit clear still holds its E1, so the brain
// takes E2; with the bit set its claim is skipped and the brain takes E1.
// [orig: Entity_UpdateInfantryAI - `test byte [eax+24h], 2` @0x4BB108..0x4BB10C,
//  no Health read in the walk @0x4BB0F0..0x4BB149]
void test_ai_entry_claim_reads_the_dead_bit_alone() {
	for (const bool dead_bit : {false, true}) {
		Rig r;
		r.w.cached.local_player = {};
		r.veh().item_type = 6;
		r.veh().item_attrib = 0;
		EntryPointProvider pose;
		pose.points = { { "E1", { 110, 200, 10 } }, { "E2", { 110, 210, 10 } } };
		r.w.pose_provider = &pose;
		Entity other = r.player();
		other.net_id = 89;
		other.health = 0;
		other.alive = false;
		if (dead_bit) other.flags |= kEntityFlagDead;
		const auto oh = r.w.registry.spawn(0, other);
		auto &ob = *r.sys.at(r.sys.attach(oh));
		ob.slot.f[37] = 125;
		ob.slot.f[38] = 11;
		ob.inf.board_entry_slot = 1;
		r.player().net_id = 88;
		auto &brain = *r.sys.at(r.sys.attach(r.player_h));
		brain.slot.f[37] = 125;
		brain.slot.f[38] = 11;
		brain.self->position.x.q16 = 100 * 65536;
		brain.self->position.y.q16 = 210 * 65536;
		brain.self->position.z.q16 = 10 * 65536;
		r.player().position = { 100.0f, 210.0f, 10.0f };
		r.sys.infantry_think(brain, r.w);
		CHECK(brain.inf.board_entry_slot == (dead_bit ? 1 : 2));
	}
}

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
		brain.self->position.x.q16 = x * 65536;
		brain.self->position.y.q16 = y * 65536;
		brain.self->position.z.q16 = 10 * 65536;
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
	// UseGun bypasses the staging sequence and admits an EWeap on arrival. The
	// seat it takes is the gun's UseGun: a ctrlx seat on a carrier without
	// PlayerControl refuses (D-NET-398) [orig: Entity_AttachToVehicleSlot @0x4947cc].
	r.veh().item_attrib = kItemAttribEweap;
	r.veh().seats[0].type = SeatType::Gunner;
	r.veh().seats[0].source_name = "UseGun";
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
	brain.self->position.x.q16 = 100 * 65536;
	brain.self->position.y.q16 = 200 * 65536;
	brain.self->position.z.q16 = 10 * 65536;
	r.player().spawn_position = { 100, 200, 10 };
	r.sys.infantry_think(brain, r.w);
	CHECK(brain.inf.move_mode == 0);
	CHECK(!r.player().mounted);
	CHECK(r.player().health > 0);
	r.veh().health = 0;
	r.veh().last_attacker = r.veh_h;
	brain.self->position.x.q16 += 9 * 65536;
	r.sys.infantry_think(brain, r.w);
	CHECK(r.player().health > 0); // zero health alone is not the dead bit [orig: @0x4BB211]
	brain.self->position.x.q16 -= 9 * 65536;
	r.veh().engine_flags |= kEntityFlagDead;
	r.sys.infantry_think(brain, r.w);
	CHECK(r.player().health > 0); // dead carrier, walker still at spawn
	brain.self->position.x.q16 += 9 * 65536;
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
    enemy.has_item_def = true; // the scan skips a row with no ItemDef [orig: @0x435A4F]
    enemy.team = 2; // player team defaults 0... stamp both sides below
    enemy.health = 150;
    enemy.alive = true;
    EntityHandle eh = r.w.registry.spawn(0, enemy);
    r.player().team = 1;
    r.veh().item_type = 1; // only a vehicle asks for its enemy occupant [orig: @0x435E58]
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

    // Same-team, dead-bit, ItemDef-less or detached riders do not block the
    // vehicle; a rider's Health or life alone does not unblock it, only its dead
    // bit (D-NET-586) [orig: Vehicle_HasEnemyOccupant @0x4359F0 (@0x435A4D,
    // @0x435A4F, @0x435A65)]. The occupied seat itself remains hidden
    // independently, leaving the passenger seat.
    ee->team = 1;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->team = 2;
    ee->alive = false;
    CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 0);
    ee->alive = true;
    ee->health = 0;
    CHECK(!r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 0);
    ee->health = 150;
    ee->engine_flags |= kEntityFlagDead;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->engine_flags &= ~kEntityFlagDead;
    ee->has_item_def = false;
    CHECK(r.w.vehicles.find_nearest_free_seat(r.player(), hit, false));
    CHECK(label_count() == 1);
    ee->has_item_def = true;
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
    gun.has_item_def = true; // seats need the def (D-NET-422)
    gun.has_graphic_model = true;
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
    CHECK(r.player().equipped_adm_index == 7); // no AdmDef store at the attach (NEW-L7-3)
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
    // The detach leaves the gun's MountSlot owner standing; only the gunner's own
    // destroy takes it (the port's dead-handle twin of retail's dangling word).
    // [orig: Entity_DetachFromVehicle @0x4355F0 - no +0x24 store; Entity_Destroy @0x43E810]
    CHECK(r.w.registry.get(gh)->primary_weapon_owner == r.player_h);
    r.w.vehicles.release_despawned_slot_owner(r.player_h);
    CHECK(!r.w.registry.get(gh)->primary_weapon_owner.valid());
}

// Retail's shared EWeap slot resolver uses the live MountSlot+0x5e bit 8 to
// choose between an addeweap child's embedded slot and its groundEntity
// carrier's vehicle slot. The authored G/C flags are capabilities, not this
// mutable route bit. [orig: Entity_GetWeaponSlots @0x5460E0; NetPacket_WritePlayerState
// @0x4FF6B0 (the route @0x4FFE18)]
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
    // entity's own embedded MountSlot. The route bit with no groundEntity is
    // retail's wild +0x474 off a null pointer; ours resolves no slot there.
    Entity direct_only;
    direct_only.primary_weapon_slot_adm = 3;
    direct_only.has_item_def = true;
    direct_only.has_graphic_model = true;
    direct_only.item_attrib = kItemAttribEweap;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(direct_only) ==
          &direct_only.primary_weapon_slot);
    direct_only.primary_weapon_slot.redirect_to_parent_slot = true;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(direct_only) == nullptr);
    // A PlayerControl row (attrib 0x40) keeps its own slot under the route bit whatever its def
    // type; a type-1 row without the attrib follows the route (here to no groundEntity).
    // [orig: Entity_GetWeaponSlots @0x5460E0 - `test al, 40h` @0x5460F6, +0x474 @0x5460FA]
    Entity controlled = direct_only;
    controlled.item_type = 2;
    controlled.item_attrib = kItemAttribEweap | kItemAttribPlayerControl;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(controlled) == &controlled.primary_weapon_slot);
    controlled.item_type = 1;
    controlled.item_attrib = kItemAttribEweap;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(controlled) == nullptr);
    // The phase-8 record keys the def type, not the attrib: a type-1 row writes its own slot
    // under the route bit, a PlayerControl gun of another type routes to its groundEntity
    // (here none: the zero words).
    // [orig: NetPacket_WritePlayerState @0x4FF6B0 - Entity_IsMountableGun @0x4FFE0F, the
    //  type-1 leg @0x4FFE5F..0x4FFE65, the ground checks @0x4FFE27..0x4FFE3F]
    CHECK(w.vehicles.resolve_phase8_ammo_slot(controlled) == &controlled.primary_weapon_slot);
    controlled.item_type = 2;
    controlled.item_attrib = kItemAttribEweap | kItemAttribPlayerControl;
    CHECK(w.vehicles.resolve_phase8_ammo_slot(controlled) == nullptr);
    controlled.primary_weapon_slot.redirect_to_parent_slot = false;
    CHECK(w.vehicles.resolve_phase8_ammo_slot(controlled) == &controlled.primary_weapon_slot);

    Entity parent;
    parent.kind = EntityKind::Item;
    parent.has_item_def = true;
    parent.has_graphic_model = true;
    parent.item_type = 1;
    parent.item_attrib = kItemAttribEweap;
    parent.primary_weapon_slot.clip = 41;
    const EntityHandle parent_h = w.registry.spawn(1, parent);
    const uint64_t parent_spawn_id =
            w.registry.get(parent_h)->registry_spawn_id;

    Entity child;
    child.kind = EntityKind::Item;
    child.has_item_def = true;
    child.has_graphic_model = true;
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
    // The phase-8 record's writer and reader take the same carrier slot: its
    // groundEntity passes their ground checks.
    CHECK(w.vehicles.resolve_phase8_ammo_slot(*live_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);
    CHECK(const_world.vehicles.resolve_phase8_ammo_slot(const_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);

    // The similarly named attachment pointer is not a substitute for retail's
    // groundEntity relationship.
    live_child->ground_target = EntityHandle{};
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) == nullptr);
    live_child->ground_target = parent_h;

    // The route reads the groundEntity's slot with no check of that entity: not
    // the addeweap identity, nor its def, type or attrib, nor the lifetime the
    // relationship was made with. A replacement at the handle (here a plain
    // defless row) is the slot the route names.
    // [orig: Entity_GetWeaponSlots @0x5460E0 - the bit test @0x546101, the
    //  groundEntity read @0x546110, +0x474 @0x546113]
    w.registry.despawn(parent_h);
    Entity replacement;
    replacement.kind = EntityKind::Item;
    replacement.primary_weapon_slot.clip = 13;
    CHECK(w.registry.spawn_at(parent_h, replacement) == parent_h);
    live_child = w.registry.get(child_h);
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child)->clip == 13);
    // The phase-8 record's writer and reader inline the route with the setter's
    // four ground checks, so a defless ground neither writes nor takes the words.
    // [orig: NetPacket_WritePlayerState @0x4FF6B0 - @0x4FFE27..0x4FFE3F, the zero
    //  words @0x4FFE7F; NapiNPClientMsg_0x00A @0x42FEC0 - @0x43050C..0x430524]
    CHECK(w.vehicles.resolve_phase8_ammo_slot(*live_child) == nullptr);
    live_child->emplacement_parent = EntityHandle{};
    live_child->emplacement_parent_spawn_id = 0;
    CHECK(w.vehicles.resolve_mounted_ammo_slot(*live_child) ==
          &w.registry.get(parent_h)->primary_weapon_slot);
}

// An optic weapon (Flags & 3, here sighted) seeds its vehicle slot's elevation
// from the scope-zero table at its initial step; a plain one keeps zero.
// [orig: WeaponSlot_InitFromEntityDef @0x5466C0 -> WeaponSlot_InitFromDef
//  @0x53EE70 -- +0x04 = def[+0x3B0 + 4*step] under def+8 & 3 @0x53EEDA..0x53EEEC]
void test_sighted_vehicle_weapon_seeds_its_zero_pitch() {
    for (const int32_t flags : {0x2, 0x0}) {
        World w;
        w.tables.weapons.entries.resize(2);
        WeaponTableEntry &gun = w.tables.weapons.entries[1];
        gun.name = "WPN_SIGHTED";
        gun.valid = true;
        gun.clipsize = 1;
        gun.startrounds = 5;
        gun.flags = flags;
        WeaponScopeZero &zero = gun.action_fsm.scope_zero;
        zero.max_steps = 10;
        zero.step_metres = 100;
        zero.default_metres = 300;
        for (int i = 0; i < 40; ++i) zero.elevation[size_t(i)] = (i + 1) * 1000;
        Entity mount;
        mount.primary_weapon = gun.name;
        CHECK(w.vehicles.init_weapon_slot_from_entity_def(mount));
        const int16_t step = weapon_scope_zero_initial(zero);
        CHECK(mount.primary_weapon_slot.scope_zero == step);
        if (flags != 0) {
            CHECK(mount.primary_weapon_slot.zero_pitch == weapon_scope_zero_pitch(zero, step));
            CHECK(mount.primary_weapon_slot.zero_pitch != 0);
        } else {
            CHECK(mount.primary_weapon_slot.zero_pitch == 0);
        }
    }
}

void test_entity_def_slot_init_seeds_the_words() {
    World w;
    w.tables.weapons.entries.resize(4);
    WeaponTableEntry &weapon = w.tables.weapons.entries[3];
    weapon.name = "WPN_LATE_MOUNT";
    weapon.clipsize = 12;
    weapon.startrounds = 41;
    weapon.valid = true;

    Entity mount;
    mount.primary_weapon = weapon.name;
    CHECK(w.vehicles.init_weapon_slot_from_entity_def(mount));
    CHECK(mount.primary_weapon_slot_adm == 3);
    CHECK(mount.primary_weapon_slot.clip == 12);
    CHECK(mount.primary_weapon_slot.reserve == 29);

    // The init clears the whole slot first, its owner and route bit with it, and
    // seeds it afresh: a re-run (a respawned aircraft's class init) refills it.
    // [orig: WeaponSlot_InitFromEntityDef @0x5466C0 - the memset @0x5466D3]
    mount.primary_weapon_slot.clip = 7;
    mount.primary_weapon_slot.reserve = 23;
    mount.primary_weapon_slot.redirect_to_parent_slot = true;
    mount.primary_weapon_owner = EntityHandle::make(0, 1);
    CHECK(w.vehicles.init_weapon_slot_from_entity_def(mount));
    CHECK(mount.primary_weapon_slot.clip == 12);
    CHECK(mount.primary_weapon_slot.reserve == 29);
    CHECK(!mount.primary_weapon_slot.redirect_to_parent_slot);
    CHECK(!mount.primary_weapon_owner.valid());
    // An unknown name (or "null") leaves the slot clear and unbound.
    mount.primary_weapon = "null";
    CHECK(!w.vehicles.init_weapon_slot_from_entity_def(mount));
    CHECK(mount.primary_weapon_slot_adm == kAdmSlotNone);
    CHECK(mount.primary_weapon_slot.clip == 0 && mount.primary_weapon_slot.reserve == 0);
    mount.primary_weapon = weapon.name;

    // The reserve word is retail's: raw startrounds when either word is 0xFFFF,
    // else the unclamped 16-bit difference. [orig: WeaponSlot_InitFromEntityDef
    //  @0x546717..0x546733]
    struct Seed {
        int16_t clipsize, startrounds;
        int32_t clip, reserve;
    };
    const Seed seeds[] = {
        {30, 10, 30, -20},  // more clip than rounds: a negative word, not 0
        {-1, 25, -1, 25},   // clipless: the raw startrounds
        {12, -1, 12, -1},   // startrounds 0xFFFF: raw
    };
    for (const Seed &s : seeds) {
        weapon.clipsize = s.clipsize;
        weapon.startrounds = s.startrounds;
        Entity fresh;
        fresh.primary_weapon = weapon.name;
        CHECK(w.vehicles.init_weapon_slot_from_entity_def(fresh));
        CHECK(fresh.primary_weapon_slot.clip == s.clip);
        CHECK(fresh.primary_weapon_slot.reserve == s.reserve);
    }
}

// An emplacement's slot starts its optic at the WeaponSlot_InitFromDef seed,
// never at the lazy max: a slot initialized without an owner entity skips the
// class-6 sniper lock, so even a sniper-class gunner's cannon starts at the
// floor. [orig: WeaponSlot_InitFromEntityDef entityPtr 0 @0x546706;
//  WeaponSlot_InitFromDef @0x53EEF7 / @0x53EF2D..0x53EF44]
void test_entity_def_slot_init_seeds_the_zoom() {
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
    CHECK(w.vehicles.init_weapon_slot_from_entity_def(gun));
    CHECK(gun.primary_weapon_slot.scope_zoom == 2);
    // A re-run of the init (a respawned aircraft's class init) re-seeds the zoom;
    // no mount runs it, so a stepped zoom survives every mount.
    gun.primary_weapon_slot.scope_zoom = 6;
    CHECK(w.vehicles.init_weapon_slot_from_entity_def(gun));
    CHECK(gun.primary_weapon_slot.scope_zoom == 2);
    Entity roof;
    roof.primary_weapon = rcws.name;
    CHECK(w.vehicles.init_weapon_slot_from_entity_def(roof));
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
// [orig: Entity_UpdateAllEntities -- `cmp g_NapiNPCtx.is_in_session,0`
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

// An enemy body standing on an empty vehicle with a refNum: the hold scan walks
// the vehicle's refNum group list, not the pools. A manned EWeap gun on the list,
// standing on the vehicle, holds it and the team stays; the same gun off the list
// (its refNum byte matches, it never joined) holds nothing and the body's team is
// copied.
// [orig: Entity_UpdateAllEntities -- the list read @0x4C2521..0x4C2545, the
//  tests @0x4C2549..0x4C2572, the hold @0x4C2586, the team copy @0x4C25A4;
//  the join Entity_SpawnFromBMSRecord @0x40EC23..0x40EC45]
void test_hold_scan_walks_the_refnum_list() {
    for (const bool member : {true, false}) {
        Rig r;
        r.veh().item_type = 1;
        r.veh().item_attrib |= kItemAttribPlayerControl;
        r.veh().team = 2;
        r.veh().ref_num = 9;
        r.player().team = 1;
        Entity crew;
        crew.kind = EntityKind::Organic;
        crew.item_id = 2072;
        crew.health = 150;
        crew.alive = true;
        crew.team = 2;
        const EntityHandle crew_h = r.w.registry.spawn(0, crew);
        Entity gun;
        gun.kind = EntityKind::Item;
        gun.has_item_def = true;
        gun.item_type_index = 4;
        gun.item_attrib = kItemAttribEweap;
        gun.ref_num = 9;
        gun.ref_group_member = member;
        gun.ground_target = r.veh_h;
        gun.primary_occupant = crew_h;
        const EntityHandle gun_h = r.w.registry.spawn(1, gun);
        CHECK(r.w.registry.ref_group(9).size() == (member ? 1u : 0u));
        AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
        body.inf.active = true;
        body.inf.is_local_player = true;
        body.health = r.player().health;
        body.team = 1;
        r.player().ground_target = r.veh_h; // standing on it, not seated
        TickContext ctx{};
        ctx.world = &r.w;
        ctx.is_authority = true;
        ctx.logic_tick = 1;
        r.w.logic_tick = 1;
        r.w.update_all_entities(ctx);
        CHECK(r.veh().team == (member ? 2 : 1));
        // The destroy takes the gun off the list.
        r.w.registry.despawn(gun_h);
        CHECK(r.w.registry.ref_group(9).empty());
    }
}

// The SP lose epilog runs a reduced entity update: every pool-1 row's pose is
// copied into its saved pose, only the row a player drives is visited, and the
// update is not counted; the pool-0 walk still runs (it wakes the driven
// vehicle on tick 8). Once the epilog lifts, the full update visits and counts.
// [orig: Entity_UpdateAllEntities -- `cmp g_EpilogScreenActive,0`
//  @0x4C211D, the walk @0x4C239A..0x4C2408, the tail @0x4C2624..0x4C2639]
void test_epilog_entity_update() {
    Rig r;
    r.veh().item_type = 1;
    r.veh().item_attrib |= kItemAttribPlayerControl;
    Entity prop;
    prop.kind = EntityKind::Item;
    prop.has_item_def = true;
    prop.has_graphic_model = true;
    prop.item_id = 999;
    prop.item_type_index = 1; // the def row's ordinal: the walks' +0x1C gate
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
    r.w.epilog.screen_active = true; // the lose cine's build [orig: @0x57450C]
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

    r.w.epilog.screen_active = false; // the round-over RESTART key's clear [orig: @0x49c879]
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

// The riders' death tick (D-NET-897): a ground hull whose mover burns its Health to 0 in
// update T commits its death in update T+1, and its rider dies in T+1. The pool-1 visit thinks
// before its mover, so T's think still reads a live hull. T+1's follow-waypoint think queues the
// death event by the hull's planar speed (3 at >= 1057, else 4), the mover head requests 21, and
// the timed dispatch after the pool-1 walk commits 21 (DYING, step 16) on a moving hull or 23
// (DEAD, step 62) on a still one. Either enter runs the death transforms (Flags 6) and queues the
// kz blast, which the same update's explosion drain lands on the seated rider (kz type 4 passes
// the seat gate). The next visit's think re-arms the clock from the new state's step.
// Retail's 03TR hull 0x1014 (LP-s10 h / gh, LP-s12 jm and k3): Health 0 at tick 4783; Flags 6,
// state 21 and both riders dead at 4784; +0x2AC 15 at 4785.
// [orig: Entity_UpdatePool1Slot @0x4B8DD0 - the think gate @0x4B8E1B, its call @0x4B8E3C, the
//  mover @0x4B8E53, the decrement @0x4B8EA0; AI_HandleEvent_HelicopterCombatD @0x467730 (row
//  16's tick) - the Health test @0x467747, the speed @0x467954..0x46798F, `cmp eax, 421h`
//  @0x4679A4, type 4 @0x4679B6; AI_HandleEvent_HelicopterCombatB @0x4679F0 (row 16's event) -
//  3 -> 21 @0x467A3A, 4 -> 23 @0x467A2B; Entity_UpdateVehiclePhysics @0x48AF00 - the pend 21
//  @0x48AFD9..0x48AFF6, the critical burn @0x48B06C..0x48B083; Entity_UpdateAllEntities - the
//  timed dispatch @0x4C2226, the explosion drain @0x4C223F]
void test_a_hull_commits_its_death_the_update_after_health_zero() {
	for (const bool moving : { true, false }) {
		const int failures_before = failures;
		Rig r;
		VehicleTraits traits = truck_traits();
		traits.family = VehicleFamily::Ground;
		traits.critical_hp = 100;
		traits.critical_drain = 40;
		r.w.vehicles.traits.set(r.veh().item_id, traits);
		ItemDeathTraits death;
		death.unit_type = 1;
		death.has_husk = true;
		death.husk_model_loaded = true;
		death.kz = 10.0f;
		r.w.tables.item_death_traits.set(r.veh().item_id, death);
		// The death blast's row.
		r.w.tables.ammo.entries.resize(2);
		r.w.tables.ammo.entries[0].name = "AT_NULL";
		r.w.tables.ammo.entries[0].valid = true;
		AmmoTableEntry &blast = r.w.tables.ammo.entries[1];
		blast.name = kAmmoKzOrganicBlast;
		blast.valid = true;
		blast.kztype = ammo_kz::kRadiusBlast;
		blast.kz_damage = 1000;
		blast.kz_minradius = 0.5f;
		blast.kz_maxradius = 5.0f;
		// An AI driver keeps the hull out of the parked arm's state-22 stamp; the rider sits in
		// the vehicle seat, on another team than the hull.
		Entity npc;
		npc.kind = EntityKind::Organic;
		npc.item_id = 2072;
		npc.health = 150;
		npc.alive = true;
		npc.team = 1;
		const EntityHandle driver = r.w.registry.spawn(0, npc);
		CHECK(r.w.vehicles.process_attach(driver, r.veh_h, 1));
		r.player().team = 1;
		CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
		AiEntity &hull = test_common::brain_hull(r.w, r.veh_h);
		hull.profile.flags100 |= 2; // no target scan in T's live think
		AiBrain &b = hull.brain;
		b.f[AiBrain::kCurState] = 16;
		b.f[AiBrain::kPendState] = 16;
		b.f[AiBrain::kStep] = 0;
		r.veh().spawn_phase = 0; // the think runs at every visit
		r.veh().health = 40;     // the critical band's last burn
		r.w.logic_tick = 52;     // (52 + 36 * 11) & 63 == 0: T burns
		TickContext ctx{};
		ctx.is_authority = true;
		// T: the think reads a live hull, then the mover burns it to 0.
		r.w.update_all_entities(ctx);
		++r.w.logic_tick;
		CHECK(r.veh().health == 0);
		CHECK(((r.veh().flags | r.veh().engine_flags) & (kEntityFlagDead | kEntityFlagHusk)) == 0);
		CHECK(b.f[AiBrain::kCurState] == 16);
		CHECK(r.player().health == 150 && r.player().mounted);
		// The planar velocity words (+0x98 / +0x9C) and the speed register (+0x29C) T's mover
		// leaves. The words are read twice in T+1: by the think, ahead of the mover, and by the
		// dying state's enter in the timed dispatch, after it; the grounded straight arm in
		// between stores the register times the forward row into them, so a moving hull is one
		// whose register holds its speed.
		// [orig: AI_TransitionToDeath_GroundVehicle @0x467B20 - the +0x9C / +0x98 reads
		//  @0x467C09 / @0x467C0F, `cmp eax, 421h` @0x467C4C; Entity_UpdateVehiclePhysics
		//  @0x48AF00 - the +0x29C products @0x48CCF4..0x48CD52, the stores @0x48CD7F / @0x48CD85]
		r.veh().veh.vel_x = moving ? 3000 : 100;
		r.veh().veh.vel_y = moving ? 4000 : 0;
		r.veh().veh.speed = moving ? 5000 : 0;
		// T+1: the death commits and the blast lands on the rider.
		r.w.update_all_entities(ctx);
		++r.w.logic_tick;
		CHECK((r.veh().engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
				(kEntityFlagDead | kEntityFlagHusk));
		CHECK(b.f[AiBrain::kCurState] == (moving ? 21 : 23));
		CHECK(r.player().health <= 0);
		// T+2: the think re-arms the clock from the new state's step.
		r.w.update_all_entities(ctx);
		CHECK(b.f[AiBrain::kCurState] == (moving ? 21 : 23));
		CHECK(r.veh().spawn_phase == (moving ? 15 : 61));
		if (failures != failures_before)
			std::printf("  (moving %d: state %d pend %d clock %d health %d flags 0x%x/0x%x vel %d %d "
					"speed %d rider health %d mounted %d)\n",
					int(moving), b.f[AiBrain::kCurState], b.f[AiBrain::kPendState],
					r.veh().spawn_phase, r.veh().health, r.veh().flags, r.veh().engine_flags,
					r.veh().veh.vel_x, r.veh().veh.vel_y, r.veh().veh.speed, r.player().health,
					int(r.player().mounted));
	}
}

// D-NET-897's falling-death stop: a dying hull (state 21) under the routed falling callback
// lands on its husk's collision section 0 floor, the ground less |z min| (retail's Dsuv1X, the
// 03TR SUV's husk: -20522), the tail commits the integrated pose below it, the generic callback
// holds it there, and the next 16-tick think finds it still and commits 23 (DEAD) although the
// velocity words still read a moving hull. Retail's 03TR hull 0x1014 (LP-s15 h897, run a): z
// 70792 (2812 above its floor 67980) and slide -4060 at 4814; it lands at 4815 at 66398 with
// the slide -4394, takes 0 at 4816 and commits 23 at 4817 with the clock at 15.
// [orig: Entity_UpdateFallingDeathPhysics @0x493F70 - +0x54 @0x49404D..0x49405E, the landing
//  test @0x494108..0x49410C, the tail @0x494215..0x494221; Entity_ProcessFallingDeathPhysics
//  @0x461D30 - the stop @0x461E67..0x461EA2; AI_TickState_VehicleDying @0x467CD0 - the call
//  @0x467CE8 ahead of the speed and still tests; AI_HandleEvent_VehicleDying @0x457F50]
void test_a_dying_hull_rests_on_its_husk_floor_and_dies_on_the_first_still_visit() {
	const int failures_before = failures;
	Rig r(30.0f);
	VehicleTraits traits = truck_traits();
	traits.family = VehicleFamily::Ground;
	r.w.vehicles.traits.set(r.veh().item_id, traits);
	ItemDeathTraits death;
	death.unit_type = 1; // the routed falling callback
	death.has_husk = true;
	death.husk_model_loaded = true;
	death.kz = 10.0f;
	death.husk_rest_z_lo_q16 = -20522;
	death.husk_rest_z_hi_q16 = 118053;
	r.w.tables.item_death_traits.set(r.veh().item_id, death);
	r.w.tables.ammo.entries.resize(2);
	r.w.tables.ammo.entries[0].name = "AT_NULL";
	r.w.tables.ammo.entries[0].valid = true;
	AmmoTableEntry &blast = r.w.tables.ammo.entries[1];
	blast.name = kAmmoKzOrganicBlast;
	blast.valid = true;
	blast.kztype = ammo_kz::kRadiusBlast;
	blast.kz_damage = 1000;
	blast.kz_minradius = 0.5f;
	blast.kz_maxradius = 5.0f;
	// A flat ten-unit field, dry.
	std::vector<uint16_t> heights(16 * 16, 10 * 256);
	std::vector<int> sectors(256, 1);
	opennova::terrain::TerrainHeightField terrain;
	terrain.heightmap = heights.data();
	terrain.dim = 16;
	terrain.layout.sector_grid = sectors.data();
	r.w.tables.terrain = &terrain;
	r.w.env.water_z = INT32_MIN;
	AiEntity &hull = test_common::brain_hull(r.w, r.veh_h);
	AiBrain &b = hull.brain;
	b.f[AiBrain::kCurState] = 21;
	b.f[AiBrain::kPendState] = 21;
	b.f[AiBrain::kStep] = 16;
	Entity &v = r.veh();
	v.health = 0;
	v.alive = false;
	v.flags |= kEntityFlagDead | kEntityFlagHusk;
	v.engine_flags |= kEntityFlagDead | kEntityFlagHusk;
	v.death_motion = DeathMotionMode::Falling;
	v.spawn_phase = 2; // the think comes on the third update
	const int32_t ground = opennova::terrain::height_field_height_bilinear_fixed(
			terrain, v.position.x.q16, v.position.y.q16);
	const int32_t floor = ground - 20522;
	const int32_t x0 = v.position.x.q16;
	const int32_t y0 = v.position.y.q16;
	v.position.set_q16({x0, y0, floor + 2812});
	v.veh.vel_x = 1176;
	v.veh.vel_y = -3923;
	v.veh.slide_z = -4060;
	TickContext ctx{};
	ctx.is_authority = true;
	// 4815: no think; the routed callback lands and commits the integrated pose.
	r.w.update_all_entities(ctx);
	++r.w.logic_tick;
	const int32_t rest = floor + 2812 - 4394;
	CHECK(v.position.z.q16 == rest);
	CHECK(v.position.x.q16 == x0 + 1176 && v.position.y.q16 == y0 - 3923);
	CHECK(v.veh.slide_z == -4394);
	CHECK(v.death_motion == DeathMotionMode::Generic);
	CHECK(b.f[AiBrain::kCurState] == 21);
	// 4816: the generic callback holds the pose and zeroes the fall.
	r.w.update_all_entities(ctx);
	++r.w.logic_tick;
	CHECK(v.position.z.q16 == rest && v.position.x.q16 == x0 + 1176);
	CHECK(v.veh.slide_z == 0);
	CHECK(v.veh.vel_x == 1176 && v.veh.vel_y == -3923);
	CHECK(b.f[AiBrain::kCurState] == 21);
	// 4817: the think finds the hull still and the destroy event commits 23.
	r.w.update_all_entities(ctx);
	CHECK(b.f[AiBrain::kCurState] == 23);
	CHECK(v.spawn_phase == 15);
	CHECK(v.position.z.q16 == rest);
	if (failures != failures_before)
		std::printf("  (dying hull: state %d pend %d clock %d z %d (rest %d) slide %d motion %d)\n",
				b.f[AiBrain::kCurState], b.f[AiBrain::kPendState], v.spawn_phase,
				v.position.z.q16, rest, v.veh.slide_z, int(v.death_motion));
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
    ve.self->position.x.q16 = 100 << 16;
    ve.self->position.y.q16 = 200 << 16;
    ve.self->position.z.q16 = 10 << 16;
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

    // No controller: the mover's occupant block takes the parked arm.
    // [orig: @0x48c002-0x48c02d]
    {
		const float x0 = r.veh().position.x;
		r.w.vehicles.tick_motor(r.veh(), t);
        CHECK(b.f[AiBrain::kCurState] == 22);
		CHECK(b.f[AiBrain::kPendState] == 16); // pending callback state is untouched
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
        // The mover runs the AI leg in place at its occupant block.
        r.w.vehicles.tick_motor(r.veh(), t);
        drove = drove || r.veh().veh.cmd_speed != 0;
        AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->self->position.x.q16 = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->self->position.y.q16 = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->self->position.z.q16 = static_cast<int32_t>(r.veh().position.z * 65536.0f);
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

    // The input block tests the hull, never the occupant: a controller that dies
    // seated leaves its seat at its own death edge, which the pool-0 walk runs
    // ahead of the hull's pool-1 update, and the hull then parks on its empty
    // seat. [orig: Entity_UpdateInfantryAI @0x4B9910 (the +0x16C test @0x4B9C57,
    // the detach @0x4B9C60); Entity_UpdateVehiclePhysics @0x48AF00
    // (@0x48B972..0x48B987, the parked arm @0x48C002)]
    r.w.registry.get(nh)->health = 0;
    r.w.registry.get(nh)->alive = false;
    CHECK(r.w.vehicles.detach(nh)); // the death edge's detach
    {
        r.w.vehicles.tick_motor(r.veh(), t);
        CHECK(b.f[AiBrain::kCurState] == 22);
        CHECK(r.veh().veh.cmd_speed == 0); // the parked hold, not the stale drive
    }
    // A live controller under a hull a script disabled (Flags 0x10000000) parks
    // as well: the hull's bit, not the occupant, decides.
    // [orig: WacCmd_DisableSsn @0x4F7690 (@0x4F76DD); @0x48B980..0x48B987]
    r.w.registry.get(nh)->health = 150;
    r.w.registry.get(nh)->alive = true;
    CHECK(r.w.vehicles.process_attach(nh, r.veh_h, 1));
    r.veh().flags |= kEntityFlagScriptDisabled;
    r.veh().veh.cmd_speed = 40 * 293;
    {
        r.w.vehicles.tick_motor(r.veh(), t);
        CHECK(b.f[AiBrain::kCurState] == 22);
        CHECK(r.veh().veh.cmd_speed == 0);
    }
    r.veh().flags &= ~kEntityFlagScriptDisabled;
}

// The pool-1 avoid BRAKE: a neighbor whose footprint ellipse overlaps ours and
// sits within ~30 deg of dead ahead damps the command speed by the id/counter
// factor ((id + (counter<<8)) & 0x7FFF) + 0x4000 per tick, the counter being
// the entity-update counter; a neighbor BEHIND does not
// [orig: @0x48bd8f-0x48bf26; Entity_UpdateVehiclePhysics @0x48BF26].
void test_ai_drive_avoid_brake(size_t capacity) {
    Rig r(30.0f);
    const Entity seed = r.veh();
    r.w.registry.configure_pool(1, capacity);
    CHECK(r.w.registry.spawn_at(r.veh_h, seed) == r.veh_h);
    const VehicleTraits t = truck_traits();
    r.w.vehicles.traits.set(r.veh().item_id, t);
    const int ai_idx = r.sys.attach(r.veh_h);
    AiEntity &ve = *r.sys.at(ai_idx);
    ve.self->position.x.q16 = 100 << 16;
    ve.self->position.y.q16 = 200 << 16;
    ve.self->position.z.q16 = 10 << 16;
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
    r2.w.vehicles.tick_motor(r2.veh(), t);
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
    test_common::brain_hull(r.w, r.veh_h); // the truck's class init: its mover runs
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    Entity &drv = r.player();
    drv.net_move_input = 0x08; // moving forward
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    CHECK(r.veh().veh.handbrake_latched == 0);
    drv.net_move_input = 0x08 | 0x80; // + lean right = handbrake
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK((r.veh().flags & 0x8u) != 0);
    CHECK(r.veh().veh.handbrake_latched == 1);
    CHECK(r.veh().veh.cmd_speed == 0);
    drv.net_move_input = 0x08;
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.veh().veh.handbrake_latched == 0);
    // The old latch still selects analog input on the release tick; digital
    // forward resumes on the following visit. [orig: @0x48B9D2..0x48B9E5]
    CHECK(r.veh().veh.cmd_speed == 0);
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // A def without handBrake ignores the key.
    t.hand_brake = 0;
    drv.net_move_input = 0x08 | 0x80;
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.veh().veh.handbrake_latched == 0);
    CHECK(r.veh().veh.cmd_speed == t.player_speed);
    // The crashed byte stops the command regardless.
    r.veh().veh.crashed = 1;
    drv.net_move_input = 0x08;
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK(r.veh().veh.cmd_speed == 0);
    r.veh().veh.crashed = 0;
    // The MoveOrder merge: a nonzero direction with the move bit latches the
    // free-look bit on the occupant's own word [orig: @0x48b847..0x48b897].
    drv.net_move_input = 0x08 | 0x01;
    r.w.vehicles.tick_motor(r.veh(), t);
    CHECK((drv.net_move_input & 0x10u) != 0);
    drv.net_move_input = 0x08; // straight: no latch
    r.w.vehicles.tick_motor(r.veh(), t);
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
        ve.self->position.x.q16 = 100 << 16;
        ve.self->position.y.q16 = 200 << 16;
        ve.self->position.z.q16 = 10 << 16;
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
// stamp). The AI leg's own parked test reads the dead bit, but the dead hull's
// mover bails to its matrix tail ahead of its input block: its registers keep
// their last values.
// [orig: ctan @0x48954B..0x489560, the bail @0x4893E9..0x4893ED; cveh
//  @0x48B972..0x48B987, the bail @0x48B810..0x48B814]
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
        d.r.w.vehicles.tick_motor(d.r.veh(), d.t);
        CHECK(d.r.veh().veh.cmd_speed == 40 * 293);
        CHECK(d.brain().brain.f[AiBrain::kCurState] == 16);
        d.r.veh().flags |= kEntityFlagDead;
        VehicleDriveCmd parked;
        d.r.sys.vehicle_ai_drive(d.r.w, d.r.veh(), ctrl, d.t, parked);
        CHECK(!parked.ai_drive);
        CHECK(d.brain().brain.f[AiBrain::kCurState] == 22);
        // The dead hull's mover bails ahead of its input block: no AI leg, no state
        // stamp, the registers keep their last values.
        d.r.sys.for_handle(d.r.veh_h)->brain.f[AiBrain::kCurState] = 16;
        d.r.w.vehicles.tick_motor(d.r.veh(), d.t);
        CHECK(d.r.veh().veh.cmd_speed == 40 * 293);
        CHECK(d.brain().brain.f[AiBrain::kCurState] == 16);
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
          r.veh().heading);
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
        ve->self->position.x.q16 = to_fixed(r.veh().position.x);
        ve->self->position.y.q16 = to_fixed(r.veh().position.y);
        ve->self->position.z.q16 = to_fixed(r.veh().position.z);
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
		CHECK((h.r.veh().flags & kEntityFlagEngineOn) != 0);
		// The engine bit is the hull's own Flags 0x80, so the live record's Flags byte
		// carries it to every client's engine-off override [orig: `or [esi+24h], 80h`
		// @0x491E05; the client test @0x491C95].
		CHECK((opennova::replication::snapshot_of(h.r.veh()).state_flags & 0x80u) != 0);
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
        CHECK((h.r.veh().flags & kEntityFlagEngineOn) == 0);
    }
    {
        HeloRig h(/*rotor_full=*/false); // cold rotor: the LABEL_328 park
        h.route_to(300, 200, 40);
        const float z0 = h.r.veh().position.z;
        h.tick(200);
        CHECK(h.r.veh().veh.part_spin.speed < kRotorSpeedMax);
        CHECK(h.r.veh().veh.cmd_speed == 0);
        CHECK((h.r.veh().flags & kEntityFlagEngineOn) == 0);
        CHECK(h.r.veh().position.z <= z0 + 1.0f);
    }
}

// A player in a helicopter's control seat on the AUTHORITY: the host skips the whole client
// interp leg, so the hull holds the heading it was placed with as the player boards, though
// promotion staged that heading as the smooth target no record has replaced
// [orig: `cmp g_NapiNPCtx.is_authority, 0; jnz loc_490CD0` @0x49095E over
//  @0x490C2A..0x490CCA; the allocator's +0x240 = Yaw @0x460314]. The step once ran here
// and added the staged heading raw on each of 20 ticks: a hull placed at 150 deg turned to
// 270 deg under its pilot.
void test_helo_player_boarding_holds_heading() {
    HeloRig h(/*rotor_full=*/false);
    Entity &veh = h.r.veh();
    const int32_t heading = bam_heading_from_mission_yaw_deg(150.0);
    veh.heading = heading;
    veh.veh.yaw_seeded = false;
    veh.veh.net_smooth_heading = heading;
    veh.veh.net_interp_progress = 0;
    h.ctrl->player_class = 1;
    h.ctrl->flags |= kEntityFlagPlayer;
    h.ctrl->engine_flags |= kEntityFlagPlayer;
    h.tick(40);
    CHECK(veh.veh.yaw_bam == heading);
    CHECK(veh.heading == heading);
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
    ve.self->position.x.q16 = 100 << 16;
    ve.self->position.y.q16 = 200 << 16;
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
    ve.self->position.x.q16 = 100 << 16;
    ve.self->position.y.q16 = 200 << 16;
    ve.self->position.z.q16 = 10 << 16;
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
    test_common::brain_hull(r.w, r.veh_h); // the truck's class init: its mover runs

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
    // [orig: Input_HandleActionBinding_0 @0x4E0420
    //  (the Pitch stores @0x4E0D39..0x4E0D5C, @0x4E0F50, @0x4E111C) -> entity+0x10/+0x14].
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
		r.veh().roll_bam = spawn_angle_bam(180);
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

// The floating attach-label list [orig: HUD_DrawVehicleSeatAndArmoryLabels @0x5a3290
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
// when the player cannot fire [orig: !Player_IsOpticalViewVisible() || entity == nearest @0x5a3354].
void test_attach_labels_can_fire_gate() {
    Rig r(1.0f);
    Entity veh2;
    veh2.net_id = 12;
    veh2.kind = EntityKind::Item;
    veh2.has_item_def = true; // seats need the def (D-NET-422)
    veh2.has_graphic_model = true;
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
    crate.has_item_def = true; // armory points need the def (D-NET-422)
    crate.has_graphic_model = true;
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
    player_seed.item_type_index = 3; // the def row's ordinal: the +0x1C slice gate
    player_seed.position = {100.0f, 200.0f, 10.0f};
    player_seed.heading = placed_heading_bam(90); // facing +x, down the row of seats (the aim cone)
    player_seed.health = 100;
    player_seed.alive = true;
    player_seed.team = 1;
    const EntityHandle player_h = w.registry.spawn(0, player_seed);
    CHECK(player_h.valid());

    EntityHandle enemy_occupied_vehicle;
    for (int i = 0; i < 32; ++i) {
        Entity vehicle;
        vehicle.kind = EntityKind::Item;
        vehicle.item_type_index = 7; // the def row's ordinal: the +0x1C slice gate
        vehicle.has_item_def = true; // seats need the def (D-NET-422)
        vehicle.has_graphic_model = true;
        vehicle.item_type = 1; // a vehicle: only def type 1 asks for an enemy occupant
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
        organic.item_type_index = 3; // the def row's ordinal: the +0x1C slice gate
        organic.has_item_def = true;
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
    wall.heading = placed_heading_bam(90);
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
    ve.self->position.x.q16 = 100 << 16;
    ve.self->position.y.q16 = 200 << 16;
    ve.self->position.z.q16 = 10 << 16;
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

	// The same face as an EWeap child (attrib 0x20 without PlayerControl) riding a
	// PlayerControl carrier answers with its CARRIER as the mass source; the truck,
	// more than twice the gun's size, only wakes the small gun (Flags 0x40) and its
	// probe passes on through. [orig: the mounted-child mass source
	//  @0x462E26..0x462E4F; the wake-and-skip arm of Entity_CheckCollisionState]
	{
		Entity carrier{};
		carrier.kind = EntityKind::Item;
		carrier.alive = true;
		carrier.item_attrib = kItemAttribPlayerControl;
		carrier.position = {300.0f, 300.0f, 10.0f};
		const EntityHandle carrier_h = r.w.registry.spawn(1, carrier);
		Entity &gun = *r.w.registry.get(wh);
		const uint32_t attrib = gun.item_attrib;
		const float radius = gun.bound_radius;
		gun.item_attrib = kItemAttribEweap;
		gun.ground_target = carrier_h;
		gun.bound_radius = 0.5f;
		gun.flags &= ~0x40u;
		r.veh().position = {146.8f, 200.0f, 10.0f};
		for (int i = 0; i < 17; ++i) cw.build_tick_tables(r.w);
		const int32_t probe_pos[3] = {static_cast<int32_t>(146.8 * 65536), 200 << 16, 10 << 16};
		const int32_t probes[1][3] = { { probe_pos[0], probe_pos[1], probe_pos[2] + 65536 } };
		const int32_t radii[1] = { 98304 };
		VehicleProbeForce force[1];
		EntityHandle hit;
		const int sev = cw.resolve_vehicle_probes(
				r.w, r.veh(), probe_pos, probes, radii, 1, 2500000, 3400000, force, hit);
		CHECK(sev == 0);
		CHECK(!hit.valid());
		CHECK((r.w.registry.get(wh)->flags & 0x40u) != 0);
		Entity &restored = *r.w.registry.get(wh);
		restored.item_attrib = attrib;
		restored.ground_target = {};
		restored.bound_radius = radius;
		restored.flags &= ~0x40u;
		r.w.registry.despawn(carrier_h);
		r.veh().position = {100.0f, 200.0f, 10.0f};
	}

    for (int i = 0; i < 950; ++i) {
        cw.build_tick_tables(r.w); // self-gated to the 17-tick cadence
		stamp_saved_live_pose(r.veh());
		r.w.vehicles.tick_motor(r.veh(), t); // the AI leg runs in place
		AiEntity *ve2 = r.sys.for_handle(r.veh_h);
        ve2->self->position.x.q16 = static_cast<int32_t>(r.veh().position.x * 65536.0f);
        ve2->self->position.y.q16 = static_cast<int32_t>(r.veh().position.y * 65536.0f);
        ve2->self->position.z.q16 = static_cast<int32_t>(r.veh().position.z * 65536.0f);
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
    // An addeweap frame's heading is read back off the composed fixed matrix,
    // which moves it by up to about 0.003 degrees near a quarter turn: retail's
    // TDH_I3A SUV 0x1015 streams heading 0xC0000000 and its gun 0x1037
    // 0xBFFF800A (D-NET-513). [orig: Math_FixedPointMatrixToEulerAngles @0x613310]
    CHECK(std::abs(mission_yaw_deg_from_bam_heading(pose.heading) - 0.25) < 0.005);
    CHECK(std::abs(pose.pitch * kDegreesPerBam - 0.25) < 0.001);
    CHECK(std::abs(pose.roll * kDegreesPerBam + 0.25) < 0.001);
    // A point without an authored direction (a zero vector) builds an all-zero
    // look-at frame, so the composed rotation is zero and every euler reads 0;
    // the position still rides the full frame (D-NET-513).
    // [orig: Math_BuildDirectionLookAtMatrix @0x612C90 - the zero arms @0x612CFB,
    //  @0x612D53, @0x612DF6; Math_FixedPointMatrixToEulerAngles @0x613310]
    point.rot_x = 0;
    CHECK(resolve_model_mounted_pose(model, carrier, anchor, nullptr, 0, pose));
    CHECK(pose.heading == 0 && pose.pitch == 0 && pose.roll == 0);
    CHECK(std::abs(pose.position.x - from_fixed(expected[0])) < 0.001);
    // The attachment's part bound, signed: any negative and an index past the part
    // count ride part 0; the count itself reads past the posed rows in retail and
    // resolves nothing here (D-3DI-8). [orig: Bone_BuildAttachmentMatrix
    // @ 0x56C672..0x56C685]
    const MountedPose root_pose = pose;
    for (const int32_t mapped : {-1, -7, 2}) {
        point.subobject_index = mapped;
        MountedPose remapped;
        CHECK(resolve_model_mounted_pose(model, carrier, anchor, nullptr, 0, remapped));
        CHECK(std::abs(remapped.position.x - root_pose.position.x) < 0.0001 &&
                std::abs(remapped.position.y - root_pose.position.y) < 0.0001 &&
                std::abs(remapped.position.z - root_pose.position.z) < 0.0001);
    }
    point.subobject_index = 1;
    MountedPose past;
    CHECK(!resolve_model_mounted_pose(model, carrier, anchor, nullptr, 0, past));
    point.subobject_index = 0;

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
        veh.heading = placed_heading_bam(static_cast<int16_t>(yaw));
        veh.pitch_bam = 0;
        veh.roll_bam = 0;
        const Vec3 got = entity_local_point_world(veh, L);
        const double a = -static_cast<double>(mission_yaw_whole_deg(veh.heading)) * 3.14159265358979323846 / 180.0;
        const double ca = std::cos(a), sa = std::sin(a);
        CHECK(std::fabs(got.x - (veh.position.x + (L.x * ca - L.y * sa))) < 1e-3);
        CHECK(std::fabs(got.y - (veh.position.y + (L.x * sa + L.y * ca))) < 1e-3);
        CHECK(std::fabs(got.z - (veh.position.z + L.z)) < 1e-3);
    }

    // (b) rolled/pitched carrier vs an independent double-precision euler
    // composition of the SAME convention the collision matrix builder uses:
    // Rz(heading) * Ry(-pitch) * Rx(roll) over the RAW model point (seat_local
    // is pre-swizzled, raw = (L.y, -L.x, L.z)), heading = bam(90 - yaw).
    veh.heading = placed_heading_bam(20);
    veh.pitch_bam = spawn_angle_bam(-6);
    veh.roll_bam = spawn_angle_bam(16);
    const Vec3 got = entity_local_point_world(veh, L);
    const double d2r = 3.14159265358979323846 / 180.0;
    const double h = (90.0 - static_cast<double>(mission_yaw_whole_deg(veh.heading))) * d2r;
    const double pt = -degrees_from_angle_bam(veh.pitch_bam) * d2r;
    const double rl = degrees_from_angle_bam(veh.roll_bam) * d2r;
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
    veh.heading = placed_heading_bam(0);
    veh.pitch_bam = 0;
    veh.roll_bam = spawn_angle_bam(25);
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
//  Bone_BuildAttachmentMatrix @0x435FFA; labels @0x5A3553]
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

    // Without the attrib a ctrlx attach refuses (D-NET-398), so the claimant
    // that keeps the counter is a UseGun gunner's.
    // [orig: Entity_AttachToVehicleSlot @0x4947cc; the gate @0x4356EF..0x4356F4]
    r.veh().item_attrib &= ~kItemAttribPlayerControl;
    CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    CHECK(!r.veh().primary_occupant.valid() && !r.player().mounted);
    Seat gun;
    gun.type = SeatType::Gunner;
    gun.bone_index = 3;
    gun.source_name = "UseGun";
    r.veh().seats.push_back(gun);
    CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 3));
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

// D-NET-448: on a vehicle (BoneCallback_Simple) the request snaps a sitex rider to the seat
// bone's yaw, the fpatan of the userpoint's direction words through the carrier's matrix,
// and the static pose takes the same bone pose; not the carrier heading less a whole-degree
// offset. A pitched carrier turns the direction before the fpatan.
// [orig: Entity_RequestVehicleAttach @0x4364A0 (the bone call @0x436540, the store
//  @0x4365BF..0x4365C3); Entity_GetBoneTransformAndOrientation @0x4B0C50 (@0x4B0D42,
//  @0x4B0D64..0x4B0D81, @0x4B0DA1..0x4B0DBB); EntityDef_LoadModelsAndCallbacks @0x439F50
//  (@0x43A05A -> @0x43A072)]
static void test_request_snaps_to_the_seat_bone_yaw() {
    Rig r;
    r.veh().item_type = 1;
    r.veh().pitch_bam = bam_from_degrees_wrapped(7);
    Seat &sit = r.veh().seats[1];
    sit.bone_direction[0] = 3 << 16;
    sit.bone_direction[1] = 4 << 16;
    sit.bone_direction[2] = 1 << 16;
    sit.yaw_offset = 53; // the whole-degree stand-in the static leg took
    MountedPose bone;
    seat_bone_pose_on_carrier_matrix(r.veh(), sit, bone);
    VehicleSeatSelection pick;
    pick.vehicle = r.veh_h;
    pick.seat_index = 1;
    pick.type = SeatType::Passenger;
    r.w.vehicles.request_attach_heading(r.player_h, pick);
    CHECK(r.player().heading == bone.heading);
    CHECK(bone.heading != opennova::io::bam_sub(emplaced_gun_frame_heading(r.veh()),
            bam_from_degrees_wrapped(53)));
    Entity rider = r.player();
    const MountedPose pose = r.w.vehicles.pose_mounted_occupant(rider, r.veh(), sit);
    CHECK(pose.heading == bone.heading && pose.pitch == bone.pitch && pose.roll == bone.roll);
    CHECK(pose.position.x.q16 == bone.position.x.q16 && pose.position.y.q16 == bone.position.y.q16 &&
          pose.position.z.q16 == bone.position.z.q16);
    // A carrier that is no vehicle keeps the static stand-in.
    r.veh().item_type = 0;
    r.w.vehicles.request_attach_heading(r.player_h, pick);
    CHECK(r.player().heading == opennova::io::bam_sub(emplaced_gun_frame_heading(r.veh()),
            bam_from_degrees_wrapped(53)));
}

// A body that boards in its own pass's think ends that pass on foot: the org1
// motor keys its seat block on the mounted-live local its head took, so the
// seat pose waits for the next pass. [orig: Entity_UpdateInfantryAI
// mounted-live local @0x4B9960..0x4B9985, its test @0x4BE8F0, the request's
// test @0x4BF4B3]
static void test_board_pass_stays_on_foot() {
    Rig r;
    r.w.cached.local_player = {};
    r.veh().item_type = 1;
    r.veh().item_attrib = kItemAttribPlayerControl;
    r.veh().spawn_position = r.veh().position; // at its spawn anchor: enterable
    r.player().net_id = 88; // key = tick + 36 * 88: tick 16 thinks
    AiEntity &body = *r.sys.at(r.sys.attach(r.player_h));
    body.inf.active = true;
    body.net_id = 88;
    body.health = 150;
    body.slot.f[37] = kCommandAttachAnySeat;
    body.slot.f[38] = 11;
    VehicleSeatSelection best;
    CHECK(find_best_vehicle_seat(r.w, r.veh_h, r.player_h, best));
    const Vec3 seat = entity_local_point_world(
            r.veh(), r.veh().seats[static_cast<size_t>(best.seat_index)].seat_local);
    // One unit off the seat point, inside the two-unit arrival ring.
    const int32_t start[3] = {to_fixed(seat.x + 1.0f), to_fixed(seat.y), to_fixed(seat.z)};
    body.self->position.set_q16({start[0], start[1], start[2]});
    r.player().position = {seat.x + 1.0f, seat.y, seat.z};
    r.sys.is_authority = true;
    r.sys.tick_infantry(body, r.w, 16);
    CHECK(r.player().mounted);
    CHECK(body.self->position.x.q16 == start[0] && body.self->position.y.q16 == start[1]);
    r.sys.tick_infantry(body, r.w, 17);
    CHECK(std::abs(body.self->position.x.q16 - to_fixed(seat.x)) <= 2);
    CHECK(std::abs(body.self->position.y.q16 - to_fixed(seat.y)) <= 2);
}

// A rider its seat-bone lookup drops still runs the seat block's tail: its
// legs snap to the body and its look takes the mounted chase, then the +-90
// degree clamp about the body; it takes no ground chase. A client keeps the
// rider and clamps inside the seat block. [orig: Entity_UpdateInfantryAI legs
// @0x4BEF36..0x4BEF51, look @0x4BEF57..0x4BEF97, clamp @0x4BEF9A..0x4BEFED; the
// ground chase @0x4BE8FD..0x4BEBE7 sits behind the mounted-live test @0x4BE8F0]
static void test_seat_bone_failure_runs_the_seat_tail() {
    struct NoSeatBone final : IPoseProvider {
        bool resolve_seat_bone(World &, const Entity &, int) override { return false; }
    };
    for (const bool authority : {true, false}) {
        World w;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        NoSeatBone provider;
        w.pose_provider = &provider;
        w.ai.root_motion = &channel_only; // the occupant holds its channel
        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.health = 100;
        vehicle.alive = true;
        vehicle.has_item_def = true;
        vehicle.has_graphic_model = true;
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
        AiEntity *body = w.ai.at(w.ai.attach(occupant_h));
        body->net_id = 100;
        body->health = 100;
        body->inf.active = true;
        CHECK(w.vehicles.process_attach(occupant_h, vehicle_h, 7));
        body->inf.body_heading = 0x10000000;
        body->inf.target_heading = 0x30000000; // a ground chase would turn the body
        body->inf.leg_yaw[0] = body->inf.leg_yaw[1] = 0;
        body->inf.leg_target[0] = body->inf.leg_target[1] = 0;
        body->heading = 0x4F000000;
        body->pitch = 0;
        body->inf.aim_heading = 0x7F000000;
        body->inf.aim_pitch = 0x00800000;
        body->inf.aim_valid = true;
        w.ai.is_authority = authority;
        w.ai.tick_infantry(*body, w, 1);
        CHECK(w.registry.get(occupant_h)->mounted == !authority);
        CHECK(body->inf.body_heading == 0x10000000);
        CHECK(body->inf.leg_yaw[0] == 0x10000000 && body->inf.leg_yaw[1] == 0x10000000);
        CHECK(body->inf.leg_target[0] == 0x10000000 && body->inf.leg_target[1] == 0x10000000);
        // The mounted look step (+0x02000000), then the clamp 90 degrees off the body.
        CHECK(body->heading == 0x50000000);
        CHECK(body->pitch == 0x00100000);
    }
}

// A player body's seat-bone lookup failing poses nothing and requests no seat
// animation: the authority detaches the rider and kills no one, a client keeps it
// seated, and both run the seat block's tail (groundEntity from parentEntity, the
// legs and their targets to the body heading). The pass's end is the next case's.
// A carrier whose bone resolves poses the rider on its moved frame.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the bone call @0x4B6510, its
//  test @0x4B6518..0x4B651A, the seat animation @0x4B65A8..0x4B65C3 (posed arm
//  only), Entity_DetachFromVehicleIfServer @0x4359D0 called @0x4B6627, the tail
//  @0x4B662F..0x4B66A3]
static void test_player_seat_bone_failure_detaches_on_the_authority() {
    struct NoSeatBone final : IPoseProvider {
        bool resolve_seat_bone(World &, const Entity &, int) override { return false; }
    };
    struct SeatBone final : IPoseProvider {};
    for (const bool resolves : {false, true}) {
        for (const bool authority : {true, false}) {
            auto w_heap = std::make_unique<World>();
            World &w = *w_heap;
            w.registry.configure_pool(0, 4);
            w.registry.configure_pool(1, 4);
            Entity vehicle;
            vehicle.net_id = 200;
            vehicle.kind = EntityKind::Item;
            vehicle.has_item_def = true;
            vehicle.has_graphic_model = true;
            vehicle.position = {10.0f, 20.0f, 30.0f};
            vehicle.health = 100;
            vehicle.alive = true;
            Seat seat;
            seat.type = SeatType::Passenger;
            seat.bone_index = 1;
            seat.pose_index = 3; // the posed arm requests sit_3
            seat.seat_local = {1.0f, 2.0f, 3.0f};
            vehicle.seats.push_back(seat);
            const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);
            Entity player;
            player.net_id = 100;
            player.kind = EntityKind::Organic;
            player.player_class = 8;
            player.health = 100;
            player.alive = true;
            const EntityHandle player_h = w.registry.spawn(0, player);
            AiEntity &body = *w.ai.at(w.ai.attach(player_h));
            body.inf.active = true;
            body.net_is_remote_peer = true; // a player body (org2)
            CHECK(w.vehicles.process_attach(player_h, vehicle_h, 1));
            NoSeatBone no_bone;
            SeatBone bone;
            w.pose_provider = resolves ? static_cast<IPoseProvider *>(&bone)
                                       : static_cast<IPoseProvider *>(&no_bone);
            // The carrier moves; a posed rider follows it.
            w.registry.get(vehicle_h)->position = {50.0f, 60.0f, 70.0f};
            const auto seated_at = w.registry.get(player_h)->position.words();
            body.inf.body_heading = 0x10000000;
            body.inf.leg_yaw[0] = body.inf.leg_yaw[1] = 0;
            body.inf.leg_target[0] = body.inf.leg_target[1] = 0;
            const int anim_before = body.inf.anim_state;
            w.ai.is_authority = authority;
            CHECK(w.ai.pose_if_mounted(body, w));
            const Entity &rider = *w.registry.get(player_h);
            CHECK(rider.health == 100);
            if (resolves) {
                CHECK(rider.mounted);
                CHECK(rider.position.words() != seated_at);
                CHECK(body.inf.anim_state == anim_state::kSit + 3);
                continue;
            }
            CHECK(rider.mounted == !authority);
            CHECK(rider.ground_target == (authority ? EntityHandle{} : vehicle_h));
            CHECK(rider.position.words() == seated_at);
            CHECK(body.inf.body_heading == 0x10000000);
            CHECK(body.inf.leg_yaw[0] == 0x10000000 && body.inf.leg_yaw[1] == 0x10000000);
            CHECK(body.inf.leg_target[0] == 0x10000000 && body.inf.leg_target[1] == 0x10000000);
            CHECK(body.inf.anim_state == anim_before);
        }
    }
}

// The authority's REMOTE rider through its whole body update: the seat block is
// the pass's one pose, after the org2 head, so a seat-bone failure detaches the
// rider there and the pass still ends at the mounted site the selector chose: the
// body stays where it sat, its held lean key takes no on-foot ramp (and no seated
// one, its seat type 0 by then), and the 4th-tick selection does not run.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the selector @0x4B6386, the
//  Entity_DetachFromVehicleIfServer call @0x4B6627, the non-local mounted eye
//  @0x4B66D9 and its return @0x4B6907, ahead of the 4th-tick selector @0x4B70CE
//  and the on-foot ramp @0x4B7DBF]
static void test_remote_rider_detach_pass_ends_at_the_mounted_site() {
    struct NoSeatBone final : IPoseProvider {
        bool resolve_seat_bone(World &, const Entity &, int) override { return false; }
    };
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    w.ai.root_motion = &channel_only; // the rider holds its channel
    NoSeatBone no_bone;
    w.pose_provider = &no_bone;
    Entity vehicle;
    vehicle.net_id = 200;
    vehicle.kind = EntityKind::Item;
    vehicle.has_item_def = true;
    vehicle.has_graphic_model = true;
    vehicle.position = {10.0f, 20.0f, 30.0f};
    vehicle.health = 100;
    vehicle.alive = true;
    Seat seat;
    seat.type = SeatType::Passenger;
    seat.bone_index = 1;
    vehicle.seats.push_back(seat);
    const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);
    Entity player;
    player.net_id = 100;
    player.kind = EntityKind::Organic;
    player.player_class = 8;
    player.health = 100;
    player.alive = true;
    const EntityHandle player_h = w.registry.spawn(0, player);
    AiEntity &body = *w.ai.at(w.ai.attach(player_h));
    body.inf.active = true;
    body.health = 100;
    body.net_is_remote_peer = true;
    CHECK(w.vehicles.process_attach(player_h, vehicle_h, 1));
    Entity &rider = *w.registry.get(player_h);
    rider.net_move_input = 0x40; // standing, the lean-left key held
    body.inf.lean_angle = 0;
    body.inf.idle_counter = 0;
    const auto seated_at = rider.position.words();
    w.ai.is_authority = true;
    // A 4th tick: an on-foot standing body's selection counts its idle pass
    // [orig: @0x4B727B..0x4B7293].
    w.ai.update_organic(body, w, 4);
    CHECK(!rider.mounted);
    CHECK(rider.health == 100);
    CHECK(rider.position.words() == seated_at);
    CHECK(body.inf.lean_left);
    CHECK(body.inf.lean_angle == 0);
    CHECK(body.inf.idle_counter == 0);
}

// A player body that dies seated leaves its seat at its death edge on the authority
// alone (Entity_DetachFromVehicleIfServer). Off the authority it keeps the seat, and the
// player body's seat site, which reads the parent and its def with no Health word, still
// poses the dead body there until the record detaches it.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the death edge's detach
//  @0x4B4C08..0x4B4C11, the seat-site selector @0x4B637C..0x4B6392;
//  Entity_DetachFromVehicleIfServer @0x4359D0 (the is_authority test @0x4359D7)]
static void test_a_seated_death_detaches_on_the_authority_alone() {
    for (const bool authority : {true, false}) {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);
        w.ai.root_motion = &channel_only; // the rider holds its channel
        Entity vehicle;
        vehicle.net_id = 200;
        vehicle.kind = EntityKind::Item;
        vehicle.has_item_def = true;
        vehicle.has_graphic_model = true;
        vehicle.position = {10.0f, 20.0f, 30.0f};
        vehicle.health = 100;
        vehicle.alive = true;
        Seat seat;
        seat.type = SeatType::Passenger;
        seat.bone_index = 1;
        vehicle.seats.push_back(seat);
        const EntityHandle vehicle_h = w.registry.spawn(1, vehicle);
        Entity player;
        player.net_id = 100;
        player.kind = EntityKind::Organic;
        player.player_class = 8;
        player.health = 100;
        player.alive = true;
        const EntityHandle player_h = w.registry.spawn(0, player);
        AiEntity &body = *w.ai.at(w.ai.attach(player_h));
        body.inf.active = true;
        body.health = 100;
        CHECK(w.vehicles.process_attach(player_h, vehicle_h, 1));
        Entity &rider = *w.registry.get(player_h);
        rider.health = 0;
        body.health = 0;
        w.ai.is_authority = authority;
        infantry_death_edge(w.ai, body, w, &rider, /*org1=*/false, 4);
        CHECK(rider.mounted == !authority);
        if (!authority) CHECK(w.ai.pose_if_mounted(body, w)); // dead, still seated
    }
}

// Through the whole entity update: the pool-1 walk and the attached-child
// refresh run ahead of the pool-0 bodies, so a body that boards an attached gun
// in its pass's think ends the update where it walked, and the next update
// seats it. [orig: Entity_UpdateInfantryAI mounted-live local
// @0x4B9960..0x4B9985; Entity_UpdateAllEntities, the pool-1 walk
// @0x4C2158..0x4C21F1 ahead of the pool-0 walk @0x4C2426]
static void test_board_pass_on_an_attached_gun_stays_on_foot() {
    World w;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    w.ai.root_motion = &channel_only; // the soldier holds its channel
    Entity carrier;
    carrier.net_id = 30;
    carrier.kind = EntityKind::Item;
    carrier.has_item_def = true;
    carrier.has_graphic_model = true;
    carrier.health = 100;
    carrier.alive = true;
    const EntityHandle carrier_h = w.registry.spawn(1, carrier);
    Entity gun;
    gun.net_id = 31;
    gun.kind = EntityKind::Item;
    gun.has_item_def = true;
    gun.has_graphic_model = true;
    gun.item_type = 6;
    gun.item_attrib = kItemAttribEweap;
    gun.health = 100;
    gun.alive = true;
    gun.position = {20.0f, 0.0f, 0.0f};
    gun.emplacement_parent = carrier_h;
    gun.emplacement_parent_spawn_id = w.registry.get(carrier_h)->registry_spawn_id;
    gun.ground_target = carrier_h;
    Seat seat;
    seat.type = SeatType::Gunner;
    seat.bone_index = 1;
    seat.source_name = "UseGun";
    seat.seat_local = {1.0f, 0.0f, 1.0f};
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w.registry.spawn(1, gun);
    const Vec3 point = entity_local_point_world(*w.registry.get(gun_h), seat.seat_local);
    Entity soldier;
    soldier.net_id = 88; // key = tick + 36 * 88: tick 16 thinks
    soldier.kind = EntityKind::Organic;
    soldier.health = 100;
    soldier.alive = true;
    soldier.position = {point.x + 0.5f, point.y, point.z};
    const EntityHandle soldier_h = w.registry.spawn(0, soldier);
    AiEntity &body = *w.ai.at(w.ai.attach(soldier_h));
    body.inf.active = true;
    body.net_id = 88;
    body.health = 100;
    body.slot.f[37] = kCommandAttachAnySeat;
    body.slot.f[38] = 31;
    const int32_t start[3] = {to_fixed(point.x + 0.5f), to_fixed(point.y), to_fixed(point.z)};
    body.self->position.set_q16({start[0], start[1], start[2]});
    TickContext ctx{};
    ctx.world = &w;
    ctx.is_authority = true;
    ctx.logic_tick = 16;
    w.update_all_entities(ctx);
    CHECK(w.registry.get(soldier_h)->mounted);
    CHECK(w.registry.get(soldier_h)->mount_target == gun_h);
    CHECK(body.self->position.x.q16 == start[0] && body.self->position.y.q16 == start[1]);
    ctx.logic_tick = 17;
    w.update_all_entities(ctx);
    CHECK(std::abs(body.self->position.x.q16 - to_fixed(point.x)) <= 2);
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
        w.ai.root_motion = &channel_only; // the boarders hold their channels
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
        // The attach needs the def (D-NET-422); a def-less carrier loses it
        // after the boarding below.
        vehicle.has_item_def = true;
        vehicle.has_graphic_model = true;
        // Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
        vehicle.item_attrib = kItemAttribPlayerControl;
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
        w.registry.get(vehicle_h)->has_item_def = item_def;
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

// An addeweap child with no anchor row takes its carrier's root pose only while the
// carrier's matrix bit (Flags 0x20000) is clear; with it set (a mover's tail sets it
// every tick, a decoration's model init once) the child keeps its own pose. The
// child's own bit is set either way, and a teleport's clear lets the next compose
// copy again. [orig: Entity_UpdateTransformAndTurret @0x440CA0 (the gate
// @0x4410EA..0x4410F1, the copy @0x441153..0x4411BC, the child's bit @0x4411C2);
// Entity_UpdateVehiclePhysics @0x48AF00 (the tail @0x48D43E..0x48D451);
// Entity_TeleportTeamToSpawn @0x43D390 (the clears)]
void test_an_anchorless_child_copies_only_a_stale_carrier() {
    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.registry.configure_pool(1, 4);
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.has_item_def = true;
    carrier.position = {10.0f, 20.0f, 3.0f};
    carrier.heading = placed_heading_bam(90);
    carrier.health = 100;
    carrier.alive = true;
    const EntityHandle carrier_h = w.registry.spawn(1, carrier);
    Entity child;
    child.kind = EntityKind::Item;
    child.position = {50.0f, 60.0f, 7.0f};
    child.emplacement_parent = carrier_h;
    const EntityHandle child_h = w.registry.spawn(1, child);
    Seat anchor;
    anchor.type = SeatType::Gunner;
    anchor.bone_index = 0; // no anchor row
    anchor.attachment_frame = true;
    anchor.yaw_offset = 30; // an authored row offset the root copy never applies
    const auto compose = [&]() {
        w.vehicles.pose_mounted_occupant(*w.registry.get(child_h), *w.registry.get(carrier_h),
                                         anchor);
    };
    // The carrier's savedLivePose words (+0x80..+0x94), unlike its live words.
    const auto stamp_carrier_saved = [&](int32_t base) {
        Entity &c = *w.registry.get(carrier_h);
        c.saved_live_pos[0] = base + 1;
        c.saved_live_pos[1] = base + 2;
        c.saved_live_pos[2] = base + 3;
        c.saved_live_yaw = base + 4;
        c.saved_live_pitch = base + 5;
        c.saved_live_roll = base + 6;
        c.saved_live_valid = true;
    };
    const auto child_saved_is = [&](int32_t base) {
        const Entity &c = *w.registry.get(child_h);
        return c.saved_live_valid && c.saved_live_pos[0] == base + 1 &&
               c.saved_live_pos[1] == base + 2 && c.saved_live_pos[2] == base + 3 &&
               c.saved_live_yaw == base + 4 && c.saved_live_pitch == base + 5 &&
               c.saved_live_roll == base + 6;
    };
    stamp_carrier_saved(0x10000);
    CHECK(!w.registry.get(child_h)->saved_live_valid);
    // The carrier's bit clear (a mover not yet ticked): the root copy.
    compose();
    CHECK(w.registry.get(child_h)->position.x.q16 == w.registry.get(carrier_h)->position.x.q16);
    CHECK(w.registry.get(child_h)->position.y.q16 == w.registry.get(carrier_h)->position.y.q16);
    // The copy takes the carrier's Yaw word as it is, no seat offset
    // [orig: @0x441151..0x441177].
    CHECK(w.registry.get(child_h)->heading == w.registry.get(carrier_h)->heading);
    // ... and its savedLivePose words as they are [orig: @0x44117A..0x4411B6].
    CHECK(child_saved_is(0x10000));
    CHECK((w.registry.get(child_h)->engine_flags & kEntityFlagMatrixBuilt) != 0);
    // A mover tick moves the carrier, restamps its savedLivePose and sets its bit:
    // the child holds both.
    w.registry.get(carrier_h)->position = {30.0f, 40.0f, 5.0f};
    stamp_carrier_saved(0x20000);
    w.registry.get(carrier_h)->engine_flags |= kEntityFlagMatrixBuilt;
    compose();
    CHECK(w.registry.get(child_h)->position.x == 10.0f &&
          w.registry.get(child_h)->position.y == 20.0f &&
          w.registry.get(child_h)->position.z == 3.0f);
    CHECK(child_saved_is(0x10000));
    // A teleport clears the carrier's bit: the next compose copies again.
    w.registry.get(carrier_h)->engine_flags &= ~kEntityFlagMatrixBuilt;
    compose();
    CHECK(w.registry.get(child_h)->position.x == 30.0f &&
          w.registry.get(child_h)->position.y == 40.0f &&
          w.registry.get(child_h)->position.z == 5.0f);
    CHECK(child_saved_is(0x20000));
}

// An anchored addeweap child's savedLivePose takes its own Position and Yaw / Pitch / Roll
// words as they stood before the compose (never its carrier's, and whatever the carrier's
// matrix bit), so after each compose it holds the previous compose's pose: the class init's
// compose stores its spawn words. [orig: Entity_UpdateTransformAndTurret @0x440CA0 -
// Bone_BuildAttachmentMatrix @0x44109D, savedLivePose @0x4410A2..0x4410D7,
// Math_FixedPointMatrixToEulerAngles @0x4410DD; Entity_InitBoneReferences @0x441470 (the
// class init's update @0x44164B)]
void test_an_anchored_child_saves_its_previous_pose() {
    auto wp = std::make_unique<World>();
    World &w = *wp;
    w.registry.configure_pool(1, 4);
    FakeMountedPoseProvider provider;
    w.pose_provider = &provider;
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.has_item_def = true;
    carrier.position = {10.0f, 20.0f, 3.0f};
    carrier.heading = placed_heading_bam(90);
    carrier.health = 100;
    carrier.alive = true;
    // A mover's tail set the carrier's bit; its savedLivePose words differ from the child's.
    carrier.engine_flags |= kEntityFlagMatrixBuilt;
    carrier.saved_live_pos[0] = 0x10001;
    carrier.saved_live_yaw = 0x10004;
    carrier.saved_live_valid = true;
    const EntityHandle carrier_h = w.registry.spawn(1, carrier);
    Entity child;
    child.kind = EntityKind::Item;
    child.position = {50.0f, 60.0f, 7.0f};
    child.heading = placed_heading_bam(30);
    child.pitch_bam = spawn_angle_bam(4);
    child.roll_bam = spawn_angle_bam(-5);
    child.emplacement_parent = carrier_h;
    const EntityHandle child_h = w.registry.spawn(1, child);
    Seat anchor;
    anchor.type = SeatType::Gunner;
    anchor.bone_index = 2; // the 1-based anchor row
    anchor.attachment_frame = true;
    const auto compose = [&]() {
        w.vehicles.pose_mounted_occupant(*w.registry.get(child_h), *w.registry.get(carrier_h),
                                         anchor);
    };
    const auto child_saved_is = [&](const FixedVec3 &pos, int32_t yaw, int32_t pitch,
                                    int32_t roll) {
        const Entity &c = *w.registry.get(child_h);
        return c.saved_live_valid && c.saved_live_pos[0] == pos.x &&
               c.saved_live_pos[1] == pos.y && c.saved_live_pos[2] == pos.z &&
               c.saved_live_yaw == yaw && c.saved_live_pitch == pitch &&
               c.saved_live_roll == roll;
    };
    CHECK(!w.registry.get(child_h)->saved_live_valid);
    // The class init's compose: the spawn words are saved, the bone frame's are taken.
    const MountedPose first = pose_degrees({11.0f, 12.0f, 13.0f}, 21, -8, 9);
    provider.pose = first;
    compose();
    CHECK(child_saved_is(FixedVec3{to_fixed(50.0f), to_fixed(60.0f), to_fixed(7.0f)},
                         placed_heading_bam(30), spawn_angle_bam(4), spawn_angle_bam(-5)));
    CHECK(w.registry.get(child_h)->position.x.q16 == first.position.x.q16);
    CHECK(w.registry.get(child_h)->heading == first.heading);
    // The next tick's compose saves the first frame's words.
    const MountedPose second = pose_degrees({14.0f, 15.0f, 16.0f}, 25, -4, 3);
    provider.pose = second;
    compose();
    CHECK(child_saved_is(first.position.q16(), first.heading, first.pitch, first.roll));
    CHECK(w.registry.get(child_h)->position.y.q16 == second.position.y.q16);
    CHECK(w.registry.get(child_h)->roll_bam == second.roll);
    // A frame that did not move leaves a zero delta.
    compose();
    CHECK(child_saved_is(second.position.q16(), second.heading, second.pitch, second.roll));
}

// D-NET-586: the process refuses the requester on its dead bit alone: a body at zero
// Health or with its life cleared but no dead bit is still admitted (its death edge
// raises the bit, D-AI-9), and the bit in either Flags view refuses.
// [orig: Entity_ProcessVehicleAttach @0x435AA0 (`test [edi+24h], al` @0x435AFE, the
//  refusal @0x435B01)]
void test_the_rider_gate_reads_the_dead_bit_alone() {
    {
        Rig r;
        r.player().health = 0;
        r.player().alive = false;
        CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
    }
    {
        Rig r;
        r.player().engine_flags |= kEntityFlagDead;
        CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
        CHECK(!r.player().mounted);
    }
}

int main() {
    test_the_rider_gate_reads_the_dead_bit_alone();
    test_an_anchorless_child_copies_only_a_stale_carrier();
    test_an_anchored_child_saves_its_previous_pose();
    test_use_scan_poses_every_seat_kind_live();
    test_claimant_detach_cuts_vehicle_slot_action();
    test_npc_detach_zeroes_the_equipped_byte();
    test_board_pass_stays_on_foot();
    test_request_snaps_to_the_seat_bone_yaw();
    test_seat_bone_failure_runs_the_seat_tail();
    test_player_seat_bone_failure_detaches_on_the_authority();
    test_remote_rider_detach_pass_ends_at_the_mounted_site();
    test_a_seated_death_detaches_on_the_authority_alone();
    test_board_pass_on_an_attached_gun_stays_on_foot();
    test_use_scan_and_label_follow_live_seat_pose();
    test_seat_and_emplacement_keep_subdegree_carrier_pose();
    test_script_remove_releases_carrier_and_occupant_ownership();
	test_vehicle_pending_death_survives_drive_tick();
	test_a_hull_commits_its_death_the_update_after_health_zero();
	test_a_dying_hull_rests_on_its_husk_floor_and_dies_on_the_first_still_visit();
	test_seat_frame_matches_collision_frame();
	test_usegun_attach_presnaps_local_look();
    test_host_crewed_helicopter_rotor_turns();
    test_seated_body_claims_its_vehicle();
    test_epilog_entity_update();
    test_same_team_hold_scan_follows_the_session();
    test_hold_scan_walks_the_refnum_list();
    test_remote_player_control_seat_preserves_wire_look();
    test_remote_passenger_keeps_its_wire_look();
    test_live_pose_provider_and_static_fallback();
    test_npc_seat_bone_failure_kills_boarders_and_detaches();
    test_entity_pose_seat_bone_lookup();
    test_toggle_nearest_seat();
    test_unarmed_usegun_needs_the_session();
    test_usegun_round_trip_reads_the_personal_bucket();
    test_script_mount_reads_the_boot_equipped_slot();
    test_binoculars_survive_all_but_the_camera_reset();
    test_action6_reads_ls_parent_with_retails_gates();
    test_best_seat_walks_vehicle_children();
    test_best_seat_uses_proximity_order();
    test_seat_gates_read_the_dead_bit_alone();
    test_use_scan_reads_the_dead_bit_alone();
    test_label_pass_carrier_block_has_no_type_test();
    test_attach_scan_never_built_fallback_and_initial_empty_slice();
    test_post_epoch_player_spawn_discovers_nearby_seat_immediately();
    test_restore_refreshes_completed_candidate_epoch();
    test_toggle_deck_best_seat();
    test_queued_player_mount_consumes_teleport_fallback_once();
    test_vehicle_respawn_clears_matching_teleport_ground_backup();
    test_toggle_dismount_and_swap();
	test_scan_uses_live_eye_offset();
	test_enemy_on_carried_gun_blocks_root_and_sibling();
	test_carrier_without_a_def_takes_no_rider();
	test_carrier_without_a_model_takes_no_rider();
	test_label_distance_takes_the_unlifted_words();
	test_attach_yaw_snaps_at_the_request_alone();
	test_vehicle_admission_stationary_threshold_and_deck();
	test_seated_board_any_upgrades_on_64_tick_phase();
	test_ai_authored_entry_claim_and_stages();
	test_ai_entry_claim_reads_the_dead_bit_alone();
	test_ai_boarding_moving_and_destroyed_carrier_admission();
	test_enemy_occupant_blocks_scan();
    test_bms_mount_predicates();
    test_mounted_ammo_slot_route();
    test_entity_def_slot_init_seeds_the_words();
    test_sighted_vehicle_weapon_seeds_its_zero_pitch();
    test_entity_def_slot_init_seeds_the_zoom();
    test_ai_drive_leg();
    test_ai_drive_avoid_brake(64);
    test_ai_drive_avoid_brake(9600); // revx02 must not apply one neighbor's brake repeatedly
    test_ai_drive_avoid_quantized_footprints();
    test_stuck_check();
    test_min_ai_crew_clamp();
    test_handbrake_latch();
    test_ground_waits_for_boarders();
    test_zero_health_hull_keeps_its_driver_leg();
    test_submerged_player_driver_takes_the_ai_leg();
    test_mover_command_registers_are_the_brain_words();
    test_helo_ai_flight();
    test_helo_player_boarding_holds_heading();
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
