// Player view-state tests [orig: CNetPlayerInterp_Setup @ 0x4df36e;
// ThirdPersonCamera_Update @ 0x437af0; Player_ToggleWeaponScope @ 0x4df0c0..401;
// Render_SetViewAndProjectionMatrices @ 0x58d900]: the fixed-tick scope ease and
// anchor chase (render-cadence invariance — the review's 30/60/144 fps case),
// the fov policy, and the input-dispatch gates in front of the FSM requests.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstring>
#include <formats/def/def.h>
#include <runtime/world/player_weapon.h>

#include <runtime/renderer/aspect_ratio.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/player_view.h>
#include <runtime/world/tp_camera_mount.h>
#include <runtime/world/weapon_fsm.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

uint64_t pose_interp_fingerprint(uint64_t hash, const PlayerViewBiasInterp &interp) {
    const auto word = [&hash](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xFFu;
            hash *= UINT64_C(1099511628211);
        }
    };
    const auto pose = [&word](const PlayerViewPose &value) {
        for (float component : value.position_q16) {
            uint32_t bits;
            std::memcpy(&bits, &component, sizeof(bits));
            word(bits);
        }
        for (uint32_t component : value.rotation_bam) word(component);
    };
    word(interp.remaining);
    pose(interp.velocity);
    pose(interp.current);
    pose(interp.target);
    word(interp.active ? 1u : 0u);
    for (int32_t component : interp.position_bias_q16) word(static_cast<uint32_t>(component));
    for (int32_t component : interp.rotation_bias_bam) word(static_cast<uint32_t>(component));
    return hash;
}

void test_authored_pose_interp_matches_original_six_lane_traces() {
    // Raw-machine oracles: retail JO 1.7.5.7 CNetPlayerInterp_Setup 4DDFD0,
    // Player_StepFpViewBiasInterp 4DDD20, including its real CRT ftol calls.
    // FNV-1a covers every word of counter, six velocities, current, target,
    // active and six published biases: initial setup + twenty ticks. The
    // reverse row additionally fingerprints setup after tick 3, with a
    // deliberately unrelated idle source proving that active state wins.
    struct Case {
        uint32_t steps;
        PlayerViewPose hip;
        PlayerViewPose ads;
        int reverse_after;
        uint64_t fingerprint;
    };
    const Case cases[] = {
        {15, {{256, 512, 768}, {0x10000000u, 0x10000000u, 0xF0000000u}},
             {{512, -256, 1024}, {0x08000000u, 0x20000000u, 0x10000000u}}, -1,
             UINT64_C(0x0A55F92C1B3BA86C)},
        {7, {{-6796.8f, 5952, -42240}, {0x10000000u, 0x10000000u, 0xF0000000u}},
            {{-13452.8f, 10368, -40640}, {0x08000000u, 0x20000000u, 0x10000000u}}, -1,
            UINT64_C(0x2D18B3D799CDE452)},
        {1, {{1, 2, 3}, {0x7FFFFF80u, 0x7FFFFF81u, 0xFFFFFFFFu}},
            {{4, 5, 6}, {0, 0, 0}}, -1, UINT64_C(0x788BD8A6C8CD08D0)},
        {15, {{0, 0, 0}, {0x7FFFFF80u, 0x7FFFFF81u, 0xFFFFFFFFu}},
             {{0, 0, 0}, {0, 0, 0}}, -1, UINT64_C(0x7ACB032789A2F89A)},
        {7, {{11, 22, 33}, {0x12340001u, 0xFFFFFFFEu, 4}},
            {{11, 22, 33}, {0x12340000u, 0xFFFFFFFFu, 0}}, -1,
            UINT64_C(0x46035FCD17F8B54C)},
        {7, {{11, 22, 33}, {1, 2, 3}}, {{11, 22, 33}, {1, 2, 3}}, -1,
            UINT64_C(0x2D64F6B9C5C7BCFE)},
        {15, {{256, 512, 768}, {0x10000000u, 0x10000000u, 0xF0000000u}},
             {{512, -256, 1024}, {0x08000000u, 0x20000000u, 0x10000000u}}, 3,
             UINT64_C(0x97C74E18FC4A6BC1)},
    };
    for (const Case &c : cases) {
        PlayerViewBiasInterp interp;
        player_view_bias_interp_setup(interp, c.steps, c.hip, c.ads);
        uint64_t hash = pose_interp_fingerprint(UINT64_C(14695981039346656037), interp);
        for (int tick = 0; tick < 20; ++tick) {
            if (tick == c.reverse_after) {
                const PlayerViewPose unrelated{{99, 88, 77}, {7, 8, 9}};
                player_view_bias_interp_setup(interp, 15, unrelated, c.hip);
                hash = pose_interp_fingerprint(hash, interp);
            }
            player_view_bias_interp_step(interp, c.hip);
            hash = pose_interp_fingerprint(hash, interp);
        }
        if (hash != c.fingerprint)
            std::printf("pose trace mismatch: steps %u reverse %d, got %llx expected %llx\n",
                    c.steps, c.reverse_after, static_cast<unsigned long long>(hash),
                    static_cast<unsigned long long>(c.fingerprint));
        CHECK(hash == c.fingerprint);
    }
}

void test_authored_pose_interp_keeps_original_snap_and_completion_rules() {
    const PlayerViewPose hip{{256, 512, 768}, {0x10000000u, 0x10000000u, 0xF0000000u}};
    const PlayerViewPose ads{{512, -256, 1024}, {0x08000000u, 0x20000000u, 0x10000000u}};
    PlayerViewBiasInterp interp;
    player_view_bias_interp_setup(interp, 15, hip, ads);
    CHECK(interp.remaining == 0 && interp.active);
    CHECK(interp.velocity.rotation_bam[0] == 0x00888888u);
    CHECK(interp.velocity.rotation_bam[1] == 0xFEEEEEEFu);
    CHECK(interp.velocity.rotation_bam[2] == 0xFDDDDDDEu);
    player_view_bias_interp_step(interp, hip);
    CHECK(interp.current.rotation_bam[0] == 0x0F777778u);
    CHECK(interp.current.rotation_bam[1] == ads.rotation_bam[1]);
    CHECK(interp.current.rotation_bam[2] == ads.rotation_bam[2]);
    CHECK(interp.position_bias_q16[0] == 17 && interp.position_bias_q16[1] == -51);
    for (int tick = 1; tick < 15; ++tick) player_view_bias_interp_step(interp, hip);
    CHECK(interp.active); // all lanes landed, latch waits one more call
    for (int i = 0; i < 3; ++i) CHECK(interp.current.rotation_bam[i] == ads.rotation_bam[i]);
    player_view_bias_interp_step(interp, hip);
    CHECK(!interp.active);

    // An idle all-zero setup arms seven ticks without copying source into
    // current; a running zero-delta setup only deactivates once spent.
    interp = {};
    player_view_bias_interp_setup(interp, 7, hip, hip);
    CHECK(interp.active && interp.remaining == 7);
    CHECK(interp.current.rotation_bam[0] == 0 && interp.current.position_q16[0] == 0);
    const PlayerViewPose zero;
    player_view_bias_interp_setup(interp, 1, ads, zero);
    CHECK(interp.active && interp.remaining == 7);
    for (int tick = 0; tick < 7; ++tick) player_view_bias_interp_step(interp, hip);
    CHECK(!interp.active);
}

void test_authored_pose_def_promotes_parser_precision_and_wrapping_bam() {
    using namespace opennova::def;
    const char text[] =
            "weapon \"POSE\"\npos 1,2,3,179.99999,180.00001,359.99999\n"
            "tpos 4,5,6,360,-0.1,356.750\nend\n";
    DefWeaponsFile parsed{};
    CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text), sizeof(text) - 1,
                                  &parsed) == 0);
    CHECK(parsed.count == 1);
    if (parsed.count == 1) {
        const DefWeaponDef &row = parsed.entries[0];
        // Original Math_ParseFixedPoint16 is unsigned digit syntax: a
        // leading minus yields zero, while the 360-degree seam wraps in BAM.
        CHECK(row.pos_rotation_deg_q16[0] == 11796479);
        CHECK(row.pos_rotation_deg_q16[1] == 11796481);
        CHECK(row.pos_rotation_deg_q16[2] == 23592959);
        CHECK(row.tpos_rotation_deg_q16[1] == 0);
        const WeaponInstallData install = weapon_install_data_from_def(row);
        CHECK(install.view_hip_pose.position_q16[0] == 256.0f);
        CHECK(install.view_ads_pose.position_q16[2] == 1536.0f);
        const uint32_t hip[3] = {0x7FFFFECAu, 0x80000036u, 0xFFFFFE4Au};
        const uint32_t ads[3] = {0xFFFFFF00u, 0u, 0xFDB05A08u};
        for (int i = 0; i < 3; ++i) {
            CHECK(install.view_hip_pose.rotation_bam[i] == hip[i]);
            CHECK(install.view_ads_pose.rotation_bam[i] == ads[i]);
        }
    }
    def_free_weapons(&parsed);
}

// A bound optical slot with distinct authored poses: position lane 0 spans
// 315 Q16 (15 * 21 = 7 * 45) and rotation lane 0 spans 0x06900000 (15 *
// 0x700000 = 7 * 0xF00000), so every production ease length divides exactly
// and each lane snaps on its last step. The raise's rotation lane snaps on
// tick 1 (the unsigned-velocity quirk); the position lane paces the ease.
PlayerViewState bound_view() {
    PlayerViewState v;
    v.weapon_pose_bound = true;
    v.weapon_ads_pose = {{315.0f, 0.0f, 0.0f}, {0x06900000u, 0u, 0u}};
    return v;
}

// The promoter fires on the tick the interp's active latch drops: the lanes
// snap on the fifteenth step and the stepper reports done on the SIXTEENTH
// call [orig: Player_StepFpViewBiasInterp @0x4DDD47..0x4DDDC3 (the all-zero
// velocity return clears active only on the call after the snaps);
// Player_UpdatePerFrame promoter @0x4de4d9..0x4de4f7].
void test_scope_ease_settles_on_the_call_after_the_last_snap() {
    PlayerViewState v = bound_view();
    const float eye[3] = {0, 0, 0};
    CHECK(player_view_set_engaged(v, true, false));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == -21.0f); // (hip - tpos) / 15
    for (int i = 1; i <= kScopeEaseSteps; ++i) {
        CHECK(!player_view_scope_settled(v));
        player_view_tick(v, eye);
        CHECK(v.weapon_pose_interp.current.position_q16[0] == 21.0f * static_cast<float>(i));
        CHECK(v.weapon_pose_interp.position_bias_q16[0] == 21 * i);
    }
    CHECK(player_view_scope_ease_active(v)); // the lanes landed; the latch holds
    CHECK(!player_view_scope_settled(v));
    player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v));
    CHECK(player_view_scope_settled(v));
    CHECK(player_view_scope_fraction(v) == 1.0f);
    player_view_tick(v, eye); // idle: nothing steps, nothing re-promotes
    CHECK(player_view_scope_settled(v) && !player_view_scope_ease_active(v));
    CHECK(v.weapon_pose_interp.position_bias_q16[0] == 315);
    CHECK(player_view_set_engaged(v, false, false));
    CHECK(!player_view_scope_settled(v)); // cleared at the toggle @0x4df20c
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 21.0f); // tpos -> hip
    for (int i = kScopeEaseSteps - 1; i >= 0; --i) {
        player_view_tick(v, eye);
        CHECK(v.weapon_pose_interp.current.position_q16[0] == 21.0f * static_cast<float>(i));
    }
    CHECK(player_view_scope_ease_active(v));
    player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v));
    CHECK(!player_view_scope_settled(v)); // the promoter mirrors the OFF target
    CHECK(player_view_scope_fraction(v) == 0.0f);
}

