// The local player's view cluster, orchestrated (world/local_player_view.h)
// [orig: Player_ToggleWeaponScope @0x4df0c0 — the refusal ladder @0x4df29c /
//  @0x4df12d / @0x4df177; the binocular action 26 gate (g_FireChargeStartTick
//  @0xB76800); the NVG action 41 Inset restore latch; the arbiter feed
//  Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b and the death stamp
//  @0x4b4d00]: the gates in front of the primitives and the order the tick
//  runs them in, pinned where they used to live in the Godot binding.
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <base/io/rotating_prng.h>
#include <cstdio>
#include <vector>

#include <formats/def/def.h>
#include <runtime/controls/binding_set.h>
#include <runtime/controls/player_actions.h>
#include <runtime/hud/tip_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/local_player.h>
#include <runtime/world/collision.h>
#include <cstring>
#include <runtime/world/player_present.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>
#include <runtime/world/weapon_scope_zero.h>

using namespace opennova::world;
using namespace opennova::def;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// A world with one live local organic, the shape the view cluster ticks over.
struct LocalWorld {
    World w;
    AiSystem &ai = w.ai;
    EntityHandle local;

    LocalWorld() {
        w.registry.configure_pool(0, 8);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.item_id = 0x14B9;
        seed.net_id = 1;
        seed.position = {10.0f, 20.0f, 3.0f};
        seed.alive = true;
        seed.health = 100;
        local = w.registry.spawn(0, seed);
        w.cached.local_player = local;
    }
    Entity &entity() { return *w.registry.get(local); }
};

LocalPlayerWeapon scoped_weapon(uint32_t flags, uint32_t flags2 = 0) {
    LocalPlayerWeapon w;
    w.active = true;
    w.def.flags = static_cast<int32_t>(flags);
    w.def.flags2 = static_cast<int32_t>(flags2);
    return w;
}

void settle_ease(PlayerViewState &v) {
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kScopeEaseSteps + 1; ++i) player_view_tick(v, eye);
}

// --- the mounted first-person camera ---------------------------------------
// A seated rider's mode-0 view belongs to the carrier: the `tank` input
// class's virtual-display camera callback, or the gun's posed CAMERA
// userpoint. [orig: Camera_ComputeThirdPersonView @0x437DAC..0x437EAD; the
//  tank callback @0x44A190; Entity_GetBoneWorldPosition @0x545E60]
struct CameraPoses : IPoseProvider {
    EntityHandle queried;
    int userpoint = 0;
    bool resolve_userpoint_transform(World &, EntityHandle h, int index, int32_t out[6]) override {
        queried = h;
        userpoint = index;
        const int32_t pose[6] = {40 * 65536, 50 * 65536, 6 * 65536,
                0x10000000, 0x02000000, 0x00800000};
        std::copy_n(pose, 6, out);
        return true;
    }
};

void test_mounted_first_person_camera_belongs_to_the_carrier() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity *body = lw.ai.for_handle(lw.local);
    body->inf.active = true;
    body->inf.is_local_player = true;
    body->pos[0] = 10 * 65536; body->pos[1] = 20 * 65536; body->pos[2] = 3 * 65536;
    Entity &rider = lw.entity();
    rider.has_item_def = true;
    rider.item_type = 3;
    int32_t out[6];
    CHECK(!local_player_mounted_camera(lw.w, rider, out)); // on foot

    // The stock M1A1: `Virtualdisplay tankdrvr camera` + `input_function tank`,
    // the TankDrvr "Camera" record (1.8267, -0.0968, 0.2702).
    Entity hull;
    hull.has_item_def = true;
    hull.item_type = 1;
    hull.position = {100.0f, 200.0f, 10.0f};
    hull.veh.yaw_seeded = true;
    hull.input_class = 2;
    hull.virtual_display_camera = true;
    hull.virtual_display_camera_q16[0] = 119716;
    hull.virtual_display_camera_q16[1] = -6344;
    hull.virtual_display_camera_q16[2] = 17708;
    const EntityHandle hull_handle = lw.w.registry.spawn(0, hull);
    rider.mounted = true;
    rider.mount_target = hull_handle;
    rider.mount_type = SeatType::Controller;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    // Carrier matrix x record, then 0x3000 back along the rider's view (+X).
    CHECK(out[0] == 100 * 65536 + 119716 - 0x3000);
    CHECK(out[1] == 200 * 65536 - 6344 && out[2] == 10 * 65536 + 17708);
    CHECK(out[3] == 0 && out[4] == 0);
    // The pull-back follows the RIDER's rotation triple, not the hull's.
    body->heading = 0x40000000;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(std::abs(out[0] - (100 * 65536 + 119716)) <= 2);
    CHECK(std::abs(out[1] - (200 * 65536 - 6344 - 0x3000)) <= 2);
    CHECK(out[3] == 0x40000000);
    body->heading = 0;
    // The rider's Roll word is the seat carry's full BAM32 roll: the driver's
    // view banks with the hull smoothly, never in the registry's whole-degree
    // steps. [orig: Camera_ComputeThirdPersonView @0x437D86 reads [esi+18h];
    //  Entity_AttachToBoneAndUpdateTransform @0x5463D0 writes it through
    //  Math_FixedPointMatrixToEulerAngles @0x54656F]
    body->roll = bam_from_degrees_wrapped(2.49);
    rider.roll = 2; // the whole-degree mirror
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(out[5] == body->roll);
    body->roll = 0;
    rider.roll = 0;
    // The `tank` render class draws the virtual display instead of the hull
    // for the local CLAIMANT in mode 0 only. [orig: 0x449EF0 @0x449F12..0x449F27]
    {
        Entity &vehicle = *lw.w.registry.get(hull_handle);
        vehicle.item_id = 100164;
        vehicle.virtual_display_model = "tankdrvr";
        VehicleTraits traits;
        traits.render_family = VehicleRenderFamily::Tank;
        lw.w.vehicles.traits.set(vehicle.item_id, traits);
        PlayerViewState mode;
        CHECK(!local_view_draws_virtual_display(lw.w, mode, vehicle)); // a passenger's hull draws
        vehicle.primary_occupant = lw.local;
        CHECK(local_view_draws_virtual_display(lw.w, mode, vehicle));
        LocalPlayerWeapon none;
        LocalPlayerViewTracker tracker;
        LocalPlayerViewFrame drawn;
        local_player_view_frame(&lw.w, none, mode, tracker, drawn);
        CHECK(drawn.virtual_display_active && drawn.virtual_display_carrier == hull_handle);
        CHECK(drawn.virtual_display_model == "tankdrvr");
        mode.camera_mode = 1;
        mode.third_person = true;
        CHECK(!local_view_draws_virtual_display(lw.w, mode, vehicle)); // the chase draws the hull
        mode = PlayerViewState();
        traits.render_family = VehicleRenderFamily::Ground;
        lw.w.vehicles.traits.set(vehicle.item_id, traits);
        CHECK(!local_view_draws_virtual_display(lw.w, mode, vehicle)); // `cveh` never swaps
        vehicle.primary_occupant = EntityHandle();
    }
    // A troop / null row takes the carrier's Position + CameraOffset and ITS
    // rotation triple. [orig: @0x4DC710]
    Entity &live_hull = *lw.w.registry.get(hull_handle);
    live_hull.input_class = 1;
    live_hull.eye_offset_z = 2 * 65536;
    live_hull.veh.yaw_bam = 0x20000000;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(out[0] == 100 * 65536 && out[2] == 12 * 65536 && out[3] == 0x20000000);
    // All three of the carrier's words are full width: a motorized hull's
    // pitch and roll are its BAM attitude, like its heading, never the
    // whole-degree mirrors. [orig: @0x4DC732..0x4DC741]
    live_hull.veh.air_pitch_bam = bam_from_degrees_wrapped(2.49);
    live_hull.veh.air_roll_bam = bam_from_degrees_wrapped(-1.51);
    live_hull.pitch = 2;
    live_hull.roll = -2;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(out[4] == live_hull.veh.air_pitch_bam && out[5] == live_hull.veh.air_roll_bam);
    live_hull.veh.air_pitch_bam = live_hull.veh.air_roll_bam = 0;
    live_hull.pitch = live_hull.roll = 0;
    // No virtual display on a plain vehicle: the person legs keep the view.
    live_hull.virtual_display_camera = false;
    CHECK(!local_player_mounted_camera(lw.w, rider, out));

    // An EWEAP poses its own CAMERA userpoint: position AND rotation.
    Entity gun;
    gun.has_item_def = true;
    gun.item_type = 6;
    gun.item_attrib = kItemAttribEweap;
    gun.position = {30.0f, 40.0f, 5.0f};
    gun.camera_userpoint_byte = 3;
    const EntityHandle gun_handle = lw.w.registry.spawn(0, gun);
    CollisionWorld collision;
    lw.w.collision = &collision;
    collision.assign_entity(gun_handle, collision.add_model(CollisionModel{}),
            lw.w.registry.get(gun_handle)->registry_spawn_id);
    CameraPoses poses;
    lw.w.pose_provider = &poses;
    rider.mount_target = gun_handle;
    rider.mount_type = SeatType::Gunner;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(poses.queried == gun_handle && poses.userpoint == 3);
    CHECK(out[0] == 40 * 65536 && out[2] == 6 * 65536);
    CHECK(out[3] == 0x10000000 && out[4] == 0x02000000 && out[5] == 0x00800000);
    // The frame hands that pose to the camera in first person only.
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.camera_pose_valid && frame.mounted_camera);
    CHECK(frame.camera.eye[0] == 40.0f && frame.camera.eye[1] == 50.0f && frame.camera.eye[2] == 6.0f);
    CHECK(std::fabs(frame.camera.pitch_deg - 2.8125f) < 1e-4f);
    view.third_person = true;
    view.camera_mode = 1;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(!frame.mounted_camera);
    // Without the userpoint the view is the gun's raw pose; a PlayerControl
    // EWEAP never takes the seat-bone leg.
    lw.w.registry.get(gun_handle)->camera_userpoint_byte = 0;
    CHECK(local_player_mounted_camera(lw.w, rider, out));
    CHECK(out[0] == 30 * 65536 && out[1] == 40 * 65536 && out[2] == 5 * 65536);
    lw.w.registry.get(gun_handle)->item_attrib |= kItemAttribPlayerControl;
    CHECK(!local_player_mounted_camera(lw.w, rider, out));
    lw.w.pose_provider = nullptr;
    lw.w.collision = nullptr;
}

// Entity_GetBoneWorldPosition's rider legs: a non-EWEAP parent hands back the
// rider's Position + CameraOffset and its full-width rotation triple; an
// EWEAP with no model to pose does the same, then adds the rider's +0x94
// word to the pitch. [orig: Entity_GetBoneWorldPosition @0x545E60 — not an
//  EWEAP @0x545EB7..0x545EEA; no skeleton @0x545F17..0x545F4E, the +0x94 add
//  @0x545F48..0x545F4E]
void test_seat_bone_pose_rider_legs() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity *body = lw.ai.for_handle(lw.local);
    body->inf.active = true;
    body->pos[0] = 10 * 65536; body->pos[1] = 20 * 65536; body->pos[2] = 3 * 65536;
    body->heading = 0x12345678;
    body->pitch = 0x01000000;
    body->roll = bam_from_degrees_wrapped(1.3);
    body->inf.eye_offset_z = 0x18000;
    Entity &rider = lw.entity();
    rider.roll = 1; // the whole-degree mirror
    rider.saved_live_roll = 0x00400000;
    Entity parent;
    parent.has_item_def = true;
    parent.item_type = 1;
    parent.position = {30.0f, 40.0f, 5.0f};
    const EntityHandle parent_handle = lw.w.registry.spawn(0, parent);
    rider.mounted = true;
    rider.mount_target = parent_handle;
    rider.mount_type = SeatType::Gunner;
    int32_t out[6];
    CHECK(local_player_seat_bone_pose(lw.w, rider, out));
    CHECK(out[0] == 10 * 65536 && out[1] == 20 * 65536 && out[2] == 3 * 65536 + 0x18000);
    CHECK(out[3] == body->heading && out[4] == body->pitch && out[5] == body->roll);
    // An EWEAP without a model (no collision world to pose it in).
    lw.w.registry.get(parent_handle)->item_attrib = kItemAttribEweap;
    CHECK(local_player_seat_bone_pose(lw.w, rider, out));
    CHECK(out[3] == body->heading && out[5] == body->roll);
    CHECK(out[4] == body->pitch + rider.saved_live_roll);
}

// A seat neither carrier leg admits (no virtual display, not an EWEAP) hands
// the view to the legs an on-foot person takes. On an ordinary ground entity
// that is the person leg — the doubled recoil, the torso + lean/4 roll and
// the 0.1875 pull-back over the seated CameraOffset. While the ground entity
// is a vehicle in its crashed or settled latch, the ground-entity leg sets
// the eye along that vehicle's up axis instead — the longest CameraOffset
// seen as the lift — under the entity's own triple; the next person-leg
// compose clears the lift.
// [orig: Camera_ComputeThirdPersonView @0x437D10 — the def type test
//  @0x437E89, the ground-entity leg @0x437EB5..0x437F97 (its lift
//  @0x437EE5..0x437F0B, Math_ExtractRow2FromFixedPoint22 @0x6137A0 via
//  @0x437F2B, the scaled add @0x437F33..0x437F91), the person leg
//  @0x437F9C..0x438031 clearing the lift @0x437F9C]
void test_unadmitted_seat_takes_the_ground_or_person_leg() {
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity *body = lw.ai.for_handle(lw.local);
    body->inf.active = true;
    body->inf.is_local_player = true;
    body->pos[0] = 10 * 65536; body->pos[1] = 20 * 65536; body->pos[2] = 3 * 65536;
    body->heading = 0x40000000; // mission yaw 0: the view faces +Y
    body->roll = bam_from_degrees_wrapped(1.25);
    body->inf.torso_roll = bam_from_degrees_wrapped(4.0);
    Entity &rider = lw.entity();
    rider.has_item_def = true;
    rider.item_type = 3;
    rider.eye_offset_z = 0x18000; // 1.5 u above the seat
    Entity jeep;
    jeep.has_item_def = true;
    jeep.item_type = 1;
    jeep.position = {10.0f, 20.0f, 2.0f};
    jeep.veh.yaw_seeded = true;
    jeep.veh.yaw_bam = 0x40000000;
    jeep.veh.air_roll_bam = 0x08000000; // banked 11.25 degrees
    const EntityHandle jeep_handle = lw.w.registry.spawn(0, jeep);
    rider.mounted = true;
    rider.mount_target = jeep_handle;
    rider.ground_target = jeep_handle;
    rider.mount_type = SeatType::Passenger;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.camera_pose_valid && !frame.mounted_camera);
    CHECK(near(frame.camera.eye[0], 10.0f) && near(frame.camera.eye[1], 20.0f - 0.1875f));
    CHECK(near(frame.camera.eye[2], 4.5f));
    CHECK(near(frame.camera.roll_deg, 4.0f));

    // The crashed jeep: its up axis times the 1.5 u lift over the Position.
    lw.w.registry.get(jeep_handle)->veh.crashed = 1;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    const int32_t jeep_pos[3] = {10 * 65536, 20 * 65536, 2 * 65536};
    const CollisionMatrix jeep_frame =
            collision_matrix_from_euler(0x40000000, 0, 0x08000000, jeep_pos);
    const int32_t up[3] = {jeep_frame.m[2] >> 6, jeep_frame.m[6] >> 6, jeep_frame.m[10] >> 6};
    CHECK(up[0] != 0 || up[1] != 0); // the bank tips the axis
    CHECK(view.ground_leg_lift_q16 == 0x18000);
    for (int i = 0; i < 3; ++i) {
        const int32_t lift = static_cast<int32_t>((int64_t(up[i]) * 0x18000 + 0x8000) >> 16);
        CHECK(near(frame.camera.eye[i], static_cast<float>((body->pos[i] + lift) / 65536.0)));
    }
    CHECK(near(frame.camera.roll_deg, 1.25f));
    CHECK(near(frame.camera.pitch_deg, 0.0f));
    // A shorter CameraOffset keeps the longest lift; the settle byte admits
    // the leg as well as the crash byte.
    rider.eye_offset_z = 0x10000;
    lw.w.registry.get(jeep_handle)->veh.crashed = 0;
    lw.w.registry.get(jeep_handle)->veh.settle_2f0 = 1;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(view.ground_leg_lift_q16 == 0x18000);
    // Back on the person leg the lift clears.
    lw.w.registry.get(jeep_handle)->veh.settle_2f0 = 0;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(view.ground_leg_lift_q16 == 0);
    CHECK(near(frame.camera.eye[2], 4.0f));
}

