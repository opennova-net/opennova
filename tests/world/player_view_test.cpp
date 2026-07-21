// Player view-state tests [orig: CNetPlayerInterp_Setup @ 0x4df36e;
// ThirdPersonCamera_Update @ 0x437af0; Player_ToggleWeaponScope @ 0x4df0c0..401;
// Render_SetViewAndProjectionMatrices @ 0x58d900]: the fixed-tick scope ease and
// anchor chase (render-cadence invariance — the review's 30/60/144 fps case),
// the fov policy, and the input-dispatch gates in front of the FSM requests.
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "world/player_view.h"
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
    // byte_B7653B blocks scope-UP on Scoped weapons (@ 0x4df29c) and, while SETTLED
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
    if (failures == 0) std::printf("player_view_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