// A bound def whose hip and tpos coincide: the idle zero-delta Setup arms the
// counter instead of velocities, and the stepper clears active when that
// counter spends, on the FIFTEENTH call; an unbound slot deactivates on its
// first step and promotes at once [orig: CNetPlayerInterp_Setup
// @0x4DE0CC..0x4DE11F; Player_StepFpViewBiasInterp counter @0x4DDD3C..0x4DDD45,
// all-zero return @0x4DDD47..0x4DDDC3, null slot/Def @0x4DDD2B..0x4DDDBC].
void test_zero_span_ease_promotes_when_its_counter_spends() {
    PlayerViewState v;
    v.weapon_pose_bound = true;
    const float eye[3] = {0, 0, 0};
    CHECK(player_view_set_engaged(v, true, false));
    CHECK(v.weapon_pose_interp.remaining == static_cast<uint32_t>(kScopeEaseSteps));
    CHECK(player_view_scope_ease_active(v));
    for (int i = 1; i < kScopeEaseSteps; ++i) {
        player_view_tick(v, eye);
        CHECK(player_view_scope_ease_active(v) && !player_view_scope_settled(v));
        CHECK(player_view_scope_fraction(v) == 0.0f); // a zero-span raise reports its source
    }
    player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v) && player_view_scope_settled(v));
    CHECK(player_view_scope_fraction(v) == 1.0f);

    PlayerViewState unbound;
    CHECK(player_view_set_engaged(unbound, true, false));
    CHECK(player_view_scope_ease_active(unbound));
    player_view_tick(unbound, eye);
    CHECK(!player_view_scope_ease_active(unbound) && player_view_scope_settled(unbound));
}

// The promoted byte is independent of the target. A category-key camera
// reset retains it and the rotation bookkeeping; a no-weapon clear discards it.
void test_scope_reset_preserves_original_switch_bookkeeping() {
    PlayerViewState v;
    v.scope_engaged = v.scope_settled = true;
    v.scope_hipfire = false;
    auto &interp = v.weapon_pose_interp;
    interp.active = true;
    interp.remaining = 37;
    interp.velocity = {{1, 2, 3}, {0x11111111u, 0x80000001u, 0xFFFFFFFFu}};
    interp.current = {{4, 5, 6}, {0x22222222u, 0x33333333u, 0x44444444u}};
    interp.target = {{7, 8, 9}, {0x55555555u, 0x66666666u, 0x77777777u}};
    for (int i = 0; i < 3; ++i) {
        interp.position_bias_q16[i] = 11 + i;
        interp.rotation_bias_bam[i] = 14 + i;
    }
    // Executed original Player_ResetCameraAndMovementState @0x4DE1F0 on
    // this 21-word state: counter=37, rotations copied from velocity, position
    // triples zero, active=0, promoted=1, engaged=0, hipfire=1; biases unchanged.
    player_view_weapon_switch_reset(v);
    CHECK(!v.scope_engaged && player_view_scope_settled(v) && v.scope_hipfire);
    CHECK(!interp.active && interp.remaining == 37 && !v.weapon_pose_bound);
    const uint32_t velocity[3] = {0x11111111u, 0x80000001u, 0xFFFFFFFFu};
    const uint32_t target[3] = {0x55555555u, 0x66666666u, 0x77777777u};
    for (int i = 0; i < 3; ++i) {
        CHECK(interp.velocity.position_q16[i] == 0 && interp.current.position_q16[i] == 0 &&
              interp.target.position_q16[i] == 0);
        CHECK(interp.current.rotation_bam[i] == velocity[i] &&
              interp.velocity.rotation_bam[i] == velocity[i] && interp.target.rotation_bam[i] == target[i]);
        CHECK(interp.position_bias_q16[i] == 11 + i && interp.rotation_bias_bam[i] == 14 + i);
    }
    const float eye[3] = {};
    player_view_tick(v, eye);
    CHECK(player_view_scope_settled(v)); // an idle reset does not run the promoter
    player_view_scope_reset(v);
    CHECK(!v.scope_engaged && !v.scope_settled && v.scope_hipfire);
    CHECK(!v.weapon_pose_interp.active && v.weapon_pose_interp.remaining == 0);
}

// The review's fps case: the SAME simulated time must produce the SAME state no
// matter how the render loop groups the ticks (one per frame at 60 fps, four
// then zero at 15/144 fps, ...). The state is a pure function of the tick
// count, so any grouping of N ticks lands identically.
void test_equal_ticks_equal_state_regardless_of_frame_grouping() {
    const float eye[3] = {100.0f, -40.0f, 12.0f};

    PlayerViewState per_frame = bound_view(); // "60 fps": one tick per render frame
    CHECK(player_view_set_engaged(per_frame, true, false));
    per_frame.debug_third_person_on_foot = true;  // on-foot 3P = the debug override
    for (int i = 0; i < 24; ++i) player_view_tick(per_frame, eye);

    PlayerViewState bursty = bound_view();    // "uneven fps": frames of 4/0/3/0/1... ticks
    CHECK(player_view_set_engaged(bursty, true, false));
    bursty.debug_third_person_on_foot = true;
    const int frames[] = {4, 0, 3, 0, 1, 7, 0, 0, 2, 5, 0, 2};
    int total = 0;
    for (int n : frames) {
        for (int i = 0; i < n; ++i) player_view_tick(bursty, eye);
        total += n;
    }
    CHECK(total == 24);
    CHECK(per_frame.scope_settled && bursty.scope_settled);
    CHECK(per_frame.weapon_pose_interp.active == bursty.weapon_pose_interp.active);
    for (int i = 0; i < 3; ++i) {
        CHECK(per_frame.weapon_pose_interp.current.position_q16[i] ==
              bursty.weapon_pose_interp.current.position_q16[i]);
        CHECK(per_frame.weapon_pose_interp.position_bias_q16[i] ==
              bursty.weapon_pose_interp.position_bias_q16[i]);
    }
    CHECK(per_frame.tp_anchor_valid && bursty.tp_anchor_valid);
    for (int i = 0; i < 3; ++i) CHECK(per_frame.tp_anchor[i] == bursty.tp_anchor[i]);
}

void test_anchor_chase_quarter_step_and_seeding() {
    PlayerViewState v;
    float eye[3] = {8.0f, 0.0f, 4.0f};
    // On foot, third person is only ever the debug override (the arbiter
    // resolves the chase preference to first person outside a control seat).
    v.debug_third_person_on_foot = true;
    player_view_tick(v, eye); // first 3P tick seeds AT the eye
    CHECK(v.third_person);
    CHECK(v.tp_anchor_valid);
    CHECK(v.tp_anchor[0] == 8.0f && v.tp_anchor[2] == 4.0f);

    // Move the eye: each tick closes exactly a quarter of the gap. [orig: @ 0x437c8d]
    eye[0] = 16.0f;
    player_view_tick(v, eye);
    CHECK(v.tp_anchor[0] == 10.0f);
    player_view_tick(v, eye);
    CHECK(v.tp_anchor[0] == 11.5f);

    // Leaving third person invalidates; re-entering re-seeds at the current eye.
    v.debug_third_person_on_foot = false;
    player_view_tick(v, eye);
    CHECK(!v.third_person && !v.tp_anchor_valid);
    v.debug_third_person_on_foot = true;
    player_view_tick(v, eye);
    CHECK(v.tp_anchor_valid && v.tp_anchor[0] == 16.0f);
}

// THE MODE ARBITER [orig: Render_ProcessMainSceneFrame @ 0x5ca1d2..0x5ca1f4]:
// the chase preference defaults to selected [orig: Client_ResetGameSessionState
// @ 0x42ca3c], resolves to third person only in a control seat, and falls back
// to first person the tick the seat goes (dismount, or a gunner/passenger seat
// — the board and detach paths themselves never touch the camera
// [orig: Entity_ProcessVehicleAttach @ 0x435aa0; Entity_DetachFromVehicle
//  parentSlot = 0 @ 0x435921]). The view actions flip the preference and
// re-resolve at once; the debug override is the only on-foot third person.
void test_mode_arbiter() {
    PlayerViewState v;
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    CHECK(v.third_person_selected);
    player_view_tick(v, eye);
    CHECK(!v.third_person);          // on foot: first person despite the preference
    CHECK(!v.tp_anchor_valid);
    v.mount.control_seat = true;     // boarded a driver/control seat
    player_view_tick(v, eye);
    CHECK(v.third_person);           // the chase, without any camera write on board
    CHECK(v.tp_anchor_valid);
    v.mount.control_seat = false;    // dismounted (or a gunner/passenger seat)
    player_view_tick(v, eye);
    CHECK(!v.third_person && !v.tp_anchor_valid);
    // view1st / viewwithgun: first person selected keeps a driver in first person.
    player_view_set_third_person_selected(v, false);
    v.mount.control_seat = true;
    player_view_tick(v, eye);
    CHECK(!v.third_person);
    // viewchase: the preference resolves immediately in the seat.
    player_view_set_third_person_selected(v, true);
    CHECK(v.third_person);
    // The debug override: third person on foot, the one non-stock mode.
    v.mount.control_seat = false;
    v.debug_third_person_on_foot = true;
    player_view_tick(v, eye);
    CHECK(v.third_person);
    v.debug_third_person_on_foot = false;
    player_view_tick(v, eye);
    CHECK(!v.third_person);
    CHECK(v.camera_mode == 0);

    // The full arbiter [orig: Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b].
    // Dead on foot -> the death camera (mode 4).
    v.local_dead = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 4 && !v.third_person);
    // g_RulesFlags bit 0 keeps the seat-derived mode instead.
    v.rules_no_death_cam = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0);
    v.mount.control_seat = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 1 && v.third_person);
    v.mount.control_seat = false;
    v.rules_no_death_cam = false;
    // The death screen overrides the dead bit: sub-mode 1 -> the chase,
    // 0 / 2 -> first person, anything else keeps the seat verdict.
    v.death_screen_active = true;
    v.death_screen_submode = 1;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 1);
    v.death_screen_submode = 0;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0);
    v.death_screen_submode = 2;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0);
    v.death_screen_submode = 7;
    v.mount.control_seat = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 1);
    v.mount.control_seat = false;
    v.death_screen_active = false;
    // Alive again -> first person.
    v.local_dead = false;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0);
    // The end-of-round gate on foot -> 4; in a seat it does not.
    v.round_ended = true;
    v.on_foot = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 4);
    v.on_foot = false;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0);
    v.round_ended = false;
    v.on_foot = true;
    // The in-session force-first-person rule (bit 0x40) beats the seat chase.
    v.mount.control_seat = true;
    v.in_session = true;
    v.rules_force_first_person = true;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 0 && !v.third_person);
    v.rules_force_first_person = false;
    player_view_tick(v, eye);
    CHECK(v.camera_mode == 1);
    v.mount.control_seat = false;
    v.in_session = false;
}

