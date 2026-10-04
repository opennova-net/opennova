// Retail gameplay seams: 0x4DFA40, 0x4E0EC3, 0x4DC750, 0x445DB0,
// 0x446060 and 0x488AB0. The jo-c guided vectors cover the separate motor.
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/hud_combat_feed.h>
#include <runtime/world/local_player.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/throwables.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/world.h>
#include "common/test_paths.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
using namespace opennova::world;
using namespace opennova::def;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
namespace {
constexpr int32_t degree = 11930464;
struct Rig {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    LocalPlayer local{world};
    EntityHandle shooter;
    WeaponInstallData data;
    Rig(int flags = 0, int min_pitch = 0, int max_pitch = 0) {
        world.registry.configure_pool(0, 8); world.registry.configure_pool(1, 8);
        Entity e;
        e.kind = EntityKind::Organic; e.item_type = 3; e.item_id = 1;
        e.alive = true; e.health = 100; e.flags = kEntityFlagPlayer;
        e.team = 1; e.position = {10,20,3}; e.equipped_adm_index = 1;
        shooter = world.registry.spawn(0, e);
        world.cached.local_player = shooter; world.local_player_state = &local;
        world.ai.attach(shooter);
        auto &b = *world.ai.for_handle(shooter);
        b.inf.active = true; b.inf.is_local_player = true;
        b.pos[0] = 10*65536; b.pos[1] = 20*65536; b.pos[2] = 3*65536;
        world.tables.weapons.entries.resize(2);
        auto &weapon = world.tables.weapons.entries[1];
        weapon.valid = true; weapon.name = "WPN_SPECIAL"; weapon.ammo_index = 1;
        weapon.flags = flags; weapon.turret_pitch_min_deg = min_pitch;
        weapon.turret_pitch_max_deg = max_pitch;
        world.tables.ammo.entries.resize(2);
        auto &ammo = world.tables.ammo.entries[1];
        ammo.valid = true; ammo.velocity = 248; ammo.max_age_ticks = 200;
        DefWeaponDef authored{};
        authored.flags = flags; authored.targetpitchmin = min_pitch;
        authored.targetpitchmax = max_pitch;
        data = weapon_install_data_from_def(authored);
        data.name = "WPN_SPECIAL"; data.clipsize = 4; data.rows.resize(3);
        std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "idle");
        std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "fire");
        data.rows[1].delayend = 6;
        std::snprintf(data.rows[2].name, sizeof(data.rows[2].name), "recoil");
        local_weapon_install(world, local.weapon, data, false, false, nullptr, local.view);
    }
    LocalWeaponPumpIO fire() {
        local_weapon_set_input(local.weapon, local.view, true, true, false);
        LocalWeaponPumpIO io;
        io.view = &local.view; io.is_authority = false; io.self_wire_handle = shooter.packed;
        local_weapon_pump_tick(world, local.weapon, io);
        CHECK(io.fired.valid);
        return io;
    }
};
void mortar_elevation_is_independent_of_view_pitch() {
    Rig r(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    const auto fire = r.fire();
    CHECK(fire.fired.round.dir_pitch == 85*degree);
    CHECK(r.local.input.look_pitch == 0);
    // Mouse Y SUBTRACTS from the elevation offset where it adds to the body
    // pitch: pulling back at the 85 degree stop changes nothing, pushing
    // forward lowers the tube to its 45 degree floor.
    // [orig: `sub dword_B79008, ecx` @0x4E0FE2 vs `add [eax+14h], edx` @0x4E0D39]
    Rig pulled(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    pulled.local.look(0, 3000);
    CHECK(pulled.local.input.look_pitch == 0);
    CHECK(pulled.fire().fired.round.dir_pitch == 85*degree);
    Rig moved(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    moved.local.look(0, -3000);
    CHECK(moved.local.input.look_pitch == 0);
    const auto low = moved.fire();
    CHECK(low.fired.round.dir_pitch == 45*degree);
}
// The retail mortar authors AbsorbPitch + OnlyScoped: every AbsorbPitch
// consumer asks Entity_CheckWeaponSeatFlags, which answers nothing for the
// local player until the scope is promoted. Carried unscoped it looks around
// like any other weapon. [orig: @0x540D00; consumers @0x4E0F9D / @0x4DC8A3]
void only_scoped_mortar_absorbs_pitch_once_promoted() {
    Rig r(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_ONLYSCOPED | DEF_WEAPON_FLAG_SIGHTED, -45, 85);
    CHECK(!player_view_scope_settled(r.local.view));
    CHECK(r.local.weapon.pitch_offset_bam == 40*degree); // the mount stamp reads the raw flag
    r.local.look(0, -400);
    CHECK(r.local.input.look_pitch > 0);
    CHECK(r.local.weapon.pitch_offset_bam == 40*degree);
    int32_t pose[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, false, pose);
    CHECK(pose[4] == 0); // the body pitch, no elevation offset
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(pose[4] == 85*degree);
    // Scope-up levels the body pitch for an AbsorbPitch weapon.
    // [orig: Player_ToggleWeaponScope @0x4DF302..0x4DF314]
    auto *slot = active_local_weapon_slot(r.world, r.local.weapon);
    CHECK(local_player_scope_toggle(r.world, r.local.weapon, r.local.view, *slot));
    CHECK(r.local.input.look_pitch == 0);
    r.local.view.scope_settled = true;
    r.local.look(0, -400);
    CHECK(r.local.input.look_pitch == 0);
    CHECK(r.local.weapon.pitch_offset_bam < 40*degree);
}
void mortar_elevation_survives_rebake_and_ignores_look_keys() {
    Rig r(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    r.local.look(0, -500); // an intermediate tube elevation
    int32_t before[6], after[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, before);
    CHECK(before[4] > 45*degree && before[4] < 85*degree);
    local_weapon_install(r.world, r.local.weapon, r.data, true, true, nullptr, r.local.view);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, after);
    CHECK(after[4] == before[4]);
    r.local.set_view_keys(false, true, false, false, false);
    r.world.logic_tick = 1;
    r.local.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(r.local.input.look_pitch == 0);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, after);
    CHECK(after[4] == before[4]);
    // Recoil enters reload without resetting the tube; ForceCrouch's separate
    // keep-scope behavior remains covered by weapon_fsm_test.
    r.local.weapon.slot.current = weapon_action::kReload;
    r.local.weapon.slot.next = weapon_action::kIdle;
    r.local.weapon.slot.counter = 2;
    LocalWeaponPumpIO io;
    io.view = &r.local.view; io.is_authority = false;
    local_weapon_pump_tick(r.world, r.local.weapon, io);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, after);
    CHECK(after[4] == before[4]);
}

void designator_fire_uses_the_measured_position() {
    Rig r;
    r.world.tables.ammo.entries[1].flags = DEF_AMMO_FLAG_DESIGNATETARGET;
    auto &body = *r.world.ai.for_handle(r.shooter);
    body.inf.aim_point[0] = 200*65536;
    body.inf.aim_point[1] = 30*65536;
    body.inf.aim_point[2] = 5*65536;
    int32_t aim[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, aim);
    CHECK(aim[0] == 10*65536); // aiming starts at the shooter
    const auto fire = r.fire();
    CHECK(fire.fired.round.origin_x == 200*65536);
    CHECK(fire.fired.round.origin_y == 30*65536);
    CHECK(fire.fired.round.origin_z == 5*65536);
}

struct Poses : IPoseProvider {
    EntityHandle queried;
    int userpoint = 0;
    const opennova::threedi::Threedi3di3 *model = nullptr;
    bool asked_direction = false;
    int32_t placement_pitch = 0; // the queried carrier's placement pitch
    bool resolve_userpoint_transform(World &, EntityHandle h, int index, int32_t out[6]) override {
        queried = h; userpoint = index;
        const int32_t pose[6] = {14*65536,22*65536,5*65536,5*degree,12*degree,0};
        std::copy_n(pose, 6, out); return true;
    }
    bool resolve_userpoint_frame(World &world, EntityHandle h,
            const opennova::threedi::Threedi3di3 *m, int index, int32_t out[6],
            int32_t out_direction[3]) override {
        model = m;
        asked_direction = out_direction != nullptr;
        if (const Entity *e = world.registry.get(h)) placement_pitch = e->veh.air_pitch_bam;
        if (out_direction != nullptr) {
            out_direction[0] = 65536; out_direction[1] = 0; out_direction[2] = 0;
        }
        return resolve_userpoint_transform(world, h, index, out);
    }
    bool resolve_muzzle_pose(World &, EntityHandle, int32_t out[3]) override {
        out[0] = 800*65536; out[1] = 900*65536; out[2] = 100*65536;
        return true;
    }
};
void tank_gunner_fires_from_the_selected_barrel() {
    for (int clip : {4,3}) {
        Rig r;
        Entity tank;
        tank.kind = EntityKind::Item; tank.has_item_def = true;
        tank.item_type = 1; tank.item_attrib = kItemAttribEweap | kItemAttribPlayerControl;
        tank.position = {12,20,2};
        tank.weapon_userpoint_bytes[clip & 3][0] = uint8_t(7+clip);
        const auto mount = r.world.registry.spawn(1,tank);
        auto &shooter = *r.world.registry.get(r.shooter);
        shooter.mounted = true; shooter.mount_target = mount; shooter.mount_type = SeatType::Gunner;
        r.local.weapon.slot.clip = clip;
        Poses poses; r.world.pose_provider = &poses;
        const auto fire = r.fire();
        CHECK(poses.queried == mount && poses.userpoint == 7+clip);
        CHECK(fire.fired.round.origin_x == 14*65536 && fire.fired.round.origin_z == 5*65536);
        CHECK(fire.fired.round.dir_yaw == 5*degree && fire.fired.round.dir_pitch == 12*degree);
        r.world.pose_provider = nullptr;
    }
}
// A G-attached gun routed to its parent slot fires from the HULL's userpoint
// table. [orig: Entity_CalcWeaponFirePosition @0x4DC7A9..0x4DC7D9]
void redirected_gunner_fires_from_the_hull() {
    Rig r;
    Entity hull;
    hull.kind = EntityKind::Item; hull.has_item_def = true; hull.item_type = 1;
    hull.item_attrib = kItemAttribEweap;
    hull.weapon_userpoint_bytes[0][0] = 21;
    const auto hull_handle = r.world.registry.spawn(1, hull);
    Entity gun;
    gun.kind = EntityKind::Item; gun.has_item_def = true; gun.item_type = 6;
    gun.item_attrib = kItemAttribEweap;
    gun.emplacement_attachment_flags = 2;
    gun.ground_target = hull_handle;
    gun.weapon_userpoint_bytes[0][0] = 9;
    const auto gun_handle = r.world.registry.spawn(1, gun);
    auto &shooter = *r.world.registry.get(r.shooter);
    shooter.mounted = true; shooter.mount_target = gun_handle; shooter.mount_type = SeatType::Gunner;
    Poses poses; r.world.pose_provider = &poses;
    int32_t pose[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.queried == gun_handle && poses.userpoint == 9);
    r.world.registry.get(gun_handle)->primary_weapon_slot.redirect_to_parent_slot = true;
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.queried == hull_handle && poses.userpoint == 21);
    CHECK(!poses.asked_direction);
    // The local-space helper folds an EWEAP hull's view tilt into its Pitch
    // while the point is posed, then restores it.
    // [orig: Entity_ComputeUserpointTransform fold @0x545BAB..0x545BBF,
    //  undone @0x545BF4..0x545C09]
    Entity &tilted = *r.world.registry.get(hull_handle);
    tilted.veh.yaw_seeded = true;
    tilted.veh.air_pitch_bam = 5*degree;
    tilted.veh.view_tilt_bam = -3*degree;
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.queried == hull_handle && poses.placement_pitch == 2*degree);
    CHECK(r.world.registry.get(hull_handle)->veh.air_pitch_bam == 5*degree);
    r.world.pose_provider = nullptr;
}
// A fired def carrying a third-person model fires from its resolved launch
// point on that model, posed through the gun, and never reads the gun's slot
// bytes; a zero launch index is the raw leg (the gun's own pose).
// [orig: Entity_ComputeUserpointWorldTransform gfx3 leg @0x545D06..0x545D85,
//  raw copy @0x545E1F]
void gfx3_weapon_fires_from_its_launch_point() {
    Rig r;
    const auto gfx3 = std::make_shared<opennova::threedi::Threedi3di3>();
    auto &weapon = r.world.tables.weapons.entries[1];
    weapon.third_person_model_asset = gfx3;
    weapon.launch_userpoint = 3;
    Entity gun;
    gun.kind = EntityKind::Item; gun.has_item_def = true; gun.item_type = 6;
    gun.item_attrib = kItemAttribEweap;
    gun.position = {12,20,2};
    for (auto &barrel : gun.weapon_userpoint_bytes) barrel[0] = 9;
    const auto gun_handle = r.world.registry.spawn(1, gun);
    auto &shooter = *r.world.registry.get(r.shooter);
    shooter.mounted = true; shooter.mount_target = gun_handle; shooter.mount_type = SeatType::Gunner;
    Poses poses; r.world.pose_provider = &poses;
    const auto fire = r.fire();
    CHECK(poses.queried == gun_handle && poses.userpoint == 3 && poses.model == gfx3.get());
    CHECK(fire.fired.round.origin_x == 14*65536 && fire.fired.round.origin_z == 5*65536);
    weapon.launch_userpoint = 0;
    poses.userpoint = 0; poses.model = nullptr;
    int32_t pose[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.userpoint == 0 && poses.model == nullptr);
    CHECK(pose[0] == 12*65536 && pose[1] == 20*65536 && pose[2] == 2*65536);
    r.world.pose_provider = nullptr;
}
// The controller helper asks for its point's direction, which turns the fire
// euler; the gunner arms ask for none.
// [orig: Entity_CalcWeaponFirePosition outDirection @0x4DC829 ->
//  Entity_ComputeUserpointTransform @0x4DC83A; gunner @0x4DC7F6 (NULL)]
void controller_fire_asks_for_the_point_direction() {
    Rig r;
    Entity carrier;
    carrier.kind = EntityKind::Item; carrier.has_item_def = true; carrier.item_type = 1;
    carrier.item_attrib = kItemAttribEweap;
    carrier.weapon_userpoint_bytes[0][0] = 11;
    const auto carrier_handle = r.world.registry.spawn(1, carrier);
    auto &shooter = *r.world.registry.get(r.shooter);
    shooter.mounted = true; shooter.mount_target = carrier_handle;
    shooter.mount_type = SeatType::Controller;
    Poses poses; r.world.pose_provider = &poses;
    int32_t pose[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.queried == carrier_handle && poses.userpoint == 11 && poses.asked_direction);
    shooter.mount_type = SeatType::Gunner;
    local_weapon_fire_pose(r.world, r.local.weapon, 4, true, pose);
    CHECK(poses.queried == carrier_handle && !poses.asked_direction);
    r.world.pose_provider = nullptr;
}
// The commander's line starts at the attached gun's point for the gun's own
// slot: a gfx3 def's launch point on that model, else the gun's byte.
// [orig: HUD_DrawScopeOverlayDetails -> Entity_ComputeUserpointWorldTransform
//  @0x59E680, the occupant's slot @0x59E66A]
void commander_line_starts_at_the_gun_launch_point() {
    Rig r;
    CollisionWorld collision;
    r.world.collision = &collision;
    Entity hull;
    hull.kind = EntityKind::Item; hull.has_item_def = true; hull.item_type = 1;
    hull.item_id = 30;
    const auto hull_handle = r.world.registry.spawn(1, hull);
    Entity commander_seat;
    commander_seat.kind = EntityKind::Item; commander_seat.has_item_def = true;
    commander_seat.item_id = 31; commander_seat.ground_target = hull_handle;
    const auto seat_handle = r.world.registry.spawn(1, commander_seat);
    Entity gunner;
    gunner.kind = EntityKind::Organic; gunner.alive = true; gunner.health = 100;
    gunner.mounted = true;
    const auto gunner_handle = r.world.registry.spawn(0, gunner);
    Entity gun;
    gun.kind = EntityKind::Item; gun.has_item_def = true; gun.item_id = 32;
    gun.item_attrib = kItemAttribEweap;
    gun.emplacement_parent = hull_handle;
    gun.emplacement_attachment_flags = 1;
    gun.primary_occupant = gunner_handle;
    gun.primary_weapon_slot_adm = 1;
    gun.position = {12,20,2};
    for (auto &barrel : gun.weapon_userpoint_bytes) barrel[0] = 9;
    const auto gun_handle = r.world.registry.spawn(1, gun);
    auto &shooter = *r.world.registry.get(r.shooter);
    shooter.mounted = true; shooter.mount_target = seat_handle;
    shooter.mount_type = SeatType::Controller;
    r.local.weapon.def.flags |= 8;
    Poses poses; r.world.pose_provider = &poses;
    LocalPlayerViewFrame frame;
    LocalPlayerViewTracker tracker;
    frame.scope_details_active = true;
    fill_hud_combat_view(r.world, r.local.weapon, frame, tracker, true);
    CHECK(frame.hud_combat.commander_valid);
    CHECK(poses.queried == gun_handle && poses.userpoint == 9 && poses.model == nullptr);
    const auto gfx3 = std::make_shared<opennova::threedi::Threedi3di3>();
    r.world.tables.weapons.entries[1].third_person_model_asset = gfx3;
    r.world.tables.weapons.entries[1].launch_userpoint = 4;
    frame = {};
    frame.scope_details_active = true;
    fill_hud_combat_view(r.world, r.local.weapon, frame, tracker, true);
    CHECK(poses.queried == gun_handle && poses.userpoint == 4 && poses.model == gfx3.get());
    r.world.pose_provider = nullptr;
    r.world.collision = nullptr;
}
// The engine provider's direction leg: the record's authored direction through
// the posed bone, and the euler turned by that direction's local yaw.
// [orig: Userpoint_ComputeWorldTransform @0x56C52A..0x56C604]
void pose_provider_turns_the_euler_by_the_point_direction() {
    auto storage = std::make_unique<World>();
    World &world = *storage;
    world.registry.configure_pool(1, 2);
    Entity seed;
    seed.kind = EntityKind::Item; seed.position = {10,20,3}; seed.yaw = 0;
    const auto handle = world.registry.spawn(1, seed);
    opennova::threedi::ThreediUserPoint point{};
    point.x = 65536; point.rot_y = 65536; point.subobject_index = -1;
    opennova::threedi::Threedi3di3 model{};
    model.user_points = &point;
    model.user_point_count = 1;
    EntityPoseProvider provider;
    int32_t plain[6], turned[6], direction[3];
    CHECK(provider.resolve_userpoint_frame(world, handle, &model, 1, plain, nullptr));
    CHECK(provider.resolve_userpoint_frame(world, handle, &model, 1, turned, direction));
    CHECK(turned[0] == plain[0] && turned[1] == plain[1] && turned[2] == plain[2]);
    // Mission yaw 0 is heading 90 deg: local +Y points along world -X, and the
    // point's euler turns a further quarter turn.
    CHECK(direction[0] < -65500 && direction[1] > -16 && direction[1] < 16 && direction[2] == 0);
    const int32_t turn = int32_t(uint32_t(turned[3]) - uint32_t(plain[3]));
    CHECK(turn > 0x40000000 - 0x10000 && turn < 0x40000000 + 0x10000);
    CHECK(!provider.resolve_userpoint_frame(world, handle, &model, 2, plain, nullptr));
}
// The point's part bound, signed: -1 and an index past the part count ride part 0,
// the root, posed by its PANM; the count itself (one past the parts) reads past the
// posed rows in retail and stays in the entity frame here (D-3DI-8). The fixture's
// one part swings on a free-running rotation-x sine, so part 0's frame differs from
// the entity's.
// [orig: Userpoint_ComputeWorldTransform @0x56C474..0x56C489]
void pose_provider_maps_the_userpoint_part_as_retail() {
    opennova::threedi::Threedi3di3 model{};
    const std::string path = std::string(test_paths_repo_root(__FILE__)) +
            "/fixtures/threedi/synth/house_lod0_sine_rotx.3di";
    CHECK(opennova::threedi::threedi_3di3_read(path.c_str(), &model) == 0);
    if (model.lods == nullptr || model.lod_count == 0) return;
    auto storage = std::make_unique<World>();
    World &world = *storage;
    world.registry.configure_pool(1, 2);
    Entity seed;
    seed.kind = EntityKind::Item; seed.position = {10,20,3}; seed.yaw = 0;
    const auto handle = world.registry.spawn(1, seed);
    // One point per part index: the root (0), -1, the count (1), past it (2), -2.
    opennova::threedi::ThreediUserPoint points[5] = {};
    const int parts[5] = {0, -1, 1, 2, -2};
    for (int i = 0; i < 5; ++i) {
        points[i].x = 3 * 65536; points[i].y = 65536; points[i].subobject_index = parts[i];
    }
    opennova::threedi::ThreediUserPoint *saved_points = model.user_points;
    const uint32_t saved_count = model.user_point_count;
    model.user_points = points;
    model.user_point_count = 5;
    EntityPoseProvider provider;
    provider.panm_time_override_ms = 250; // the sine's swing away from rest (zero at 750)
    int32_t at[5][6] = {};
    for (int i = 0; i < 5; ++i)
        CHECK(provider.resolve_userpoint_frame(world, handle, &model, i + 1, at[i], nullptr));
    CHECK(opennova::threedi::threedi_panm_lod_has_live(model, 0) && model.lods[0].render_object_count == 1);
    const auto same = [&](int a, int b) {
        return at[a][0] == at[b][0] && at[a][1] == at[b][1] && at[a][2] == at[b][2];
    };
    CHECK(same(1, 0));   // -1 is the root
    CHECK(same(3, 0));   // past the count is the root
    CHECK(!same(2, 0));  // the count itself is not the posed root...
    CHECK(same(2, 4));   // ...and lands in the entity frame like -2
    model.user_points = saved_points;
    model.user_point_count = saved_count;
    opennova::threedi::threedi_3di3_free(&model);
}
// ctank input does not translate lean keys into bike jump/brake flags.
// [orig: Entity_UpdateTankVehiclePhysics @0x489675..0x4896A7]
void tank_input_preserves_non_input_flags() {
    Rig r;
    VehicleTraits traits; traits.family = VehicleFamily::Tank;
    traits.physics = 1; traits.player_speed = 65536;
    Entity tank;
    auto &pilot = *r.world.registry.get(r.shooter);
    pilot.net_move_input = 0xC9; // forward-left, both lean bits
    stage_player_vehicle_input(r.world, tank, pilot, traits);
    CHECK((tank.flags & 0x28u) == 0);
    CHECK(tank.veh.cmd_speed == 65536 && tank.veh.steer_target_bam > 0);
    tank.flags = 0x28u; pilot.net_move_input = 8;
    stage_player_vehicle_input(r.world, tank, pilot, traits);
    CHECK((tank.flags & 0x28u) == 0x28u);
}

