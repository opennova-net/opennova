// The local player's view cluster, orchestrated (world/local_player_view.h)
// [orig: Player_ToggleWeaponScope @0x4df0c0 — the refusal ladder @0x4df29c /
//  @0x4df12d / @0x4df177; the binocular action 26 gate (g_fireChargeStartTick
//  @0xB76800); the NVG action 41 Inset restore latch; the arbiter feed
//  Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b and the death stamp
//  @0x4b4d00]: the gates in front of the primitives and the order the tick
//  runs them in, pinned where they used to live in the Godot binding.
#include <cstdint>
#include <cmath>
#include <base/io/rotating_prng.h>
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
    LocalPlayerWeapon w = scoped_weapon(0);
    w.power_throw_start_tick = 40;
    PlayerViewState v;
    LocalPlayerViewTracker t;
    const uint32_t prng_at_start = lw.w.prng16_state;
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t));
    CHECK(!v.binoculars_requested);
    // A refused toggle never activates the view, so the latch never draws.
    CHECK(lw.w.prng16_state == prng_at_start);
    CHECK(!t.binocular_sway_latched);
    w.power_throw_start_tick = 0;
    CHECK(local_player_binoculars_toggle(lw.w, w, v, t));
    CHECK(v.binoculars_requested);
    CHECK(v.binoculars_view_active);
    // The ACTIVATION latch draws exactly one word and seeds the displacement.
    CHECK(lw.w.prng16_state != prng_at_start);
    CHECK(t.binocular_sway_latched);
    CHECK(t.binocular_yaw_offset_deg != 0.0f || t.binocular_pitch_offset_deg != 0.0f);
    const uint32_t prng_after_seed = lw.w.prng16_state;
    // Holding the view up does not re-draw.
    local_player_binocular_sway_latch(lw.w, v, t);
    CHECK(lw.w.prng16_state == prng_after_seed);
    CHECK(!local_player_binoculars_toggle(lw.w, w, v, t)); // dropping the request
    CHECK(lw.w.prng16_state == prng_after_seed);
    CHECK(!t.binocular_sway_latched);
    CHECK(t.binocular_yaw_offset_deg == 0.0f && t.binocular_pitch_offset_deg == 0.0f);
}

// The seed rides the ACTIVATION, not the raw toggle: a raise made while a
// movement key is held draws nothing until the view actually comes up.
// [orig: the dword_29D6BA8 latch @0x5ca3e1..0x5ca3f3, cleared @0x5ca4b0]
void test_binocular_sway_seeds_once_per_activation() {
    LocalWorld lw;
    LocalPlayerWeapon w = scoped_weapon(0);
    PlayerViewState v;
    LocalPlayerViewTracker t;
    v.move_held = true; // suppresses the raise
    const uint32_t prng_at_start = lw.w.prng16_state;
    CHECK(local_player_binoculars_toggle(lw.w, w, v, t));
    CHECK(v.binoculars_requested);
    CHECK(!v.binoculars_view_active);
    CHECK(lw.w.prng16_state == prng_at_start); // suppressed: no draw yet
    v.move_held = false;
    local_player_view_refresh(&lw.w, v);
    local_player_binocular_sway_latch(lw.w, v, t);
    CHECK(v.binoculars_view_active);
    CHECK(lw.w.prng16_state != prng_at_start);
    const uint32_t first = lw.w.prng16_state;
    // Suppress again, then re-activate: the next activation draws afresh.
    v.move_held = true;
    local_player_view_refresh(&lw.w, v);
    local_player_binocular_sway_latch(lw.w, v, t);
    CHECK(!t.binocular_sway_latched);
    v.move_held = false;
    local_player_view_refresh(&lw.w, v);
    local_player_binocular_sway_latch(lw.w, v, t);
    CHECK(lw.w.prng16_state != first);
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
    CHECK(!view.scope_engaged && !view.weapon_pose_interp.active);
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

    // The route: next (+1) steps +2, prev (-1) steps -2 with the GF_SCOPE click,
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
    replacement.player.carry_scoped_aim_drift_from(previous.player);
    // A mission reset seeds new aim and PRNG, but not the oscillators. A
    // non-boundary scope raise continues the previous drift and direction.
    replacement.sample(1, -428, -816);
    CHECK(replacement.w.prng16_state == World::kMissionPrng16Seed);
    // Simultaneous native sessions still own independent local state.
    ScopedAimFixture independent;
    independent.sample(1, 0, 0);
    CHECK(independent.w.prng16_state == World::kMissionPrng16Seed);
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
    f.player.apply_player_input_pre_tick();
    TickContext ctx;
    ctx.world = &f.w;
    ctx.is_authority = true;
    ctx.logic_tick = 0;
    f.ai.tick(f.w, ctx); // includes the unconditional recoil draw before scoped drift
    f.player.sync_local_mounted_input_heading();
    CHECK(f.body().heading == -408 && f.body().pitch == -396);
    CHECK(f.player.input.look_heading == -408 && f.player.input.look_pitch == -396);
    // The next pre-tick input copy must not erase the body's aim additions.
    f.player.apply_player_input_pre_tick();
    CHECK(f.body().inf.target_heading == -408 && f.body().inf.look_pitch == -396);
    LocalPlayerViewFrame frame = f.player.view_frame();
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
        f.player.apply_player_input_pre_tick();
        TickContext ctx;
        ctx.world = &f.w;
        ctx.is_authority = true;
        ctx.logic_tick = 0;
        f.ai.tick(f.w, ctx);
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

int main() {
    test_scoped_aim_follows_local_view_clamps_and_leg_chase();
    test_scoped_aim_original_sequences();
    test_scoped_aim_gates_and_independent_stance_resets();
    test_scoped_aim_body_input_camera_and_fired_round();
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
    test_scope_toggle_refuses_inactive_weapon();
    test_scope_up_refused_while_moving_on_scoped_weapon();
    test_inset_scope_refused_under_nvg();
    test_mid_ease_toggle_refused_then_forcescoped_pins_the_sight();
    test_binoculars_refused_while_power_throw_charges_and_rng_untouched();
    test_binocular_sway_seeds_once_per_activation();
    test_binoculars_refused_scoped_in_gunner_seat();
    test_nvg_drops_a_settled_inset_scope_and_restores_it();
    test_nvg_over_a_non_inset_scope_leaves_it_alone();
    test_tick_stamps_the_death_camera_on_the_local_dead_edge();
    test_tick_without_a_player_resolves_first_person();
    test_frame_reads_the_state_and_the_card_selector();
    test_authored_rotation_bias_continues_through_air_reload_and_rebake();
    test_airborne_view_bias_keeps_interp_and_resumes_on_landing();
    test_airborne_bias_is_separate_from_reload_and_force_scope_admission();
    test_scope_fov_target_and_render_queries_share_weather_state();
    test_scope_zoom_clamps_and_weapon_category_fov_reset();
    test_weapon_cycle_route_steps_the_zoom_and_the_mount_clamp();
    test_frame_chase_shake_consumes_the_tick();
    test_use_item_vehicle_loadout_zone_gates();
    test_set_eye_mirrors_the_head_into_the_world();
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
