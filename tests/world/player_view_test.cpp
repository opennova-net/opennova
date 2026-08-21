// Player view-state tests [orig: CNetPlayerInterp_Setup @ 0x4df36e;
// ThirdPersonCamera_Update @ 0x437af0; Player_ToggleWeaponScope @ 0x4df0c0..401;
// Render_SetViewAndProjectionMatrices @ 0x58d900]: the fixed-tick scope ease and
// anchor chase (render-cadence invariance — the review's 30/60/144 fps case),
// the fov policy, and the input-dispatch gates in front of the FSM requests.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "terrain_query/height_field.h"
#include "world/angle.h"
#include "world/player_view.h"
#include "world/tp_camera_mount.h"
#include "world/weapon_fsm.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

void test_scope_ease_is_fifteen_ticks_exactly() {
    PlayerViewState v;
    const float eye[3] = {0, 0, 0};
    v.scope_engaged = true;
    for (int i = 1; i <= kScopeEaseSteps; ++i) {
        player_view_tick(v, eye);
        CHECK(v.scope_step == i);
    }
    CHECK(player_view_scope_fraction(v) == 1.0f);
    player_view_tick(v, eye); // saturates, never overshoots
    CHECK(v.scope_step == kScopeEaseSteps);
    v.scope_engaged = false;
    for (int i = kScopeEaseSteps - 1; i >= 0; --i) {
        player_view_tick(v, eye);
        CHECK(v.scope_step == i);
    }
    CHECK(player_view_scope_fraction(v) == 0.0f);
}

// The review's fps case: the SAME simulated time must produce the SAME state no
// matter how the render loop groups the ticks (one per frame at 60 fps, four
// then zero at 15/144 fps, ...). The state is a pure function of the tick
// count, so any grouping of N ticks lands identically.
void test_equal_ticks_equal_state_regardless_of_frame_grouping() {
    const float eye[3] = {100.0f, -40.0f, 12.0f};

    PlayerViewState per_frame;       // "60 fps": one tick per render frame
    per_frame.scope_engaged = true;
    per_frame.third_person = true;
    for (int i = 0; i < 24; ++i) player_view_tick(per_frame, eye);

    PlayerViewState bursty;          // "uneven fps": frames of 4/0/3/0/1... ticks
    bursty.scope_engaged = true;
    bursty.third_person = true;
    const int frames[] = {4, 0, 3, 0, 1, 7, 0, 0, 2, 5, 0, 2};
    int total = 0;
    for (int n : frames) {
        for (int i = 0; i < n; ++i) player_view_tick(bursty, eye);
        total += n;
    }
    CHECK(total == 24);
    CHECK(per_frame.scope_step == bursty.scope_step);
    CHECK(per_frame.tp_anchor_valid && bursty.tp_anchor_valid);
    for (int i = 0; i < 3; ++i) CHECK(per_frame.tp_anchor[i] == bursty.tp_anchor[i]);
}

void test_anchor_chase_quarter_step_and_seeding() {
    PlayerViewState v;
    float eye[3] = {8.0f, 0.0f, 4.0f};
    v.third_person = true;
    player_view_tick(v, eye); // first 3P tick seeds AT the eye
    CHECK(v.tp_anchor_valid);
    CHECK(v.tp_anchor[0] == 8.0f && v.tp_anchor[2] == 4.0f);

    // Move the eye: each tick closes exactly a quarter of the gap. [orig: @ 0x437c8d]
    eye[0] = 16.0f;
    player_view_tick(v, eye);
    CHECK(v.tp_anchor[0] == 10.0f);
    player_view_tick(v, eye);
    CHECK(v.tp_anchor[0] == 11.5f);

    // Leaving third person invalidates; re-entering re-seeds at the current eye.
    v.third_person = false;
    player_view_tick(v, eye);
    CHECK(!v.tp_anchor_valid);
    v.third_person = true;
    player_view_tick(v, eye);
    CHECK(v.tp_anchor_valid && v.tp_anchor[0] == 16.0f);
}