// Both shake legs add their yaw delta to the BAM HEADING, so the mission yaw
// (90 - heading) takes it negated while pitch and roll take it as is.
// [orig: `add g_ViewRotYaw, edx` @0x4380D9 in the mode-0 IIR block;
//  `add g_ViewRotPitch` @0x4380D0 / `add g_ViewRotRoll` @0x4380C7]
void test_shake_turns_the_bam_heading() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity *body = lw.ai.for_handle(lw.local);
    body->inf.active = true;
    body->heading = 0x40000000;
    lw.w.weather.core.oscillator.prng = 0x6B8B4567u;
    LocalPlayerWeapon w;
    PlayerViewState v;
    v.shake.counter = 40;
    v.shake.yaw = 0x01234567;
    v.shake.pitch = -0x00ABCDEF;
    v.shake.roll = 0x00765432;
    PlayerViewState quiet = v;
    quiet.shake.counter = 0;
    LocalPlayerViewTracker t;
    LocalPlayerViewFrame base, shaken;
    local_player_view_frame(&lw.w, w, quiet, t, base);
    CameraShakeState step = v.shake;
    int32_t d_yaw = 0, d_pitch = 0, d_roll = 0;
    camera_shake_sample(step, lw.w.weather.core.oscillator.prng, d_yaw, d_pitch, d_roll);
    CHECK(d_yaw != 0 && d_pitch != 0 && d_roll != 0);
    local_player_view_frame(&lw.w, w, v, t, shaken);
    const double deg = 360.0 / 4294967296.0;
    CHECK(std::fabs(shaken.camera.yaw_deg - (base.camera.yaw_deg - float(d_yaw * deg))) < 1e-4f);
    CHECK(std::fabs(shaken.camera.pitch_deg - (base.camera.pitch_deg + float(d_pitch * deg))) < 1e-4f);
    CHECK(std::fabs(shaken.camera.roll_deg - (base.camera.roll_deg + float(d_roll * deg))) < 1e-4f);
    CHECK(v.shake.yaw == step.yaw && v.shake.pitch == step.pitch && v.shake.roll == step.roll);
}

// The mounted chase look-ahead: its target is the carrier's own matrix x
// (6, 0, 0), so a hull pitched up lifts it, and EVERY compose steps it — the
// rendered frame's as well as the quantum's — while an observed frame leaves
// it where it was. [orig: parentMatrix(+0xB4) through
//  Math_TransformPointFixedPoint22 @0x412E90 via @0x438855; the ease
//  @0x43885A..0x4388AF inside Camera_ComputeThirdPersonView @0x437D10,
//  called per quantum @0x526781 and per rendered frame @0x5CA34D]
void test_chase_lookahead_follows_the_hull_on_every_compose() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity *body = lw.ai.for_handle(lw.local);
    body->inf.active = true;
    body->inf.is_local_player = true;
    body->heading = 0x40000000;
    Entity &rider = lw.entity();
    rider.has_item_def = true;
    rider.item_type = 3;
    Entity hull;
    hull.has_item_def = true;
    hull.item_type = 1;
    hull.position = {10.0f, 20.0f, 2.0f};
    hull.bound_radius = 5.0f;
    hull.veh.yaw_seeded = true;
    hull.veh.yaw_bam = 0x40000000; // mission yaw 0
    hull.veh.air_pitch_bam = bam_from_degrees_wrapped(10.0);
    const EntityHandle hull_handle = lw.w.registry.spawn(0, hull);
    rider.mounted = true;
    rider.mount_target = hull_handle;
    rider.mount_type = SeatType::Driver;
    PlayerViewState v;
    v.third_person_selected = true;
    LocalPlayerViewTracker t;
    local_player_view_tick(&lw.w, v, t, {});
    CHECK(v.camera_mode == 1 && v.mount.control_seat);
    const int32_t zero[3] = {0, 0, 0};
    const int32_t ahead[3] = {6 << 16, 0, 0};
    int32_t target[3];
    collision_matrix_from_euler(0x40000000, bam_from_degrees_wrapped(10.0), 0, zero)
            .rotate_point(ahead, target);
    CHECK(target[2] > 0); // the pitched hull lifts the point
    CHECK(v.mount.lookahead_target_q16[0] == target[0] &&
          v.mount.lookahead_target_q16[1] == target[1] &&
          v.mount.lookahead_target_q16[2] == target[2]);
    int32_t expected[3] = {v.lookahead_q16[0], v.lookahead_q16[1], v.lookahead_q16[2]};
    LocalPlayerWeapon w;
    LocalPlayerViewFrame frame;
    for (int f = 0; f < 3; ++f) {
        local_player_view_frame(&lw.w, w, v, t, frame);
        for (int i = 0; i < 3; ++i) expected[i] += (target[i] - expected[i] + 16) >> 5;
        CHECK(v.lookahead_q16[0] == expected[0] && v.lookahead_q16[1] == expected[1] &&
              v.lookahead_q16[2] == expected[2]);
        local_player_view_observe(&lw.w, w, v, t, frame);
        CHECK(v.lookahead_q16[0] == expected[0] && v.lookahead_q16[1] == expected[1] &&
              v.lookahead_q16[2] == expected[2]);
    }
    CHECK(v.lookahead_q16[2] > 0);
}

// --- the scope toggle's refusal ladder ------------------------------------

void test_scope_toggle_refuses_inactive_weapon() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    w.active = false;
    PlayerViewState v;
    WeaponSlotState slot;
    CHECK(!local_player_scope_toggle(lw.w, w, v, slot));
    CHECK(!v.scope_engaged);
}

void test_scope_up_refused_while_moving_on_scoped_weapon() {
    LocalWorld lw;
    // [orig: g_MovementKeyHeld && (flags & 1) -> return @0x4df29c]
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    PlayerViewState v;
    v.move_held = true;
    WeaponSlotState slot;
    CHECK(!local_player_scope_toggle(lw.w, w, v, slot));
    CHECK(!v.scope_engaged);
    v.move_held = false;
    CHECK(local_player_scope_toggle(lw.w, w, v, slot));
    CHECK(v.scope_engaged);
}

void test_inset_scope_refused_under_nvg() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED, DEF_WEAPON_FLAG2_INSET);
    PlayerViewState v;
    v.nvg_active = true;
    WeaponSlotState slot;
    CHECK(!local_player_scope_toggle(lw.w, w, v, slot));
    v.nvg_active = false;
    CHECK(local_player_scope_toggle(lw.w, w, v, slot));
    CHECK(v.scope_engaged);
    // The Inset step count: this poseless fixture's idle zero-delta Setup arms
    // the counter with it [orig: Setup 7 @0x4df355; the zero branch
    // @0x4DE0CC..0x4DE11F].
    CHECK(v.weapon_pose_interp.remaining == static_cast<uint32_t>(kScopeEaseStepsInset));
}

void test_mid_ease_toggle_refused_then_forcescoped_pins_the_sight() {
    LocalWorld lw;
    // [orig: the !activeFlag gate @0x4df177; the ForceScoped pin @0x4df12d]
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED | DEF_WEAPON_FLAG_FORCESCOPED);
    PlayerViewState v;
    WeaponSlotState slot;
    CHECK(local_player_scope_toggle(lw.w, w, v, slot));
    CHECK(v.scope_engaged);
    CHECK(player_view_scope_ease_active(v));
    CHECK(!local_player_scope_toggle(lw.w, w, v, slot)); // mid-ease: refused
    CHECK(v.scope_engaged);
    settle_ease(v);
    CHECK(!player_view_scope_ease_active(v));
    CHECK(!local_player_scope_toggle(lw.w, w, v, slot)); // settled ForceScoped: pinned
    CHECK(v.scope_engaged);
    // Without ForceScoped the settled sight un-scopes.
    LocalPlayerWeapon plain = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    CHECK(local_player_scope_toggle(lw.w, plain, v, slot));
    CHECK(!v.scope_engaged);
}

// --- binoculars ----------------------------------------------------------

void test_binoculars_refused_while_power_throw_charges_and_rng_untouched() {
    LocalWorld lw;
    LocalPlayer player(lw.w);
    player.weapon = scoped_weapon(0);
    player.weapon.power_throw_start_tick = 40;
    const uint32_t before = lw.w.prng16_state;
    CHECK(!local_player_binoculars_toggle(lw.w, player.weapon, player.view, player.view_tracker));
    CHECK(!player.view.binoculars_requested);
    player.present_view_frame();
    CHECK(lw.w.prng16_state == before);
    CHECK(!player.view_tracker.binocular_sway_latched);
}

// Input and fixed ticks must not consume the render-owned seed. Even a
// raise/lower pair between frames leaves the shared mission stream untouched.
// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0, latch @0x5ca3e1..0x5ca3f3]
void test_binocular_sway_seeds_once_per_activation() {
    LocalWorld lw;
    LocalPlayer player(lw.w);
    player.weapon = scoped_weapon(0);
    auto &v = player.view;
    auto &t = player.view_tracker;
    const auto toggle = [&]() {
        return local_player_binoculars_toggle(lw.w, player.weapon, v, t);
    };
    const auto tick = [&]() {
        local_player_view_tick(&lw.w, v, t, {});
    };
    uint32_t expected = lw.w.prng16_state;
    CHECK(toggle());
    tick();
    CHECK(lw.w.prng16_state == expected);
    CHECK(!toggle());
    tick();
    player.present_view_frame();
    CHECK(lw.w.prng16_state == expected);
    CHECK(!t.binocular_sway_latched);

    CHECK(toggle());
    tick();
    CHECK(lw.w.prng16_state == expected);
    // Observing the view is not a rendered frame: it neither draws the seed
    // nor sets the latch.
    player.view_frame();
    CHECK(lw.w.prng16_state == expected);
    CHECK(!t.binocular_sway_latched);
    player.present_view_frame();
    opennova::io::rotating_prng_next16(expected);
    CHECK(lw.w.prng16_state == expected);
    CHECK(t.binocular_sway_latched);
    const float yaw = t.binocular_yaw_offset_deg;
    const float pitch = t.binocular_pitch_offset_deg;
    CHECK(yaw != 0.0f || pitch != 0.0f);
    // No rendered down frame: keep this activation's offsets and seed.
    CHECK(!toggle());
    tick();
    CHECK(toggle());
    tick();
    player.present_view_frame();
    CHECK(lw.w.prng16_state == expected);
    CHECK(t.binocular_yaw_offset_deg == yaw && t.binocular_pitch_offset_deg == pitch);

    // Movement suppresses the optical view. Rendering that state clears the
    // latch; the next rendered activation, not the release tick, draws again.
    v.movement_input = true;
    tick();
    CHECK(!v.binoculars_view_active);
    player.present_view_frame();
    CHECK(!t.binocular_sway_latched);
    v.movement_input = false;
    tick();
    CHECK(lw.w.prng16_state == expected);
    player.present_view_frame();
    opennova::io::rotating_prng_next16(expected);
    CHECK(lw.w.prng16_state == expected);
}

void test_binoculars_refused_scoped_in_gunner_seat() {
    LocalWorld lw;
    Entity &e = lw.entity();
    e.mounted = true;
    e.mount_type = SeatType::Gunner;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    PlayerViewState v;
    v.scope_engaged = true;
    LocalPlayerViewTracker t;
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t));
    v.scope_engaged = false;
    CHECK(local_player_binoculars_toggle(lw.w, w, v, t));
}

// --- NVG: the Inset restore latch -----------------------------------------

void test_nvg_drops_a_settled_inset_scope_and_restores_it() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED, DEF_WEAPON_FLAG2_INSET);
    PlayerViewState v;
    WeaponSlotState slot;
    CHECK(local_player_scope_toggle(lw.w, w, v, slot));
    settle_ease(v);
    CHECK(v.scope_engaged && !player_view_scope_ease_active(v));
    int toggles = 0;
    const auto scope_toggle = [&]() -> bool {
        ++toggles;
        return local_player_scope_toggle(lw.w, w, v, slot);
    };
    // NVG on over a settled Inset scope: the scope drops, the restore latches.
    CHECK(local_player_nvg_toggle(lw.w, w, v, scope_toggle));
    CHECK(v.nvg_active);
    CHECK(toggles == 1);
    CHECK(!v.scope_engaged);
    CHECK(w.nvg_scope_restore);
    settle_ease(v);
    // NVG off: the latch is consumed and the scope comes back up.
    CHECK(!local_player_nvg_toggle(lw.w, w, v, scope_toggle));
    CHECK(!v.nvg_active);
    CHECK(toggles == 2);
    CHECK(v.scope_engaged);
    CHECK(!w.nvg_scope_restore);
}

void test_nvg_over_a_non_inset_scope_leaves_it_alone() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    PlayerViewState v;
    WeaponSlotState slot;
    CHECK(local_player_scope_toggle(lw.w, w, v, slot));
    settle_ease(v);
    int toggles = 0;
    const auto scope_toggle = [&]() -> bool { ++toggles; return false; };
    CHECK(local_player_nvg_toggle(lw.w, w, v, scope_toggle));
    CHECK(toggles == 0);
    CHECK(v.scope_engaged);
    CHECK(!w.nvg_scope_restore);
}

// --- the tip producers: the scope toggle, NVG and the binocular edge -------

// [orig: Player_ToggleWeaponScope — up 11/13/15 @0x4df39c..0x4df3de, down
//  12/14/16 @0x4df241..0x4df282; case 41 — on 7 @0x4e06ec, off 8 @0x4e06a7;
//  Player_UpdatePerFrame — the toggle edge 9/10 @0x4de3e9..0x4de41a]
void test_tip_events_from_scope_nvg_and_binoculars() {
    using opennova::hud::TipEvent;
    const auto events = [](World &w) {
        std::vector<uint8_t> out = w.out.tip_events;
        w.out.tip_events.clear();
        return out;
    };
    {
        LocalWorld lw;
        LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED | DEF_WEAPON_FLAG_SHOWELEVATION);
        PlayerViewState v;
        WeaponSlotState slot;
        CHECK(local_player_scope_toggle(lw.w, w, v, slot));
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventScopeElevationOn});
        settle_ease(v);
        CHECK(local_player_scope_toggle(lw.w, w, v, slot));
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventScopeElevationOff});
        // A plain Scoped sight (no ShowElevation) raises nothing.
        LocalPlayerWeapon plain = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
        settle_ease(v);
        CHECK(local_player_scope_toggle(lw.w, plain, v, slot));
        CHECK(events(lw.w).empty());
    }
    {
        LocalWorld lw;
        LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED | DEF_WEAPON_FLAG_USEDESIGNATOR);
        w.def_name = "wpn_designator";
        PlayerViewState v;
        WeaponSlotState slot;
        CHECK(local_player_scope_toggle(lw.w, w, v, slot));
        CHECK(events(lw.w) == (std::vector<uint8_t>{opennova::hud::kTipEventDesignatorWeaponOn,
                opennova::hud::kTipEventDesignatorOn}));
        settle_ease(v);
        CHECK(local_player_scope_toggle(lw.w, w, v, slot));
        CHECK(events(lw.w) == (std::vector<uint8_t>{opennova::hud::kTipEventDesignatorWeaponOff,
                opennova::hud::kTipEventDesignatorOff}));
    }
    {
        LocalWorld lw;
        LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
        PlayerViewState v;
        const auto no_scope = []() -> bool { return false; };
        CHECK(local_player_nvg_toggle(lw.w, w, v, no_scope));
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventNvgOn});
        CHECK(!local_player_nvg_toggle(lw.w, w, v, no_scope));
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventNvgOff});
    }
    {
        LocalWorld lw;
        LocalPlayerWeapon w = scoped_weapon(0);
        PlayerViewState v;
        LocalPlayerViewTracker t;
        LocalViewSessionInputs s;
        // The first tick seeds the previous toggle: nothing raised.
        local_player_view_tick(&lw.w, v, t, s);
        CHECK(events(lw.w).empty());
        CHECK(local_player_binoculars_toggle(lw.w, w, v, t));
        local_player_view_tick(&lw.w, v, t, s);
        CHECK(v.binoculars_view_active);
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventBinocularsOn});
        local_player_view_tick(&lw.w, v, t, s); // no edge
        CHECK(events(lw.w).empty());
        CHECK(!local_player_binoculars_toggle(lw.w, w, v, t));
        local_player_view_tick(&lw.w, v, t, s);
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventBinocularsOff});
        // Raised while the view cannot come up (the player moving): the fade.
        v.movement_input = true;
        CHECK(local_player_binoculars_toggle(lw.w, w, v, t));
        local_player_view_tick(&lw.w, v, t, s);
        CHECK(!v.binoculars_view_active);
        CHECK(events(lw.w) == std::vector<uint8_t>{opennova::hud::kTipEventBinocularsOff});
    }
}