void test_fov_policy() {
    PlayerViewState v;
    CHECK(player_view_fov_h_deg(v, 75 << 16, false, false, 1) == 75.0f);
    // Scoped divides the weather current; Sighted selects a separate 80/zoom.
    CHECK(player_view_fov_h_deg(v, 60 << 16, true, false, 4) == 15.0f);
    CHECK(player_view_fov_h_deg(v, 60 << 16, true, true, 4) == 20.0f);
    CHECK(player_view_fov_h_deg(v, 60 << 16, false, true, 3) == 1747626.0f / 65536.0f);
    // Third-person mode clears the optical flags at the caller. Its raw
    // weather current still eases back after the target's 80-degree reset.
    v.third_person = true;
    CHECK(player_view_fov_h_deg(v, 55 << 16, false, false, 1) == 55.0f);
}

// The x87 stack keeps the angle below the truncated sine result; the second
// trig operation is cos(angle), not cos(4194304). Mission yaw reverses BAM.
// [orig: Binoculars_RandomizeSwayOffsets @0x4dd830; render adds @0x5ca403..0x5ca407]
void test_binocular_sway_axes_and_quantization() {
    struct Sample { float angle; int32_t sin_q22; int32_t cos_q22; };
    // Pinned with the binary's dbl_7C3608 (not exact tau / 2^32).
    const Sample samples[] = {{0.0f, 0, 4194304}, {0.25f, 4194303, -201},
        {0.5f, 402, -4194303}, {0.75f, -4194303, -201}, {0.125f, 2965891, 2965749}};
    constexpr double degrees_per_q22 = 360.0 * 8.0 / 4294967296.0;
    for (const auto &sample : samples) {
        float yaw, pitch;
        player_view_binocular_sway_offset(sample.angle, yaw, pitch);
        CHECK(yaw == static_cast<float>(-sample.sin_q22 * degrees_per_q22));
        CHECK(pitch == static_cast<float>(sample.cos_q22 * degrees_per_q22));
    }
}

void test_binoculars_effective_state_and_fov() {
    PlayerViewState v;
    CHECK(!v.binoculars_requested);
    CHECK(!v.binoculars_raised);
    CHECK(!v.binoculars_view_active);

    // Toggling records intent; the effective update raises the body pose and
    // first-person optical view while all gates permit it.
    CHECK(player_view_toggle_binoculars(v));
    CHECK(v.binoculars_requested);
    player_view_update_effective_modes(v, true, false);
    CHECK(v.binoculars_raised);
    CHECK(v.binoculars_view_active);
    CHECK(player_view_fov_h_deg(v, 80 << 16, false, false, 1) == kBinocularCameraFovHDeg);

    // Movement suppresses both derived states without consuming the request;
    // releasing movement restores them.
    CHECK(!player_view_move_input(v, true, 0));
    player_view_update_effective_modes(v, true, false);
    CHECK(v.binoculars_requested);
    CHECK(!v.binoculars_raised);
    CHECK(!v.binoculars_view_active);
    CHECK(!player_view_move_input(v, false, 0));
    player_view_update_effective_modes(v, true, false);
    CHECK(v.binoculars_raised && v.binoculars_view_active);

    // Third person preserves the raised pose for observers but suppresses the
    // optical view and returns the camera to the base fov.
    v.third_person = true;
    player_view_update_effective_modes(v, true, false);
    CHECK(v.binoculars_requested);
    CHECK(v.binoculars_raised);
    CHECK(!v.binoculars_view_active);
    CHECK(player_view_fov_h_deg(v, 80 << 16, false, false, 1) == kPlayerCameraFovHDeg);

    // Death and round end are reversible gates too.
    v.third_person = false;
    player_view_update_effective_modes(v, false, false);
    CHECK(v.binoculars_requested);
    CHECK(!v.binoculars_raised && !v.binoculars_view_active);
    player_view_update_effective_modes(v, true, true);
    CHECK(v.binoculars_requested);
    CHECK(!v.binoculars_raised && !v.binoculars_view_active);
    player_view_update_effective_modes(v, true, false);
    CHECK(v.binoculars_raised && v.binoculars_view_active);

    // Explicit toggle-off is the operation that clears the persistent intent.
    CHECK(!player_view_toggle_binoculars(v));
    CHECK(!v.binoculars_requested);
    CHECK(!v.binoculars_raised);
    CHECK(!v.binoculars_view_active);
}

void test_nvg_toggle_gain_and_first_person_visibility() {
    PlayerViewState v;
    CHECK(!v.nvg_active);
    CHECK(v.nvg_gain == kNvgGainMin);
    CHECK(!player_view_nvg_visible(v));

    CHECK(player_view_toggle_nvg(v));
    CHECK(player_view_nvg_visible(v));
    v.third_person = true;
    v.camera_mode = 1;
    CHECK(v.nvg_active); // camera suppression does not consume the toggle
    CHECK(!player_view_nvg_visible(v));
    v.third_person = false;
    v.camera_mode = 0;
    CHECK(player_view_nvg_visible(v));
    // The death lerp camera (mode 4) is not third person, yet retail's NVG
    // world/post legs all need g_CameraMode == 0 [orig:
    // CTerrainRenderer_BuildLightingShaderConstants @ 0x5c81fe; Render_TerrainScene
    // @ 0x610d09; Render_ProcessMainSceneFrame @ 0x5ca6b8].
    v.local_dead = true;
    player_view_resolve_mode(v);
    CHECK(v.camera_mode == 4 && !v.third_person && v.nvg_active);
    CHECK(!player_view_nvg_visible(v));
    v.local_dead = false;
    player_view_resolve_mode(v);
    CHECK(player_view_nvg_visible(v));

    CHECK(player_view_adjust_nvg_gain(v, 1) == 1);
    CHECK(player_view_adjust_nvg_gain(v, 100) == kNvgGainMax);
    CHECK(player_view_adjust_nvg_gain(v, -100) == kNvgGainMin);

    // Gain controls remain live and retained while NVG itself is inactive.
    CHECK(!player_view_toggle_nvg(v));
    CHECK(!player_view_nvg_visible(v));
    CHECK(player_view_adjust_nvg_gain(v, 3) == 3);
    CHECK(!v.nvg_active);
    CHECK(player_view_toggle_nvg(v));
    CHECK(v.nvg_gain == 3);
    CHECK(player_view_nvg_visible(v));
}

void test_fov_vertical_conversion() {
    // [orig: @ 0x58d900 fovY = 2*atan(tan(fovX/2)/aspect)] Square viewport: v == h.
    CHECK(std::fabs(fov_vertical_from_horizontal_deg(80.0f, 1.0f) - 80.0f) < 1e-4f);
    // 4:3 at 80 horizontal: 2*atan(tan(40 deg)/(4/3)) = 64.36644 degrees.
    CHECK(std::fabs(fov_vertical_from_horizontal_deg(80.0f, 4.0f / 3.0f) - 64.36644f) < 1e-3f);
    // Wider view -> smaller vertical fov, monotonically.
    CHECK(fov_vertical_from_horizontal_deg(80.0f, 16.0f / 9.0f) <
          fov_vertical_from_horizontal_deg(80.0f, 4.0f / 3.0f));
}

// [orig: Render_SetAspectRatioMode @0x58d8a7 -> Render_SetViewAndProjectionMatrices
//  @0x58d971..0x58d9de] The frame's projection keeps the HORIZONTAL fov across the
// real surface in every mode; the vertical half-extent follows the SELECTED
// ratio; the pass is the aspect-1/selected perspective stretched selected/(h/w)
// onto the surface. (Pre-fix the shell widened the horizontal fov instead.)
void test_view_projection_retail_stretch() {
    // Mode 0 (4:3) on a 16:9 surface -- retail's stock profile on a wide monitor.
    const ViewProjection wide = view_projection(80.0f, 0, 1920, 1080);
    CHECK(wide.fov_h_deg == 80.0f);
    CHECK(std::fabs(wide.fov_v_deg - 64.36644f) < 1e-3f); // the 4:3 vertical, not 16:9's
    CHECK(std::fabs(wide.aspect - 4.0f / 3.0f) < 1e-5f);
    CHECK(std::fabs(wide.scale_y - 0.75f / 0.5625f) < 1e-5f);
    CHECK(wide.target_w == 1920 && wide.target_h == 1440); // width kept, height grown
    // Mode 1 (16:10) on 1920x1200: scaleY 0.96 and the catalog's 53.4468 degrees.
    const ViewProjection tall = view_projection(80.0f, 1, 1920, 1200);
    CHECK(std::fabs(tall.scale_y - 0.96f) < 1e-5f);
    CHECK(std::fabs(tall.fov_v_deg - 53.4468f) < 1e-3f);
    CHECK(tall.target_w == 2000 && tall.target_h == 1200); // height kept, width grown
    // Mode 2 (16:9) on a 16:9 surface and the native mode: no stretch, the
    // surface itself, the vertical fov of the surface's own aspect.
    const ViewProjection matched = view_projection(80.0f, 2, 1920, 1080);
    CHECK(matched.scale_y == 1.0f);
    CHECK(matched.target_w == 1920 && matched.target_h == 1080);
    CHECK(std::fabs(matched.fov_v_deg - fov_vertical_from_horizontal_deg(80.0f, 16.0f / 9.0f)) < 1e-5f);
    const ViewProjection native = view_projection(80.0f, -1, 1920, 1080);
    CHECK(native.scale_y == 1.0f && native.fov_v_deg == matched.fov_v_deg);
    CHECK(std::fabs(native.aspect - 16.0f / 9.0f) < 1e-5f);
    // The scoped fov rides the same projection (80/zoom horizontal, mode 0 vertical).
    const ViewProjection scoped = view_projection(20.0f, 0, 1920, 1080);
    CHECK(scoped.fov_h_deg == 20.0f);
    CHECK(std::fabs(scoped.fov_v_deg - 15.0668f) < 1e-3f);
    // A sizeless surface degrades to the identity aspect.
    const ViewProjection none = view_projection(80.0f, 0, 0, 0);
    CHECK(none.scale_y == 1.0f && none.fov_v_deg == 80.0f);

    // The FP viewmodel pass shares scaleY [orig: @0x4dee7f / @0x58f6b0]: the
    // focal ratio is the ratio of the horizontal half-tangents, aspect-invariant.
    CHECK(viewmodel_focal_ratio(80.0f, 80.0f) == 1.0f);
    CHECK(std::fabs(viewmodel_focal_ratio(80.0f, 60.0f) - 1.45330f) < 1e-4f);
    CHECK(std::fabs(viewmodel_focal_ratio(20.0f, 80.0f) - 0.21014f) < 1e-4f);
    CHECK(viewmodel_focal_ratio(80.0f, 0.0f) == 1.0f);
}