void test_fov_policy() {
    PlayerViewState v;
    const float eye[3] = {0, 0, 0};
    // Hip: the witnessed 80-degree base.
    CHECK(player_view_fov_h_deg(v, 2, 4.0f) == kPlayerCameraFovHDeg);
    // Fully sighted with mag 4: 80 / 4. [orig: @ 0x4df401]
    v.scope_engaged = true;
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    CHECK(player_view_fov_h_deg(v, 2, 4.0f) == 20.0f);
    // No sighted flag, or no magnification: the base fov even when engaged.
    CHECK(player_view_fov_h_deg(v, 1, 4.0f) == kPlayerCameraFovHDeg);
    CHECK(player_view_fov_h_deg(v, 2, 0.0f) == kPlayerCameraFovHDeg);
    // Third person suppresses the zoom outright. [orig: @ 0x4df3fa]
    v.third_person = true;
    CHECK(player_view_fov_h_deg(v, 2, 4.0f) == kPlayerCameraFovHDeg);
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
    CHECK(player_view_fov_h_deg(v, 2, 8.0f) == kBinocularCameraFovHDeg);

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
    CHECK(player_view_fov_h_deg(v, 2, 8.0f) == kPlayerCameraFovHDeg);

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
    CHECK(v.nvg_active); // camera suppression does not consume the toggle
    CHECK(!player_view_nvg_visible(v));
    v.third_person = false;
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

void test_view_bias_blend() {
    PlayerViewState v;
    const float eye[3] = {0, 0, 0};
    const float pos[3] = {-19.46f, 21.19f, -161.31f};   // JOX WPN_AK47AUTO pos
    const float tpos[3] = {-62.33f, 29.19f, -152.56f};  // ... and tpos
    float out[3];
    player_view_bias_units(v, pos, tpos, out);
    CHECK(out[0] == pos[0] && out[1] == pos[1] && out[2] == pos[2]);
    v.scope_engaged = true;
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    player_view_bias_units(v, pos, tpos, out);
    CHECK(out[0] == tpos[0] && out[1] == tpos[1] && out[2] == tpos[2]);
}

void test_input_dispatch_gates() {
    WeaponFsmDef def;
    def.clip_capacity = 30;
    def.flags = 2; // sighted
    WeaponSlotState slot;
    slot.clip = 30;
    slot.reserve = 300;
    // Reload: refused on a full magazine, an empty reserve, or a clipless def.
    // [orig: input case 0xD3 @ 0x4e0420]
    CHECK(!weapon_fsm_reload_allowed(def, slot));
    slot.clip = 12;
    CHECK(weapon_fsm_reload_allowed(def, slot));
    slot.reserve = 0;
    CHECK(!weapon_fsm_reload_allowed(def, slot));
    slot.reserve = 300;
    def.clip_capacity = 0;
    CHECK(!weapon_fsm_reload_allowed(def, slot));
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
    // 1 hipfire-return @ 0x4df1c3; g_scopeHipfire writes @ 0x4df212/@ 0x4df373].
    PlayerViewState v;
    const float eye[3] = {0, 0, 0};
    CHECK(v.scope_hipfire); // [orig: g_scopeHipfire init 1]
    CHECK(player_view_set_engaged(v, true, false));
    CHECK(v.ease_steps == kScopeEaseSteps);
    CHECK(!v.scope_hipfire);
    player_view_tick(v, eye);
    CHECK(player_view_scope_ease_active(v));
    CHECK(!player_view_set_engaged(v, false, false)); // refused mid-ease
    CHECK(v.scope_engaged);
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v));
    CHECK(player_view_set_engaged(v, false, false)); // full disengage ease (not hipfire)
    CHECK(v.ease_steps == kScopeEaseSteps);
    CHECK(v.scope_step == kScopeEaseSteps);
    CHECK(v.scope_hipfire);
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    CHECK(player_view_scope_fraction(v) == 0.0f);

    // Inset weapons (flags2 0x200 — the REVX PointAim MGs / emplaced guns) latch
    // the 7-step ease both ways.
    PlayerViewState vi;
    CHECK(player_view_set_engaged(vi, true, true));
    CHECK(vi.ease_steps == kScopeEaseStepsInset);
    for (int i = 0; i < kScopeEaseStepsInset; ++i) player_view_tick(vi, eye);
    CHECK(player_view_scope_fraction(vi) == 1.0f);
    CHECK(player_view_set_engaged(vi, false, true));
    CHECK(vi.ease_steps == kScopeEaseStepsInset);
}

