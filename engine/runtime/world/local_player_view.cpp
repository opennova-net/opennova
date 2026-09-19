// The local player's view cluster, orchestrated -- see local_player_view.h.
// [orig: Input_HandleActionBinding_0 @0x4e0420 case 6; Player_ToggleWeaponScope
//  @0x4df0c0; Player_UpdatePerFrame call @0x42c18e; Render_ProcessMainSceneFrame
//  @0x5ca1f4..0x5ca24b; Camera_SetTrackedEntity @0x439257;
//  ThirdPersonCamera_Update @0x437b70; Player_UpdateFirstPersonCamera @0x4dd380]

#include <runtime/world/local_player_view.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <utility>

#include <formats/def/def.h>        // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <formats/mission/bms.h>    // bms::AttribFlags::StartWithNVGOn
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/death_camera.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed
#include <runtime/world/vehicle_mount.h> // vehicle_prepare_weapon_slot, mount_blocks_weapon_channel
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>

using namespace opennova::def;

namespace opennova::world {

namespace {

constexpr double kPi = io::kPi;

const Entity *local_entity(World *world) {
    return world != nullptr ? world->registry.get(world->cached.local_player) : nullptr;
}

// The mode-4 transition computes the lerp camera once [orig:
// Camera_SetTrackedEntity @0x439257 -> Camera_ComputeThirdPersonPositions
// @0x438b80]. The anchor: the joiner's S2C 0x52 triple (the last received one,
// zeros like retail's globals before any), the authority's +0x178 killer
// entity position, else the player itself.
void enter_death_camera(World &world, const Entity &e, PlayerViewState &v,
                        const LocalViewSessionInputs &s) {
    const int32_t player[3] = {
        to_fixed(e.position.x), to_fixed(e.position.y), to_fixed(e.position.z)};
    int32_t anchor[3] = {player[0], player[1], player[2]};
    if (s.joiner) {
        if (s.death_camera_target_known) {
            anchor[0] = s.death_camera_target[0];
            anchor[1] = s.death_camera_target[1];
            anchor[2] = s.death_camera_target[2];
        }
    } else if (const Entity *killer = world.registry.get(e.last_attacker)) {
        anchor[0] = to_fixed(killer->position.x);
        anchor[1] = to_fixed(killer->position.y);
        anchor[2] = to_fixed(killer->position.z);
    }
    // The probe's bone leg (Entity_ComputeCollisionForceFromBones @0x4afff0
    // over the tracked entity's collision-bone list) is unported, and with no
    // bone list retail returns the full reach untouched (@0x4378c4): the
    // count-0 path, fed here with the world terrain for when the bones land.
    const terrain::TerrainHeightField *terrain =
        world.ai.terrain;
    const CameraTerrainSampler sampler = [terrain](int32_t x, int32_t y) -> int32_t {
        if (terrain == nullptr || !terrain->valid()) return INT32_MIN / 2;
        // Engine ground plane (x, y) -> the atlas' (x, -y) sample, the
        // calc_average_ground_height mapping player_view.cpp uses.
        return to_fixed(terrain::height_field_height_world_bilinear(
            *terrain, static_cast<float>(x) / 65536.0f, -static_cast<float>(y) / 65536.0f));
    };
    const CameraRayProbe probe = [&sampler](const int32_t origin[3], const int32_t dir[3],
                                            int32_t lift, int32_t max_dist) {
        return death_camera_probe_terrain(sampler, /*bone_count=*/0, origin, dir, lift,
                                          max_dist);
    };
    const uint32_t start_tick = v.death_cam.start_tick;
    death_camera_compute(player, anchor, probe, v.death_cam);
    v.death_cam.start_tick = start_tick;
}

// The reload-only predicate also gates optical-view admission. NoCardSwitch
// exempts this reload leg; it does not exempt the camera's airborne leg.
// [orig: Player_IsReloadingCardSwitchWeapon @0x4dcdd0; optical gate @0x5cf7be;
// Player_UpdateFirstPersonCamera @0x4dd439/@0x4dd4cc]
bool reloading_card_switch_weapon(const LocalPlayerWeapon &w, const WeaponSlotState *slot) {
    return w.active && slot != nullptr && slot->current == weapon_action::kReload &&
           (w.def.flags & weapon_flag::kNoCardSwitch) == 0;
}

bool suppress_view_bias(const LocalPlayerWeapon &w, const WeaponSlotState *slot,
                        const Entity *player) {
    // Skip the interpolated bias without changing the scope's target or ease.
    // ForceScoped affects optical admission, not either camera-bias skip.
    // [orig: Player_UpdateFirstPersonCamera @0x4dd380, rotation @0x4dd40d/0x4dd414,
    // position @0x4dd49f/0x4dd4a6]
    return (player != nullptr &&
            ((player->flags | player->engine_flags) & kEntityFlagInAir) != 0) ||
           reloading_card_switch_weapon(w, slot);
}

} // namespace

void local_player_camera_reset(World *world, const LocalPlayerWeapon &w, PlayerViewState &v) {
    // [orig: Player_ResetCameraAndMovementState @0x4DE1F0]
    player_view_weapon_switch_reset(v);
    v.weapon_pose_bound = w.active && (w.def.flags & 3) != 0; // @0x4de287..0x4de2a7
    if (world != nullptr) world->weather.core.scalar_channels.camera_fov_target_fp = 80 << 16;
    v.binoculars_view_active = false; // @0x4de2ad
    v.binoculars_requested = false;   // @0x4de2b3
}

void local_player_view_reset(World *world, LocalPlayerWeapon &w, PlayerViewState &v,
                             LocalPlayerViewTracker &t) {
    local_player_camera_reset(world, w, v);
    // The raised pose is re-derived from the cleared toggle every tick
    // [orig: Player_UpdatePerFrame @0x4de388]; drop it and the seeded aim
    // displacement with the request here.
    v.binoculars_raised = false;
    t.binocular_yaw_offset_deg = 0.0f;
    t.binocular_pitch_offset_deg = 0.0f;
    t.binocular_sway_latched = false; // [orig: dword_29D6BA8 @0x5ca4b0]
    v.nvg_gain = kNvgGainMin;
    v.nvg_active = world != nullptr &&
                   (world->tables.mission_attrib_flags &
                    static_cast<uint32_t>(bms::AttribFlags::StartWithNVGOn)) != 0;
    w.nvg_scope_restore = false;
    local_player_view_refresh(world, v);
}

void local_player_view_refresh(World *world, PlayerViewState &v) {
    const Entity *local = local_entity(world);
    // Camera changes also update layer admission on frames without a body tick.
    // [orig: Camera_SetTrackedEntity @0x4391D0 -> sub_75BE80 @0x75BE80]
    if (local != nullptr) world->cached.sound_listener_view_flags = v.camera_mode == 0 ? 2 : 4;
    const bool alive = local != nullptr && local->alive && local->health > 0;
    const bool round_ended = world != nullptr && world->match.outcome().ended;
    player_view_update_effective_modes(v, alive, round_ended);
}

bool local_player_mount_slot_select(World &world, const LocalPlayerWeapon &w,
                                    MountSlotSelectRequest &out) {
    out = MountSlotSelectRequest();
    if (!w.active) return false;
    Entity *player = world.registry.get(world.cached.local_player);
    Entity *mount = player != nullptr && player->mounted && player->use_gun_slot_swapped &&
                            player->mount_target.valid()
                        ? world.registry.get(player->mount_target)
                        : nullptr;
    if (mount == nullptr || !mount->has_item_def || mount->item_type == 1u ||
        (mount->item_attrib & kItemAttribEweap) == 0u ||
        (mount->emplacement_attachment_flags & 0x02u) == 0u ||
        !world.vehicles.prepare_weapon_slot(*mount))
        return false;
    const bool use_parent_slot = !mount->primary_weapon_slot.redirect_to_parent_slot;
    Entity *parent = nullptr;
    bool route_valid = !use_parent_slot;
    if (use_parent_slot && mount->ground_target.valid() &&
        mount->emplacement_parent == mount->ground_target &&
        mount->emplacement_parent_spawn_id != 0) {
        parent = world.registry.get(mount->ground_target);
        route_valid = parent != nullptr &&
                      parent->registry_spawn_id == mount->emplacement_parent_spawn_id &&
                      parent->has_item_def && parent->item_type == 1u &&
                      (parent->item_attrib & kItemAttribEweap) != 0u &&
                      world.vehicles.prepare_weapon_slot(*parent);
    }
    if (!route_valid) return false;
    out.applies = true;
    out.use_parent_slot = use_parent_slot;
    out.mount = player->mount_target;
    if (parent != nullptr) out.parent = mount->ground_target;
    return true;
}

void local_player_apply_mount_slot_select(World &world, LocalPlayerWeapon &w,
                                          const MountSlotSelectRequest &req, PlayerViewState &v) {
    Entity *player = world.registry.get(world.cached.local_player);
    Entity *mount = world.registry.get(req.mount);
    Entity *parent = req.use_parent_slot ? world.registry.get(req.parent) : nullptr;
    if (player == nullptr || mount == nullptr || (req.use_parent_slot && parent == nullptr))
        return;
    mount->primary_weapon_slot.redirect_to_parent_slot = req.use_parent_slot;
    player->equipped_adm_index =
        req.use_parent_slot ? parent->primary_weapon_slot_adm : mount->primary_weapon_slot_adm;
    sync_local_usegun_weapon_transition(world, w, v);
}

int32_t local_player_scope_zoom(const LocalPlayerWeapon &w, WeaponSlotState &slot) {
    // [orig: Player_GetClampedWeaponElevation @0x4DC6B0]
    int32_t maximum = static_cast<int32_t>(w.scope_max_mag);
    if (maximum == 0) maximum = 1;
    if (slot.scope_zoom == 0) slot.scope_zoom = maximum;
    if (slot.scope_zoom < 0) slot.scope_zoom = 0;
    else if (slot.scope_zoom > maximum) slot.scope_zoom = maximum;
    return slot.scope_zoom;
}

ScopeZoomLimits local_player_scope_zoom_limits(const World &world, const LocalPlayerWeapon &w,
                                               int32_t scope_min_mag) {
    // [orig: EquippedSlot->Def +0 (category) @0x4dbe2f / @0x4dfaee, +0x98
    //  @0x4dbe29 / @0x4dfadd]
    ScopeZoomLimits limits;
    limits.scope_min_mag = scope_min_mag;
    const int index = w.active ? world.tables.weapons.index_of(w.def_name.c_str()) : -1;
    limits.category =
        index >= 0 ? world.tables.weapons.entries[static_cast<size_t>(index)].category : 0;
    return limits;
}

int32_t local_player_scope_zoom_floor(const World &world, const ScopeZoomLimits &limits,
                                      int32_t scope_max_mag) {
    // [orig: Player_AdjustWeaponElevation @0x4dbe29 ecx = Def+0x98; the lock
    //  @0x4dbe2f..0x4dbe3f: player+0x294 == 6 && Def+0 == 3 && !byte_A821F0
    //  -> ecx = Def+0x90; Player_MountWeaponSlot @0x4dfadd..0x4dfb01 the same;
    //  byte_A821F0 = rules.allow_sniper_scope_zoom]
    const Entity *player = world.registry.get(world.cached.local_player);
    const bool sniper_lock = player != nullptr && player->player_class == 6 &&
                             limits.category == 3 && !world.rules.allow_sniper_scope_zoom;
    return sniper_lock ? scope_max_mag : limits.scope_min_mag;
}

bool local_player_adjust_scope_zoom(World &world, LocalPlayerWeapon &w, const PlayerViewState &v,
                                    WeaponSlotState &slot, const ScopeZoomLimits &limits,
                                    int32_t delta) {
    // [orig: Player_AdjustWeaponElevation @0x4dbdf0 -- Player_CanFireWeapon
    //  @0x4dbdfc, EquippedSlot @0x4dbe07, Def @0x4dbe0e]
    if (!w.active || !local_player_scope_view_visible(world, w, v)) return false;
    const int32_t maximum = static_cast<int32_t>(w.scope_max_mag);
    const int32_t current = slot.scope_zoom;
    int32_t next = current + delta;                                   // @0x4dbe26
    const int32_t floor = local_player_scope_zoom_floor(world, limits, maximum);
    if (next >= floor) {                                              // @0x4dbe47
        if (next > maximum) next = maximum;                           // @0x4dbe4d..0x4dbe57
    } else {
        next = floor;                                                 // @0x4dbe49
    }
    if (next != current) {                                            // @0x4dbe5b
        // Sound_PlayInterfaceTriggerSet(dword_24E08B4) @0x4dbe64: the
        // non-positional interface play of the "GF_SCOPE" set.
        ScriptSoundEvent click;
        click.name = kScopeZoomStepSoundset;
        click.kind = ScriptSoundEvent::Kind::Interface;
        world.out.script_sounds.push_back(std::move(click));
    }
    slot.scope_zoom = next;                                           // @0x4dbe6c
    return next != current;
}

void local_player_scope_zoom_mount_clamp(const World &world, const ScopeZoomLimits &limits,
                                         int32_t def_flags, int32_t scope_max_mag,
                                         WeaponSlotState &slot) {
    // [orig: Player_MountWeaponSlot -- Def+8 Flags & 1 @0x4dfacf..0x4dfad1,
    //  Def+0x90 != 0 @0x4dfad3..0x4dfadb, the floor @0x4dfadd..0x4dfb01, the
    //  clamp of MountSlot+0xC @0x4dfb03..0x4dfb13]
    if ((def_flags & static_cast<int32_t>(DEF_WEAPON_FLAG_SCOPED)) == 0 || scope_max_mag == 0)
        return;
    const int32_t floor = local_player_scope_zoom_floor(world, limits, scope_max_mag);
    if (slot.scope_zoom < floor) slot.scope_zoom = floor;                      // @0x4dfb0a
    else if (slot.scope_zoom > scope_max_mag) slot.scope_zoom = scope_max_mag; // @0x4dfb13
}

WeaponCycleRoute local_player_weapon_cycle_route(World &world, LocalPlayerWeapon &w,
                                                 const PlayerViewState &v,
                                                 const ScopeZoomLimits &limits,
                                                 int32_t direction) {
    // [orig: Input_HandleActionBinding_0 case 0xD4 @0x4e130c / 0xD6 @0x4e1364:
    //  g_binocularsViewActive || g_fireChargeStartTick -> return]
    if (v.binoculars_view_active || w.power_throw_start_tick != 0)
        return WeaponCycleRoute::kRefused;
    // EquippedSlot && Def && Def+0x98 != Def+0x90 && Player_CanFireWeapon()
    // @0x4e1312..0x4e1333 / @0x4e136a..0x4e138b
    WeaponSlotState *slot = active_local_weapon_slot(world, w);
    if (w.active && slot != nullptr &&
        limits.scope_min_mag != static_cast<int32_t>(w.scope_max_mag) &&
        local_player_scope_view_visible(world, w, v)) {
        // Player_AdjustWeaponElevation(2) @0x4e13d5 (212) / (-2) @0x4e1394 (214)
        local_player_adjust_scope_zoom(world, w, v, *slot, limits, direction > 0 ? 2 : -2);
        return WeaponCycleRoute::kZoomStep;
    }
    return WeaponCycleRoute::kCycle; // Player_CycleWeaponSlot(direction) @0x4e1341 / @0x4e13a4
}

bool local_player_in_vehicle_loadout_zone(const World &world) {
    // [orig: Input_HandleActionBinding_0 case 0xB1 -- parentSlot == 0 @0x4e0a91,
    //  Flags & 0x800 @0x4e0ab2 (the type-11 volume touch, entity.h)]
    const Entity *e = world.registry.get(world.cached.local_player);
    return e != nullptr && !e->mounted &&
           ((e->flags | e->engine_flags) & kEntityFlagVehicleLoadoutZone) != 0;
}

bool local_player_vehicle_zone_team_matches(const World &world) {
    // [orig: @0x4e0ad8 edx = player->groundEntity (+0x28); @0x4e0adb al =
    //  groundEntity->Team byte (+0x162); 0 opens @0x4e0ae3, else it must equal
    //  the player's Team byte @0x4e0ae5..0x4e0aeb]
    const Entity *e = world.registry.get(world.cached.local_player);
    if (e == nullptr) return false;
    const Entity *ground = world.registry.get(e->ground_target);
    const uint8_t ground_team = ground != nullptr ? ground->team : 0;
    return ground_team == 0 || ground_team == e->team;
}

namespace {
int32_t sighted_fov_target(int32_t zoom) {
    // Reciprocal is truncated to Q16 before the rounded multiply by 80 Q16.
    // [orig: Player_ToggleWeaponScope @0x4DF401..0x4DF430]
    if (zoom < 1) zoom = 1;
    return static_cast<int32_t>((int64_t(80 << 16) * (65536 / zoom) + 0x8000) >> 16);
}

// Despite its original name, this is the optical-view gate. Its target writes
// run at each body/weapon/HUD/render query, including frames with no simulation tick.
// [orig: Player_CanFireWeapon @0x5CF780..0x5CF8D5]
bool scope_view_visible(World &world, const LocalPlayerWeapon &w, const PlayerViewState &v,
                        const Entity &player, WeaponSlotState &slot) {
    if (!w.active || (player.mounted && is_vehicle_control_seat(player.mount_type)))
        return false;
    const bool force = (w.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0;
    const bool no_card = (w.def.flags & weapon_flag::kNoCardSwitch) != 0 && !force;
    if (reloading_card_switch_weapon(w, &slot) && !no_card) return false;
    // The PROMOTED byte [orig: Player_IsEquippedWeaponScoped reads
    // g_weaponScopeActive], never the target or the ease.
    const bool active = player_view_scope_settled(v);
    const bool scoped = active && (w.def.flags & DEF_WEAPON_FLAG_SCOPED) != 0;
    const bool sighted = active && (w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
                         slot.current != weapon_action::kSwitchFrom;
    // These fields share retail storage; use the motor/input mirror where it
    // has not yet been published back into the registry on this tick.
    const AiEntity *body = world.ai.for_handle(player.handle);
    const uint32_t flags = player.flags | player.engine_flags;
    const bool moving = body != nullptr ? body->inf.player_moving
                                       : (player.net_move_input & 8u) != 0;
    const bool airborne = (flags & kEntityFlagInAir) != 0 ||
                          (body != nullptr && body->inf.airborne);
    // [orig: entity Flags & 0x2002 gate @0x5CF7FB; ForceScoped override @0x5CF845]
    bool visible = (flags & kEntityFlagDead) == 0 && !airborne && v.camera_mode == 0 &&
                   (!moving || sighted) && (scoped || sighted);
    if (force && v.camera_mode == 0) {
        visible = true;
    } else if (!sighted) {
        const int32_t position_z = body != nullptr ? body->pos[2] : to_fixed(player.position.z);
        const int32_t eye_z = static_cast<int32_t>(
            uint32_t(position_z) + uint32_t(player.eye_offset_z));
        if ((flags & 0x8000u) != 0 || eye_z < world.env.water_z)
            visible = false;
    }
    if (!player_view_scope_ease_active(v) &&
        !(player.mounted && player.mount_type == SeatType::Gunner)) {
        auto &target = world.weather.core.scalar_channels.camera_fov_target_fp;
        if (!visible) target = 80 << 16;
        else if (sighted) target = sighted_fov_target(local_player_scope_zoom(w, slot));
    }
    return visible;
}
}

bool local_player_scope_view_visible(World &world, LocalPlayerWeapon &w,
                                      const PlayerViewState &v) {
    const Entity *player = world.registry.get(world.cached.local_player);
    WeaponSlotState *slot = active_local_weapon_slot(world, w);
    return player != nullptr && slot != nullptr && scope_view_visible(world, w, v, *player, *slot);
}

bool local_player_set_scope(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                            WeaponSlotState &slot, bool engaged) {
    if (!player_view_scope_request_pending(v, engaged)) return true;
    if (!player_view_set_engaged(v, engaged, (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
        return false;
    auto &target = world.weather.core.scalar_channels.camera_fov_target_fp;
    if (!engaged) target = 80 << 16; // [orig: @0x4DF218]
    if ((w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0)
        target = engaged && v.camera_mode == 0
            ? sighted_fov_target(local_player_scope_zoom(w, slot)) : 80 << 16;
    return true;
}

bool local_player_scope_toggle(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                               WeaponSlotState &active_slot) {
    if (!w.active) return false;
    // currentAction not in {RELOAD, SWITCHFROM}, then the Player_ToggleWeaponScope
    // view/definition gates. [orig: Player_ToggleWeaponScope @0x4df0c0]
    if (!weapon_fsm_scope_toggle_allowed(w.def, active_slot)) return false;
    // The toggle branches on the PROMOTED byte, not the target: a promoted
    // sight disengages, anything else engages [orig: the g_weaponScopeActive
    // branch @0x4df17f].
    const bool promoted = player_view_scope_settled(v);
    // ForceScoped pins the raised sight: un-scoping is refused once promoted
    // [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @0x4df12d].
    if (promoted && (w.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0) return false;
    // Inset optics cannot be raised under NVG. Non-Inset sights retain the
    // original independent behavior.
    if (!promoted && v.nvg_active && (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0)
        return false;
    // Every toggle is refused while the previous ease runs
    // [orig: (flags & 3) && !g_fpCameraInterp.activeFlag @0x4df177].
    if (player_view_scope_ease_active(v)) return false;
    // Scope-UP is refused while a movement key is held on a Scoped weapon
    // [orig: the engage branch's g_movementKeyHeld && (flags & 1) -> return @0x4df29c].
    if (!promoted && player_view_scope_up_blocked(v, w.def.flags)) return false;
    // The toggle latches this ease's step count (7 for Inset weapons, else 15;
    // 1 on the hipfire-return leg) [orig: Setup @0x4df1b3..0x4df36e].
    if (!local_player_set_scope(world, w, v, active_slot, !promoted))
        return false;
    // The engage leg forces the promoted byte to 1 around its seat-flag
    // queries, so an OnlyScoped AbsorbPitch weapon levels the body pitch as
    // the sight comes up; the tube elevation then rides the offset alone.
    // [orig: g_weaponScopeActive = 1 @0x4DF2A2; AbsorbPitch query @0x4DF302
    //  -> Pitch = 0 @0x4DF314; g_weaponScopeActive = 0 @0x4DF31D]
    if (!promoted && (w.def.flags & DEF_WEAPON_FLAG_ABSORBPITCH) != 0)
        local_player_level_pitch(world);
    if (!promoted)
        weapon_fsm_queue_scope_up(active_slot);
    else
        weapon_fsm_queue_scope_down(active_slot);
    return true;
}

bool local_player_binoculars_toggle(World &world, const LocalPlayerWeapon &w,
                                    PlayerViewState &v, LocalPlayerViewTracker &) {
    const Entity *local = world.registry.get(world.cached.local_player);
    if (local == nullptr) return false;
    // Retail refuses binoculars while a PowerThrow charge is live. Allowing the
    // view to rise would suppress held weapon input and turn the charge into an
    // unintended release [orig: g_fireChargeStartTick @0xB76800; action 26 gate].
    if (w.power_throw_start_tick != 0) return false;
    // An active scope also blocks binoculars in a gunner parent slot.
    if (v.scope_engaged && local->mounted && local->mount_type == SeatType::Gunner)
        return false;
    const bool requested = player_view_toggle_binoculars(v);
    // The SEED is not this action's: retail draws it from the render frame the
    // first time the optical view is actually up (local_player_binocular_sway_latch
    // below), so a raise refused by movement/death/round end/third person draws
    // nothing at all.
    local_player_view_refresh(&world, v);
    return requested;
}

// The once-per-activation sway seed [orig: Render_ProcessMainSceneFrame
// @0x5ca3d3..0x5ca3f8, the clear @0x5ca4b0]. The retail latch dword_29D6BA8
// gates one Binoculars_RandomizeSwayOffsets @ 0x4dd830 call per activation,
// and is cleared on every frame
// the optical view is down. The draw comes off the SAME PRNG_Next16 owner
// retail uses (World::next_prng16), so a raise consumes one word of the shared
// mission stream exactly as retail's does.
void local_player_binocular_sway_latch(World &world, const PlayerViewState &v,
                                       LocalPlayerViewTracker &t) {
    if (!v.binoculars_view_active) {
        t.binocular_sway_latched = false; // [orig: dword_29D6BA8 = 0 @0x5ca4b0]
        return;
    }
    if (t.binocular_sway_latched) return;
    t.binocular_sway_latched = true; // [orig: dword_29D6BA8 = 1 @0x5ca3e9]
    // PRNG_Next16() << 16 read as a full-circle BAM32 fraction is our
    // `unit_random` in [0, 1); the fixed-radius circle is kBinocularAimOffsetDeg.
    // [orig: Binoculars_RandomizeSwayOffsets @ 0x4dd830]
    const float unit_random = static_cast<float>(world.next_prng16()) / 65536.0f;
    player_view_binocular_sway_offset(unit_random, t.binocular_yaw_offset_deg,
                                      t.binocular_pitch_offset_deg);
}

bool local_player_nvg_toggle(World &world, LocalPlayerWeapon &w, PlayerViewState &v,
                             const std::function<bool()> &scope_toggle) {
    if (world.registry.get(world.cached.local_player) == nullptr) return false;
    if (!v.nvg_active) {
        w.nvg_scope_restore = false;
        if (w.active && player_view_scope_settled(v) &&
            (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0) {
            w.nvg_scope_restore = scope_toggle();
        }
        return player_view_toggle_nvg(v);
    }
    // Clear NVG before the normal scope-up request so the Inset refusal no
    // longer applies, then consume the one-shot restore latch.
    player_view_toggle_nvg(v);
    const bool restore_scope = w.nvg_scope_restore;
    w.nvg_scope_restore = false;
    if (restore_scope && !v.scope_engaged) scope_toggle();
    return false;
}

void local_player_view_tick(World *world, const LocalPlayerWeapon &w, PlayerViewState &v,
                            LocalPlayerViewTracker &t, const LocalViewSessionInputs &s) {
	t.hud_hit_feedback_frames = s.hud_hit_feedback_frames;
	t.hud_service = s.hud_service;
	t.hud_designations = s.hud_designations;
    if (world == nullptr || !world->cached.local_player.valid()) {
        // No seat without a player: the arbiter resolves to first person (or
        // the debug override) before the effective modes read the mode.
        v.mount = MountedCameraInput();
        player_view_resolve_mode(v);
        if (world != nullptr)
            world->cached.sound_listener_view_flags = v.camera_mode == 0 ? 2 : 4;
        player_view_update_effective_modes(v, false,
                                           world != nullptr && world->match.outcome().ended);
        v.tp_anchor_valid = false;
        return;
    }
    const Entity *e = world->registry.get(world->cached.local_player);
    if (!e) return;
    // The mounted camera's carrier read, refreshed every tick: only a CONTROL
    // seat (the retail parentSlot 2/5 test) takes the mounted leg, and the
    // carrier's pose/radius/class feed the chase target, the back-off and the
    // watercraft eye drop [orig: Camera_ComputeThirdPersonView @0x437D10 --
    // the +0x168 seat test, parentEntity +0x16C, boundRadius +0, the unitType
    // +0x196 in {3,4} test @0x43861D..0x43864C; see player_view.h]. The same
    // seat test is the arbiter's [orig: Render_ProcessMainSceneFrame
    // @0x5ca1e2..0x5ca1f2], so the read precedes the mode resolve and the
    // effective-mode refresh below.
    MountedCameraInput mount;
    const Entity *carrier = e->mounted ? world->registry.get(e->mount_target) : nullptr;
    if (carrier != nullptr && is_vehicle_control_seat(e->mount_type)) {
        int32_t pitch_bam = 0, roll_bam = 0;
        carrier_pose_fixed(*carrier, mount.carrier_pos_q16, mount.carrier_yaw_bam, pitch_bam,
                           roll_bam);
        mount.control_seat = true;
        // The carrier's unit forward for the 6 u look-ahead. Retail takes the
        // chassis matrix's first column (parentMatrix +0xB4 x (6,0,0)); this
        // reads the carrier through carrier_pose_fixed, whose yaw is the one
        // attitude term every mover family stamps, so the forward is the
        // yaw-only form (sin yaw, cos yaw, 0) in mission space -- the pitch/roll
        // fold of the full chassis matrix is not composed here [orig:
        // Camera_ComputeThirdPersonView @0x438811..0x4388b5, see
        // docs/world/world-wac-ai-re.md section 14.6].
        const double forward_yaw_rad =
            mission_yaw_deg_from_bam_heading(mount.carrier_yaw_bam) * (kPi / 180.0);
        mount.carrier_forward[0] = static_cast<float>(std::sin(forward_yaw_rad));
        mount.carrier_forward[1] = static_cast<float>(std::cos(forward_yaw_rad));
        mount.carrier_forward[2] = 0.0f;
        mount.bound_radius = carrier->bound_radius;
        mount.watercraft = carrier->item_unit_type == 3 || carrier->item_unit_type == 4;
        mount.water_z = static_cast<float>(world->env.water_z) / 65536.0f;
    }
    v.mount = mount;
    // The remaining arbiter inputs [orig: Render_ProcessMainSceneFrame
    // @0x5ca1f4..0x5ca24b; see player_view.h]. The two g_rules_flags bits are
    // admin `set` commands with no wire fold yet: carried false.
    v.local_dead = s.local_dead;
    v.death_screen_active = s.death_screen_active;
    v.death_screen_submode = s.death_screen_submode;
    v.round_ended = world->match.outcome().ended || s.end_round_known;
    v.on_foot = !e->mounted;
    v.in_session = s.in_session;
    v.view_tick = world->logic_tick;
    // The death stamp (retail: g_camera_lerp_start_tick = current_tick on the
    // local death path @0x4b4d00 / the 0x13 self record @0x42ec0f): the local
    // dead EDGE.
    if (v.local_dead && !t.camera_local_dead_seen) v.death_cam.start_tick = world->logic_tick;
    t.camera_local_dead_seen = v.local_dead;
    const int mode_before = v.camera_mode;
    player_view_resolve_mode(v);
    if (v.camera_mode == 4 && mode_before != 4) enter_death_camera(*world, *e, v, s);
    local_player_view_refresh(world, v);
    // The per-tick movement delta the FP motion lead samples per render frame
    // (retail: the (position - entity+0x80 prev-position) << 8 samples
    // @0x437bb2/0x437b92/0x437ba2 -- player_view.h carries the witness).
    if (t.tick_prev_valid) {
        t.tick_delta[0] = e->position.x - t.tick_prev_pos[0];
        t.tick_delta[1] = e->position.y - t.tick_prev_pos[1];
        t.tick_delta[2] = e->position.z - t.tick_prev_pos[2];
    }
    t.tick_prev_pos[0] = e->position.x;
    t.tick_prev_pos[1] = e->position.y;
    t.tick_prev_pos[2] = e->position.z;
    t.tick_prev_valid = true;
    // Chase the current motor's CameraOffset, re-anchored after movement.
    // [orig: ThirdPersonCamera_Update @0x437B70..0x437B76]
    const Vec3 current_eye = player_eye_position(*e);
    const float eye[3] = {current_eye.x, current_eye.y, current_eye.z};
    player_view_tick(v, eye);
    // The per-quantum camera compose advances the three shake IIR filters
    // from the tick's weather PRNG word (the deltas it yields are overwritten
    // by the rendered frame's own compose below, exactly as retail's per-frame
    // call re-derives the view) [orig: Game_ProcessMainFrame @ 0x526774 ->
    //  Camera_ComputeThirdPersonView @ 0x526781, the mode-0 block
    //  @ 0x43803c..0x4380df]. Any other camera mode leaves the filters be —
    // the chase leg (@ 0x438939..0x4389e5) is stateless, so its per-quantum
    // evaluation has nothing to advance and only the frame sample renders.
    if (v.camera_mode == 0) {
        int32_t d_yaw = 0;
        int32_t d_pitch = 0;
        int32_t d_roll = 0;
        camera_shake_sample(v.shake, world->weather.core.oscillator.prng, d_yaw, d_pitch, d_roll);
    }
}

// HUD stance differs from the body's animation stance while mounted.
// [orig: HUD_BuildEntityInfo @0x4B8440 -- MoveOrder stance @0x4B860C..0x4B8636,
//  seat switch @0x4B863D..0x4B8767, organic mounted @0x4B876D..0x4B8779,
//  parachute @0x4B8780..0x4B8786]
static void fill_hud_context(World *world, const Entity *local,
        const LocalPlayerWeapon &weapon, LocalPlayerViewFrame &out) {
    if (!world || !local) return;
    const AiEntity *body = world->ai.for_handle(local->handle);
    out.hud_stance = body ? int(body->inf.stance) : 0;
    out.hud_weapon_category = weapon.active ? weapon.hud_category : 0;
    // The misleadingly named Player_IsVehicleHasAutoAim reads the equipped
	// weapon's Inset flag, not the occupied seat. The combat feed adds the
	// separate Sighted hit-feedback exception after the promoted scope read.
    // [orig: @0x4DCCB0..0x4DCCDA; crosshair draw gate @0x592AFA]
    out.hud_keep_crosshair_while_aimed =
        weapon.active && (weapon.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0;
    const Entity *mount = world->registry.get(local->mount_target);
    if (mount && mount->has_item_def) {
        out.hud_mount_slot = int(local->mount_type);
        switch (local->mount_type) {
        case SeatType::Passenger:
        case SeatType::Controller:
        case SeatType::Driver:
            out.hud_stance = 3;
            break;
        case SeatType::Gunner:
            // A gunner reads "Emplaced" (4) whatever carries the gun. Retail's
            // carrier-is-a-vehicle -> 3 leg tests hudInfo+0x234, which the frame
            // builder has just memset to zero, so it never fires.
            // [orig: HUD_RenderAllOverlays memset @0x5A80A5..0x5A80B1 before the
            //  call @0x5A80BC; the stale read @0x4B84D1 and its dead store
            //  @0x4B8507; the EmplacedStance - 1 override @0x4B8539..0x4B8549]
            out.hud_stance = 4;
            if (weapon.active && weapon.emplaced_stance != 0)
                out.hud_stance = uint8_t(uint32_t(weapon.emplaced_stance) - 1u);
            break;
        default: break;
        }
    }
    if (body && (local->flags & kEntityFlagMounted) != 0) out.hud_stance = 3;
    if ((local->flags & kEntityFlagParachute) != 0) out.hud_stance = 5;
}

void local_player_view_frame(World *world, LocalPlayerWeapon &w, const PlayerViewState &v,
		LocalPlayerViewTracker &t, LocalPlayerViewFrame &out) {
    out = LocalPlayerViewFrame();
    WeaponSlotState *active_slot =
        world != nullptr ? active_local_weapon_slot(*world, w) : nullptr;
    out.scope_engaged = v.scope_engaged;
    out.scope_settled = player_view_scope_settled(v);
    out.binoculars_requested = v.binoculars_requested;
    out.binoculars_raised = v.binoculars_raised;
    out.binoculars_view_active = v.binoculars_view_active;
    out.binocular_yaw_offset_deg = t.binocular_yaw_offset_deg;
    out.binocular_pitch_offset_deg = t.binocular_pitch_offset_deg;
    out.nvg_active = v.nvg_active;
    out.nvg_visible = player_view_nvg_visible(v);
    out.nvg_gain = v.nvg_gain;
    // The three fullscreen damage-feedback quads and the HUD early return
    // [orig: Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48;
    //  HUD_RenderAllOverlays @0x5a8098]. The death screen owns its own frame,
    // so retail skips all three while it is up [orig: @0x5cab9a..0x5caba1].
    if (!v.death_screen_active) {
        out.screen_flash_white_alpha = v.flash.white;
        out.screen_flash_red_alpha = screen_flash_red_draw_alpha(v.flash, v.camera_mode);
        out.screen_flash_revive = v.flash.revive;
        out.screen_flash_revive_channel = screen_flash_revive_channel(v.flash);
    }
    out.hud_overlays_suppressed = screen_flash_hud_overlays_suppressed(v.flash);
    const Entity *local = local_entity(world);
    out.mounted = local != nullptr && local->mounted;
    fill_hud_context(world, local, w, out);
    // The RESOLVED camera mode and the chase preference behind it
    // [orig: g_camera_mode @0xA890C8; g_camera_third_person_selected @0xA860DF].
    out.third_person = v.third_person;
    out.third_person_selected = v.third_person_selected;
    // The resolved mode word (0 first person, 1 chase, 4 the death lerp camera).
    out.camera_mode = v.camera_mode;
    // The camera's mounted leg is engaged: a control seat with a live carrier
    // (the per-tick carrier read) AND the resolved third person -- the compose
    // fork's own gate.
    out.camera_mounted = v.mount.control_seat && v.third_person;
    // Structural proxy for Player_IsVehicleHasAttackCapability until mounted
    // weapon inventory is modeled: these seat classes replace the on-foot
    // upper-body weapon channel; passenger seats do not.
    out.vehicle_attack_context = local != nullptr && mount_blocks_weapon_channel(*local);
    out.scope_fraction = player_view_scope_fraction(v);
    out.suppress_view_bias = suppress_view_bias(w, active_slot, local);
    // On the supported on-foot first-person path, the standard SIGHTS card
    // replaces the FP viewmodel once ADS settles. Scoped and Sighted are
    // asymmetric selectors; NoCardSwitch clears both unless ForceScoped
    // overrides it. The frame draws the card or the FP viewmodel, never both.
    // [orig: Render_ProcessMainSceneFrame @0x5ca299..0x5ca304 /
    //  @0x5caaf3..0x5cab15; suppression @0x4dcce0]
    const bool optical_view = world != nullptr && local != nullptr && active_slot != nullptr &&
                              !v.death_screen_active &&
                              local_player_scope_view_visible(*world, w, v);
    out.scope_card_active = optical_view &&
                            weapon_sights_card_eligible(w.def, *active_slot) &&
                            player_view_scope_settled(v) && !v.binoculars_view_active;
    // The thermal view (local_player_view.h): optical_view IS the CanFire
    // verdict this frame, so the latched byte is that AND the equipped def's
    // Thermal bit; the terrain ramps key on the def bit in first person alone
    // [orig: Render_ProcessMainSceneFrame @0x5ca290 -> @0x5ca2da..0x5ca2e3;
    //  Render_TerrainScene @0x610e51..0x610e5b].
    const bool thermal_def = w.active && (w.def.flags2 & DEF_WEAPON_FLAG2_THERMAL) != 0;
    out.thermal_view = optical_view && thermal_def;
    out.thermal_terrain_view = thermal_def && v.camera_mode == 0;
    const bool sighted = out.scope_card_active &&
                         (w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
                         active_slot->current != weapon_action::kSwitchFrom;
    const bool scoped = out.scope_card_active &&
                        (w.def.flags & DEF_WEAPON_FLAG_SCOPED) != 0 &&
                        (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) == 0;
    // The modern main-scene branches: Sighted requires a nonzero max-zero
    // definition; Scoped always applies its slot offsets. Binoculars bypass
    // both. [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0,
    // Sighted @0x5ca452..0x5ca465; Scoped @0x5ca494..0x5ca4a0]
    out.scope_camera_zero_active = sighted ? w.def.scope_zero.max_steps != 0 : scoped;
    const int32_t current_fov = world != nullptr
        ? world->weather.core.scalar_channels.camera_fov_fp : 80 << 16;
    const int32_t zoom = sighted || scoped ? local_player_scope_zoom(w, *active_slot) : 1;
    out.fov_h_deg = player_view_fov_h_deg(v, current_fov, scoped, sighted, zoom);
    // HUD_DrawScopeOverlayDetails tests CanFire and the two promoted flag
    // selectors, independently of the scene's Inset/NoCardSwitch card fork.
    // [orig: HUD_DrawScopeOverlayDetails @ 0x59e420, gate @0x59e47f]
    out.scope_details_scoped = out.scope_settled && (w.def.flags & DEF_WEAPON_FLAG_SCOPED) != 0;
    const bool details_sighted = out.scope_settled && (w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
        active_slot != nullptr && active_slot->current != weapon_action::kSwitchFrom;
    out.scope_details_active = optical_view && (out.scope_details_scoped || details_sighted) &&
        !v.binoculars_view_active;
    out.scope_weapon_flags = w.def.flags;
    out.scope_magnification = active_slot != nullptr ? active_slot->scope_zoom : 1;
    out.scope_max_range_q16 = w.def.scope_zero.max_range_q16;
    out.scope_zero_word = active_slot != nullptr ? active_slot->scope_zero : 0;
    out.scope_zero_max = w.def.scope_zero.max_steps;
    out.scope_zero_step = w.def.scope_zero.step_metres;
    out.scope_zero_default = w.def.scope_zero.default_metres;
	out.inset_scope_active = optical_view && out.scope_details_scoped &&
			(w.def.flags2 & DEF_WEAPON_FLAG2_INSET) && !v.binoculars_view_active;
	out.inset_fov_over_zoom = out.inset_scope_active
			? float(current_fov) / 65536.0f / local_player_scope_zoom(w, *active_slot)
			: 0;
    out.aim_range_q16 = w.aim_range_q16;
	if (world)
		fill_hud_combat_view(*world, w, out, t, optical_view);
	out.hud_combat.state.dead = out.hud_combat.state.dead || v.death_screen_active;
    out.tp_anchor[0] = v.tp_anchor[0];
    out.tp_anchor[1] = v.tp_anchor[1];
    out.tp_anchor[2] = v.tp_anchor[2];
    out.tp_anchor_valid = v.tp_anchor_valid;
    // The composed camera pose + its FP components -- one native composition
    // (player_view.h, S8): the shell converts frames and stamps the Camera3D
    // node. The recoil doubling and torso+lean/4 roll stay exported separately
    // for diagnostics/probes; authoritative look pitch never inherits the
    // camera-only doubling.
    // [orig: Camera_ComputeThirdPersonView @0x437d10 -- the on-foot person leg
    //  @0x437f9c..0x438031, the TP leg @0x438100..0x4383e2, recoil @0x437fc7,
    //  roll @0x437fe6]
    if (world == nullptr || !world->cached.local_player.valid()) return;
    const AiEntity *p = world->ai.for_handle(world->cached.local_player);
    if (p == nullptr) return;
    const Entity *e = world->registry.get(world->cached.local_player);
    out.fp_terms_valid = true;
    out.fp_pitch_recoil_deg = player_view_fp_pitch_recoil_deg(p->inf.recoil_pitch);
    out.fp_roll_deg = player_view_fp_roll_deg(p->inf.torso_roll, p->inf.lean_angle);
    if (e == nullptr) return;
    // Compose from the body aim. The rendered view adds optical offsets
    // afterwards, matching the main-scene consumer in LocalPlayer::view_frame.
    float aim_yaw = static_cast<float>(mission_yaw_deg_from_bam_heading(p->heading));
    float aim_pitch = static_cast<float>(static_cast<double>(p->pitch) * kDegreesPerBam);
    const float position[3] = {e->position.x, e->position.y, e->position.z};
    // CameraOffset already includes the current motor's posed head and its
    // on-foot terrain floor. Re-anchor it after the motor's translation.
    // [orig: Camera_ComputeThirdPersonView @0x437FA5..0x437FB7]
    const Vec3 current_eye = player_eye_position(*e);
    const float anchor_eye[3] = {current_eye.x, current_eye.y, current_eye.z};
    const bool seated_eye = e->mounted;
    player_view_compose_camera(v, position, anchor_eye, p->inf.active,
                               world->ai.terrain,
                               (e->flags & kEntityFlagIndoors) != 0, aim_yaw, aim_pitch,
                               p->inf.recoil_pitch, p->inf.torso_roll, p->inf.lean_angle,
                               // The carrier leg: a seated occupant's view rotation is
                               // the entity triple, and the person leg is jumped over
                               // entirely. The roll is the seat-carried hull bank the
                               // mount pose wrote, never the standing torso tilt.
                               seated_eye, static_cast<float>(e->roll), out.camera);
	out.inset_camera = out.camera; // unshaken pose, before either scene sample
    // The camera shake, per rendered frame: retail's scene frame calls
    // Camera_ComputeThirdPersonView once more per frame after the per-quantum
    // call [orig: Render_ProcessMainSceneFrame @ 0x5ca34d]. First person
    // samples the three IIR filters again from the current weather PRNG word
    // (unchanged between ticks) [orig: the mode-0 block @ 0x43803c..0x4380df,
    //  the >> 6 applies @ 0x4380b0..0x4380d9]; the chase applies the
    // STATELESS sin/cos chain over the raw counter and the engine tick
    // [orig: the mode>=1 block @ 0x438939..0x4389e5 — the mode-4 lerp then
    //  overwrites the rotation wholesale, so only mode 1 renders it]. The
	// Inset overlay samples the same filters once again below (@0x5C9841).
    if (world != nullptr) {
        int32_t d_yaw = 0;
        int32_t d_pitch = 0;
        int32_t d_roll = 0;
        if (!out.camera.third_person) {
            camera_shake_sample(v.shake, world->weather.core.oscillator.prng, d_yaw, d_pitch,
                                d_roll);
        } else if (out.camera_mode == 1) {
            camera_shake_sample_chase(v.shake, world->weather.core.oscillator.prng,
                                      world->logic_tick, d_yaw, d_pitch, d_roll);
        }
        constexpr float kDegPerBam = 360.0f / 4294967296.0f;
        out.camera.yaw_deg += static_cast<float>(d_yaw) * kDegPerBam;
        out.camera.pitch_deg += static_cast<float>(d_pitch) * kDegPerBam;
        out.camera.roll_deg += static_cast<float>(d_roll) * kDegPerBam;
    }
	if (out.inset_scope_active) {
		int32_t yaw = 0, pitch = 0, roll = 0;
		camera_shake_sample(v.shake, world->weather.core.oscillator.prng, yaw, pitch, roll);
		constexpr double degrees_per_bam = 360.0 / 4294967296.0;
		out.inset_camera.yaw_deg += float(yaw * degrees_per_bam);
		out.inset_camera.pitch_deg += float(pitch * degrees_per_bam);
		out.inset_camera.roll_deg += float(roll * degrees_per_bam);
		out.inset_camera.yaw_deg -= float(active_slot->zero_yaw * degrees_per_bam);
		out.inset_camera.pitch_deg -= float(active_slot->zero_pitch * degrees_per_bam);
	}
    out.camera_pose_valid = true;
}

void local_player_viewmodel_rotation_bias(World *world, const LocalPlayerWeapon &w,
                                         const PlayerViewState &v, int32_t out_bam[3]) {
    const WeaponSlotState *slot = world != nullptr ? active_local_weapon_slot(*world, w) : nullptr;
    const bool suppressed = suppress_view_bias(w, slot, local_entity(world));
    // Bone's hip rotation remains the base; only the published interpolation
    // difference is suppressed. ForceScoped does not bypass this camera gate.
    // [orig: Player_UpdateFirstPersonCamera @0x4DD40D..0x4DD456]
    for (int i = 0; i < 3; ++i)
        out_bam[i] = suppressed ? 0 : v.weapon_pose_interp.rotation_bias_bam[i];
}

void local_player_viewmodel_bias(World *world, const LocalPlayerWeapon &w,
                                 const PlayerViewState &v, LocalPlayerViewTracker &t,
                                 const float pos_raw_units[3],
                                 int viewport_w, int viewport_h, float out[3]) {
    const WeaponSlotState *active_slot =
        world != nullptr ? active_local_weapon_slot(*world, w) : nullptr;
    player_view_bias_view_units(v, suppress_view_bias(w, active_slot, local_entity(world)),
                                pos_raw_units, out);
    // The per-frame motion lead: the witnessed pre-rotation add takes the
    // world-delta components RAW onto the view-frame lanes (no frame
    // conversion) [orig: Player_UpdateFirstPersonCamera @0x4dd549..0x4dd56c,
    // see player_view.h].
    int32_t lead[3];
    player_view_motion_lead_update(t.motion_lead, t.tick_delta, lead);
    for (int i = 0; i < 3; ++i) out[i] += static_cast<float>(lead[i]) / 65536.0f;
    // The 4:3 framing drop -- the 3w<=4h rule [orig: Player_UpdateFirstPersonCamera
    // @0x4dd571..0x4dd578 -> player_view_narrow_aspect]; the caller only
    // samples the viewport.
    if (player_view_narrow_aspect(viewport_w, viewport_h))
        out[2] -= static_cast<float>(kFpNarrowAspectDropQ16) / 65536.0f;
}

} // namespace opennova::world