// The NVG scene's pass [orig: NVG_RenderScene @0x5d2954..0x5d296d; NVG_RenderScopedScene
// @0x5d29e4..0x5d2a2a]: every arm rasterises the 512 square, the frame-shaped
// arms at the frame's frustum (its aspect kept: non-square texels), the Scoped
// arm at its square frustum.
void test_nvg_view_projection() {
    opennova::renderer::FrameFxNvgPlan frame_arm;
    frame_arm.scene = frame_arm.composite = true;
    opennova::renderer::FrameFxNvgPlan lens_arm = frame_arm;
    lens_arm.lens = true;
    opennova::renderer::FrameFxNvgPlan sighted_arm = frame_arm;
    sighted_arm.sighted = true;
    const ViewProjection wide = view_projection(80.0f, 0, 1920, 1080);
    const ViewProjection nvg = nvg_view_projection(wide, frame_arm, 0.75f, 1);
    CHECK(nvg.fov_h_deg == wide.fov_h_deg && nvg.fov_v_deg == wide.fov_v_deg);
    CHECK(nvg.aspect == wide.aspect);
    CHECK(nvg.target_h == 512 && nvg.target_w == 512); // retail's columns, not 512 x 4/3
    const ViewProjection native = view_projection(80.0f, -1, 1920, 1080);
    const ViewProjection native_nvg = nvg_view_projection(native, frame_arm, 0.5625f, 1);
    CHECK(native_nvg.target_h == 512 && native_nvg.target_w == 512);
    CHECK(native_nvg.aspect == native.aspect && native_nvg.fov_v_deg == native.fov_v_deg);
    const ViewProjection lens = nvg_view_projection(wide, lens_arm, 0.75f, 4);
    CHECK(lens.fov_h_deg == 15.0f && lens.fov_v_deg == 15.0f && lens.aspect == 1.0f);
    CHECK(lens.target_w == 512 && lens.target_h == 512);
    // The Sighted arm keeps the frame's shape at 80 / zoom
    // [orig: NVG_RenderSightedScene @0x5d2aa9..0x5d2ada].
    const ViewProjection sighted = nvg_view_projection(wide, sighted_arm, 0.75f, 4);
    CHECK(sighted.fov_h_deg == 20.0f && sighted.aspect == wide.aspect);
    CHECK(std::fabs(sighted.fov_v_deg - fov_vertical_from_horizontal_deg(20.0f, wide.aspect)) < 1e-5f);
    CHECK(sighted.target_w == 512 && sighted.target_h == 512);
}

// [orig: Game_RunVideoTestDialog @0x53ed3e..0x53ed6b] The first launch's video
// test seeds the cfg's display_16x9 word from the primary desktop: 1 (the
// widescreen row) when width / height exceeds the single-precision 1.34, else 0,
// as the strict x87 compare -- the same word a saved profile then reads back.
void test_fresh_profile_aspect_seed() {
    using opennova::renderer::fresh_profile_aspect_mode;
    CHECK(fresh_profile_aspect_mode(1024, 768) == 0);  // 4:3
    CHECK(fresh_profile_aspect_mode(1280, 1024) == 0); // 5:4
    CHECK(fresh_profile_aspect_mode(1920, 1080) == 1); // 16:9
    CHECK(fresh_profile_aspect_mode(1920, 1200) == 1); // 16:10
    CHECK(fresh_profile_aspect_mode(1366, 1024) == 0); // 1.334: under the line
    CHECK(fresh_profile_aspect_mode(1340, 1000) == 0); // 1.34 exactly: not past it
    CHECK(fresh_profile_aspect_mode(1341, 1000) == 1); // the first ratio past it
    // The literal is flt_7D15AC, single precision (1.34000003...): a quotient
    // between the double 1.34 and it stays under the line.
    CHECK(fresh_profile_aspect_mode(134000001, 100000000) == 0);
    // The jnp form drops an unordered compare to 0: a sizeless surface.
    CHECK(fresh_profile_aspect_mode(0, 0) == 0);
}

void test_view_bias_blend() {
    PlayerViewState v;
    const float eye[3] = {0, 0, 0};
    const float pos[3] = {-19.46f, 21.19f, -161.31f};   // JOX WPN_AK47AUTO pos
    const float tpos[3] = {-62.33f, 29.19f, -152.56f};  // ... and tpos
    // The bound poses are the def's *256 Q16 copies of those file values
    // (weapon_install_data_from_def).
    v.weapon_pose_bound = true;
    for (int i = 0; i < 3; ++i) {
        v.weapon_hip_pose.position_q16[i] = pos[i] * 256.0f;
        v.weapon_ads_pose.position_q16[i] = tpos[i] * 256.0f;
    }
    float out[3];
    player_view_bias_units(v, pos, out);
    CHECK(out[0] == pos[0] && out[1] == pos[1] && out[2] == pos[2]);
    CHECK(player_view_set_engaged(v, true, false));
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    // Every lane has snapped to tpos by its fifteenth step (a lane whose
    // fourteenth remainder rounds under its velocity snaps one call early);
    // the published bias is the truncating ftol of (tpos - pos) * 256, so the
    // position lands within one 1/256 file unit of tpos, never on a float blend.
    player_view_bias_units(v, pos, out);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(out[i] - tpos[i]) < 1.0f / 256.0f);
    player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v) && player_view_scope_settled(v));
    player_view_bias_units(v, pos, out);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(out[i] - tpos[i]) < 1.0f / 256.0f);
}

void test_input_dispatch_gates() {
    WeaponFsmDef def;
    def.clip_capacity = 30;
    def.flags = 2; // sighted
    WeaponSlotState slot;
    slot.clip = 30;
    slot.reserve = 300;
    // Reload: refused on a full magazine or a ZERO reserve; a clipless def
    // (clipsize -1) and a negative pool still queue the row.
    // [orig: input case 0xD3 @0x4e12a7..0x4e12e0 — clip == clipsize skip,
    //  pool == 0 skip, no capacity gate]
    CHECK(!weapon_fsm_reload_allowed(def, slot));
    slot.clip = 12;
    CHECK(weapon_fsm_reload_allowed(def, slot));
    slot.reserve = 0;
    CHECK(!weapon_fsm_reload_allowed(def, slot));
    slot.reserve = -1; // the shipped -1 startrounds pool passes the zero test
    CHECK(weapon_fsm_reload_allowed(def, slot));
    slot.reserve = 300;
    def.clip_capacity = -1; // clipless: the row still queues (no refill later)
    CHECK(weapon_fsm_reload_allowed(def, slot));
    def.clip_capacity = 30;

    // Scope toggle: refused during RELOAD/SWITCHFROM and for unscoped defs.
    // [orig: input case 6 @ 0x4e0420; Player_ToggleWeaponScope @ 0x4df0c0]
    slot.current = weapon_action::kIdle;
    CHECK(weapon_fsm_scope_toggle_allowed(def, slot));
    slot.current = weapon_action::kReload;
    CHECK(!weapon_fsm_scope_toggle_allowed(def, slot));
    slot.current = weapon_action::kSwitchFrom;
    CHECK(!weapon_fsm_scope_toggle_allowed(def, slot));
    slot.current = weapon_action::kIdle;
    def.flags = 0;
    CHECK(!weapon_fsm_scope_toggle_allowed(def, slot));
    def.flags = 1; // scoped counts too (Flags & 3)
    CHECK(weapon_fsm_scope_toggle_allowed(def, slot));
}

} // namespace

void test_toggle_latch_refusal_and_inset() {
    // The witnessed toggle protocol [orig: Player_ToggleWeaponScope — the
    // !activeFlag refusal @ 0x4df177; Setup 15 @ 0x4df36e / 7 Inset @ 0x4df355 /
    // 1 hipfire-return @ 0x4df1c3; g_ScopeHipfire writes @ 0x4df212/@ 0x4df373].
    PlayerViewState v = bound_view();
    const float eye[3] = {0, 0, 0};
    CHECK(v.scope_hipfire); // [orig: g_ScopeHipfire init 1]
    CHECK(player_view_set_engaged(v, true, false));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == -21.0f); // 15 steps, hip -> tpos
    CHECK(!v.scope_hipfire);
    player_view_tick(v, eye);
    CHECK(player_view_scope_ease_active(v));
    CHECK(!player_view_set_engaged(v, false, false)); // refused mid-ease
    CHECK(v.scope_engaged);
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye); // the 16th call reports done
    CHECK(!player_view_scope_ease_active(v));
    CHECK(player_view_set_engaged(v, false, false)); // full disengage ease (not hipfire)
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 21.0f); // 15 steps, tpos -> hip
    CHECK(v.weapon_pose_interp.current.position_q16[0] == 315.0f);
    CHECK(v.scope_hipfire);
    for (int i = 0; i <= kScopeEaseSteps; ++i) player_view_tick(v, eye);
    CHECK(player_view_scope_fraction(v) == 0.0f && !player_view_scope_ease_active(v));

    // Inset weapons (flags2 0x200 — the REVX PointAim MGs / emplaced guns) latch
    // the 7-step ease both ways.
    PlayerViewState vi = bound_view();
    CHECK(player_view_set_engaged(vi, true, true));
    CHECK(vi.weapon_pose_interp.velocity.position_q16[0] == -45.0f); // 315 / 7
    for (int i = 0; i <= kScopeEaseStepsInset; ++i) player_view_tick(vi, eye);
    CHECK(player_view_scope_fraction(vi) == 1.0f && !player_view_scope_ease_active(vi));
    CHECK(player_view_set_engaged(vi, false, true));
    CHECK(vi.weapon_pose_interp.velocity.position_q16[0] == 45.0f);
}

void test_unscope_on_move_and_up_refusal() {
    // The movement-held latch legs [orig: Player_PackInputStateToEntity @ 0x4df450]:
    // g_MovementKeyHeld blocks scope-UP on Scoped weapons (@ 0x4df29c) and, while
    // PROMOTED at scope on a Scoped (flags 1) weapon, forces the toggle
    // (@ 0x4df4c9..0x4df4ec).
    const int32_t kScoped = 1;         // weapon.def flags: Scoped
    const int32_t kSighted = 2;        // Sighted (no auto-unscope leg of its own)
    PlayerViewState v = bound_view();
    const float eye[3] = {0, 0, 0};

    // Movement alone never fires the leg from the hip.
    CHECK(!player_view_move_input(v, true, kScoped));
    CHECK(v.move_held);
    // The scope-UP refusal while moving, Scoped only [orig: @ 0x4df29c].
    CHECK(player_view_scope_up_blocked(v, kScoped));
    CHECK(!player_view_scope_up_blocked(v, kSighted));
    CHECK(!player_view_move_input(v, false, kScoped));
    CHECK(!v.move_held);
    CHECK(!player_view_scope_up_blocked(v, kScoped));

    // Raise and settle the scope; a Sighted def's raise ignores movement (no
    // entitySlotPtr leg without Flags & 1 @0x4df52c).
    CHECK(player_view_set_engaged(v, true, false));
    player_view_tick(v, eye);
    CHECK(player_view_scope_ease_active(v));
    CHECK(!player_view_move_input(v, true, kSighted));
    CHECK(player_view_scope_ease_active(v) && !v.scope_hipfire);
    CHECK(!player_view_move_input(v, false, kSighted));
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye); // 16 calls in all
    CHECK(!player_view_scope_ease_active(v));
    CHECK(player_view_scope_settled(v));

    // Promoted + movement: the auto-unscope fires, Scoped weapons only
    // [orig: g_WeaponScopeActive && Def->Flags & 1 @ 0x4df4c9..0x4df4ea].
    CHECK(!player_view_move_input(v, true, kSighted));
    CHECK(player_view_move_input(v, true, kScoped));
    // The caller then runs the standard disengage (the full 15-step return —
    // hipfire was cleared at the raise).
    CHECK(player_view_set_engaged(v, false, false));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 21.0f); // 315 / 15
    CHECK(v.scope_hipfire);
    CHECK(!player_view_scope_settled(v));
}