void test_unscope_on_move_and_up_refusal() {
    // The movement-held latch legs [orig: Player_PackInputStateToEntity @ 0x4df450]:
    // g_movementKeyHeld blocks scope-UP on Scoped weapons (@ 0x4df29c) and, while SETTLED
    // at scope on a Scoped (flags 1) weapon, forces the toggle (@ 0x4df4c9..0x4df4ec).
    const int32_t kScoped = 1;         // weapon.def flags: Scoped
    const int32_t kSighted = 2;        // Sighted (no auto-unscope leg of its own)
    PlayerViewState v;
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

    // Raise and settle the scope; mid-ease movement does NOT fire the settled leg
    // (the mid-ease reversal is the witnessed-deferred tri-state follow-up).
    CHECK(player_view_set_engaged(v, true, false));
    player_view_tick(v, eye);
    CHECK(player_view_scope_ease_active(v));
    CHECK(!player_view_move_input(v, true, kScoped));
    CHECK(!player_view_move_input(v, false, kScoped));
    for (int i = 0; i < kScopeEaseSteps; ++i) player_view_tick(v, eye);
    CHECK(!player_view_scope_ease_active(v));

    // Settled + movement: the auto-unscope fires, Scoped weapons only
    // [orig: g_weaponScopeActive && Def->Flags & 1 @ 0x4df4c9..0x4df4ea].
    CHECK(!player_view_move_input(v, true, kSighted));
    CHECK(player_view_move_input(v, true, kScoped));
    // The caller then runs the standard disengage (the full 15-step return —
    // hipfire was cleared at the raise).
    CHECK(player_view_set_engaged(v, false, false));
    CHECK(v.ease_steps == kScopeEaseSteps);
    CHECK(v.scope_hipfire);
}

