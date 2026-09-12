// The local player's view cluster, orchestrated (world/local_player_view.h)
// [orig: Player_ToggleWeaponScope @0x4df0c0 — the refusal ladder @0x4df29c /
//  @0x4df12d / @0x4df177; the binocular action 26 gate (g_fireChargeStartTick
//  @0xB76800); the NVG action 41 Inset restore latch; the arbiter feed
//  Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b and the death stamp
//  @0x4b4d00]: the gates in front of the primitives and the order the tick
//  runs them in, pinned where they used to live in the Godot binding.
#include <cstdint>
#include <cstdio>
#include <vector>

#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
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
    // [orig: g_movementKeyHeld && (flags & 1) -> return @0x4df29c]
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
    CHECK(v.ease_steps == kScopeEaseStepsInset); // the Inset ease latch
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
    LocalPlayerWeapon w = scoped_weapon(0);
    w.power_throw_start_tick = 40;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    int samples = 0;
    const auto rng = [&samples]() -> float { ++samples; return 0.25f; };
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t, rng));
    CHECK(!v.binoculars_requested);
    CHECK(samples == 0); // a refused toggle never draws
    w.power_throw_start_tick = 0;
    CHECK(local_player_binoculars_toggle(lw.w, w, v, t, rng));
    CHECK(v.binoculars_requested);
    CHECK(samples == 1); // one draw seeds the aim displacement
    CHECK(t.binocular_yaw_offset_deg != 0.0f || t.binocular_pitch_offset_deg != 0.0f);
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t, rng)); // dropping the request
    CHECK(samples == 1);
    CHECK(t.binocular_yaw_offset_deg == 0.0f && t.binocular_pitch_offset_deg == 0.0f);
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
    const auto rng = []() -> float { return 0.5f; };
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t, rng));
    v.scope_engaged = false;
    CHECK(local_player_binoculars_toggle(lw.w, w, v, t, rng));
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

// --- the tick: arbiter feed, the death stamp edge, the mode-4 entry --------

void test_tick_stamps_the_death_camera_on_the_local_dead_edge() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(0);
    PlayerViewState v;
    LocalPlayerViewTracker t;
    LocalViewSessionInputs s;
    lw.w.logic_tick = 100;
    local_player_view_tick(&lw.w, w, v, t, s);
    CHECK(v.camera_mode == 0);
    CHECK(v.on_foot);
    CHECK(!v.in_session);
    CHECK(v.view_tick == 100);
    CHECK(t.tick_prev_valid);
    // The local dead edge: stamped once at the transition tick, the lerp
    // camera computed on the mode-4 entry, both stable while dead.
    lw.w.logic_tick = 101;
    s.local_dead = true;
    local_player_view_tick(&lw.w, w, v, t, s);
    CHECK(v.local_dead);
    CHECK(v.camera_mode == 4);
    CHECK(v.death_cam.start_tick == 101);
    lw.w.logic_tick = 102;
    local_player_view_tick(&lw.w, w, v, t, s);
    CHECK(v.camera_mode == 4);
    CHECK(v.death_cam.start_tick == 101); // no re-stamp while dead
    // The movement delta sampler follows the entity between ticks.
    lw.entity().position.x += 2.0f;
    local_player_view_tick(&lw.w, w, v, t, s);
    CHECK(t.tick_delta[0] == 2.0f);
    CHECK(t.tick_delta[1] == 0.0f);
}

void test_tick_without_a_player_resolves_first_person() {
    World w;
    LocalPlayerWeapon weapon = scoped_weapon(0);
    PlayerViewState v;
    v.tp_anchor_valid = true;
    v.mount.control_seat = true;
    LocalPlayerViewTracker t;
    LocalViewSessionInputs s;
    local_player_view_tick(&w, weapon, v, t, s);
    CHECK(!v.mount.control_seat);
    CHECK(v.camera_mode == 0);
    CHECK(!v.tp_anchor_valid);
    local_player_view_tick(nullptr, weapon, v, t, s);
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
    CHECK(!view.scope_engaged && view.scope_step == 0);
    CHECK(channels.camera_fov_fp == (60 << 16));
    CHECK(channels.camera_fov_target_fp == (80 << 16));
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
    // The engaged, settled sight on a Sighted weapon selects the card (first
    // person, no binoculars) [orig: Render_ProcessMainSceneFrame @0x5ca299].
    v.scope_engaged = true;
    settle_ease(v);
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(f.scope_fraction == 1.0f);
    CHECK(f.fov_h_deg == kPlayerCameraFovHDeg / 4.0f);
    v.binoculars_view_active = true;
    local_player_view_frame(&lw.w, w, v, t, f);
    CHECK(!f.scope_card_active); // the optical view wins over the card
    CHECK(f.fov_h_deg == kBinocularCameraFovHDeg);
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

void test_set_eye_mirrors_the_head_into_the_world() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(0);
    const float eye[3] = {1.0f, 2.0f, 3.0f};
    local_player_set_eye(&lw.w, w, eye, true);
    CHECK(w.eye_valid);
    CHECK(w.eye_mission[2] == 3.0f);
    CHECK(lw.w.cached.local_head_valid);
    CHECK(lw.w.cached.local_head.y == 2.0f);
    const float offset[3] = {0.0f, 0.0f, 1.5f};
    local_player_set_eye_offset(&lw.w, offset, true);
    CHECK(lw.w.cached.local_head_offset_valid);
    CHECK(lw.w.cached.local_head_offset.z == 1.5f);
    local_player_set_eye(nullptr, w, eye, false); // a null world only drops the sample
    CHECK(!w.eye_valid);
}

} // namespace

// --- the heat window's water gate at the local pump (D-WPN-29) ---------------
// [orig: WeaponAction_ProcessFrame @ 0x540e60 — `Position.Z > Env_WaterHeightFixed
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
    lw.w.rules.session_open = true;
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

int main() {
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
    test_scope_toggle_refuses_inactive_weapon();
    test_scope_up_refused_while_moving_on_scoped_weapon();
    test_inset_scope_refused_under_nvg();
    test_mid_ease_toggle_refused_then_forcescoped_pins_the_sight();
    test_binoculars_refused_while_power_throw_charges_and_rng_untouched();
    test_binoculars_refused_scoped_in_gunner_seat();
    test_nvg_drops_a_settled_inset_scope_and_restores_it();
    test_nvg_over_a_non_inset_scope_leaves_it_alone();
    test_tick_stamps_the_death_camera_on_the_local_dead_edge();
    test_tick_without_a_player_resolves_first_person();
    test_frame_reads_the_state_and_the_card_selector();
    test_scope_fov_target_and_render_queries_share_weather_state();
    test_scope_zoom_clamps_and_weapon_category_fov_reset();
    test_frame_chase_shake_consumes_the_tick();
    test_set_eye_mirrors_the_head_into_the_world();
    test_pump_feeds_the_heat_window_water_gate_from_the_body_z();
    test_weapon_trace_records_one_sample_per_pump_tick();
    test_weapon_trace_samples_since_is_incremental();
    test_local_weapon_input_block_mirrors_the_pump_gate();
    if (failures == 0) std::printf("local_player_view_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