bool near_eq(float a, float b, float eps = 0.0005f);

// The three interp legs of the movement pack on a Scoped def
// [orig: Player_PackInputStateToEntity @0x4df500..0x4df63b].
void test_move_reversal_and_auto_re_raise() {
    const int32_t kScoped = 1;
    const float eye[3] = {0, 0, 0};

    // (a) a movement key during the raise: the interp re-targets the hip FROM
    // ITS OWN POSE over a fresh 15 steps, the engaged target stays latched,
    // and the promoter then promotes "scoped" at the hip
    // [orig: @0x4df548..0x4df56c; CNetPlayerInterp_Setup @0x4de006..0x4de01a].
    // Five ticks of the raise put the position lane at 105 (5 * 21), so the
    // reversal's fresh 15-step velocity is an exact 7 and the lanes snap on
    // the fifteenth step of the return, done on the sixteenth call.
    PlayerViewState v = bound_view();
    CHECK(player_view_set_engaged(v, true, false));
    for (int i = 0; i < 5; ++i) player_view_tick(v, eye);
    CHECK(v.weapon_pose_interp.current.position_q16[0] == 105.0f);
    CHECK(!player_view_move_input(v, true, kScoped)); // no toggle: not promoted
    CHECK(v.scope_engaged && !player_view_scope_settled(v));
    CHECK(v.scope_hipfire && player_view_scope_ease_active(v));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 7.0f); // from its own pose
    CHECK(v.weapon_pose_interp.velocity.rotation_bam[0] == 0x00700000u);
    CHECK(near_eq(player_view_scope_fraction(v), 105.0f / 315.0f));
    // A further move frame changes nothing (hipfire already set).
    CHECK(!player_view_move_input(v, true, kScoped));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 7.0f);
    for (int i = 1; i <= kScopeEaseSteps; ++i) {
        player_view_tick(v, eye);
        CHECK(v.weapon_pose_interp.current.position_q16[0] ==
              7.0f * static_cast<float>(kScopeEaseSteps - i));
        CHECK(!player_view_scope_settled(v));
    }
    player_view_tick(v, eye);
    CHECK(player_view_scope_fraction(v) == 0.0f);
    CHECK(!player_view_scope_ease_active(v));
    CHECK(v.scope_engaged && player_view_scope_settled(v)); // promoted at the hip
    // Still moving: the step-1 toggle now fires; the caller's disengage takes the
    // 1-step hipfire-return leg (sourced at tpos, as retail's idle Setup does),
    // whose single step snaps every lane and whose next call reports done.
    CHECK(player_view_move_input(v, true, kScoped));
    CHECK(player_view_set_engaged(v, false, false));
    CHECK(v.weapon_pose_interp.velocity.position_q16[0] == 315.0f); // 315 / 1
    CHECK(v.weapon_pose_interp.current.position_q16[0] == 315.0f);   // idle Setup: tpos
    CHECK(!v.scope_engaged && !player_view_scope_settled(v));
    player_view_tick(v, eye);
    CHECK(v.weapon_pose_interp.current.position_q16[0] == 0.0f && player_view_scope_ease_active(v));
    player_view_tick(v, eye);
    CHECK(player_view_scope_fraction(v) == 0.0f && !player_view_scope_ease_active(v));

    // (c) the release re-raise: promoted at the hip with hipfire set, the key
    // up drops the promoted byte, keeps the target and starts the 15-step
    // hip -> tpos ease with hipfire cleared [orig: @0x4df607..0x4df63b].
    PlayerViewState r = bound_view();
    CHECK(player_view_set_engaged(r, true, false));
    for (int i = 0; i < 5; ++i) player_view_tick(r, eye);
    CHECK(!player_view_move_input(r, true, kScoped)); // (a)
    for (int i = 0; i <= kScopeEaseSteps; ++i) player_view_tick(r, eye);
    CHECK(player_view_scope_settled(r) && r.scope_hipfire && !player_view_scope_ease_active(r));
    CHECK(!player_view_move_input(r, false, kScoped)); // (c)
    CHECK(r.scope_engaged && !player_view_scope_settled(r) && !r.scope_hipfire);
    CHECK(player_view_scope_ease_active(r));
    CHECK(r.weapon_pose_interp.velocity.position_q16[0] == -21.0f); // hip -> tpos, 15 steps
    CHECK(player_view_scope_fraction(r) == 0.0f);
    for (int i = 0; i <= kScopeEaseSteps; ++i) player_view_tick(r, eye);
    CHECK(player_view_scope_fraction(r) == 1.0f);
    CHECK(player_view_scope_settled(r));
    // Every toggle is refused while (c) runs, like any ease [orig: @0x4df177].
    PlayerViewState mid = r;
    CHECK(!player_view_move_input(mid, false, kScoped)); // idle promoted at tpos: nothing
    CHECK(!player_view_scope_request_pending(mid, true)); // the reached target: a no-op
    CHECK(player_view_set_engaged(mid, true, false));

    // The pinned defs (ForceScoped 0x20000000 / Emplaced 0x80): (a) still
    // reverses (it precedes the flags re-read @0x4df57c) but LABEL_33's own
    // term refuses the re-raise, so the sight parks promoted at the hip
    // [orig: @0x4df58e -> @0x4df607 with the 0x20000080 term].
    const int32_t kEmplacedScoped = kScoped | 0x80;
    PlayerViewState p = bound_view();
    CHECK(player_view_set_engaged(p, true, false));
    for (int i = 0; i < 5; ++i) player_view_tick(p, eye);
    CHECK(!player_view_move_input(p, true, kEmplacedScoped));
    CHECK(p.scope_hipfire && player_view_scope_ease_active(p));
    for (int i = 0; i <= kScopeEaseSteps; ++i) player_view_tick(p, eye);
    CHECK(player_view_scope_settled(p) && player_view_scope_fraction(p) == 0.0f);
    CHECK(!player_view_move_input(p, false, kEmplacedScoped));
    CHECK(p.scope_hipfire && !player_view_scope_ease_active(p) && player_view_scope_settled(p));

    // The reversed-on-the-first-frame quirk: a move frame before the first
    // tick re-targets a zero delta on an active interp whose counter is spent
    // (a nonzero-delta Setup never armed it), which deactivates the interp
    // outright without a promotion [orig: CNetPlayerInterp_Setup
    // @0x4de0fe..0x4de11b; the promoter only runs behind a step @0x4de4c7].
    // The target stays latched, unpromoted, and the next toggle press
    // RE-RAISES it (the promoted-byte branch @0x4df17f).
    PlayerViewState q = bound_view();
    CHECK(player_view_set_engaged(q, true, false));
    CHECK(q.weapon_pose_interp.remaining == 0u);
    CHECK(!player_view_move_input(q, true, kScoped));
    CHECK(q.scope_engaged && !player_view_scope_settled(q) && q.scope_hipfire);
    CHECK(!player_view_scope_ease_active(q));
    player_view_tick(q, eye);
    CHECK(!player_view_scope_settled(q)); // no promotion without a step
    CHECK(!player_view_move_input(q, false, kScoped)); // (c) needs the promoted byte
    CHECK(!player_view_scope_ease_active(q));
    CHECK(player_view_scope_request_pending(q, true));
    CHECK(player_view_set_engaged(q, true, false));
    CHECK(player_view_scope_ease_active(q) && !q.scope_hipfire);
}

bool near_eq(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

// The march landing table [orig: @ 0x438213..0x43832e]: the reset distance 1.0
// lands 0.75u back; a sub-half-metre distance stays at the pivot; the gate
// passes >= 8.0 through untouched.
void test_tp_effective_distance_march() {
    CHECK(player_view_tp_effective_distance(1.0f) == 0.75f);
    CHECK(player_view_tp_effective_distance(0.3f) == 0.0f);
    CHECK(player_view_tp_effective_distance(0.5f) == 0.25f);
    CHECK(player_view_tp_effective_distance(8.0f) == 8.0f);
    CHECK(player_view_tp_effective_distance(512.0f) == 512.0f);
}

// The FP leg: the doubled recoil, the torso+lean/4 roll, the motor eye,
// and the 0.1875 pull-back along the (recoiled) forward.
void test_compose_camera_first_person() {
    PlayerViewState v;
    const float position[3] = {10.0f, 20.0f, 5.0f};
    const float anchor[3] = {10.0f, 20.0f, 6.6f};
    PlayerCameraPose pose;
    // recoil 1 deg (BAM), torso roll 2 deg, lean 4 deg -> roll 2 + 1 = 3 deg.
    const int32_t deg_bam = 11930465; // 2^32 / 360, rounded
    player_view_compose_camera(v, position, anchor, true, nullptr, false,
            90.0f, 0.0f, deg_bam, 2 * deg_bam, 4 * deg_bam, false, 0.0f, pose);
    CHECK(!pose.third_person);
    CHECK(near_eq(pose.yaw_deg, 90.0f));
    CHECK(near_eq(pose.pitch_deg, 2.0f, 0.01f)); // twice the 1-deg accumulator
    CHECK(near_eq(pose.roll_deg, 3.0f, 0.01f));  // torso 2 + lean 4 / 4
    // yaw 90: mission forward ~= (+cos(pitch)*1, ~0, sin(pitch)); the eye pulls
    // 0.1875 BACK along it from the anchor.
    CHECK(near_eq(pose.eye[0], anchor[0] - 0.1875f * std::cos(2.0f * 3.14159265f / 180.0f), 0.001f));
    CHECK(near_eq(pose.eye[2], anchor[2] - 0.1875f * std::sin(2.0f * 3.14159265f / 180.0f), 0.001f));

    // The exact motor eye passes through without the capsule fallback floor.
    const float low_anchor[3] = {10.0f, 20.0f, 5.0f};
    player_view_compose_camera(v, position, low_anchor, true, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 5.0f, 0.001f));

    // No anchor: the non-person +1.0 bump over position under the entity's
    // own rotation triple, then straight to the tail — no pull-back, no
    // recoil doubling, no torso/lean roll [orig: @ 0x437e8f, the jump to the
    // shake @0x437E99; the triple @0x437D86..0x437D9B].
    player_view_compose_camera(v, position, position, false, nullptr, false,
            0.0f, 0.0f, deg_bam, 2 * deg_bam, 4 * deg_bam, false, 3.5f, pose);
    CHECK(near_eq(pose.eye[0], 10.0f, 0.001f));
    CHECK(near_eq(pose.eye[1], 20.0f, 0.001f));
    CHECK(near_eq(pose.eye[2], 6.0f, 0.001f));
    CHECK(near_eq(pose.pitch_deg, 0.0f, 0.001f));
    CHECK(near_eq(pose.roll_deg, 3.5f, 0.001f));
}