bool near_eq(float a, float b, float eps = 0.0005f) {
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

// The FP leg: the doubled recoil, the torso+lean/4 roll, the 0.125 eye floor,
// and the 0.1875 pull-back along the (recoiled) forward.
void test_compose_camera_first_person() {
    PlayerViewState v;
    const float position[3] = {10.0f, 20.0f, 5.0f};
    const float anchor[3] = {10.0f, 20.0f, 6.6f};
    PlayerCameraPose pose;
    // recoil 1 deg (BAM), torso roll 2 deg, lean 4 deg -> roll 2 + 1 = 3 deg.
    const int32_t deg_bam = 11930465; // 2^32 / 360, rounded
    player_view_compose_camera(v, position, anchor, true, nullptr, false,
            90.0f, 0.0f, deg_bam, 2 * deg_bam, 4 * deg_bam, pose);
    CHECK(!pose.third_person);
    CHECK(near_eq(pose.yaw_deg, 90.0f));
    CHECK(near_eq(pose.pitch_deg, 2.0f, 0.01f)); // twice the 1-deg accumulator
    CHECK(near_eq(pose.roll_deg, 3.0f, 0.01f));  // torso 2 + lean 4 / 4
    // yaw 90: mission forward ~= (+cos(pitch)*1, ~0, sin(pitch)); the eye pulls
    // 0.1875 BACK along it from the anchor.
    CHECK(near_eq(pose.eye[0], anchor[0] - 0.1875f * std::cos(2.0f * 3.14159265f / 180.0f), 0.001f));
    CHECK(near_eq(pose.eye[2], anchor[2] - 0.1875f * std::sin(2.0f * 3.14159265f / 180.0f), 0.001f));

    // The floor: an anchor below position + 0.125 clamps up [orig: @ 0x4b6b98].
    const float low_anchor[3] = {10.0f, 20.0f, 5.0f};
    player_view_compose_camera(v, position, low_anchor, true, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 5.125f, 0.001f));

    // No anchor: the non-person +1.0 bump over position [orig: @ 0x437e8f].
    player_view_compose_camera(v, position, position, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 6.0f, 0.001f));
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
    const float anchor[3] = {100.0f, -4.0f, 6.6f}; // below terrain 8.0
    PlayerCameraPose pose;
    player_view_compose_camera(v, position, anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    // yaw 0 pitch 0: fwd = (0, 1, 0); the pull-back rides Y, the Z is the
    // floored eye = 8.0 + 0.0625.
    CHECK(near_eq(pose.eye[2], 8.0625f, 0.001f));

    // INDOORS skips the floor [orig: the Flags & 0x800000 gate @ 0x4b6c08].
    player_view_compose_camera(v, position, anchor, true, &field, true,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 6.6f, 0.001f));

    // An eye already above the floored height passes through untouched.
    const float high_anchor[3] = {100.0f, -4.0f, 9.5f};
    player_view_compose_camera(v, position, high_anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 9.5f, 0.001f));

    // The non-person bump path has no terrain leg [orig: the fallback branch
    // @ 0x4b6b92 stores its offset with only the 0x2000 floor].
    player_view_compose_camera(v, position, position, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 6.0f, 0.001f));

    // The neighbor probes: a ridge one column to +X raises the floor through
    // the +0.25 probe's bilinear tap — the max fold over the five samples.
    // Column x=101 at 24.0u: probe x=100.25 -> 8 + (24-8)*0.25 = 12.0.
    for (int z = 0; z < kDim; ++z) heightmap[z * kDim + 101] = 24 * 256;
    player_view_compose_camera(v, position, anchor, true, &field, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 12.0625f, 0.001f));
}

