// Retail gameplay seams: 0x4DFA40, 0x4E0EC3, 0x4DC750, 0x445DB0,
// 0x446060 and 0x488AB0. The jo-c guided vectors cover the separate motor.
#include <runtime/world/local_player.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/throwables.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_motor_detail.h>
#include <runtime/world/world.h>
#include <algorithm>
#include <cstdio>
#include <memory>
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
    r.local.apply_player_input_pre_tick();
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
    bool resolve_userpoint_transform(World &, EntityHandle h, int index, int32_t out[6]) override {
        queried = h; userpoint = index;
        const int32_t pose[6] = {14*65536,22*65536,5*65536,5*degree,12*degree,0};
        std::copy_n(pose, 6, out); return true;
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
    r.world.pose_provider = nullptr;
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
int main() {
    mortar_elevation_is_independent_of_view_pitch();
    mortar_elevation_survives_rebake_and_ignores_look_keys();
    designator_fire_uses_the_measured_position();
    only_scoped_mortar_absorbs_pitch_once_promoted();
    tank_gunner_fires_from_the_selected_barrel();
    redirected_gunner_fires_from_the_hull();
    guided_rounds_track_the_target_aim_origin();
    tank_input_preserves_non_input_flags();
    if (!failures) std::puts("special_weapon_parity: all checks passed");
    return failures ? 1 : 0;
}