// The D-INF-18 terrain floor [orig: Entity_UpdateInfantryPlayerBody
// @ 0x4b6c08..0x4b6ca4]: five bilinear samples (eye column + 0.25u along each
// ground axis), each + 0.0625, max-folded into a floor on the head-bone eye Z —
// skipped indoors, and never applied on the non-person bump path.
void test_compose_camera_terrain_floor() {
    // A flat 512x512 identity atlas at 8.0 world units (raw16 = units * 256) —
    // the tests/world/ground_height_test.cpp wiring: all sector cells id 1, so
    // world (x, z) indexes the atlas directly and one texel is one world unit.
    constexpr int kDim = 512;
    std::vector<uint16_t> heightmap(kDim * kDim, 8 * 256);
    std::vector<int> sector_grid(256, 1);
    opennova::terrain::TerrainHeightField field;
    field.heightmap = heightmap.data();
    field.dim = kDim;
    field.layout.sector_grid = sector_grid.data();
    field.layout.origin_x = 0;
    field.layout.origin_y = 0;

    PlayerViewState v;
    // Mission y = -4 samples atlas z = +4 (the engine-y -> atlas-z negation).
    const float position[3] = {100.0f, -4.0f, 5.0f};
    float anchor[3] = {100.0f, -4.0f, 6.6f}; // below terrain 8.0
    PlayerCameraPose pose;
    player_view_floor_eye_to_terrain(&field, false, anchor);
    player_view_compose_camera(v, position, anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    // yaw 0 pitch 0: fwd = (0, 1, 0); the pull-back rides Y, the Z is the
    // floored eye = 8.0 + 0.0625.
    CHECK(near_eq(pose.eye[2], 8.0625f, 0.001f));

    anchor[2] = 6.6f;
    player_view_floor_eye_to_terrain(&field, true, anchor);
    // INDOORS skips the floor [orig: the Flags & 0x800000 gate @ 0x4b6c08].
    player_view_compose_camera(v, position, anchor, true, &field, true,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 6.6f, 0.001f));

    // An eye already above the floored height passes through untouched.
    const float high_anchor[3] = {100.0f, -4.0f, 9.5f};
    player_view_compose_camera(v, position, high_anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 9.5f, 0.001f));

    // The non-person bump path has no terrain leg [orig: the fallback branch
    // @ 0x4b6b92 stores its offset with only the 0x2000 floor].
    player_view_compose_camera(v, position, position, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 6.0f, 0.001f));

    // The neighbor probes: a ridge one column to +X raises the floor through
    // the +0.25 probe's bilinear tap — the max fold over the five samples.
    // Column x=101 at 24.0u: probe x=100.25 -> 8 + (24-8)*0.25 = 12.0.
    for (int z = 0; z < kDim; ++z) heightmap[z * kDim + 101] = 24 * 256;
    player_view_floor_eye_to_terrain(&field, false, anchor);
    player_view_compose_camera(v, position, anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 12.0625f, 0.001f));
}

// The chase's final look-at: mission yaw (clockwise from +Y, [0, 360)) and
// pitch from the eye to the target [orig: fpatan @0x4388BD / @0x43891B].
void chase_look_at(const float eye[3], const float target[3], float &yaw_deg, float &pitch_deg) {
    const double tx = static_cast<double>(target[0]) - eye[0];
    const double ty = static_cast<double>(target[1]) - eye[1];
    const double tz = static_cast<double>(target[2]) - eye[2];
    const double to_deg = 180.0 / 3.14159265358979323846;
    double yaw = std::atan2(tx, ty) * to_deg;
    if (yaw < 0.0) yaw += 360.0;
    yaw_deg = static_cast<float>(yaw);
    pitch_deg = static_cast<float>(std::atan2(tz, std::sqrt(tx * tx + ty * ty)) * to_deg);
}

// The TP leg [orig: Camera_ComputeThirdPersonView @0x437D10 — the anchor-
// translated view matrix @0x438179, the pivot @0x43817E..0x4381D9, the eye
// @0x4383E0..0x4383FB, the march gates @0x4381CB / @0x4381E3 and its no-force
// exit @0x438334..0x438341, the look-at @0x4387DF..0x43892D]: the chased
// anchor wins over the live eye; the eye backs off the ANCHOR (the
// march-landed 0.75 only with proximity candidates at hand, else the full
// 1.0); the pivot nudged 0.125 along forward+left+up is the LOOK-AT TARGET,
// never the eye, so the view turns left and up off the seed; roll stays 0
// and the recoil doubling does NOT apply.
void test_compose_camera_third_person() {
    PlayerViewState v;
    v.third_person = true;
    v.tp_anchor_valid = true;
    v.tp_anchor[0] = 1.0f;
    v.tp_anchor[1] = 2.0f;
    v.tp_anchor[2] = 3.0f;
    const float position[3] = {0.0f, 0.0f, 0.0f};
    const float anchor[3] = {9.0f, 9.0f, 9.0f}; // must be ignored
    const int32_t deg_bam = 11930465;
    // yaw 0 pitch 0: mission fwd = (0, 1, 0), left = (-1, 0, 0), up = (0, 0, 1).
    const float pivot[3] = {1.0f - 0.125f, 2.0f + 0.125f, 3.0f + 0.125f};
    for (const bool candidates : {false, true}) {
        PlayerCameraPose pose;
        player_view_compose_camera(v, position, anchor, true, nullptr, false,
                0.0f, 0.0f,
                deg_bam /* recoil must not leak into TP */, deg_bam, deg_bam, candidates, 0.0f,
                pose);
        CHECK(pose.third_person);
        CHECK(pose.roll_deg == 0.0f);
        const float back = candidates ? 0.75f : kTpDistance;
        CHECK(near_eq(pose.eye[0], 1.0f));
        CHECK(near_eq(pose.eye[1], 2.0f - back));
        CHECK(near_eq(pose.eye[2], 3.0f));
        float yaw = 0.0f, pitch = 0.0f;
        chase_look_at(pose.eye, pivot, yaw, pitch);
        CHECK(near_eq(pose.yaw_deg, yaw, 1e-4f));
        CHECK(near_eq(pose.pitch_deg, pitch, 1e-4f));
        CHECK(pose.yaw_deg > 270.0f && pose.pitch_deg > kTpOrbitPitchDeg + 1.0f);
    }
}

// THE MOUNTED LEG [orig: the mount-state 2/5 arm of mode 1 — the lift
// @0x437B1F..0x437B4B, the ease @0x437C56..0x437C79, the distance
// @0x438121..0x438136, the quarter yaw @0x438138..0x43814A, the pitch
// @0x438150, the clearances @0x438409..0x438456, the slope march
// @0x43846E..0x438619, the watercraft drop @0x43861D..0x43864C].
PlayerViewState mounted_state(float bound_radius, float carrier_z = 10.0f) {
    PlayerViewState v;
    v.third_person = true;
    v.mount.control_seat = true;
    v.mount.carrier_pos_q16[0] = 0;
    v.mount.carrier_pos_q16[1] = 0;
    v.mount.carrier_pos_q16[2] = static_cast<int32_t>(carrier_z * 65536.0f);
    v.mount.carrier_yaw_bam = bam_heading_from_mission_yaw_deg(0.0); // mission yaw 0
    v.mount.bound_radius = bound_radius;
    v.mount.water_z = -1000.0f;
    // A level carrier at mission yaw 0 (= +Y): its matrix x (6, 0, 0).
    v.mount.lookahead_target_q16[0] = 0;
    v.mount.lookahead_target_q16[1] = 6 * 0x10000;
    v.mount.lookahead_target_q16[2] = 0;
    // The anchor and the look-ahead as the composes would have settled them:
    // carrier + lift, and the carrier's forward x 6.
    v.tp_anchor_valid = true;
    v.tp_anchor[0] = 0.0f;
    v.tp_anchor[1] = 0.0f;
    v.tp_anchor[2] = carrier_z + mount_anchor_lift(bound_radius);
    for (int i = 0; i < 3; ++i)
        v.tp_anchor_q16[i] = static_cast<int32_t>(v.tp_anchor[i] * 65536.0f);
    v.lookahead_q16[0] = 0;
    v.lookahead_q16[1] = 6 * 0x10000;
    v.lookahead_q16[2] = 0;
    return v;
}