// In flight the motors steer at the target's AIM ORIGIN (a person's phased
// CameraOffset point), not at its raw Position.
// [orig: Entity_ComputeWeaponFireOrigin @0x43B4B0 -- stng @0x4463A3, jvln @0x446C2D;
//  person leg @0x43B4C2..0x43B523]
void guided_rounds_track_the_target_aim_origin() {
    for (auto motor : {ThrowClass::kJavelin,ThrowClass::kStinger}) {
        Rig r;
        r.world.tables.ammo.entries[1].tracer_item_friendly = 10;
        r.world.throwables.classes.set({10,ThrowClass::kNone,motor});
        Entity target;
        target.kind = EntityKind::Organic; target.item_id = 2;
        target.has_item_def = true; target.item_type = 3;
        target.eye_offset_z = 2*65536;
        target.position = {200,30,4}; target.health = 100; target.alive = true;
        const auto h = r.world.registry.spawn(1,target);
        r.world.registry.get(r.shooter)->last_fire_target = h;
        r.world.ai.for_handle(r.shooter)->slot.f[3] = int(h.packed)+1;
        // The acquisition service is separate from live target pose resolution.
        r.world.round_sim.guided_inputs_provider = [h](const LiveRound &, GuidedInputs &in) {
            in.acquisition_ran = true; in.acquired = true; in.acquired_target = h.packed;
            in.owner_target = h.packed;
        };
        Poses poses; r.world.pose_provider = &poses;
        RoundSpawnParams p;
        p.owner = r.shooter; p.shooter_handle = r.shooter.packed;
        p.origin = {10,20,3}; p.ammo_index = 1;
        const int index = r.world.round_sim.spawn(r.world,p,RoundConsequenceMode::VisualOnly);
        CHECK(index >= 0);
        if (index >= 0) {
            auto &round = r.world.round_sim.rounds[index];
            r.world.round_sim.tick(r.world,nullptr);
            r.world.round_sim.tick(r.world,nullptr);
            r.world.registry.get(h)->position = {210,35,6};
            r.world.round_sim.tick(r.world,nullptr);
            CHECK(round.guided.target == h.packed);
            // Phase 0 (tick 0, net id 0): half the XY eye offset plus the
            // (-64, -32) * 64 jitter, never the feet.
            CHECK(round.guided.steer[0] == 210*65536 - 64*64);
            CHECK(round.guided.steer[1] == 35*65536 - 32*64);
        }
        r.world.pose_provider = nullptr;
    }
}
}
// The authority's own local round runs Server_ClientFiredRound's local pass,
// which scores one FIRE event (field 2 plus the FIRE value) for the shooter's
// slot; a joiner's predicted round scores nothing.
// [orig: Entity_FireWeaponAndSendPacket @0x42BD80 (the authority's
//  Server_ClientFiredRound call @0x42BF34); Server_ClientFiredRound @0x50BAA0
//  (the event-1 call @0x50C727)]
void authority_local_fire_scores_one_shot() {
    for (const bool authority : {true, false}) {
        Rig r;
        MatchRules rules;
        rules.game_type = 0x10000u; // TDM
        rules.score_values.emplace();
        (*rules.score_values)[0] = 2; // FIRE
        r.world.match.configure(rules);
        r.world.match.upsert_player({r.shooter, 0, "Local"});
        local_weapon_set_input(r.local.weapon, r.local.view, true, true, false);
        LocalWeaponPumpIO io;
        io.view = &r.local.view;
        io.is_authority = authority;
        io.self_wire_handle = r.shooter.packed;
        local_weapon_pump_tick(r.world, r.local.weapon, io);
        // The authority appends its own ring record; a joiner reports its
        // predicted round for the C2S 0x06 wire instead.
        CHECK(authority ? r.world.out.rounds.count == 1 : io.fired.valid);
        const MatchPlayer *row = r.world.match.player(r.shooter);
        CHECK(row != nullptr && row->stats[MatchStats::kShotsFired] == (authority ? 1 : 0));
        CHECK(row != nullptr && row->stats[MatchStats::kPoints] == (authority ? 2 : 0));
    }
}

int main() {
    authority_local_fire_scores_one_shot();
    mortar_elevation_is_independent_of_view_pitch();
    mortar_elevation_survives_rebake_and_ignores_look_keys();
    designator_fire_uses_the_measured_position();
    only_scoped_mortar_absorbs_pitch_once_promoted();
    tank_gunner_fires_from_the_selected_barrel();
    redirected_gunner_fires_from_the_hull();
    gfx3_weapon_fires_from_its_launch_point();
    controller_fire_asks_for_the_point_direction();
    commander_line_starts_at_the_gun_launch_point();
    pose_provider_turns_the_euler_by_the_point_direction();
    pose_provider_maps_the_userpoint_part_as_retail();
    guided_rounds_track_the_target_aim_origin();
    tank_input_preserves_non_input_flags();
    if (!failures) std::puts("special_weapon_parity: all checks passed");
    return failures ? 1 : 0;
}