// --- the tick: arbiter feed, the death stamp edge, the mode-4 entry --------

void test_tick_stamps_the_death_camera_on_the_local_dead_edge() {
    LocalWorld lw;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    LocalViewSessionInputs s;
    lw.w.logic_tick = 100;
    local_player_view_tick(&lw.w, v, t, s);
    CHECK(v.camera_mode == 0);
    CHECK(v.on_foot);
    CHECK(!v.in_session);
    CHECK(v.view_tick == 100);
    CHECK(t.tick_prev_valid);
    // The local dead edge: stamped once at the transition tick, the lerp
    // camera computed on the mode-4 entry, both stable while dead.
    lw.w.logic_tick = 101;
    s.local_dead = true;
    local_player_view_tick(&lw.w, v, t, s);
    CHECK(v.local_dead);
    CHECK(v.camera_mode == 4);
    CHECK(v.death_cam.start_tick == 101);
    lw.w.logic_tick = 102;
    local_player_view_tick(&lw.w, v, t, s);
    CHECK(v.camera_mode == 4);
    CHECK(v.death_cam.start_tick == 101); // no re-stamp while dead
    // The movement delta sampler follows the entity between ticks.
    lw.entity().position.x += 2.0f;
    local_player_view_tick(&lw.w, v, t, s);
    CHECK(t.tick_delta[0] == 2.0f);
    CHECK(t.tick_delta[1] == 0.0f);
}

void test_tick_without_a_player_resolves_first_person() {
    World w;
    PlayerViewState v;
    v.tp_anchor_valid = true;
    v.mount.control_seat = true;
    LocalPlayerViewTracker t;
    LocalViewSessionInputs s;
    local_player_view_tick(&w, v, t, s);
    CHECK(!v.mount.control_seat);
    CHECK(v.camera_mode == 0);
    CHECK(!v.tp_anchor_valid);
    local_player_view_tick(nullptr, v, t, s);
    CHECK(v.camera_mode == 0);
}

// --- the frame read --------------------------------------------------------

void test_scope_fov_target_and_render_queries_share_weather_state() {
    LocalWorld lw;
    lw.w.weather.seed(WeatherSeed{});
    LocalPlayerWeapon weapon = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED);
    weapon.scope_max_mag = 3.0f;
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    auto &channels = lw.w.weather.core.scalar_channels;
    CHECK(local_player_scope_toggle(lw.w, weapon, view, weapon.slot));
    CHECK(weapon.slot.scope_zoom == 3);
    CHECK(channels.camera_fov_target_fp == 1747600); // 80 * trunc(65536/3)
    CHECK(channels.camera_fov_fp == (80 << 16));
    WeatherTickEvents events;
    lw.w.weather.tick_sim(&lw.w, events);
    CHECK(channels.camera_fov_fp == 4805970);
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.fov_h_deg == 4805970.0f / 65536.0f); // independent of 15-step pose
    CHECK(channels.camera_fov_target_fp == 1747600); // no mid-ease reset
    settle_ease(view);
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.scope_card_active);
    CHECK(frame.fov_h_deg == 1747626.0f / 65536.0f); // direct 80/3, then ftol
    weapon.slot.scope_zoom = 2;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.fov_h_deg == 40.0f);
    CHECK(channels.camera_fov_target_fp == (40 << 16));
    CHECK(local_player_scope_toggle(lw.w, weapon, view, weapon.slot));
    CHECK(channels.camera_fov_target_fp == (80 << 16));

    // Scoped optics divide the live weather current and do not replace a
    // scripted target. A resolved third-person view does reset the target.
    weapon.def.flags = DEF_WEAPON_FLAG_SCOPED;
    view = PlayerViewState{};
    channels.camera_fov_fp = 60 << 16;
    channels.camera_fov_target_fp = 50 << 16;
    CHECK(local_player_scope_toggle(lw.w, weapon, view, weapon.slot));
    settle_ease(view);
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.fov_h_deg == 30.0f);
    CHECK(channels.camera_fov_target_fp == (50 << 16));
    view.third_person = true;
    view.camera_mode = 1;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(!frame.scope_card_active && frame.fov_h_deg == 60.0f);
    CHECK(channels.camera_fov_target_fp == (80 << 16));

    // A gunner seat bypasses the optical query's target writer.
    view.third_person = false;
    view.camera_mode = 0;
    weapon.def.flags = DEF_WEAPON_FLAG_SIGHTED;
    lw.entity().mounted = true;
    lw.entity().mount_type = SeatType::Gunner;
    channels.camera_fov_target_fp = 55 << 16;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.scope_card_active && frame.fov_h_deg == 40.0f);
    CHECK(channels.camera_fov_target_fp == (55 << 16));
    local_player_view_reset(&lw.w, weapon, view, tracker);
    CHECK(!view.scope_engaged && !view.weapon_pose_interp.active);
    CHECK(channels.camera_fov_fp == (60 << 16));
    CHECK(channels.camera_fov_target_fp == (80 << 16));
}

// The FP draw's own gates the frame publishes [orig:
// Player_RenderViewModelIfAlive @0x4E0145 (Flags & 2) / @0x4E014B
// (g_EndRoundWinnerTeam); Player_RenderFirstPersonViewModel @0x4DEDD9..0x4DEDF1
// (flags1 & Emplaced skips the showhud bit) / @0x4DEDF7..0x4DEE19 (CanFire &&
// Player_IsEquippedWeaponScoped && flags2 & Inset skips the model)].
void test_frame_publishes_the_fp_draw_gates() {
    LocalWorld lw;
    lw.w.weather.seed(WeatherSeed{});
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;

    LocalPlayerWeapon emplaced = scoped_weapon(DEF_WEAPON_FLAG_EMPLACED);
    local_player_view_frame(&lw.w, emplaced, view, tracker, frame);
    CHECK(frame.fp_def_emplaced);
    CHECK(!frame.fp_inset_scoped && !frame.fp_local_dead && !frame.fp_round_winner_set);

    // A settled scope on an Inset def skips the model; the same scope on a
    // plain Scoped def keeps it (the card switch decides that one).
    LocalPlayerWeapon inset = scoped_weapon(DEF_WEAPON_FLAG_SCOPED, DEF_WEAPON_FLAG2_INSET);
    CHECK(local_player_scope_toggle(lw.w, inset, view, inset.slot));
    settle_ease(view);
    local_player_view_frame(&lw.w, inset, view, tracker, frame);
    CHECK(frame.fp_inset_scoped);
    CHECK(!frame.fp_def_emplaced);
    LocalPlayerWeapon plain = scoped_weapon(DEF_WEAPON_FLAG_SCOPED);
    local_player_view_frame(&lw.w, plain, view, tracker, frame);
    CHECK(!frame.fp_inset_scoped);

    // The alive gate folds from the session each tick: the dead bit and the
    // decided winner of the S2C 0x1D header.
    PlayerViewState ticked;
    LocalViewSessionInputs s;
    s.local_dead = true;
    local_player_view_tick(&lw.w, ticked, tracker, s);
    local_player_view_frame(&lw.w, plain, ticked, tracker, frame);
    CHECK(frame.fp_local_dead);
    CHECK(!frame.fp_round_winner_set);
    s.local_dead = false;
    s.end_round_winner_team = 2;
    local_player_view_tick(&lw.w, ticked, tracker, s);
    local_player_view_frame(&lw.w, plain, ticked, tracker, frame);
    CHECK(!frame.fp_local_dead);
    CHECK(frame.fp_round_winner_set);
}

void test_scope_zoom_clamps_and_weapon_category_fov_reset() {
    LocalWorld lw;
    LocalPlayerWeapon weapon = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED);
    WeaponSlotState slot;
    weapon.scope_max_mag = 4.0f;
    CHECK(local_player_scope_zoom(weapon, slot) == 4);
    slot.scope_zoom = 9;
    CHECK(local_player_scope_zoom(weapon, slot) == 4);
    slot.scope_zoom = -2;
    CHECK(local_player_scope_zoom(weapon, slot) == 0);
    CHECK(local_player_scope_zoom(weapon, slot) == 4);
    weapon.scope_max_mag = 0;
    slot.scope_zoom = 0;
    CHECK(local_player_scope_zoom(weapon, slot) == 1);

    // The zoom STEP [orig: Player_AdjustWeaponElevation @0x4dbdf0]: +/-2 over
    // [scope_min_mag, scope_max_mag] with the click on a change, the CanFire
    // verdict in front (ForceScoped in first person holds it).
    LocalPlayerWeapon stepped = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED | DEF_WEAPON_FLAG_FORCESCOPED);
    stepped.scope_max_mag = 10.0f;
    PlayerViewState step_view;
    WeaponSlotState step_slot;
    ScopeZoomLimits limits;
    limits.category = 3;
    CHECK(local_player_scope_zoom(stepped, step_slot) == 10); // the lazy seed
    lw.w.out.script_sounds.clear();
    CHECK(local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, -2));
    CHECK(step_slot.scope_zoom == 8);
    CHECK(lw.w.out.script_sounds.size() == 1);
    CHECK(lw.w.out.script_sounds[0].name == kScopeZoomStepSoundset);
    CHECK(lw.w.out.script_sounds[0].kind == ScriptSoundEvent::Kind::Interface);
    for (int i = 0; i < 3; ++i) CHECK(local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, -2));
    CHECK(step_slot.scope_zoom == 2);
    CHECK(!local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, -2)); // the floor
    CHECK(step_slot.scope_zoom == 2);
    CHECK(lw.w.out.script_sounds.size() == 4); // no click without a change
    for (int i = 0; i < 4; ++i) CHECK(local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, 2));
    CHECK(step_slot.scope_zoom == 10);
    CHECK(!local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, 2)); // the cap
    CHECK(step_slot.scope_zoom == 10);
    // The class-6 sniper lock on a Primary def without the session bit floors
    // at the max; the bit, or another class/category, restores scope_min_mag
    // [orig: @0x4dbe2f..0x4dbe3f].
    lw.entity().player_class = 6;
    CHECK(local_player_scope_zoom_floor(lw.w, limits, 10) == 10);
    step_slot.scope_zoom = 8;
    CHECK(local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, -2));
    CHECK(step_slot.scope_zoom == 10);
    lw.w.rules.allow_sniper_scope_zoom = true; // byte_A821F0
    CHECK(local_player_scope_zoom_floor(lw.w, limits, 10) == 2);
    lw.w.rules.allow_sniper_scope_zoom = false;
    limits.category = 2;
    CHECK(local_player_scope_zoom_floor(lw.w, limits, 10) == 2);
    lw.entity().player_class = 8;
    limits.category = 3;
    limits.scope_min_mag = 4;
    CHECK(local_player_scope_zoom_floor(lw.w, limits, 10) == 4);
    // The CanFire gate: no equipped weapon, no step, no click.
    stepped.active = false;
    lw.w.out.script_sounds.clear();
    CHECK(!local_player_adjust_scope_zoom(lw.w, stepped, step_view, step_slot, limits, -2));
    CHECK(step_slot.scope_zoom == 10 && lw.w.out.script_sounds.empty());

    for (int i = 0; i < 3; ++i) {
        WeaponTableEntry row;
        row.valid = true;
        row.name = "fov_weapon_" + std::to_string(i);
        row.category = i == 2 ? 2 : 1;
        lw.w.tables.weapons.entries.push_back(row);
    }
    weapon.def_name = "fov_weapon_0";
    WeaponInstallData data;
    data.name = "fov_weapon_1";
    PlayerViewState view;
    auto &channels = lw.w.weather.core.scalar_channels;
    channels.camera_fov_fp = 30 << 16;
    channels.camera_fov_target_fp = 35 << 16;
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    CHECK(channels.camera_fov_target_fp == (35 << 16)); // same category
    data.name = "fov_weapon_2";
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    CHECK(channels.camera_fov_target_fp == (80 << 16));
    CHECK(channels.camera_fov_fp == (30 << 16));
    channels.camera_fov_target_fp = 35 << 16;
    data.name = "fov_weapon_0";
    data.flags = DEF_WEAPON_FLAG_FORCESCOPED;
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    CHECK(channels.camera_fov_target_fp == (35 << 16));
}

// The weapon-cycle actions' dispatcher leg [orig: Input_HandleActionBinding_0
// cases 0xD4/0xD6 @0x4e130c..0x4e13ae] and the mount-time zoom clamp
// [orig: Player_MountWeaponSlot @0x4dfacf..0x4dfb16], over the limits built
// from the weapon table's category and the rules' allowSniperScopeZoom bit.
void test_weapon_cycle_route_steps_the_zoom_and_the_mount_clamp() {
    LocalWorld lw;
    WeaponTableEntry row;
    row.valid = true;
    row.name = "route_primary";
    row.category = 3;
    lw.w.tables.weapons.entries.push_back(row);
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED | DEF_WEAPON_FLAG_FORCESCOPED);
    w.def_name = "route_primary";
    w.scope_max_mag = 10.0f;
    PlayerViewState v;
    const ScopeZoomLimits limits = local_player_scope_zoom_limits(lw.w, w, 2);
    CHECK(limits.category == 3 && limits.scope_min_mag == 2);
    LocalPlayerWeapon unknown = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED);
    unknown.def_name = "route_unknown";
    CHECK(local_player_scope_zoom_limits(lw.w, unknown, 4).category == 0);
    CHECK(local_player_scope_zoom_limits(lw.w, unknown, 4).scope_min_mag == 4);

    // The mount clamp: Sighted-only (no Flags & 1) and a zero max leave the slot
    // alone; a Scoped def lands a fresh slot on the floor and an over-max
    // carry-over on the max.
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 0);
    w.def.flags |= static_cast<int32_t>(DEF_WEAPON_FLAG_SCOPED);
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 0, w.slot);
    CHECK(w.slot.scope_zoom == 0);
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 2);
    w.slot.scope_zoom = 14;
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 10);
    w.slot.scope_zoom = 6;
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 6);
    // The class-6 lock at mount floors the zoom at the max; the session bit
    // (World::rules.allow_sniper_scope_zoom = byte_A821F0) lifts it.
    lw.entity().player_class = 6;
    w.slot.scope_zoom = 4;
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 10);
    lw.w.rules.allow_sniper_scope_zoom = true;
    w.slot.scope_zoom = 4;
    local_player_scope_zoom_mount_clamp(lw.w, limits, w.def.flags, 10, w.slot);
    CHECK(w.slot.scope_zoom == 4);
    lw.w.rules.allow_sniper_scope_zoom = false;
    lw.entity().player_class = 8;

    // The route: cycleweaponP (+1) steps +2, cycleweaponN (-1) steps -2 with the GF_SCOPE click,
    // and no cycle; at the cap the step still takes the route, just silently.
    lw.w.out.script_sounds.clear();
    w.slot.scope_zoom = 4;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, 1) == WeaponCycleRoute::kZoomStep);
    CHECK(w.slot.scope_zoom == 6);
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, -1) == WeaponCycleRoute::kZoomStep);
    CHECK(w.slot.scope_zoom == 4);
    CHECK(lw.w.out.script_sounds.size() == 2);
    CHECK(lw.w.out.script_sounds[1].name == kScopeZoomStepSoundset);
    w.slot.scope_zoom = 10;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, 1) == WeaponCycleRoute::kZoomStep);
    CHECK(w.slot.scope_zoom == 10 && lw.w.out.script_sounds.size() == 2);
    // Refused outright: the binocular view, then a live PowerThrow charge.
    v.binoculars_view_active = true;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, 1) == WeaponCycleRoute::kRefused);
    v.binoculars_view_active = false;
    w.power_throw_start_tick = 5;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, -1) == WeaponCycleRoute::kRefused);
    w.power_throw_start_tick = 0;
    CHECK(w.slot.scope_zoom == 10);
    // scope_min_mag == scope_max_mag (the shipped 2x optics) cycles; so does a
    // lowered optical view (no ForceScoped pin, hip view) and no equipped weapon.
    ScopeZoomLimits fixed = limits;
    fixed.scope_min_mag = 10;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, fixed, 1) == WeaponCycleRoute::kCycle);
    w.def.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_SCOPED);
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, 1) == WeaponCycleRoute::kCycle);
    CHECK(w.slot.scope_zoom == 10);
    w.active = false;
    CHECK(local_player_weapon_cycle_route(lw.w, w, v, limits, 1) == WeaponCycleRoute::kCycle);
    CHECK(lw.w.out.script_sounds.size() == 2);
}