void test_compose_camera_mounted() {
    const float position[3] = {0.0f, 0.0f, 10.0f};
    const float no_anchor[3] = {0.0f, 0.0f, 0.0f};
    PlayerCameraPose pose;

    // r = 4: lift 1.5 (above the 1.0 floor), distance 1 + 1.5 * 4 = 7.
    PlayerViewState v = mounted_state(4.0f);
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(pose.third_person);
    CHECK(pose.roll_deg == 0.0f);
    // Looking straight ahead: the eye sits 7 u behind the anchor along the
    // -11.25-degree forward (yaw 0 -> mission fwd = (0, cos p, sin p)).
    const float p = -11.25f * 3.14159265f / 180.0f;
    CHECK(near_eq(pose.eye[0], 0.0f, 0.001f));
    CHECK(near_eq(pose.eye[1], -7.0f * std::cos(p), 0.001f));
    CHECK(near_eq(pose.eye[2], 11.5f - 7.0f * std::sin(p), 0.001f));
    // The final angles look at the PIVOT — the anchor nudged 0.125 along
    // forward (0, cos p, sin p) + left (-1, 0, 0) + up (0, -sin p, cos p) —
    // plus the look-ahead (6 u along the carrier's forward): the view turns
    // a little left of the carrier heading and looks DOWN at the point.
    // [orig: the pivot @0x43817E..0x4381D9 is the look-at target; the
    //  look-ahead rides it @0x43887B..0x4388A0]
    {
        const float target[3] = {-0.125f,
                0.125f * (std::cos(p) - std::sin(p)) + 6.0f,
                11.5f + 0.125f * (std::sin(p) + std::cos(p))};
        float yaw = 0.0f, pitch = 0.0f;
        chase_look_at(pose.eye, target, yaw, pitch);
        CHECK(near_eq(pose.yaw_deg, yaw, 1e-3f));
        CHECK(near_eq(pose.pitch_deg, pitch, 1e-3f));
        CHECK(pose.yaw_deg > 359.0f && pose.yaw_deg < 359.9f);
        CHECK(pose.pitch_deg < 0.0f);
    }
    // The collision march lands a 7 u chase 6.75 u back with proximity
    // candidates at hand; a tank-sized radius (8.5 u, past the 8.0 gate)
    // never marches [orig: @0x4381CB / @0x4381E3, landing @0x438334..0x438341].
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, true, 0.0f, pose);
    CHECK(near_eq(pose.eye[1], -6.75f * std::cos(p), 0.001f));
    PlayerViewState tank = mounted_state(5.0f);
    player_view_compose_camera(tank, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, true, 0.0f, pose);
    CHECK(near_eq(pose.eye[1], -8.5f * std::cos(p), 0.001f));

    // The lift floor: r = 1 lifts 1.0, not 0.375; distance 2.5.
    PlayerViewState small = mounted_state(1.0f);
    player_view_compose_camera(small, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[1], -2.5f * std::cos(p), 0.001f));
    CHECK(near_eq(pose.eye[2], 11.0f - 2.5f * std::sin(p), 0.001f));

    // The QUARTER look yaw: a 40-degree look offset orbits the EYE 10 degrees
    // around the anchor (the eye sits 7 u back along the damped yaw); the
    // final yaw is the look-at from there to the carrier-forward point, which
    // lands between the carrier heading and the damped orbit.
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            40.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    const float ten = 10.0f * 3.14159265f / 180.0f;
    CHECK(near_eq(pose.eye[0], -7.0f * std::sin(ten) * std::cos(p), 0.002f));
    CHECK(near_eq(pose.eye[1], -7.0f * std::cos(ten) * std::cos(p), 0.002f));
    CHECK(pose.yaw_deg > 0.5f && pose.yaw_deg < 10.0f);
    // ... and it is symmetric.
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            -40.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[0], 7.0f * std::sin(ten) * std::cos(p), 0.002f));
    CHECK(pose.yaw_deg > 350.0f && pose.yaw_deg < 359.5f);

    // The WATERCRAFT drop: half the radius off the eye.
    PlayerViewState boat = mounted_state(4.0f);
    boat.mount.watercraft = true;
    PlayerCameraPose boat_pose;
    player_view_compose_camera(boat, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, boat_pose);
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(boat_pose.eye[2], pose.eye[2] - 2.0f, 0.001f));

    // The water floor's polarity: with the water at 100 an entity ABOVE it
    // (position z 200) gets the eye raised to 100.25, a submerged one keeps
    // its underwater eye.
    PlayerViewState wet = mounted_state(4.0f);
    wet.mount.water_z = 100.0f;
    const float above[3] = {0.0f, 0.0f, 200.0f};
    player_view_compose_camera(wet, above, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(near_eq(pose.eye[2], 100.25f, 0.001f));
    player_view_compose_camera(wet, position /* z 10, submerged */, no_anchor, false,
            nullptr, false, 0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
    CHECK(pose.eye[2] < 100.0f);
}

// The mounted terrain legs: the +0.25 floor and the slope raise over a ramp.
void test_compose_camera_mounted_terrain() {
    constexpr int kDim = 512;
    std::vector<uint16_t> heightmap(kDim * kDim, 8 * 256); // flat at 8.0
    std::vector<int> sector_grid(256, 1);
    opennova::terrain::TerrainHeightField field;
    field.heightmap = heightmap.data();
    field.dim = kDim;
    field.layout.sector_grid = sector_grid.data();
    field.layout.origin_x = 0;
    field.layout.origin_y = 0;

    // The carrier at (100, -100, 10): the anchor is 11.5, the eye ~12.9.
    PlayerViewState v = mounted_state(4.0f);
    v.mount.carrier_pos_q16[0] = 100 * 0x10000;
    v.mount.carrier_pos_q16[1] = -100 * 0x10000;
    v.tp_anchor[0] = 100.0f;
    v.tp_anchor[1] = -100.0f;
    const float position[3] = {100.0f, -100.0f, 10.0f};
    const float no_anchor[3] = {0.0f, 0.0f, 0.0f};
    PlayerCameraPose flat;
    player_view_compose_camera(v, position, no_anchor, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, flat);
    // Flat ground far below: the terrain floor does not move the eye, but the
    // slope raise's 0.333-per-unit margin ALWAYS applies (the running max is
    // seeded at 0.0, so a downhill run never lowers it): the eye is floored at
    // anchor + 0.333 * the horizontal distance.
    const float p = -11.25f * 3.14159265f / 180.0f;
    const float horizontal = 7.0f * std::cos(p);
    const float raw_z = 11.5f - 7.0f * std::sin(p);
    const float margin_floor = 11.5f + horizontal * 0.333f;
    CHECK(margin_floor > raw_z);
    CHECK(near_eq(flat.eye[2], margin_floor, 0.002f));

    // Ground just above the eye: the terrain floor (+0.25) and the slope
    // raise both apply and the eye ends above the ground.
    for (auto &h : heightmap) h = 13 * 256;
    PlayerCameraPose raised;
    player_view_compose_camera(v, position, no_anchor, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, raised);
    CHECK(raised.eye[2] >= 13.25f);

    // A ramp rising steeply BEHIND the carrier (the eye side, mission -y =
    // atlas +z) raises the eye above the flat result.
    for (int z = 0; z < kDim; ++z)
        for (int x = 0; x < kDim; ++x)
            heightmap[z * kDim + x] = static_cast<uint16_t>(
                    (8 * 256) + (z > 100 ? (z - 100) * 512 : 0));
    PlayerCameraPose ramp;
    player_view_compose_camera(v, position, no_anchor, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, false, 0.0f, ramp);
    CHECK(ramp.eye[2] > flat.eye[2] + 1.0f);
}

// The mounted anchor ease in 16.16: a sixteenth per tick on x/y and a
// thirty-second on z toward carrier + lift, mirrored into the float anchor.
void test_tick_mounted_anchor_ease() {
    PlayerViewState v;
    // The ease math under a mode that stays third person across the seat
    // change (the debug override); the arbiter's own transitions are
    // test_mode_arbiter's.
    v.debug_third_person_on_foot = true;
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    player_view_tick(v, eye); // seeds the anchor at the eye
    CHECK(v.tp_anchor_valid);
    v.mount.control_seat = true;
    v.mount.carrier_pos_q16[0] = 16 * 0x10000;
    v.mount.carrier_pos_q16[1] = 0;
    v.mount.carrier_pos_q16[2] = 32 * 0x10000;
    v.mount.bound_radius = 4.0f; // lift 1.5 -> target z 33.5
    v.mount.lookahead_target_q16[1] = 6 * 0x10000;
    player_view_tick(v, eye);
    // x: a sixteenth of 16 = 1.0; z: a thirty-second of 33.5 = 1.046875.
    CHECK(v.tp_anchor_q16[0] == 0x10000);
    CHECK(near_eq(v.tp_anchor[0], 1.0f));
    CHECK(v.tp_anchor_q16[2] == (33 * 0x10000 + 0x8000 + 16) >> 5);
    CHECK(near_eq(v.tp_anchor[2], 1.046875f, 0.0001f));
    // The tick eases the anchor only: the look-ahead belongs to the compose
    // (test_mounted_lookahead_eases_on_every_compose).
    CHECK(v.lookahead_q16[0] == 0 && v.lookahead_q16[1] == 0 && v.lookahead_q16[2] == 0);
    // Dismounting returns to the quarter-step float ease from where it was.
    v.mount.control_seat = false;
    player_view_tick(v, eye);
    CHECK(near_eq(v.tp_anchor[0], 0.75f, 0.0001f));
}

// THE LOOK-AHEAD EASE belongs to every compose of the mounted chase: each
// call steps it a thirty-second per axis toward the carrier matrix x (6, 0,
// 0) — a pitched hull's target climbs with it — so the per-quantum and the
// per-frame composes both advance it, and a higher frame rate eases faster.
// [orig: g_camera_lookahead @0x43885A..0x4388AF inside
//  Camera_ComputeThirdPersonView @0x437D10, called per quantum @0x526781 and
//  per rendered frame @0x5CA34D]
void test_mounted_lookahead_eases_on_every_compose() {
    PlayerViewState v = mounted_state(5.0f);
    for (int i = 0; i < 3; ++i) v.lookahead_q16[i] = 0;
    // A hull pitched 10 degrees up: (6 cos 10, 0, 6 sin 10) turned to yaw 0.
    v.mount.lookahead_target_q16[0] = 0;
    v.mount.lookahead_target_q16[1] = 387242;
    v.mount.lookahead_target_q16[2] = 68280;
    const float position[3] = {0.0f, 0.0f, 10.0f};
    const float no_anchor[3] = {0.0f, 0.0f, 0.0f};
    int32_t expected[3] = {0, 0, 0};
    PlayerCameraPose pose;
    float last_pitch = 0.0f;
    for (int call = 0; call < 4; ++call) {
        player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
                0.0f, 0.0f, 0, 0, 0, false, 0.0f, pose);
        for (int i = 0; i < 3; ++i)
            expected[i] += (v.mount.lookahead_target_q16[i] - expected[i] + 16) >> 5;
        CHECK(v.lookahead_q16[0] == expected[0]);
        CHECK(v.lookahead_q16[1] == expected[1]);
        CHECK(v.lookahead_q16[2] == expected[2]);
        // The look-at follows the eased point up the hull's slope.
        if (call > 0) CHECK(pose.pitch_deg > last_pitch);
        last_pitch = pose.pitch_deg;
    }
    CHECK(v.lookahead_q16[2] > 0);
}

// The view-frame bias: (raw def pos + the published Q16 bias / 256) / 256;
// the NoCardSwitch reload suppression drops the published half (the hip
// offset).
void test_bias_view_units() {
    PlayerViewState v;
    const float pos[3] = {-19.46f, 21.19f, -161.31f};
    const float tpos[3] = {-62.33f, 29.19f, -152.56f};
    float out[3];
    player_view_bias_view_units(v, false, pos, out);
    CHECK(near_eq(out[0], -19.46f / 256.0f));
    CHECK(near_eq(out[2], -161.31f / 256.0f));
    // Fully sighted: the stepper published ftol((tpos - pos) * 256) per lane
    // [orig: Player_StepFpViewBiasInterp @0x4ddf53..0x4ddf85]; then suppressed.
    v.scope_engaged = v.scope_settled = true;
    for (int i = 0; i < 3; ++i)
        v.weapon_pose_interp.position_bias_q16[i] =
            static_cast<int32_t>((tpos[i] - pos[i]) * 256.0f);
    player_view_bias_view_units(v, false, pos, out);
    CHECK(near_eq(out[0], -62.33f / 256.0f));
    CHECK(near_eq(out[1], 29.19f / 256.0f));
    player_view_bias_view_units(v, true, pos, out);
    CHECK(near_eq(out[0], -19.46f / 256.0f));
    CHECK(near_eq(out[1], 21.19f / 256.0f));
}

// The FP motion lead: the damped movement-delta tracker whose >>7 clamped
// output rides the view-local offset. A movement ONSET swings the lead
// against the delta; steady movement decays it back toward zero; the clamps
// bound it at +/-1024 xy / +/-4096 z.
// [orig: tracker @ 0x437bac..0x437c0e; >>7 + clamps @ 0x4dd4f2..0x4dd54f]
void test_motion_lead_tracker() {
    PlayerViewMotionLead lead;
    int32_t out[3];
    const float still[3] = {0.0f, 0.0f, 0.0f};
    player_view_motion_lead_update(lead, still, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);

    // A 0.2 u/tick onset: the first frame swings the lead opposite the delta.
    const float onset[3] = {0.2f, 0.0f, 0.0f};
    player_view_motion_lead_update(lead, onset, out);
    CHECK(out[0] < 0);
    CHECK(out[1] == 0 && out[2] == 0);

    // Steady movement decays the lead toward zero (the tracker responds to
    // speed changes, not speed).
    int32_t first = out[0];
    for (int i = 0; i < 300; ++i)
        player_view_motion_lead_update(lead, onset, out);
    CHECK(std::abs(out[0]) < std::abs(first));
    CHECK(std::abs(out[0]) <= 1);

    // A violent delta clamps at the witnessed bounds.
    PlayerViewMotionLead hard;
    const float lurch[3] = {50.0f, 50.0f, 50.0f};
    for (int i = 0; i < 8; ++i)
        player_view_motion_lead_update(hard, lurch, out);
    const float stop[3] = {-50.0f, -50.0f, -50.0f};
    player_view_motion_lead_update(hard, stop, out);
    CHECK(std::abs(out[0]) <= 1024);
    CHECK(std::abs(out[1]) <= 1024);
    CHECK(std::abs(out[2]) <= 4096);
}