// The TP leg: the chased anchor wins over the live eye, the pivot nudges
// 0.125 along forward+left+up, the eye backs off the march-landed 0.75, roll
// stays 0 and the recoil doubling does NOT apply.
void test_compose_camera_third_person() {
    PlayerViewState v;
    v.third_person = true;
    v.tp_anchor_valid = true;
    v.tp_anchor[0] = 1.0f;
    v.tp_anchor[1] = 2.0f;
    v.tp_anchor[2] = 3.0f;
    const float position[3] = {0.0f, 0.0f, 0.0f};
    const float anchor[3] = {9.0f, 9.0f, 9.0f}; // must be ignored
    PlayerCameraPose pose;
    const int32_t deg_bam = 11930465;
    player_view_compose_camera(v, position, anchor, true, nullptr, false,
            0.0f, 0.0f,
            deg_bam /* recoil must not leak into TP */, deg_bam, deg_bam, pose);
    CHECK(pose.third_person);
    CHECK(near_eq(pose.pitch_deg, kTpOrbitPitchDeg));
    CHECK(pose.roll_deg == 0.0f);
    // yaw 0 pitch 0: mission fwd = (0, 1, 0), left = (-1, 0, 0), up = (0, 0, 1).
    // pivot = anchor + (fwd+left+up)*0.125; eye = pivot - fwd*0.75.
    CHECK(near_eq(pose.eye[0], 1.0f - 0.125f));
    CHECK(near_eq(pose.eye[1], 2.0f + 0.125f - 0.75f));
    CHECK(near_eq(pose.eye[2], 3.0f + 0.125f));
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
    v.mount.carrier_forward[0] = 0.0f; // mission yaw 0 = +Y
    v.mount.carrier_forward[1] = 1.0f;
    v.mount.carrier_forward[2] = 0.0f;
    // The anchor and the look-ahead as the tick would have settled them:
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
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(pose.third_person);
    CHECK(pose.roll_deg == 0.0f);
    // Looking straight ahead: the eye sits 7 u behind the anchor along the
    // -11.25-degree forward (yaw 0 -> mission fwd = (0, cos p, sin p)).
    const float p = -11.25f * 3.14159265f / 180.0f;
    CHECK(near_eq(pose.eye[0], 0.0f, 0.001f));
    CHECK(near_eq(pose.eye[1], -7.0f * std::cos(p), 0.001f));
    CHECK(near_eq(pose.eye[2], 11.5f - 7.0f * std::sin(p), 0.001f));
    // The final angles look at the anchor + the look-ahead (6 u along the
    // carrier's forward): the yaw stays on the carrier heading, the pitch
    // looks DOWN at it.
    CHECK(near_eq(pose.yaw_deg, 0.0f, 0.01f) || near_eq(pose.yaw_deg, 360.0f, 0.01f));
    CHECK(pose.pitch_deg < 0.0f);

    // The lift floor: r = 1 lifts 1.0, not 0.375; distance 2.5.
    PlayerViewState small = mounted_state(1.0f);
    player_view_compose_camera(small, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[1], -2.5f * std::cos(p), 0.001f));
    CHECK(near_eq(pose.eye[2], 11.0f - 2.5f * std::sin(p), 0.001f));

    // The QUARTER look yaw: a 40-degree look offset orbits the EYE 10 degrees
    // around the anchor (the eye sits 7 u back along the damped yaw); the
    // final yaw is the look-at from there to the carrier-forward point, which
    // lands between the carrier heading and the damped orbit.
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            40.0f, 0.0f, 0, 0, 0, pose);
    const float ten = 10.0f * 3.14159265f / 180.0f;
    CHECK(near_eq(pose.eye[0], -7.0f * std::sin(ten) * std::cos(p), 0.002f));
    CHECK(near_eq(pose.eye[1], -7.0f * std::cos(ten) * std::cos(p), 0.002f));
    CHECK(pose.yaw_deg > 0.5f && pose.yaw_deg < 10.0f);
    // ... and it is symmetric.
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            -40.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[0], 7.0f * std::sin(ten) * std::cos(p), 0.002f));
    CHECK(pose.yaw_deg > 350.0f && pose.yaw_deg < 359.5f);

    // The WATERCRAFT drop: half the radius off the eye.
    PlayerViewState boat = mounted_state(4.0f);
    boat.mount.watercraft = true;
    PlayerCameraPose boat_pose;
    player_view_compose_camera(boat, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, boat_pose);
    player_view_compose_camera(v, position, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(boat_pose.eye[2], pose.eye[2] - 2.0f, 0.001f));

    // The water floor's polarity: with the water at 100 an entity ABOVE it
    // (position z 200) gets the eye raised to 100.25, a submerged one keeps
    // its underwater eye.
    PlayerViewState wet = mounted_state(4.0f);
    wet.mount.water_z = 100.0f;
    const float above[3] = {0.0f, 0.0f, 200.0f};
    player_view_compose_camera(wet, above, no_anchor, false, nullptr, false,
            0.0f, 0.0f, 0, 0, 0, pose);
    CHECK(near_eq(pose.eye[2], 100.25f, 0.001f));
    player_view_compose_camera(wet, position /* z 10, submerged */, no_anchor, false,
            nullptr, false, 0.0f, 0.0f, 0, 0, 0, pose);
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
            0.0f, 0.0f, 0, 0, 0, flat);
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
            0.0f, 0.0f, 0, 0, 0, raised);
    CHECK(raised.eye[2] >= 13.25f);

    // A ramp rising steeply BEHIND the carrier (the eye side, mission -y =
    // atlas +z) raises the eye above the flat result.
    for (int z = 0; z < kDim; ++z)
        for (int x = 0; x < kDim; ++x)
            heightmap[z * kDim + x] = static_cast<uint16_t>(
                    (8 * 256) + (z > 100 ? (z - 100) * 512 : 0));
    PlayerCameraPose ramp;
    player_view_compose_camera(v, position, no_anchor, false, &field, false,
            0.0f, 0.0f, 0, 0, 0, ramp);
    CHECK(ramp.eye[2] > flat.eye[2] + 1.0f);
}