// Exercise the complete default mouse binding -> action -> optical route.
// [orig: catalog P/N @0x816A1C/@0x816A88; Input_HandleActionBinding_0 @0x4E0420]
void test_default_wheel_binding_zooms_in_away_from_the_player() {
    using namespace opennova::controls;
    LocalWorld lw;
    LocalPlayerWeapon weapon = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED | DEF_WEAPON_FLAG_FORCESCOPED);
    weapon.scope_max_mag = 10;
    weapon.slot.scope_zoom = 4;
    PlayerViewState view;
    ScopeZoomLimits limits;
    limits.scope_min_mag = 2;
    BindingSet bindings;
    std::size_t count = 0;
    const auto *rows = catalog(&count);
    const auto step = [&](uint16_t wheel, int expected) {
        const int index = bindings.mouse_event_action(wheel, [](int) { return false; });
        CHECK(index >= 0 && static_cast<std::size_t>(index) < count);
        if (index < 0 || static_cast<std::size_t>(index) >= count) return;
        const auto request = player_wheel_action(rows[index].token);
        CHECK(request && request->action == PlayerAction::WeaponCycle);
        if (!request) return;
        CHECK(local_player_weapon_cycle_route(lw.w, weapon, view, limits, request->value) == WeaponCycleRoute::kZoomStep);
        CHECK(weapon.slot.scope_zoom == expected);
    };
    step(kMouseWheelUp, 6);
    step(kMouseWheelUp, 8);
    step(kMouseWheelUp, 10);
    step(kMouseWheelUp, 10);
    step(kMouseWheelDown, 8);
    step(kMouseWheelDown, 6);
    step(kMouseWheelDown, 4);
    step(kMouseWheelDown, 2);
    step(kMouseWheelDown, 2);
    CHECK(lw.w.out.script_sounds.size() == 7);
}

void test_frame_reads_the_state_and_the_card_selector() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED);
    w.scope_max_mag = 4.0f;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    t.binocular_yaw_offset_deg = 1.5f;
    LocalPlayerViewFrame f;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.scope_engaged);
    CHECK(f.binocular_yaw_offset_deg == 1.5f);
    CHECK(f.camera_mode == 0);
    CHECK(!f.camera_mounted);
    CHECK(!f.scope_card_active);
    CHECK(f.fov_h_deg == kPlayerCameraFovHDeg);
    CHECK(!f.camera_pose_valid); // no AI entity attached: no composed pose
    // The engaged, PROMOTED sight on a Sighted weapon selects the card (first
    // person, no binoculars) [orig: Render_ProcessMainSceneFrame @0x5ca299].
    CHECK(player_view_set_engaged(v, true, false));
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.scope_engaged && !f.scope_settled && !f.scope_card_active); // mid-raise
    settle_ease(v);
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.scope_fraction == 1.0f);
    CHECK(f.scope_settled && f.scope_card_active);
    CHECK(f.fov_h_deg == kPlayerCameraFovHDeg / 4.0f);
    v.binoculars_view_active = true;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.scope_card_active); // the optical view wins over the card
    CHECK(f.fov_h_deg == kBinocularCameraFovHDeg);
}

// The FrameFX dispatch facts the frame carries for the terminal effect: the
// RAW red word (not the capped vignette alpha), the dead/session bits, the
// ticks since the death stamp, g_NVGActive, and the CanFire latches of the
// equipped def's Thermal (flags2 & 4) and Monitor (flags2 & 8) bits.
// [orig: Render_ProcessMainSceneFrame @0x5ca2da..0x5ca2f1; @0x5ca8f6..0x5caad5]
void test_frame_carries_the_framefx_dispatch_facts() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED,
                                        DEF_WEAPON_FLAG2_THERMAL | DEF_WEAPON_FLAG2_MONITOR);
    w.scope_max_mag = 4.0f;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    LocalPlayerViewFrame f;
    v.flash.red = 250;
    v.nvg_active = true;
    v.in_session = true;
    v.local_dead = true;
    v.view_tick = 400;
    v.death_cam.start_tick = 150;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.red_word == 250);
    CHECK(f.screen_flash_red_alpha == 0xC0); // the vignette's capped draw alpha
    CHECK(f.frame_fx.nvg_active);
    CHECK(f.frame_fx.in_session && f.frame_fx.local_dead);
    CHECK(f.frame_fx.death_elapsed_ticks == 250);
    CHECK(f.frame_fx.camera_mode == 0);
    CHECK(!f.frame_fx.thermal_view && !f.frame_fx.monitor_view); // no optical view yet
    // A raised, settled sight is the CanFire verdict: both latches follow it.
    v.local_dead = false;
    CHECK(player_view_set_engaged(v, true, false));
    settle_ease(v);
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.thermal_view && f.thermal_view);
    CHECK(f.frame_fx.monitor_view);
    w.def.flags2 = DEF_WEAPON_FLAG2_MONITOR;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.frame_fx.thermal_view && f.frame_fx.monitor_view);
}

// The NVG arms: the Scoped byte (a settled non-Inset Scoped sight) routes the
// NVG composite through the lens and drops the NVG.tga mask; the binocular
// byte rides along. [orig: Render_ProcessMainSceneFrame @0x5ca2be..0x5ca304,
// @0x5ca6f5..0x5ca71a]
void test_frame_carries_the_nvg_lens_arm() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SCOPED, 0);
    w.scope_max_mag = 4.0f;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    LocalPlayerViewFrame f;
    v.nvg_active = true;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.frame_fx.scoped_selector && !f.frame_fx.binoculars_view_active);
    CHECK(f.nvg_mask_visible && !f.nvg_lens_active);
    CHECK(player_view_set_engaged(v, true, false));
    settle_ease(v);
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.scoped_selector);
    CHECK(f.nvg_lens_active && !f.nvg_mask_visible);
    // Inset takes the other byte: no lens.
    w.def.flags2 = DEF_WEAPON_FLAG2_INSET;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.frame_fx.scoped_selector && !f.nvg_lens_active && f.nvg_mask_visible);
    // NVG off: neither.
    w.def.flags2 = 0;
    v.nvg_active = false;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.scoped_selector && !f.nvg_lens_active && !f.nvg_mask_visible);
}

// The Sighted byte routes the NVG scene through the Sighted arm: the card
// goes into the scene, the mask stays. [orig: Render_ProcessMainSceneFrame
// @0x5ca2cc..0x5ca2d5, @0x5ca57f..0x5ca591]
void test_frame_carries_the_nvg_sighted_arm() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED, 0);
    w.scope_max_mag = 4.0f;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    LocalPlayerViewFrame f;
    v.nvg_active = true;
    CHECK(player_view_set_engaged(v, true, false));
    settle_ease(v);
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.sighted_selector && !f.frame_fx.scoped_selector);
    CHECK(f.nvg_sights_in_scene && !f.nvg_lens_active && f.nvg_mask_visible);
    v.nvg_active = false;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.frame_fx.sighted_selector && !f.nvg_sights_in_scene);
}

// The camera's airborne skip is independent of the ongoing scope interp.
// [orig: Player_UpdateFirstPersonCamera @0x4dd40d/0x4dd414 and @0x4dd49f/0x4dd4a6]
void test_authored_rotation_bias_continues_through_air_reload_and_rebake() {
    LocalWorld lw;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    WeaponInstallData data;
    data.name = "AUTHORED_ROTATION";
    data.flags = DEF_WEAPON_FLAG_SIGHTED;
    data.view_hip_pose = {{256, 512, 768}, {0x10000000u, 0x10000000u, 0xF0000000u}};
    data.view_ads_pose = {{512, -256, 1024}, {0x08000000u, 0x20000000u, 0x10000000u}};
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    CHECK(local_player_scope_toggle(lw.w, weapon, view, weapon.slot));
    const float eye[3] = {};
    for (int i = 0; i < 3; ++i) player_view_tick(view, eye);
    int32_t out[3];
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0xFE666668u);
    CHECK(static_cast<uint32_t>(out[1]) == 0x10000000u);
    CHECK(static_cast<uint32_t>(out[2]) == 0x20000000u);

    lw.entity().flags |= kEntityFlagInAir;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    for (int i = 0; i < 4; ++i) player_view_tick(view, eye);
    CHECK(view.weapon_pose_interp.current.rotation_bam[0] == 0x0C444448u);
    CHECK(view.scope_engaged); // suppression neither cancels nor resets ADS
    lw.entity().flags &= ~kEntityFlagInAir;
    lw.entity().engine_flags |= kEntityFlagInAir;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

    // ForceScoped and NoCardSwitch never bypass the airborne camera gate.
    weapon.def.flags |= DEF_WEAPON_FLAG_FORCESCOPED | DEF_WEAPON_FLAG_NOCARDSWITCH;
    weapon.slot.current = weapon_action::kReload;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    lw.entity().engine_flags &= ~kEntityFlagInAir;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0xFC444448u);
    CHECK(out[1] == 0x10000000 && out[2] == 0x20000000);
    weapon.def.flags &= ~DEF_WEAPON_FLAG_NOCARDSWITCH;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    for (int i = 0; i < 10; ++i) player_view_tick(view, eye);
    weapon.slot.current = weapon_action::kIdle;
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0xF8000000u);
    CHECK(out[1] == 0x10000000 && out[2] == 0x20000000);

    // Render-side clip rebaking the same installed weapon preserves the
    // current authored pose. A real new mount resets the published bias.
    local_weapon_install(lw.w, weapon, data, false, true, nullptr, view);
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0xF8000000u);
    data.name = "NEW_ROTATION_MOUNT";
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    CHECK(view.weapon_pose_interp.active); // promoted same-category mount sets up one step
    player_view_tick(view, eye);
    local_player_viewmodel_rotation_bias(&lw.w, weapon, view, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0xF8000000u);
}

void test_airborne_view_bias_keeps_interp_and_resumes_on_landing() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    AiEntity &body = *lw.ai.for_handle(lw.local);
    LocalPlayerWeapon weapon = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED);
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    // Raw file units; the sight sits 240 / -240 / 480 from the hip so the *256
    // Q16 spans (61440 / -61440 / 122880) divide by 15 into exact 4096 steps.
    const float hip[3] = {256.0f, 512.0f, 768.0f};
    const float sight[3] = {496.0f, 272.0f, 1248.0f};
    view.weapon_pose_bound = true;
    for (int i = 0; i < 3; ++i) {
        view.weapon_hip_pose.position_q16[i] = hip[i] * 256.0f;
        view.weapon_ads_pose.position_q16[i] = sight[i] * 256.0f;
    }
    const float eye[3] = {};
    const auto check_bias = [&](float x, float y, float z) {
        float bias[3];
        local_player_viewmodel_bias(&lw.w, weapon, view, tracker, hip, 1920, 1080, bias);
        CHECK(std::abs(bias[0] - x) < 0.000001f);
        CHECK(std::abs(bias[1] - y) < 0.000001f);
        CHECK(std::abs(bias[2] - z) < 0.000001f);
    };
    CHECK(local_player_scope_toggle(lw.w, weapon, view, weapon.slot));
    for (int i = 0; i < 3; ++i) player_view_tick(view, eye);
    check_bias(1.1875f, 1.8125f, 3.375f); // three of the fifteen raise steps: 3 * 4096 Q16

    // The jump publishes 0x2000 immediately. Its upward velocity is not the
    // gate: the same flag continues to suppress bias while the body falls.
    lw.entity().flags |= kEntityFlagInAir;
    lw.entity().engine_flags |= kEntityFlagInAir;
    body.inf.airborne = true;
    body.inf.vel[2] = 0x1600;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.suppress_view_bias && view.scope_engaged);
    CHECK(view.weapon_pose_interp.position_bias_q16[0] == 3 * 4096);
    check_bias(1.0f, 2.0f, 3.0f);
    body.inf.vel[2] = -0x1600;
    for (int i = 0; i < 5; ++i) player_view_tick(view, eye);
    CHECK(view.weapon_pose_interp.position_bias_q16[0] == 8 * 4096);
    CHECK(player_view_scope_ease_active(view));
    check_bias(1.0f, 2.0f, 3.0f);

    // Landing exposes the current interpolated pose, not a new fifteen-step
    // raise and not the stale pose from the jump's first rendered frame.
    lw.entity().flags &= ~kEntityFlagInAir;
    lw.entity().engine_flags &= ~kEntityFlagInAir;
    body.inf.airborne = false;
    body.inf.vel[2] = 0;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(!frame.suppress_view_bias && view.weapon_pose_interp.position_bias_q16[0] == 8 * 4096);
    // 8 of 15 steps: 8 * 4096 / 65536 = 0.5 u on the 240-unit x/y lanes and
    // 8 * 8192 / 65536 = 1.0 u on the 480-unit z lane (each lane eases its own span).
    check_bias(1.5f, 1.5f, 4.0f);

    // Falling during the remaining ease also leaves the promoter running.
    // Cover the second registry flag carrier independently.
    lw.entity().engine_flags |= kEntityFlagInAir;
    body.inf.airborne = true;
    body.inf.vel[2] = -0x1600;
    settle_ease(view);
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.scope_settled && frame.suppress_view_bias && !frame.scope_card_active);
    check_bias(1.0f, 2.0f, 3.0f);
    lw.entity().engine_flags &= ~kEntityFlagInAir;
    body.inf.airborne = false;
    body.inf.vel[2] = 0;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.scope_card_active && !frame.suppress_view_bias);
    check_bias(1.9375f, 1.0625f, 4.875f); // the snapped sight: 496 / 272 / 1248 over 256

    // Only the ADS contribution drops: the independently clamped motion lead
    // and 4:3 framing drop still follow the hip offset on an airborne frame.
    lw.entity().flags |= kEntityFlagInAir;
    tracker.tick_delta[0] = 1.0f;
    tracker.tick_delta[1] = -1.0f;
    tracker.tick_delta[2] = 4.0f;
    float bias[3];
    local_player_viewmodel_bias(&lw.w, weapon, view, tracker, hip, 800, 600, bias);
    CHECK(bias[0] == 1.0f - 1024.0f / 65536.0f);
    CHECK(bias[1] == 2.0f + 1024.0f / 65536.0f);
    CHECK(bias[2] == 3.0f - (4096.0f + 1280.0f) / 65536.0f);
}

void test_airborne_bias_is_separate_from_reload_and_force_scope_admission() {
    // Original-machine probes of both skip blocks produce base (10,20,30)
    // while suppressed, otherwise base+bias (110,220,330). NoCardSwitch
    // exempts RELOAD only; ForceScoped does not exempt either bias skip.
    // Optical admission has its own ForceScoped override @0x5cf845.
    struct Case {
        uint32_t body_flags;
        bool reload;
        bool no_card;
        bool force;
        bool suppressed;
        bool optical;
    };
    const Case cases[] = {
        {0, false, false, false, false, true},
        {kEntityFlagInAir, false, false, false, true, false},
        {0, true, false, false, true, false},
        {0, true, true, false, false, true},
        {kEntityFlagInAir, true, true, false, true, false},
        {kEntityFlagInAir, false, false, true, true, true},
        {kEntityFlagInAir, true, false, true, true, false},
        {kEntityFlagInAir, true, true, true, true, true},
        {0, true, true, true, false, true},
        {kEntityFlagDrowning, false, false, false, false, true},
    };
    for (const Case &c : cases) {
        LocalWorld lw;
        lw.entity().flags |= c.body_flags;
        LocalPlayerWeapon weapon = scoped_weapon(DEF_WEAPON_FLAG_SIGHTED |
                (c.no_card ? weapon_flag::kNoCardSwitch : 0u) |
                (c.force ? DEF_WEAPON_FLAG_FORCESCOPED : 0u));
        weapon.slot.current = c.reload ? weapon_action::kReload : weapon_action::kIdle;
        const float hip[3] = {256.0f, 512.0f, 768.0f};
        const float sight[3] = {512.0f, -256.0f, 1024.0f};
        PlayerViewState view;
        view.weapon_pose_bound = true;
        for (int i = 0; i < 3; ++i) {
            view.weapon_hip_pose.position_q16[i] = hip[i] * 256.0f;
            view.weapon_ads_pose.position_q16[i] = sight[i] * 256.0f;
        }
        CHECK(player_view_set_engaged(view, true, false));
        settle_ease(view);
        LocalPlayerViewTracker tracker;
        LocalPlayerViewFrame frame;
        local_player_view_frame(&lw.w, weapon, view, tracker, frame);
        CHECK(frame.suppress_view_bias == c.suppressed);
        CHECK(local_player_scope_view_visible(lw.w, weapon, view) == c.optical);
        CHECK(view.scope_engaged && view.scope_settled && !view.weapon_pose_interp.active);
        float bias[3];
        local_player_viewmodel_bias(&lw.w, weapon, view, tracker, hip, 1920, 1080, bias);
        // The snapped lanes publish exactly (sight - hip) * 256, so the
        // unsuppressed offset is the sight over 256 to the float.
        for (int i = 0; i < 3; ++i) {
            CHECK(bias[i] == (c.suppressed ? hip[i] : sight[i]) / 256.0f);
        }
    }
}

