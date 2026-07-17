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
#include "world/entity.h"
#include "world/player_spawn.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <memory>

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

// The nearest-seat toggle: a free seat within the 4.0 u horizontal gate attaches; out of
// range does nothing. [orig: @0x435d50 gate @0x436123; @0x436950]
void test_toggle_nearest_seat() {
    {
        Rig r(2.0f);
        CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
        CHECK(r.player().mounted);
        CHECK(r.player().mount_target == r.veh_h);
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

// The deck path: standing ON the vehicle (ground_target) picks the BEST seat by weight —
// ctrl/drvr (0x2000) beats sitex (0x200000). [orig: @0x4368cf -> @0x4351f0]
void test_toggle_deck_best_seat() {
    Rig r(30.0f); // out of scan range: only the deck path can mount
    r.player().ground_target = r.veh_h;
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    CHECK(r.player().mount_type == SeatType::Controller);
}

// Toggle while mounted with no other seat in reach = dismount [orig: @0x4369c7].
// With another free seat in reach = swap [orig: @0x4369ac].
void test_toggle_dismount_and_swap() {
    Rig r(2.0f);
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    const int first_seat = r.player().mount_seat;
    // Second toggle: the OTHER free seat is still within reach -> swap, not dismount.
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(r.player().mounted);
    CHECK(r.player().mount_seat != first_seat);
    // Occupy the remaining free seat with someone else -> the next toggle dismounts.
    Entity other;
    other.kind = EntityKind::Organic;
    other.item_id = 5305;
    other.health = 150;
    other.alive = true;
    EntityHandle oh = r.w.registry.spawn(0, other);
    Entity &veh = r.veh();
    for (int i = 0; i < static_cast<int>(veh.seats.size()); ++i) {
        if (!veh.seats[i].occupant.valid()) {
            veh.seats[i].occupant = oh;
            Entity *oe = r.w.registry.get(oh);
            oe->mounted = true;
            oe->mount_target = r.veh_h;
            oe->mount_seat = static_cast<int8_t>(i);
            oe->mount_type = veh.seats[i].type;
            break;
        }
    }
    CHECK(player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(!r.player().mounted);
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
    CHECK(!player_toggle_vehicle_mount(r.w, r.player_h));
    CHECK(!r.player().mounted);
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
    CHECK(cmds.local_player_on_gun_of_ssn(500));
    CHECK(cmds.local_player_attached_to_ssn(11)); // via the carrier link
    CHECK(!cmds.local_player_driving_ssn(500));
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

} // namespace

int main() {
    test_toggle_nearest_seat();
    test_toggle_deck_best_seat();
    test_toggle_dismount_and_swap();
    test_enemy_occupant_blocks_scan();
    test_bms_mount_predicates();
    test_ai_drive_leg();
    test_redirect_and_speed_commands();
    test_local_player_drive_mirror();
    test_player_spawn_group();
    if (failures == 0) std::printf("vehicle_mount_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