// The mounted anchor ease in 16.16: a sixteenth per tick on x/y and a
// thirty-second on z toward carrier + lift, mirrored into the float anchor.
void test_tick_mounted_anchor_ease() {
    PlayerViewState v;
    v.third_person = true;
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    player_view_tick(v, eye); // seeds the anchor at the eye
    CHECK(v.tp_anchor_valid);
    v.mount.control_seat = true;
    v.mount.carrier_pos_q16[0] = 16 * 0x10000;
    v.mount.carrier_pos_q16[1] = 0;
    v.mount.carrier_pos_q16[2] = 32 * 0x10000;
    v.mount.bound_radius = 4.0f; // lift 1.5 -> target z 33.5
    v.mount.carrier_forward[0] = 0.0f;
    v.mount.carrier_forward[1] = 1.0f;
    v.mount.carrier_forward[2] = 0.0f;
    player_view_tick(v, eye);
    // x: a sixteenth of 16 = 1.0; z: a thirty-second of 33.5 = 1.046875.
    CHECK(v.tp_anchor_q16[0] == 0x10000);
    CHECK(near_eq(v.tp_anchor[0], 1.0f));
    CHECK(v.tp_anchor_q16[2] == (33 * 0x10000 + 0x8000 + 16) >> 5);
    CHECK(near_eq(v.tp_anchor[2], 1.046875f, 0.0001f));
    // The look-ahead eases a thirty-second toward forward x 6 = (0, 6, 0).
    CHECK(v.lookahead_q16[0] == 0);
    CHECK(v.lookahead_q16[1] == (6 * 0x10000 + 16) >> 5);
    CHECK(v.lookahead_q16[2] == 0);
    // Dismounting returns to the quarter-step float ease from where it was.
    v.mount.control_seat = false;
    player_view_tick(v, eye);
    CHECK(near_eq(v.tp_anchor[0], 0.75f, 0.0001f));
}

// The view-frame bias: raw def units / 256 on the eased blend; the
// NoCardSwitch reload suppression drops the ADS half (the hip offset).
void test_bias_view_units() {
    PlayerViewState v;
    const float pos[3] = {-19.46f, 21.19f, -161.31f};
    const float tpos[3] = {-62.33f, 29.19f, -152.56f};
    float out[3];
    player_view_bias_view_units(v, false, pos, tpos, out);
    CHECK(near_eq(out[0], -19.46f / 256.0f));
    CHECK(near_eq(out[2], -161.31f / 256.0f));
    // Fully sighted, then suppressed: the tpos blend collapses to the hip pos.
    v.scope_engaged = true;
    v.scope_step = v.ease_steps;
    player_view_bias_view_units(v, false, pos, tpos, out);
    CHECK(near_eq(out[0], -62.33f / 256.0f));
    player_view_bias_view_units(v, true, pos, tpos, out);
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

int main() {
    test_scope_ease_is_fifteen_ticks_exactly();
    test_equal_ticks_equal_state_regardless_of_frame_grouping();
    test_anchor_chase_quarter_step_and_seeding();
    test_fov_policy();
    test_binoculars_effective_state_and_fov();
    test_nvg_toggle_gain_and_first_person_visibility();
    test_fov_vertical_conversion();
    test_view_bias_blend();
    test_input_dispatch_gates();
    test_toggle_latch_refusal_and_inset();
    test_unscope_on_move_and_up_refusal();
    test_tp_effective_distance_march();
    test_compose_camera_first_person();
    test_compose_camera_terrain_floor();
    test_camera_shake();
    test_compose_camera_third_person();
    test_compose_camera_mounted();
    test_compose_camera_mounted_terrain();
    test_tick_mounted_anchor_ease();
    test_bias_view_units();
    test_motion_lead_tracker();
    if (failures == 0) std::printf("player_view_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