// The frame leg's shake branch selection [orig: Render_ProcessMainSceneFrame
//  @0x5ca34d -> Camera_ComputeThirdPersonView; the mode-0 IIR block
//  @0x43803c..0x4380df vs the stateless mode>=1 chain @0x438939..0x4389e5]:
// the chase (mode 1) consumes the engine tick, first person never does.
void test_frame_chase_shake_consumes_the_tick() {
    LocalWorld lw;
    lw.ai.attach(lw.local);
    LocalPlayerWeapon w = scoped_weapon(0);
    LocalPlayerViewTracker t;
    PlayerViewState v;
    v.shake.counter = 32;
    v.camera_mode = 1;
    v.third_person = true;
    lw.w.weather.core.oscillator.prng = 0x1234;
    // Same view state, two ticks: the chase pitch/roll terms move with the
    // tick (hand-check: deltas (13, 30, -23) at 100 vs (13, 14, -30) at 137).
    PlayerViewState va = v;
    PlayerViewState vb = v;
    LocalPlayerViewFrame fa, fb;
    lw.w.logic_tick = 100;
    local_player_view_frame(&lw.w, w, va, t, fa);
    CHECK(fa.camera_pose_valid);
    lw.w.logic_tick = 137;
    local_player_view_frame(&lw.w, w, vb, t, fb);
    CHECK(fa.camera.yaw_deg == fb.camera.yaw_deg);
    CHECK(fa.camera.pitch_deg != fb.camera.pitch_deg);
    CHECK(fa.camera.roll_deg != fb.camera.roll_deg);
    // First person from the same state ignores the tick entirely: the IIR
    // sample reads only the counter, the filters and the PRNG word.
    v.camera_mode = 0;
    v.third_person = false;
    PlayerViewState vc = v;
    PlayerViewState vd = v;
    LocalPlayerViewFrame fc, fd;
    lw.w.logic_tick = 100;
    local_player_view_frame(&lw.w, w, vc, t, fc);
    lw.w.logic_tick = 137;
    local_player_view_frame(&lw.w, w, vd, t, fd);
    CHECK(fc.camera.yaw_deg == fd.camera.yaw_deg);
    CHECK(fc.camera.pitch_deg == fd.camera.pitch_deg);
    CHECK(fc.camera.roll_deg == fd.camera.roll_deg);
}

// The USE-ITEM action's vehicle-loadout arm gates [orig: Input_HandleActionBinding_0
// case 0xB1 -- parentSlot @0x4e0a91, Flags & 0x800 @0x4e0ab2, the ground entity
// team byte @0x4e0ad8..0x4e0aeb].
void test_use_item_vehicle_loadout_zone_gates() {
    LocalWorld lw;
    CHECK(!local_player_in_vehicle_loadout_zone(lw.w));
    lw.entity().flags |= kEntityFlagVehicleLoadoutZone;
    CHECK(local_player_in_vehicle_loadout_zone(lw.w));
    lw.entity().mounted = true; // a seated player takes the toggle latch instead
    CHECK(!local_player_in_vehicle_loadout_zone(lw.w));
    lw.entity().mounted = false;
    lw.entity().flags &= ~kEntityFlagVehicleLoadoutZone;
    lw.entity().engine_flags |= kEntityFlagVehicleLoadoutZone; // the replica's copy
    CHECK(local_player_in_vehicle_loadout_zone(lw.w));
    // The team test: no ground entity reads team 0 (open); a foreign team refuses.
    lw.entity().team = 1;
    CHECK(local_player_vehicle_zone_team_matches(lw.w));
    Entity pad;
    pad.kind = EntityKind::Item;
    pad.item_id = 0x2000;
    pad.team = 2;
    const EntityHandle ground = lw.w.registry.spawn(0, pad);
    lw.entity().ground_target = ground;
    CHECK(!local_player_vehicle_zone_team_matches(lw.w));
    lw.w.registry.get(ground)->team = 1;
    CHECK(local_player_vehicle_zone_team_matches(lw.w));
    lw.w.registry.get(ground)->team = 0;
    CHECK(local_player_vehicle_zone_team_matches(lw.w));
    World empty;
    CHECK(!local_player_in_vehicle_loadout_zone(empty));
    CHECK(!local_player_vehicle_zone_team_matches(empty));
}

void test_view_uses_current_motor_offset_and_live_position() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(0);
    AiEntity *body = lw.ai.at(lw.ai.attach(lw.local));
    body->inf.active = true;
    body->inf.is_local_player = true;
    Entity &entity = lw.entity();
    entity.eye_offset_x = 0x4000;
    entity.eye_offset_y = -0x8000;
    entity.eye_offset_z = 0x18000;
    PlayerViewState view;
    view.debug_third_person_on_foot = true;
    LocalPlayerViewTracker tracker;
    LocalViewSessionInputs session;
    local_player_view_tick(&lw.w, view, tracker, session);
    CHECK(view.tp_anchor[0] == entity.position.x + 0.25f);
    CHECK(view.tp_anchor[1] == entity.position.y - 0.5f);
    CHECK(view.tp_anchor[2] == entity.position.z + 1.5f);

    // A moving carrier/body re-anchors without another render or input sample.
    entity.position.x += 20.0f;
    entity.position.z += 3.0f;
    view.debug_third_person_on_foot = false;
    player_view_resolve_mode(view);
    LocalPlayerViewFrame frame;
    local_player_view_frame(&lw.w, w, view, tracker, frame);
    CHECK(std::abs(frame.camera.eye[2] - (entity.position.z + 1.5f)) < 0.001f);
    CHECK(std::abs(frame.camera.eye[0] - (entity.position.x + 0.25f)) < 0.2f);
}

} // namespace

// --- the heat window's water gate at the local pump (D-WPN-29) ---------------
// [orig: WeaponAction_ProcessFrame @ 0x540e60 — `Position.Z > g_EnvWaterHeightFixed
//  || (Def->Flags & 4)` @ 0x54101c keeps the window, else the clear @ 0x54125f]:
// the pump feeds the owner's BODY Z against env.water_z, so a body at or below
// the plane drops a live window unless the def carries Underwater; no authored
// water (water_z == 0) never submerges.

LocalPlayerWeapon heated_weapon(uint32_t flags, int32_t window_end) {
    LocalPlayerWeapon w = scoped_weapon(flags);
    w.def.heat_per_shot = 1310;
    w.def.heat_decay_per_tick = 42;
    w.slot.heat_window_end_tick = window_end;
    return w;
}

void pump_once(LocalWorld &lw, LocalPlayerWeapon &w, PlayerViewState &v) {
    LocalWeaponPumpIO io;
    io.view = &v;
    local_weapon_pump_tick(lw.w, w, io);
}

void test_pump_feeds_the_heat_window_water_gate_from_the_body_z() {
    // Body at z=3.0 (the rig), water plane at 5.0: submerged -> the window clears.
    {
        LocalWorld lw;
        lw.w.logic_tick = 100;
        lw.w.env.water_z = to_fixed(5.0);
        LocalPlayerWeapon w = heated_weapon(0, 500);
        PlayerViewState v;
        pump_once(lw, w, v);
        CHECK(w.slot.heat_window_end_tick == 0); // [orig: @ 0x54125f]
    }
    // Same body, same plane, an Underwater def: the window survives.
    {
        LocalWorld lw;
        lw.w.logic_tick = 100;
        lw.w.env.water_z = to_fixed(5.0);
        LocalPlayerWeapon w = heated_weapon(0, 500);
        w.def.flags |= static_cast<int32_t>(weapon_flag::kUnderwater);
        PlayerViewState v;
        pump_once(lw, w, v);
        CHECK(w.slot.heat_window_end_tick == 500);
    }
    // Water plane below the body: above water, the window survives.
    {
        LocalWorld lw;
        lw.w.logic_tick = 100;
        lw.w.env.water_z = to_fixed(1.0);
        LocalPlayerWeapon w = heated_weapon(0, 500);
        PlayerViewState v;
        pump_once(lw, w, v);
        CHECK(w.slot.heat_window_end_tick == 500);
    }
    // No authored water: never submerged, even with a body at z=3.0 and water_z 0.
    {
        LocalWorld lw;
        lw.w.logic_tick = 100;
        lw.w.env.water_z = 0;
        LocalPlayerWeapon w = heated_weapon(0, 500);
        PlayerViewState v;
        pump_once(lw, w, v);
        CHECK(w.slot.heat_window_end_tick == 500);
    }
}

// --- the F3 Weapon window's tick trace ---------------------------------------
// Devtools instrumentation on the pump: disarmed it records nothing, armed it
// takes one sample per PUMP tick (which is why a 1-tick action or RECOIL's
// zero-length tail cannot fall between two display frames), and the ring wraps
// oldest-first rather than growing.

void test_weapon_trace_records_one_sample_per_pump_tick() {
    LocalWorld lw;
    PlayerViewState v;
    LocalPlayerWeapon w = scoped_weapon(0);

    lw.w.logic_tick = 100;
    pump_once(lw, w, v);
    CHECK(weapon_trace_samples(w).empty());
    CHECK(w.trace.empty());

    weapon_trace_arm(w, true);
    CHECK(weapon_trace_samples(w).empty());
    for (uint32_t i = 0; i < 5; ++i) {
        lw.w.logic_tick = 200 + i;
        pump_once(lw, w, v);
    }
    std::vector<WeaponTraceSample> samples = weapon_trace_samples(w);
    CHECK(samples.size() == 5);
    CHECK(samples.front().tick == 200 && samples.back().tick == 204);
    CHECK(samples.back().current == w.slot.current && samples.back().counter == w.slot.counter);

    weapon_trace_clear(w);
    CHECK(weapon_trace_samples(w).empty());
    CHECK(w.trace_armed);

    // Wrap: the ring holds a fixed window, so a long run keeps the NEWEST
    // kWeaponTraceCapacity ticks rather than growing without bound.
    for (uint32_t i = 0; i < kWeaponTraceCapacity + 7; ++i) {
        lw.w.logic_tick = 1000 + i;
        pump_once(lw, w, v);
    }
    samples = weapon_trace_samples(w);
    CHECK(samples.size() == kWeaponTraceCapacity);
    CHECK(samples.front().tick == 1000 + 7);
    CHECK(samples.back().tick == 1000 + kWeaponTraceCapacity + 6);

    weapon_trace_arm(w, false);
    CHECK(w.trace.empty());
    lw.w.logic_tick = 5000;
    pump_once(lw, w, v);
    CHECK(weapon_trace_samples(w).empty());
}

// The per-frame consumer's read: only what is newer than its cursor, walked
// back from the head so the common empty delta touches nothing, plus the
// newest tick so a restarted clock is noticeable.
void test_weapon_trace_samples_since_is_incremental() {
    LocalWorld lw;
    PlayerViewState v;
    LocalPlayerWeapon w = scoped_weapon(0);
    std::vector<WeaponTraceSample> out;
    CHECK(weapon_trace_samples_since(w, 0, true, out) == 0 && out.empty());

    weapon_trace_arm(w, true);
    for (uint32_t i = 0; i < 6; ++i) {
        lw.w.logic_tick = 300 + i;
        pump_once(lw, w, v);
    }
    CHECK(weapon_trace_samples_since(w, 0, true, out) == 305);
    CHECK(out.size() == 6 && out.front().tick == 300 && out.back().tick == 305);
    out.clear();
    CHECK(weapon_trace_samples_since(w, 303, false, out) == 305);
    CHECK(out.size() == 2 && out[0].tick == 304 && out[1].tick == 305);
    out.clear();
    weapon_trace_samples_since(w, 305, false, out);
    CHECK(out.empty());

    // Past the wrap the walk still starts at the newest and stops at the cursor.
    for (uint32_t i = 0; i < kWeaponTraceCapacity + 3; ++i) {
        lw.w.logic_tick = 1000 + i;
        pump_once(lw, w, v);
    }
    const uint32_t newest = 1000 + kWeaponTraceCapacity + 2;
    CHECK(weapon_trace_samples_since(w, newest - 4, false, out) == newest);
    CHECK(out.size() == 4 && out.front().tick == newest - 3 && out.back().tick == newest);
}

// The pump's input gate as a predicate: what a tool refuses with a reason is
// exactly what the pump would have zeroed.
void test_local_weapon_input_block_mirrors_the_pump_gate() {
    LocalWorld lw;
    LocalPlayerWeapon w;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kInactive);
    w.active = true;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kNone);
    w.usegun_switch = LocalUseGunSwitch::kAttach;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kUseGunSwitch);
    w.usegun_switch = LocalUseGunSwitch::kNone;
    lw.w.registry.configure_pool(1, 4);
    Entity carrier;
    carrier.has_item_def = true;
    const auto mount = lw.w.registry.spawn(1, carrier);
    lw.entity().mounted = true;
    lw.entity().mount_target = mount;
    lw.entity().mount_type = SeatType::Controller;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kSeat);
    lw.w.registry.get(mount)->item_attrib = kItemAttribEweap;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kNone);
    lw.w.registry.get(mount)->has_item_def = false;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kSeat);
    lw.w.registry.get(mount)->has_item_def = true;
    lw.entity().mount_type = SeatType::Driver;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kSeat);
    lw.entity().mount_type = SeatType::Gunner;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kNone);
    lw.entity().mounted = false;
    lw.entity().health = 0;
    CHECK(local_weapon_input_block(lw.w, w) == LocalWeaponInputBlock::kDead);
    CHECK(local_weapon_input_block_name(LocalWeaponInputBlock::kNone)[0] == '\0');
    CHECK(local_weapon_input_block_name(LocalWeaponInputBlock::kSeat)[0] != '\0');
}

void test_target_lock_cadence_and_audio() {
    LocalWorld lw;
    CollisionWorld collision;
    lw.w.collision = &collision;
    lw.ai.attach(lw.local);
    AiEntity &body = *lw.ai.for_handle(lw.local);
    body.pos[0] = 10 << 16; body.pos[1] = 20 << 16; body.pos[2] = 3 << 16;
    body.team = 1; lw.entity().team = 1;
    LocalPlayer local(lw.w);
    local.weapon.active = true;
    local.weapon.def_name = "LOCK";
    std::strcpy(local.weapon.def.soundlockedtone, "LOCKED");
    lw.w.tables.weapons.entries.resize(2);
    auto &def = lw.w.tables.weapons.entries[1];
    def.name = "LOCK"; def.valid = true; def.ammo_index = 1;
    lw.w.tables.ammo.entries.resize(2);
    auto &ammo = lw.w.tables.ammo.entries[1];
    ammo.valid = true; ammo.heat_det_range = 3000; ammo.boresight_maxang = 11930464 * 20;
    Entity target;
    target.item_id = 2; target.item_type = 1; target.health = 100; target.alive = true;
    target.team = 2; target.heat_sig = 500; target.position = {110, 20, 3};
    const EntityHandle h = lw.w.registry.spawn(0, target);
    lw.w.out.fire_sounds.set_listener({10, 20, 3});
    lw.w.logic_tick = 16;
    local.update_aim_target();
    CHECK(body.inf.combat_target == h);
    CHECK((body.slot.f[2] & 1) != 0);
    auto events = lw.w.out.sound_emitters.drain();
    CHECK(events.size() == 1 && events[0].set_name == "LOCKED");
    CHECK(events[0].lane == 100 && events[0].lifetime_ticks == 20);
    lw.w.registry.get(h)->team = 1;
    lw.w.logic_tick = 17;
    local.update_aim_target();
    CHECK(body.inf.combat_target == h);
    CHECK(lw.w.out.sound_emitters.drain().size() == 1);
    lw.w.logic_tick = 32;
    local.update_aim_target();
    CHECK(!body.inf.combat_target.valid() && body.slot.f[2] == -2);
    CHECK(lw.w.out.sound_emitters.drain().empty());
}

