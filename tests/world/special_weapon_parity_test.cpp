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
    Rig moved(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    moved.local.look(0, 3000);
    CHECK(moved.local.input.look_pitch == 0);
    const auto low = moved.fire();
    CHECK(low.fired.round.dir_pitch == 45*degree);
}
void mortar_elevation_survives_rebake_and_ignores_look_keys() {
    Rig r(DEF_WEAPON_FLAG_ABSORBPITCH | DEF_WEAPON_FLAG_FORCESCOPED, -45, 85);
    r.local.look(0, 500); // an intermediate tube elevation
    int32_t before[6], after[6];
    local_weapon_fire_pose(r.world, r.local.weapon, 4, before);
    CHECK(before[4] > 45*degree && before[4] < 85*degree);
    local_weapon_install(r.world, r.local.weapon, r.data, true, true, nullptr, r.local.view);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, after);
    CHECK(after[4] == before[4]);
    r.local.set_view_keys(false, true, false, false, false);
    r.world.logic_tick = 1;
    r.local.apply_player_input_pre_tick();
    CHECK(r.local.input.look_pitch == 0);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, after);
    CHECK(after[4] == before[4]);
    // Recoil enters reload without resetting the tube; ForceCrouch's separate
    // keep-scope behavior remains covered by weapon_fsm_test.
    r.local.weapon.slot.current = weapon_action::kReload;
    r.local.weapon.slot.next = weapon_action::kIdle;
    r.local.weapon.slot.counter = 2;
    LocalWeaponPumpIO io;
    io.view = &r.local.view; io.is_authority = false;
    local_weapon_pump_tick(r.world, r.local.weapon, io);
    local_weapon_fire_pose(r.world, r.local.weapon, 4, after);
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
    local_weapon_fire_pose(r.world, r.local.weapon, 4, aim);
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

void guided_rounds_track_the_target_position() {
    for (auto motor : {ThrowClass::kJavelin,ThrowClass::kStinger}) {
        Rig r;
        r.world.tables.ammo.entries[1].tracer_item_friendly = 10;
        r.world.throwables.classes.set({10,ThrowClass::kNone,motor});
        Entity target;
        target.kind = EntityKind::Item; target.item_id = 2;
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
            CHECK(round.guided.steer[0] == 210*65536 && round.guided.steer[1] == 35*65536);
        }
        r.world.pose_provider = nullptr;
    }
}
}
int main() {
    mortar_elevation_is_independent_of_view_pitch();
    mortar_elevation_survives_rebake_and_ignores_look_keys();
    designator_fire_uses_the_measured_position();
    tank_gunner_fires_from_the_selected_barrel();
    guided_rounds_track_the_target_position();
    tank_input_preserves_non_input_flags();
    if (!failures) std::puts("special_weapon_parity: all checks passed");
    return failures ? 1 : 0;
}