// The FP camera shake: counter lifecycle and the three-slice IIR.
// [orig: the mode-0 block in Camera_ComputeThirdPersonView @0x437D10; arms
//  @0x4305c1 / @0x4AF837 / @0x57EB7D; decay @0x4DE590]
void test_camera_shake() {
    using namespace opennova::world;

    // A zero counter produces NOTHING and leaves the filters untouched --
    // retail gates the whole block, so a settled camera does not quietly
    // keep filtering noise.
    {
        CameraShakeState st;
        st.roll = 12345; st.pitch = -999; st.yaw = 777;
        int32_t y = 1, pch = 1, r = 1;
        camera_shake_sample(st, 0xDEADBEEFu, y, pch, r);
        CHECK(y == 0 && pch == 0 && r == 0);
        CHECK(st.roll == 12345 && st.pitch == -999 && st.yaw == 777);
    }

    // The store cap is 255, and arming saturates rather than wrapping.
    {
        CameraShakeState st;
        camera_shake_arm(st, kShakeArmHealthDrop);
        CHECK(st.counter == 10);
        camera_shake_arm(st, kShakeArmNearMiss);
        CHECK(st.counter == 30);
        for (int i = 0; i < 40; ++i) camera_shake_arm(st, kShakeArmNearMiss);
        CHECK(st.counter == kShakeStoreMax);
    }

    // Decay is -2 with a FLOOR, not a clamp: an odd count reaches exactly 0
    // instead of idling at 1 forever.
    {
        CameraShakeState st;
        st.counter = 5;
        camera_shake_decay(st);  CHECK(st.counter == 3);
        camera_shake_decay(st);  CHECK(st.counter == 1);
        camera_shake_decay(st);  CHECK(st.counter == 0);
        camera_shake_decay(st);  CHECK(st.counter == 0);
    }

    // Sampling clamps at 64 while the STORE cap is 255, so arming past 64
    // buys DURATION, never amplitude: 64 and 255 must produce identical
    // deltas from the same PRNG word and filter state.
    {
        CameraShakeState a; a.counter = 64;
        CameraShakeState b; b.counter = kShakeStoreMax;
        int32_t ay, ap, ar, by, bp, br;
        camera_shake_sample(a, 0x12333333u, ay, ap, ar);
        camera_shake_sample(b, 0x12333333u, by, bp, br);
        CHECK(ay == by && ap == bp && ar == br);
    }

    // The three axes are slices of ONE word, so they differ from each other
    // but are reproducible from the same input -- a re-run must match, and
    // the axes must not be identical (which is what an accidental
    // single-slice-for-all-three would produce).
    {
        CameraShakeState a; a.counter = 32;
        CameraShakeState b; b.counter = 32;
        int32_t ay, ap, ar, by, bp, br;
        camera_shake_sample(a, 0x89ABCDEFu, ay, ap, ar);
        camera_shake_sample(b, 0x89ABCDEFu, by, bp, br);
        CHECK(ay == by && ap == bp && ar == br);
        CHECK(!(ay == ap && ap == ar));
    }

    // The filter and slices, pinned by HAND-COMPUTED values rather than a
    // convergence guess. With prng = 0x20 and a zeroed filter:
    //   roll  n = (int32)(0x20 << 1)  >> 5 = 64      >> 5 = 2
    //             s = (7*0 + 2)  >> 3 = 0   -> delta (64*0)     >> 6 = 0
    //   pitch n = (int32)(0x20 << 17) >> 5 = 4194304 >> 5 = 131072
    //             s = 131072     >> 3 = 16384 -> delta (64*16384) >> 6 = 16384
    //   yaw   n = (int32)(0x20 << 9)  >> 5 = 16384   >> 5 = 512
    //             s = 512        >> 3 = 64    -> delta (64*64)    >> 6 = 64
    // A different slice order, shift, or filter constant moves these.
    {
        CameraShakeState st; st.counter = 64;
        int32_t y, pch, r;
        camera_shake_sample(st, 0x20u, y, pch, r);
        CHECK(st.roll == 0 && st.pitch == 16384 && st.yaw == 64);
        CHECK(r == 0 && pch == 16384 && y == 64);
    }

    // Feeding the same word again advances the filter by the same rule from
    // its NEW state, so the second sample differs from the first -- the
    // accumulators are state, not a per-call recompute.
    {
        CameraShakeState st; st.counter = 64;
        int32_t y1, p1, r1, y2, p2, r2;
        camera_shake_sample(st, 0x20u, y1, p1, r1);
        camera_shake_sample(st, 0x20u, y2, p2, r2);
        // yaw: s = (7*64 + 512) >> 3 = 120
        CHECK(st.yaw == 120 && y2 == 120);
        CHECK(y2 != y1);
    }

    // A respawn zeroes the COUNTER only; the filters survive by design.
    {
        CameraShakeState st; st.counter = 40;
        int32_t y, pch, r;
        camera_shake_sample(st, 0xA5A5A5A5u, y, pch, r);
        const int32_t kept_yaw = st.yaw;
        st.counter = 0;   // the respawn write
        CHECK(st.yaw == kept_yaw);
    }
}

void test_camera_shake_chase() {
    using namespace opennova::world;

    // The same whole-block counter gate as the mode-0 leg: zero counter,
    // zero output [orig: @0x43892b].
    {
        CameraShakeState st;
        int32_t y = 1, pch = 1, r = 1;
        camera_shake_sample_chase(st, 0xDEADBEEFu, 12345u, y, pch, r);
        CHECK(y == 0 && pch == 0 && r == 0);
    }

    // Hand-computed pins (float32 constants, double trig, ftol truncation,
    // arithmetic >> 2 on the tick terms AFTER truncation). counter=10,
    // prng byte 0xFF, tick 0: amp = (40 * 319) >> 8 = 49;
    //   yaw   = trunc(sin(10*0.4)*49)                       = -37
    //   pitch = trunc(sin(10*2/7)*49) - (trunc(sin(0)*49)>>2)   = 13
    //   roll  = trunc(sin(10*2/11)*49) - (trunc(cos(0)*49)>>2)  = 47 - 12 = 35
    // A different constant, truncation rule, or shift moves these.
    {
        CameraShakeState st; st.counter = 10;
        int32_t y, pch, r;
        camera_shake_sample_chase(st, 0xFFu, 0u, y, pch, r);
        CHECK(y == -37 && pch == 13 && r == 35);
    }

    // A nonzero tick drives the two quartered terms; prng byte 0 still
    // yields amp = (min(4*64,255) * 64) >> 8 = 63.
    {
        CameraShakeState st; st.counter = 64;
        int32_t y, pch, r;
        camera_shake_sample_chase(st, 0x00u, 100u, y, pch, r);
        CHECK(y == 28 && pch == -18 && r == -47);
    }

    // The amp clamp is the 255 STORE cap (not the mode-0 sample clamp 64):
    // counter 255 with prng byte 0x80 gives amp (255 * 192) >> 8 = 191, and
    // the raw counter still drives the sin arguments.
    {
        CameraShakeState st; st.counter = 255;
        int32_t y, pch, r;
        camera_shake_sample_chase(st, 0x80u, 1000u, y, pch, r);
        CHECK(y == 190 && pch == -114 && r == 117);
    }

    // Stateless: a second identical call returns identical deltas and the
    // mode-0 IIR filters are never touched -- a mode flip resumes them where
    // they stopped.
    {
        CameraShakeState st; st.counter = 40;
        st.roll = 111; st.pitch = 222; st.yaw = 333;
        int32_t y1, p1, r1, y2, p2, r2;
        camera_shake_sample_chase(st, 0x5Au, 77u, y1, p1, r1);
        camera_shake_sample_chase(st, 0x5Au, 77u, y2, p2, r2);
        CHECK(y1 == y2 && p1 == p2 && r1 == r2);
        CHECK(st.roll == 111 && st.pitch == 222 && st.yaw == 333);
    }
}

// The view-action rows: each selects its chase preference and ORs its own
// BMS input-action bit (view1st 0x4000000, viewwithgun 0x10000000 -- the
// PlayerCockpitView trigger bit -- viewchase 0x8000000); a null word (off the
// authority) keeps the preference write; an action without a binding row
// changes nothing. [orig: Input_HandleActionBinding case 400 @0x49c073
// (@0x49c07a), case 401 @0x49c0d9 (@0x49c0e0), case 402 @0x49c0f6]
void test_view_actions_write_their_input_bits() {
    PlayerViewState v;
    v.mount.control_seat = true;
    uint32_t bits = 0;
    player_view_apply_view_action(v, &bits, kViewActionWithGun);
    CHECK(bits == 0x10000000u);
    CHECK(!v.third_person_selected && !v.third_person);
    player_view_apply_view_action(v, &bits, kViewActionChase);
    CHECK(bits == (0x10000000u | 0x8000000u));
    CHECK(v.third_person_selected && v.third_person);
    player_view_apply_view_action(v, &bits, kViewActionFirstPerson);
    CHECK(bits == (0x10000000u | 0x8000000u | 0x4000000u));
    CHECK(!v.third_person_selected);
    player_view_apply_view_action(v, nullptr, kViewActionChase);
    CHECK(v.third_person_selected);
    player_view_apply_view_action(v, &bits, 412);
    CHECK(bits == (0x10000000u | 0x8000000u | 0x4000000u));
    CHECK(v.third_person_selected);
}

int main() {
    test_binocular_sway_axes_and_quantization();
    test_view_actions_write_their_input_bits();
    test_authored_pose_interp_matches_original_six_lane_traces();
    test_authored_pose_interp_keeps_original_snap_and_completion_rules();
    test_authored_pose_def_promotes_parser_precision_and_wrapping_bam();
    test_scope_ease_settles_on_the_call_after_the_last_snap();
    test_zero_span_ease_promotes_when_its_counter_spends();
    test_equal_ticks_equal_state_regardless_of_frame_grouping();
    test_anchor_chase_quarter_step_and_seeding();
    test_mode_arbiter();
    test_fov_policy();
    test_binoculars_effective_state_and_fov();
    test_nvg_toggle_gain_and_first_person_visibility();
    test_fov_vertical_conversion();
    test_view_projection_retail_stretch();
    test_nvg_view_projection();
    test_fresh_profile_aspect_seed();
    test_view_bias_blend();
    test_input_dispatch_gates();
    test_toggle_latch_refusal_and_inset();
    test_unscope_on_move_and_up_refusal();
    test_move_reversal_and_auto_re_raise();
    test_scope_reset_preserves_original_switch_bookkeeping();
    test_tp_effective_distance_march();
    test_compose_camera_first_person();
    test_compose_camera_terrain_floor();
    test_camera_shake();
    test_camera_shake_chase();
    test_compose_camera_third_person();
    test_compose_camera_mounted();
    test_compose_camera_mounted_terrain();
    test_tick_mounted_anchor_ease();
    test_mounted_lookahead_eases_on_every_compose();
    test_bias_view_units();
    test_motion_lead_tracker();
    if (failures == 0) std::printf("player_view_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