// The zero-step request keys the -1 floor on the session's sniper-zoom RULE
// bit, never the game type [orig: Player_AdjustWeaponZoomLevel
// @0x4dbd0c..0x4dbd2e], clicks GF_SCOPE_ZERO only on a change
// [orig: @0x4dbd47..0x4dbd50], moves the look pitch by the elevation delta and
// recomputes the zero-yaw term [orig: @0x4dbd91..0x4dbde3].
void test_scope_zero_request_keys_on_the_rule_bit_and_clicks() {
    LocalWorld lw;
    LocalPlayer local(lw.w);
    local.weapon.active = true;
    // ForceScoped in first person: the CanFire verdict holds.
    local.weapon.def.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_FORCESCOPED);
    local.weapon.def.scope_zero.max_steps = 10;
    local.weapon.def.scope_zero.step_metres = 100;
    local.weapon.def.scope_zero.elevation[1] = 500;
    local.weapon.slot.scope_zero = 1;
    local.weapon.slot.zero_pitch = 500;
    MatchRules team;
    team.game_type = 0x10000u; // a team game admits nothing by itself
    lw.w.match.configure(team);
    lw.w.rules.mp_session = true; // in a session
    lw.w.rules.auto_scope_zero = false;
    CHECK(local.request_scope_zero(-1));
    CHECK(local.weapon.slot.scope_zero == 0);
    CHECK(local.input.look_pitch == -500);
    CHECK(lw.w.out.script_sounds.size() == 1);
    CHECK(lw.w.out.script_sounds[0].name == kScopeZeroSoundset);
    CHECK(lw.w.out.script_sounds[0].kind == ScriptSoundEvent::Kind::Interface);
    CHECK(!local.request_scope_zero(-1)); // the 0 floor in session without the rule
    CHECK(local.weapon.slot.scope_zero == 0);
    CHECK(lw.w.out.script_sounds.size() == 1); // no click without a change
    lw.w.rules.auto_scope_zero = true;
    CHECK(local.request_scope_zero(-1));
    CHECK(local.weapon.slot.scope_zero == -1);
    CHECK(lw.w.out.script_sounds.size() == 2);
    CHECK(local.weapon.slot.zero_yaw == 0); // no parallax key
    local.weapon.def.scope_zero.paralax_distance_q16 = -(2 << 16);
    CHECK(local.request_scope_zero(1));
    CHECK(local.weapon.slot.scope_zero == 0);
    // atan(2 / the 100 m floor), negated for a negative parallax: the adjust's
    // own leg, which the install below never takes.
    CHECK(local.weapon.slot.zero_yaw == 13669483);
}

// The slot install seeds the zero-yaw term (MountSlot+8) from the seeded step,
// outside the flags & 3 elevation gate and with the parallax sign kept
// [orig: WeaponSlot_InitFromDef @0x53ef4f..0x53ef8b]; an inventory-restored
// step recomputes it the same way.
void test_scope_zero_install_seeds_the_yaw_term() {
    LocalWorld lw;
    WeaponTableEntry row;
    row.valid = true;
    row.name = "paralax_weapon";
    row.category = 3;
    row.action_fsm.scope_zero.max_steps = 10;
    row.action_fsm.scope_zero.step_metres = 100;
    row.action_fsm.scope_zero.default_metres = 300;
    row.action_fsm.scope_zero.paralax_distance_q16 = -(2 << 16);
    lw.w.tables.weapons.entries.push_back(row);
    LocalPlayerWeapon weapon;
    WeaponInstallData data;
    data.name = row.name;
    PlayerViewState view;
    local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
    CHECK(weapon.slot.scope_zero == 3);
    CHECK(weapon.slot.zero_pitch == 0);      // flags & 3 clear: no elevation
    CHECK(weapon.slot.zero_yaw == -4557034); // atan(-2 / 300), sign kept
    WeaponInventory inv;
    inv.reset(lw.w.tables.weapons);
    inv.equipped_combo = 3 * 65;
    inv.slot(inv.equipped_combo)->adm_index =
        static_cast<int16_t>(lw.w.tables.weapons.entries.size() - 1);
    inv.slot(inv.equipped_combo)->scope_zero = -1;
    LocalPlayerWeapon restored;
    local_weapon_install(lw.w, restored, data, false, false, &inv, view);
    CHECK(restored.slot.scope_zero == -1);
    CHECK(restored.slot.zero_yaw == -13669483); // the 100 m floor, sign kept
}

// The zero-yaw term [orig: WeaponSlot_InitFromDef @0x53ef4f..0x53ef8b]: atan2
// of the parallax height over the zero distance (floored at 100 m) in BAM.
void test_scope_zero_yaw_term() {
    WeaponScopeZero zero;
    zero.max_steps = 10; zero.step_metres = 100;
    CHECK(weapon_scope_zero_yaw(zero, 3) == 0); // no parallax key
    zero.paralax_distance_q16 = 2 << 16;
    CHECK(weapon_scope_zero_yaw(zero, 3) == 4557034);   // atan(2 / 300)
    CHECK(weapon_scope_zero_yaw(zero, -1) == 13669483); // the 100 m floor
    zero.paralax_distance_q16 = -(2 << 16);
    CHECK(weapon_scope_zero_yaw(zero, 3) == -4557034); // the init leg keeps the sign
}

// The bake's +0xF0 max-range output [orig: @0x54530f..0x545338; store
// @0x5453f8]: the range at the lifetime's expiry tick, else at the first tick
// below the ammo's min-stable speed, over every solve.
void test_scope_zero_bake_max_range() {
    AmmoTableEntry ammo;
    ammo.velocity = 620; // 10 units per tick
    WeaponScopeZero zero;
    zero.max_steps = 1; zero.step_metres = 100;
    weapon_scope_zero_bake(zero, ammo);
    CHECK(zero.max_range_q16 == 0); // no lifetime, never below stable
    ammo.max_age_ticks = 1;
    weapon_scope_zero_bake(zero, ammo);
    // One tick of the flattest solve: its angle ends a few BAM above zero,
    // where the Q22 cosine truncates one LSB under 1.0 (retail fcos + ftol
    // @0x54520f..0x545217 agree), so the range is one LSB under the speed.
    CHECK(zero.max_range_q16 == 655359);
    ammo.max_age_ticks = 0;
    ammo.min_stable_velocity = 100000;
    weapon_scope_zero_bake(zero, ammo);
    CHECK(zero.max_range_q16 == 655359); // below stable on the first tick
}

// Direct mounts retain the current pose and velocities. Published bias alone
// clears, then the next step uses the newly bound def's hip reference.
void test_direct_mount_retains_running_pose_and_forced_mount_targets_ads() {
    LocalWorld lw;
    for (int i = 0; i < 3; ++i) {
        WeaponTableEntry row;
        row.valid = true;
        row.name = "POSE_" + std::to_string(i);
        row.category = i == 2 ? 2 : 1;
        row.rank = i;
        row.weapon_class_slot = 1;
        lw.w.tables.weapons.entries.push_back(row);
    }
    LocalPlayerWeapon w;
    PlayerViewState v;
    WeaponInstallData data;
    data.name = "POSE_0";
    data.flags = DEF_WEAPON_FLAG_SIGHTED;
    data.view_hip_pose = {{256, 512, 768}, {0x10000000u, 0x10000000u, 0xF0000000u}};
    data.view_ads_pose = {{512, -256, 1024}, {0x08000000u, 0x20000000u, 0x10000000u}};
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(local_player_scope_toggle(lw.w, w, v, w.slot));
    const float eye[3] = {};
    for (int i = 0; i < 3; ++i) player_view_tick(v, eye);
    const PlayerViewBiasInterp before = v.weapon_pose_interp;
    data.name = "POSE_1";
    data.view_hip_pose = {{10, 20, 30}, {0x02000000u, 0x03000000u, 0x04000000u}};
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(v.weapon_pose_interp.active && !v.scope_settled && v.scope_engaged);
    for (int i = 0; i < 3; ++i) {
        CHECK(v.weapon_pose_interp.current.position_q16[i] == before.current.position_q16[i]);
        CHECK(v.weapon_pose_interp.current.rotation_bam[i] == before.current.rotation_bam[i]);
        CHECK(v.weapon_pose_interp.velocity.position_q16[i] == before.velocity.position_q16[i]);
        CHECK(v.weapon_pose_interp.velocity.rotation_bam[i] == before.velocity.rotation_bam[i]);
        CHECK(v.weapon_pose_interp.position_bias_q16[i] == 0 &&
              v.weapon_pose_interp.rotation_bias_bam[i] == 0);
    }
    CHECK(v.weapon_pose_interp.remaining == before.remaining);
    player_view_tick(v, eye);
    int32_t out[3];
    local_player_viewmodel_rotation_bias(&lw.w, w, v, out);
    CHECK(static_cast<uint32_t>(out[0]) == 0x0BDDDDE0u);
    CHECK(out[1] == 0x1D000000 && out[2] == 0x0C000000);

    // A same-category promoted mount preserves the promoted byte and schedules
    // Setup(1, hip, ADS). A cross-category nonforced mount clears only promotion.
    settle_ease(v);
    CHECK(player_view_scope_settled(v));
    data.name = "POSE_0";
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(player_view_scope_settled(v) && v.weapon_pose_interp.active);
    CHECK(v.weapon_pose_interp.current.rotation_bam[0] == 0x02000000u);
    CHECK(v.weapon_pose_interp.rotation_bias_bam[0] == 0);
    data.name = "POSE_2";
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(!player_view_scope_settled(v) && v.weapon_pose_interp.active);
    CHECK(v.weapon_pose_interp.current.rotation_bam[0] == 0x02000000u);

    // Actual install with ForceScoped promotes without a user toggle and uses
    // the original one-step setup, not a fabricated already-published pose.
    local_weapon_clear(w, v);
    data.flags = DEF_WEAPON_FLAG_SIGHTED | DEF_WEAPON_FLAG_FORCESCOPED;
    data.view_hip_pose = {};
    data.view_ads_pose = {{0, 0, 0}, {0x10000000u, 0x20000000u, 0x30000000u}};
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(v.scope_engaged && player_view_scope_settled(v) && !v.scope_hipfire);
    CHECK(v.weapon_pose_interp.active && v.weapon_pose_interp.rotation_bias_bam[0] == 0);
    CHECK(!local_player_scope_toggle(lw.w, w, v, w.slot)); // forced sight stays pinned
    player_view_tick(v, eye);
    local_player_viewmodel_rotation_bias(&lw.w, w, v, out);
    CHECK(out[0] == 0x10000000 && out[1] == 0x20000000 && out[2] == 0x30000000);
    player_view_tick(v, eye);
    CHECK(!v.weapon_pose_interp.active);
    // A nonoptical mount unbinds the slot and clears activity on the next step,
    // retaining the six bookkeeping lanes. [orig: @0x4DDD2B..0x4DDDBC]
    data.flags = 0;
    local_weapon_install(lw.w, w, data, false, false, nullptr, v);
    CHECK(!v.weapon_pose_bound);
    local_weapon_clear(w, v);
    CHECK(!v.scope_engaged && !v.scope_settled && v.scope_hipfire);
}

void test_category_request_resets_before_commit_while_cycle_retains_pose() {
    LocalWorld lw;
    lw.w.tables.weapons.entries.resize(3);
    for (int i = 1; i <= 2; ++i) {
        auto &row = lw.w.tables.weapons.entries[i];
        row.valid = true;
        row.name = "REQUEST_" + std::to_string(i);
        row.category = 1;
        row.rank = i - 1;
        row.weapon_class_slot = 1;
    }
    WeaponInventory inventory;
    inventory.reset(lw.w.tables.weapons);
    inventory.equipped_combo = 65;
    inventory.slots[65].adm_index = 1;
    inventory.slots[66].adm_index = 2;
    inventory.slots[65].clip = inventory.slots[66].clip = 1;
    LocalPlayerWeapon w;
    WeaponInstallData data;
    data.name = "REQUEST_1";
    data.flags = DEF_WEAPON_FLAG_SIGHTED;
    data.view_hip_pose.rotation_bam[0] = 0x10000000u;
    data.view_ads_pose.rotation_bam[0] = 0x08000000u;
    PlayerViewState v;
    local_weapon_install(lw.w, w, data, false, false, &inventory, v);
    CHECK(local_player_scope_toggle(lw.w, w, v, w.slot));
    const float eye[3] = {};
    player_view_tick(v, eye);
    const uint32_t current = v.weapon_pose_interp.current.rotation_bam[0];
    const uint32_t velocity = v.weapon_pose_interp.velocity.rotation_bam[0];
    const int32_t published = v.weapon_pose_interp.rotation_bias_bam[0];
    const auto cycle = weapon_cycle_slot(lw.w.tables.weapons, inventory, 1,
                                         local_weapon_switch_gates(lw.w, w, &inventory));
    CHECK(cycle.kind == WeaponSwitchOutcome::kMount && !cycle.reset_view);
    handle_weapon_switch_outcome(lw.w, w, &inventory, cycle, v);
    CHECK(v.weapon_pose_interp.active && v.weapon_pose_interp.current.rotation_bam[0] == current);
    w.slot.current = weapon_action::kIdle;
    const auto category = weapon_switch_to_handle(lw.w.tables.weapons, inventory, 65,
                                                  local_weapon_switch_gates(lw.w, w, &inventory));
    CHECK(category.kind == WeaponSwitchOutcome::kMount && category.reset_view);
    handle_weapon_switch_outcome(lw.w, w, &inventory, category, v);
    CHECK(w.switch_in_flight && inventory.equipped_combo == 65); // action has not committed
    CHECK(!v.weapon_pose_interp.active && !v.scope_engaged && v.scope_hipfire);
    CHECK(v.weapon_pose_interp.current.rotation_bam[0] == velocity);
    CHECK(v.weapon_pose_interp.rotation_bias_bam[0] == published); // reset is not mount bias clear
    const auto denied = weapon_switch_to_handle(lw.w.tables.weapons, inventory, 11 * 65,
                                                local_weapon_switch_gates(lw.w, w, &inventory));
    CHECK(denied.kind == WeaponSwitchOutcome::kDeny && denied.reset_view);
    WeaponSwitchGates blocked;
    blocked.seat_blocked = true;
    CHECK(!weapon_switch_to_handle(lw.w.tables.weapons, inventory, 65, blocked).reset_view);
}

// The listen host's OWN validated fire ends its spawn protection the way the
// remote C2S 0x06 clear does (Entity_FireWeaponAndSendPacket's authority leg
// -> Server_ClientFiredRound): in an MP session a non-spectator's nonzero
// entity+292 goes to 0 on the fire commit; an SP fire and a joiner's predicted
// fire leave the word alone, and a spectator slot keeps its -1.
void test_host_own_fire_ends_spawn_protection() {
    struct Case {
        bool authority;
        bool mp_session;
        bool spectator;
        int32_t expect;
    };
    const Case cases[] = {
        {true, true, false, 0},    // the listen host in session: cleared
        {true, false, false, 620}, // SP: not this mechanism's word
        {false, true, false, 620}, // a joiner defers to the host's clear
        {true, true, true, -1},    // a spectator slot keeps its -1
    };
    for (const Case &c : cases) {
        LocalWorld lw;
        lw.ai.attach(lw.local);
        lw.w.rules.mp_session = c.mp_session;
        lw.w.tables.weapons.entries.resize(6);
        WeaponTableEntry &rifle = lw.w.tables.weapons.entries[5];
        rifle.name = "WPN_TESTRIFLE";
        rifle.category = 3;
        rifle.clipsize = 30;
        rifle.ammo_index = 1;
        rifle.valid = true;
        lw.w.tables.ammo.entries.resize(2);
        lw.w.tables.ammo.entries[1].name = "TEST_BALL";
        lw.w.tables.ammo.entries[1].velocity = 620;
        lw.w.tables.ammo.entries[1].max_age_ticks = 248;
        lw.w.tables.ammo.entries[1].valid = true;
        lw.entity().equipped_adm_index = 5;
        lw.entity().damage_state = c.spectator ? -1 : 620;
        if (c.spectator) {
            MatchPlayerIdentity id;
            id.entity = lw.local;
            lw.w.match.upsert_player(id);
            lw.w.match.set_player_spectator(lw.local, true);
        }
        LocalPlayerWeapon weapon;
        WeaponInstallData data;
        data.name = "WPN_TESTRIFLE";
        data.clipsize = 30;
        data.rows.resize(3);
        std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "idle");
        data.rows[0].delaystart = 0;
        std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "fire");
        data.rows[1].delaystart = 0;
        data.rows[1].delayend = 6;
        std::snprintf(data.rows[2].name, sizeof(data.rows[2].name), "recoil");
        data.rows[2].delaystart = 0;
        data.rows[2].delayend = 0;
        PlayerViewState view;
        local_weapon_install(lw.w, weapon, data, false, false, nullptr, view);
        local_weapon_set_input(weapon, view, true, true, false);
        LocalWeaponPumpIO io;
        io.view = &view;
        io.is_authority = c.authority;
        lw.w.logic_tick = 10;
        local_weapon_pump_tick(lw.w, weapon, io);
        CHECK(weapon.fired_serial == 1);
        CHECK(lw.entity().damage_state == c.expect);
    }
}

// The sampled values below came from executing the original 4B5966..4B5C97
// span, including PRNG_Next16, with seed 1A10101A on Jointops.exe SHA256
// b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac.
// No engine call was replaced. Initial oscillator globals and aim are zero.
struct ScopedAimFixture : LocalWorld {
    LocalPlayer player{w};
    ScopedAimFixture() {
        w.local_player_state = &player;
        ai.attach(local);
        body().inf.active = true;
        body().inf.is_local_player = true;
        body().health = 100;
        entity().flags |= kEntityFlagPlayer;
        entity().equipped_adm_index = 1;
        w.tables.weapons.entries.resize(2);
        w.tables.weapons.entries[1].valid = true;
        player.weapon.active = true;
        player.view.scope_engaged = true;
        player.view.scope_settled = true;
    }
    AiEntity &body() { return *ai.for_handle(local); }
    void sample(uint32_t tick, int32_t yaw, int32_t pitch) {
        player.apply_scoped_aim_drift(body(), tick);
        player.sync_local_mounted_input_heading();
        CHECK(body().heading == yaw && body().pitch == pitch);
        CHECK(player.input.look_heading == yaw && player.input.look_pitch == pitch);
    }
};

// The main scene applies the equipped slot's zero AFTER composing the camera.
// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0, Scoped @0x5ca494..0x5ca4a0]
void test_rendered_scope_applies_elevation_and_parallax() {
    for (const auto stance : {InfantryState::Stance::kStand, InfantryState::Stance::kProne}) {
        ScopedAimFixture f;
        f.body().inf.stance = stance;
        f.player.weapon.def.flags = DEF_WEAPON_FLAG_SCOPED;
        auto base = f.player.present_view_frame();
        CHECK(base.camera_pose_valid);
        f.player.weapon.slot.zero_pitch = 0x01000000; // 1.40625 degrees
        f.player.weapon.slot.zero_yaw = 0x00800000; // 0.703125 degrees
        auto zeroed = f.player.present_view_frame();
        CHECK(std::abs(zeroed.camera.pitch_deg - base.camera.pitch_deg + 1.40625f) < 0.00001f);
        // Mission yaw is 90 - retail BAM yaw, hence the inverted sign.
        CHECK(std::abs(zeroed.camera.yaw_deg - base.camera.yaw_deg + 0.703125f) < 0.00001f);
        f.player.weapon.def.flags = DEF_WEAPON_FLAG_SIGHTED;
        auto sighted = f.player.present_view_frame();
        CHECK(std::abs(sighted.camera.pitch_deg - base.camera.pitch_deg) < 0.00001f);
        f.player.weapon.def.scope_zero.max_steps = 10;
        sighted = f.player.present_view_frame();
        CHECK(std::abs(sighted.camera.pitch_deg - zeroed.camera.pitch_deg) < 0.00001f);
        f.player.weapon.def.flags = DEF_WEAPON_FLAG_SCOPED;
        CHECK(f.body().pitch == 0 && f.body().heading == 0);
        CHECK(f.player.input.look_pitch == 0 && f.player.input.look_heading == 0);
        f.player.view.scope_engaged = false;
        f.player.view.scope_settled = false;
        auto hip = f.player.present_view_frame();
        CHECK(std::abs(hip.camera.pitch_deg - base.camera.pitch_deg) < 0.00001f);
        CHECK(std::abs(hip.camera.yaw_deg - base.camera.yaw_deg) < 0.00001f);
    }
}

void test_scoped_aim_original_sequences() {
    struct Sample { uint32_t tick; int32_t yaw, pitch; unsigned draws; };
    struct Case { InfantryState::Stance stance; int32_t stability[3]; Sample samples[5]; };
    const Case cases[] = {
        {InfantryState::Stance::kStand, {65536,65536,65536},
            {{0,-214,-408,2}, {61,-417942,-796824,2}, {62,-431424,-821724,3},
             {185,-2142140,-1977216,4}, {186,-2155681,-1988018,6}}},
        {InfantryState::Stance::kCrouch, {65536,65536,65536},
            {{0,-136,-95,2}, {61,-265608,-185535,2}, {62,-274176,-191341,3},
             {185,-1361360,-768595,4}, {186,-1369988,-779743,6}}},
        {InfantryState::Stance::kProne, {65536,65536,65536},
            {{0,-105,-48,2}, {61,-205065,-93744,2}, {62,-211680,-96558,3},
             {185,-1051050,228315,4}, {186,-1057741,227595,6}}},
        {InfantryState::Stance::kProne, {32768,131072,98304},
            {{0,-52,-12,2}, {61,-101556,-23436,2}, {62,-104832,-24139,3},
             {185,-520520,59489,4}, {186,-523833,59325,6}}},
        {InfantryState::Stance::kCrouch, {32768,131072,98304},
            {{0,-273,-382,2}, {61,-533169,-579876,2}, {62,-550368,-592146,3},
             {185,-2732730,-1439064,4}, {186,-2750050,-1460956,6}}},
        {InfantryState::Stance::kStand, {32768,131072,98304},
            {{0,-322,-918,2}, {61,-628866,-1618434,2}, {62,-649152,-1657017,3},
             {185,-3223220,-2115612,4}, {186,-3243597,-2123349,6}}},
        {InfantryState::Stance::kStand, {0,0,0},
            {{0,0,0,2}, {61,0,0,2}, {62,0,0,3}, {185,0,0,4}, {186,0,0,6}}},
    };
    for (const Case &c : cases) {
        ScopedAimFixture f;
        f.body().inf.stance = c.stance;
        for (int stance = 0; stance < 3; ++stance)
            f.w.tables.weapons.entries[1].stability_fp16[stance] = c.stability[stance];
        size_t sample = 0;
        for (uint32_t tick = 0; tick <= 186; ++tick) {
            f.player.apply_scoped_aim_drift(f.body(), tick);
            if (tick != c.samples[sample].tick) continue;
            const Sample &expected = c.samples[sample++];
            CHECK(f.body().heading == expected.yaw && f.body().pitch == expected.pitch);
            f.player.sync_local_mounted_input_heading();
            CHECK(f.player.input.look_heading == expected.yaw);
            CHECK(f.player.input.look_pitch == expected.pitch);
            uint32_t expected_rng = World::kMissionPrng16Seed;
            for (unsigned draw = 0; draw < expected.draws; ++draw)
                opennova::io::rotating_prng_next16(expected_rng);
            CHECK(f.w.prng16_state == expected_rng);
        }
    }
}

void test_scoped_aim_gates_and_independent_stance_resets() {
    {
        ScopedAimFixture f;
        f.player.view.scope_settled = false;
        f.sample(0, 0, 0); // raising has not promoted the scope
        CHECK(f.w.prng16_state == World::kMissionPrng16Seed);
        f.player.view.scope_settled = true;
        f.sample(1, 0, 0); // engagement does not start a new phase
        f.sample(62, 0, -214); // only pitch refreshes on this boundary
        f.player.view.scope_settled = false;
        const uint32_t retained_rng = f.w.prng16_state;
        f.sample(186, 0, -214); // lower/raise retains both oscillators
        CHECK(f.w.prng16_state == retained_rng);
        f.player.view.scope_settled = true;
        f.sample(187, 0, -642);
        f.entity().mounted = true;
        f.entity().mount_type = SeatType::Gunner;
        f.sample(372, 0, -642);
        CHECK(f.w.prng16_state == retained_rng);
        f.player.view.scope_settled = false;
        f.player.view.binoculars_raised = true;
        f.sample(372, 0, -642); // third-person body pose is not binocular optics
        CHECK(f.w.prng16_state == retained_rng);
        f.player.view.binoculars_view_active = true;
        f.sample(372, -48, -908); // binocular optics bypass the gunner gate, use prone scale
        const uint32_t after_binoculars = f.w.prng16_state;
        AiEntity remote;
        remote.handle = EntityHandle::make(0, 7);
        f.player.apply_scoped_aim_drift(remote, 558);
        CHECK(remote.heading == 0 && remote.pitch == 0);
        CHECK(f.w.prng16_state == after_binoculars);
    }
    {
        ScopedAimFixture f;
        f.sample(0, -214, -408);
        f.body().inf.stance = InfantryState::Stance::kCrouch;
        f.sample(1, -642, -1224); // neither axis resets immediately
        f.sample(62, -1284, -1140); // pitch observes the stance first
        f.sample(186, -1110, -1268); // yaw observes it on its own boundary
    }
}

void test_scoped_aim_survives_kernel_replacement_without_sharing_sessions() {
    ScopedAimFixture previous;
    previous.sample(0, -214, -408);
    ScopedAimFixture replacement;
    replacement.player.carry_process_globals_from(previous.player);
    // A mission reset seeds new aim and PRNG, but not the oscillators. A
    // non-boundary scope raise continues the previous drift and direction.
    replacement.sample(1, -428, -816);
    CHECK(replacement.w.prng16_state == World::kMissionPrng16Seed);
    // Simultaneous native sessions still own independent local state.
    ScopedAimFixture independent;
    independent.sample(1, 0, 0);
    CHECK(independent.w.prng16_state == World::kMissionPrng16Seed);
}

// A direction key in the packed word drops the raw binocular toggle: the
// view lowers and stays down after the key releases. Between a joiner's send
// boundaries nothing is packed, so the toggle survives until the boundary
// pack reads the held key. [orig: Player_PackInputStateToEntity @0x4df4b2 --
// `mov g_BinocularsToggle, 0` @0x4df4c2 when the F/B/L/R word is nonzero;
// Player_UpdatePerFrame @0x4de37b re-derives the raised pose from it]
void test_pack_drops_the_binocular_toggle_on_movement() {
    ScopedAimFixture f;
    f.player.weapon.def.flags = 0;
    f.player.view.scope_engaged = false;
    f.player.view.scope_settled = false;
    CHECK(local_player_binoculars_toggle(f.w, f.player.weapon, f.player.view,
            f.player.view_tracker));
    CHECK(f.player.view.binoculars_requested);
    // Looking around and leaning are not movement.
    f.player.set_view_keys(false, false, false, true, false);
    f.player.set_movement_keys(false, false, false, false, true, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(f.player.view.binoculars_requested);
    f.player.set_view_keys(false, false, false, false, false);
    // A joiner between boundaries: the held key accumulates, nothing packs.
    f.player.set_movement_keys(true, false, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/false);
    CHECK(f.player.view.binoculars_requested);
    // The boundary pack sees the key and drops the toggle; releasing the key
    // does not bring the binoculars back.
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.view.binoculars_requested);
    f.player.set_movement_keys(false, false, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.view.binoculars_requested);
    CHECK(!f.player.view.binoculars_raised);
}

// The movement-held latch and the unscope-on-move run inside the pack, from
// the packed word: a joiner between send boundaries keeps its settled scope
// with a direction key down, and the boundary pack latches g_MovementKeyHeld
// and routes the unscope; a release clears the latch at the next pack. The
// binocular suppression instead reads the frame's input word, so it holds
// from the frame the key goes down. [orig: Player_PackInputStateToEntity
// @0x4df4b2..0x4df4f9 -- latch @0x4df4bb / @0x4df4f9, the unscope route
// @0x4df4c9..0x4df4ec; Player_UpdatePerFrame `test byte ptr g_InputFlags, 1Eh`
// @0x4de3ae]
void test_pack_owns_the_movement_latch_and_unscope() {
    ScopedAimFixture f;
    f.player.weapon.def.flags = DEF_WEAPON_FLAG_SCOPED;
    f.player.set_movement_keys(true, false, false, false, false, false, false);
    CHECK(f.player.view.scope_settled);
    CHECK(!f.player.view.move_held);
    f.player.apply_player_input_pre_tick(/*pack_input=*/false);
    CHECK(f.player.view.scope_settled);
    CHECK(!f.player.view.move_held);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.view.scope_settled);
    CHECK(f.player.view.move_held);
    f.player.set_movement_keys(false, false, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.view.move_held);

    ScopedAimFixture b;
    b.player.weapon.def.flags = 0;
    b.player.view.scope_engaged = false;
    b.player.view.scope_settled = false;
    CHECK(local_player_binoculars_toggle(b.w, b.player.weapon, b.player.view,
            b.player.view_tracker));
    CHECK(b.player.view.binoculars_raised);
    b.player.set_movement_keys(false, true, false, false, false, false, false);
    CHECK(!b.player.view.binoculars_raised);
    CHECK(b.player.view.binoculars_requested);
}

// A NoMove weapon strips the direction and lean bits from the word before the
// pack reads it; jump, the look keys and free look survive. An OnlyScoped
// weapon answers the seat-flag query only once promoted (the mortar walks
// while carried, stands while set up). [orig: Player_PackInputStateToEntity --
// Entity_CheckWeaponSeatFlags(EquippedSlot, 0x20000) @0x4df46c,
// `g_InputFlags &= 0xFFFF9FE1` @0x4df482; Entity_CheckWeaponSeatFlags @0x540D00]
void test_pack_masks_movement_under_a_nomove_weapon() {
    ScopedAimFixture f;
    f.player.weapon.def.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_NOMOVE);
    f.player.set_movement_keys(true, false, true, false, true, false, true);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.move_order.moving);
    CHECK(f.player.move_order.direction_bits == 0);
    CHECK(!f.player.move_order.lean_left);
    CHECK(f.player.move_order.jump);
    CHECK(!f.player.view.move_held);
    CHECK(f.player.input_flags.prev == kInputFlagJump);

    ScopedAimFixture m;
    m.player.weapon.def.flags =
            static_cast<int32_t>(DEF_WEAPON_FLAG_NOMOVE | DEF_WEAPON_FLAG_ONLYSCOPED);
    m.player.view.scope_engaged = false;
    m.player.view.scope_settled = false;
    m.player.set_movement_keys(true, false, false, false, false, false, false);
    m.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(m.player.move_order.moving);
    m.player.view.scope_engaged = true;
    m.player.view.scope_settled = true;
    m.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!m.player.move_order.moving);
    CHECK(m.player.view.scope_settled);
}

// The pack's analog legs: the forward/back key handlers also write the X axis
// word (-1023 / +1023; back's row dispatches after forward's, so it wins when
// both are held), shifted to a byte; a change of more than 32 latches the
// axes on, a moving pack latches them off, and only a latched pack stores
// them (else 0). The throttle has its own latch, a 0x14 deadzone, and drops
// while a lean key is in the word. [orig: Input_HandleActionBinding_0 cases
// 151/152 @0x4e0c6e..0x4e0cc5; Input_InitBindingSystem's row-order list;
// Player_PackInputStateToEntity @0x4df793..0x4df8f8]
void test_pack_analog_axes_and_their_hysteresis() {
    ScopedAimFixture f;
    f.player.weapon.def.flags = 0;
    f.player.view.scope_engaged = false;
    f.player.view.scope_settled = false;
    // Opposing keys: no movement, the X word is back's.
    f.player.set_movement_keys(true, true, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!f.player.move_order.moving);
    CHECK(f.entity().net_analog_x == 127);
    CHECK(f.entity().net_analog_y == 0 && f.entity().net_analog_z == 0);
    // Released: the latch holds, the axis follows to 0.
    f.player.set_movement_keys(false, false, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(f.entity().net_analog_x == 0);
    // Forward alone moves: the latch drops and the axes zero.
    f.player.set_movement_keys(true, false, false, false, false, false, false);
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(f.player.move_order.moving);
    CHECK(f.entity().net_analog_x == 0);

    // A set-up mortar masks the movement but not the X word.
    ScopedAimFixture m;
    m.player.weapon.def.flags = static_cast<int32_t>(DEF_WEAPON_FLAG_NOMOVE);
    m.player.set_movement_keys(true, false, false, false, false, false, false);
    m.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(!m.player.move_order.moving);
    CHECK(m.entity().net_analog_x == -128);

    // The throttle: inside the deadzone it reads 0; past it the latch stores
    // it; a lean key drops it.
    ScopedAimFixture t;
    t.player.weapon.def.flags = 0;
    t.player.view.scope_engaged = false;
    t.player.view.scope_settled = false;
    t.player.input.analog_throttle = 0x13;
    t.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(t.entity().analog_throttle == 0);
    t.player.input.analog_throttle = 64;
    t.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(t.entity().analog_throttle == 64);
    t.player.set_movement_keys(false, false, false, false, true, false, false);
    t.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(t.entity().analog_throttle == 0);
}

// The authority arm's own-slot gate: a refused local shot spawns no round
// (no ring record, no live round); an admitted one does. [orig:
// Entity_FireWeaponAndSendPacket @0x42be3a -> return 0 @0x42c079, ahead of
// Server_ClientFiredRound @0x42bf34]
void test_authority_fire_gate_refuses_the_round() {
    for (const bool admitted : {false, true}) {
        ScopedAimFixture f;
        WeaponInstallData data;
        data.name = "WPN_AIM";
        data.clipsize = 30;
        data.rows.resize(3);
        std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "idle");
        std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "fire");
        data.rows[1].delayend = 6;
        std::snprintf(data.rows[2].name, sizeof(data.rows[2].name), "recoil");
        local_weapon_install(f.w, f.player.weapon, data, false, false, nullptr, f.player.view);
        f.w.tables.weapons.entries[1].ammo_index = 1;
        f.w.tables.ammo.entries.resize(2);
        f.w.tables.ammo.entries[1].valid = true;
        local_weapon_set_input(f.player.weapon, f.player.view, true, true, false);
        LocalWeaponPumpIO io;
        io.view = &f.player.view;
        io.is_authority = true;
        io.authority_fire_admitted = admitted;
        // Off the player-class bit the server's origin-distance test passes
        // through, leaving the own-slot gate the only refusal in play.
        // [orig: Server_ClientFiredRound @0x50c172 (`Flags & 0x100`)]
        f.entity().flags &= ~kEntityFlagPlayer;
        const int before = f.w.out.rounds.count;
        local_weapon_pump_tick(f.w, f.player.weapon, io);
        CHECK(f.w.out.rounds.count == before + (admitted ? 1 : 0));
    }
}

void test_scoped_aim_body_input_camera_and_fired_round() {
    ScopedAimFixture f;
    WeaponInstallData data;
    data.name = "WPN_AIM";
    data.clipsize = 30;
    data.rows.resize(3);
    std::snprintf(data.rows[0].name, sizeof(data.rows[0].name), "idle");
    std::snprintf(data.rows[1].name, sizeof(data.rows[1].name), "fire");
    data.rows[1].delayend = 6;
    std::snprintf(data.rows[2].name, sizeof(data.rows[2].name), "recoil");
    local_weapon_install(f.w, f.player.weapon, data, false, false, nullptr, f.player.view);
    f.player.view.scope_engaged = true;
    f.player.view.scope_settled = true;
    f.w.tables.weapons.entries[1].ammo_index = 1;
    f.w.tables.ammo.entries.resize(2);
    f.w.tables.ammo.entries[1].valid = true;
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    TickContext ctx;
    ctx.world = &f.w;
    ctx.is_authority = true;
    ctx.logic_tick = 0;
    f.w.update_all_entities(ctx); // includes the unconditional recoil draw before scoped drift
    f.player.sync_local_mounted_input_heading();
    CHECK(f.body().heading == -408 && f.body().pitch == -396);
    CHECK(f.player.input.look_heading == -408 && f.player.input.look_pitch == -396);
    // The next pre-tick input copy must not erase the body's aim additions.
    f.player.apply_player_input_pre_tick(/*pack_input=*/true);
    CHECK(f.body().inf.target_heading == -408 && f.body().inf.look_pitch == -396);
    LocalPlayerViewFrame frame = f.player.present_view_frame();
    CHECK(frame.camera_pose_valid);
    CHECK(std::abs(frame.camera.yaw_deg - (90.0 + 408.0 * 360.0 / 4294967296.0)) < 0.00001);
    CHECK(std::abs(frame.camera.pitch_deg - (-396.0 * 360.0 / 4294967296.0)) < 0.000001);
    local_weapon_set_input(f.player.weapon, f.player.view, true, true, false);
    LocalWeaponPumpIO io;
    io.view = &f.player.view;
    io.is_authority = false;
    local_weapon_pump_tick(f.w, f.player.weapon, io);
    CHECK(io.fired.valid);
    CHECK(io.fired.round.dir_yaw == -408 && io.fired.round.dir_pitch == -396);
}

// The local view clamps and leg chase precede the drift additions in the
// original body: @0x4B4945..0x4B4BC6 before @0x4B5C78..0x4B5C91.
// Exercise the real motor; a direct oscillator call cannot expose re-clamping.
void test_scoped_aim_follows_local_view_clamps_and_leg_chase() {
    const auto tick = [](ScopedAimFixture &f) {
        f.player.apply_player_input_pre_tick(/*pack_input=*/true);
        TickContext ctx;
        ctx.world = &f.w;
        ctx.is_authority = true;
        ctx.logic_tick = 0;
        f.w.update_all_entities(ctx);
        f.player.sync_local_mounted_input_heading();
        uint32_t expected_rng = World::kMissionPrng16Seed;
        for (int draw = 0; draw < 3; ++draw)
            opennova::io::rotating_prng_next16(expected_rng);
        CHECK(f.w.prng16_state == expected_rng);
    };
    {
        ScopedAimFixture f;
        f.player.input.look_pitch = -0x38E38E00;
        tick(f);
        // Retail keeps this frame's drift beyond the motor's pitch limit.
        CHECK(f.body().pitch == -0x38E38E00 - 396);
        CHECK(f.player.input.look_pitch == -0x38E38E00 - 396);
    }
    {
        ScopedAimFixture f;
        f.entity().flags |= kEntityFlagLadderContact;
        f.player.input.look_heading = -0x55555500;
        tick(f);
        CHECK(f.body().heading == -0x55555500 - 408);
        CHECK(f.player.input.look_heading == -0x55555500 - 408);
        CHECK(f.body().inf.body_heading == 0);
        CHECK(f.body().inf.leg_yaw[0] == 0 && f.body().inf.leg_yaw[1] == 0);
    }
    {
        ScopedAimFixture f;
        // A yaw beyond the replant threshold makes both legs take this input
        // before the newly sampled drift changes the current view direction.
        f.player.input.look_heading = 0x20000000;
        tick(f);
        CHECK(f.body().heading == 0x20000000 - 408);
        CHECK(f.body().inf.leg_target[0] == 0x20000000);
        CHECK(f.body().inf.leg_target[1] == 0x20000000);
    }
}

// Real parsed weapon -> installed slot -> local HUD facts, across seat changes.
// Animation stance remains an infantry value; the HUD selects its mounted frame.
// [orig: HUD_BuildEntityInfo @0x4B8539..0x4B8786; Inset query @0x4DCCB0]
void test_hud_context_tracks_mount_weapon_and_dismount() {
    static const char source[] =
        "weapon \"WPN_HUD_GUN\"\ncategory 10\nclipsize 1\nEmplacedStance 2\nend\n"
        "weapon \"WPN_HUD_FOOT\"\ncategory 4\nclipsize 1\nend\n";
    DefWeaponsFile defs{};
    CHECK(def_parse_weapons_memory(reinterpret_cast<const unsigned char *>(source),
            sizeof(source) - 1, &defs) == 0);
    CHECK(defs.count == 2);
    if (defs.count != 2) { def_free_weapons(&defs); return; }
    CHECK(defs.entries[0].emplacedstance == 2);
    CHECK(defs.entries[1].emplacedstance == 0);
    LocalWorld lw;
    Entity mount;
    mount.has_item_def = true;
    mount.item_type = 3;
    const auto gun = lw.w.registry.spawn(0, mount);
    Entity carrier;
    carrier.has_item_def = true;
    carrier.item_type = 1;
    const auto hull = lw.w.registry.spawn(0, carrier);
    lw.ai.attach(lw.local);
    auto *body = lw.ai.for_handle(lw.local);
    body->inf.stance = InfantryState::Stance::kCrouch;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    const auto read = [&]() {
        local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    };
    const auto install = [&](int row) {
        local_weapon_install(lw.w, weapon, weapon_install_data_from_def(defs.entries[row]),
                false, false, nullptr, view);
    };
    install(0);
    read();
    CHECK(frame.hud_stance == 1 && frame.hud_mount_slot == 0);
    CHECK(frame.hud_weapon_category == 10);
    CHECK(!frame.hud_keep_crosshair_while_aimed);
    // Inset keeps the spread reticle even on foot; occupying a vehicle alone
    // must not make a non-Inset optic keep it up.
    weapon.def.flags2 |= DEF_WEAPON_FLAG2_INSET;
    read();
    CHECK(frame.hud_keep_crosshair_while_aimed);
    weapon.def.flags2 &= ~DEF_WEAPON_FLAG2_INSET;
    lw.entity().mounted = true;
    lw.entity().mount_target = gun;
    for (const auto seat : {SeatType::Passenger, SeatType::Controller, SeatType::Driver}) {
        lw.entity().mount_type = seat;
        read();
        CHECK(frame.hud_stance == 3 && frame.hud_mount_slot == int(seat));
        CHECK(!frame.hud_keep_crosshair_while_aimed);
        CHECK(body->inf.stance == InfantryState::Stance::kCrouch);
    }
    lw.entity().mount_type = SeatType::Gunner;
    read();
    CHECK(frame.hud_stance == 1); // authored 2 is one-based
    install(1);
    read();
    CHECK(frame.hud_stance == 4 && frame.hud_weapon_category == 4);
    // Retail's carrier-is-a-vehicle leg reads hudInfo+0x234 straight after the
    // frame builder zeroed it, so a vehicle-carried gun still reads Emplaced.
    // [orig: memset @0x5A80B1; stale read @0x4B84D1]
    lw.w.registry.get(gun)->emplacement_parent = hull;
    read();
    CHECK(frame.hud_stance == 4);
    install(0);
    read();
    CHECK(frame.hud_stance == 1); // authored stance overrides the carrier default
    lw.entity().flags |= kEntityFlagMounted;
    read();
    CHECK(frame.hud_stance == 3); // organic mounted flag wins last
    lw.entity().flags |= kEntityFlagParachute;
    read();
    CHECK(frame.hud_stance == 5);
    lw.entity().flags &= ~(kEntityFlagMounted | kEntityFlagParachute);
    lw.entity().mounted = false;
    lw.entity().mount_target = EntityHandle{};
    lw.entity().mount_type = SeatType::None;
    body->inf.stance = InfantryState::Stance::kProne;
    read();
    CHECK(frame.hud_stance == 2 && frame.hud_mount_slot == 0);
    weapon.active = false;
    weapon.def.flags2 |= DEF_WEAPON_FLAG2_INSET;
    read();
    CHECK(frame.hud_weapon_category == 0 && !frame.hud_keep_crosshair_while_aimed);
    local_player_view_frame(nullptr, weapon, view, tracker, frame);
    CHECK(frame.hud_stance == 0 && frame.hud_mount_slot == 0);
    def_free_weapons(&defs);
}

// The HUD's death gate is the death-screen latch alone: the local dead bit and
// the death lerp camera (mode 4) leave the crosshair, instrument and scope
// passes running until the latch arrives.
// [orig: g_DeathScreenActive -- HUD_DrawCrosshair @0x592646,
//  HUD_RenderOverlays @0x5A7BBC; no Flags & 2 / g_CameraMode test on either]
void test_hud_death_gate_is_the_death_screen_latch_alone() {
    LocalWorld lw;
    LocalPlayerWeapon weapon;
    PlayerViewState view;
    LocalPlayerViewTracker tracker;
    LocalPlayerViewFrame frame;
    lw.entity().flags |= kEntityFlagDead;
    view.camera_mode = 4;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.camera_mode == 4 && !frame.hud_combat.state.death_screen);
    view.death_screen_active = true;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.hud_combat.state.death_screen);
    lw.entity().flags &= ~kEntityFlagDead;
    view.camera_mode = 0;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(frame.hud_combat.state.death_screen);
    view.death_screen_active = false;
    local_player_view_frame(&lw.w, weapon, view, tracker, frame);
    CHECK(!frame.hud_combat.state.death_screen);
}

int main() {
    test_hud_context_tracks_mount_weapon_and_dismount();
    test_hud_death_gate_is_the_death_screen_latch_alone();
    test_rendered_scope_applies_elevation_and_parallax();
    test_scoped_aim_follows_local_view_clamps_and_leg_chase();
    test_scoped_aim_original_sequences();
    test_scoped_aim_gates_and_independent_stance_resets();
    test_scoped_aim_body_input_camera_and_fired_round();
    test_pack_drops_the_binocular_toggle_on_movement();
    test_pack_owns_the_movement_latch_and_unscope();
    test_pack_masks_movement_under_a_nomove_weapon();
    test_pack_analog_axes_and_their_hysteresis();
    test_authority_fire_gate_refuses_the_round();
    test_scoped_aim_survives_kernel_replacement_without_sharing_sessions();
    test_target_lock_cadence_and_audio();
    {
        // The adjust clamp's -1 floor: offline, or in session with the
        // sniper-zoom rule bit; the 0 floor in session without it.
        WeaponScopeZero zero;
        zero.max_steps = 10; zero.step_metres = 100; zero.default_metres = 300;
        CHECK(weapon_scope_zero_initial(zero) == 3);
        CHECK(weapon_scope_zero_adjust(zero, 0, -1, false, false) == -1);
        CHECK(weapon_scope_zero_adjust(zero, 0, -1, true, false) == 0);
        CHECK(weapon_scope_zero_adjust(zero, 0, -1, true, true) == -1);
        CHECK(weapon_scope_zero_adjust(zero, 10, 1, true, true) == 10);
        zero.min_steps = 2;
        CHECK(weapon_scope_zero_adjust(zero, 2, -1, false, false) == 2);
    }
    test_scope_zero_request_keys_on_the_rule_bit_and_clicks();
    test_scope_zero_install_seeds_the_yaw_term();
    test_scope_zero_yaw_term();
    test_scope_zero_bake_max_range();
    test_mounted_first_person_camera_belongs_to_the_carrier();
    test_seat_bone_pose_rider_legs();
    test_unadmitted_seat_takes_the_ground_or_person_leg();
    test_shake_turns_the_bam_heading();
    test_chase_lookahead_follows_the_hull_on_every_compose();
    test_scope_toggle_refuses_inactive_weapon();
    test_scope_up_refused_while_moving_on_scoped_weapon();
    test_inset_scope_refused_under_nvg();
    test_mid_ease_toggle_refused_then_forcescoped_pins_the_sight();
    test_binoculars_refused_while_power_throw_charges_and_rng_untouched();
    test_binocular_sway_seeds_once_per_activation();
    test_binoculars_refused_scoped_in_gunner_seat();
    test_nvg_drops_a_settled_inset_scope_and_restores_it();
    test_nvg_over_a_non_inset_scope_leaves_it_alone();
    test_tip_events_from_scope_nvg_and_binoculars();
    test_tick_stamps_the_death_camera_on_the_local_dead_edge();
    test_tick_without_a_player_resolves_first_person();
    test_frame_reads_the_state_and_the_card_selector();
    test_frame_carries_the_framefx_dispatch_facts();
    test_frame_carries_the_nvg_lens_arm();
    test_frame_carries_the_nvg_sighted_arm();
    test_authored_rotation_bias_continues_through_air_reload_and_rebake();
    test_airborne_view_bias_keeps_interp_and_resumes_on_landing();
    test_airborne_bias_is_separate_from_reload_and_force_scope_admission();
    test_scope_fov_target_and_render_queries_share_weather_state();
    test_frame_publishes_the_fp_draw_gates();
    test_scope_zoom_clamps_and_weapon_category_fov_reset();
    test_weapon_cycle_route_steps_the_zoom_and_the_mount_clamp();
    test_default_wheel_binding_zooms_in_away_from_the_player();
    test_frame_chase_shake_consumes_the_tick();
    test_use_item_vehicle_loadout_zone_gates();
    test_view_uses_current_motor_offset_and_live_position();
    test_pump_feeds_the_heat_window_water_gate_from_the_body_z();
    test_weapon_trace_records_one_sample_per_pump_tick();
    test_weapon_trace_samples_since_is_incremental();
    test_local_weapon_input_block_mirrors_the_pump_gate();
    test_direct_mount_retains_running_pose_and_forced_mount_targets_ads();
    test_category_request_resets_before_commit_while_cycle_retains_pose();
    test_host_own_fire_ends_spawn_protection();
    if (failures == 0) std::printf("local_player_view_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
