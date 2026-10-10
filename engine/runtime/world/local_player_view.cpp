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
#include <vector>

#include <formats/def/def.h>        // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <formats/mission/bms.h>    // bms::AttribFlags::StartWithNVGOn
#include <runtime/audio/oneshot_play.h> // listener view flags
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/death_camera.h>
#include <runtime/world/entity_spawn.h> // max_health_with_difficulty
#include <runtime/world/geom.h>
#include <runtime/world/tp_camera_mount.h>
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed
#include <runtime/world/vehicle_mount.h> // vehicle_prepare_weapon_slot, mount_blocks_weapon_channel
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <runtime/hud/tip_system.h>

using namespace opennova::def;

namespace opennova::world {

namespace {

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

void local_player_camera_reset(World *world, LocalPlayerWeapon &w, PlayerViewState &v) {
    // [orig: Player_ResetCameraAndMovementState @0x4DE1F0]
    player_view_weapon_switch_reset(v);
    v.weapon_pose_bound = w.active && (w.def.flags & 3) != 0; // @0x4de287..0x4de2a7
    if (world != nullptr) world->weather.core.scalar_channels.camera_fov_target_fp = 80 << 16;
    v.binoculars_view_active = false; // @0x4de2ad
    v.binoculars_requested = false;   // @0x4de2b3
    w.nvg_scope_restore = false;      // dword_B76554 = 0 @0x4de2b9
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
    local_player_view_refresh(world, v);
}

void local_player_view_refresh(World *world, PlayerViewState &v) {
    const Entity *local = local_entity(world);
    // Camera changes also update layer admission on frames without a body tick.
    // [orig: Camera_SetTrackedEntity @0x4391D0 -> sub_75BE80 @0x75BE80]
    if (local != nullptr) world->cached.sound_listener_view_flags = audio::listener_view_flags_for_camera(v.camera_mode);
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
    // [orig: Player_AdjustWeaponElevation @0x4dbdf0 -- Player_IsOpticalViewVisible
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
    //  g_BinocularsViewActive || g_FireChargeStartTick -> return]
    if (v.binoculars_view_active || w.power_throw_start_tick != 0)
        return WeaponCycleRoute::kRefused;
    // EquippedSlot && Def && Def+0x98 != Def+0x90 && Player_IsOpticalViewVisible()
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
// [orig: Player_IsOpticalViewVisible @0x5CF780..0x5CF8D5]
bool scope_view_visible(World &world, const LocalPlayerWeapon &w, const PlayerViewState &v,
                        const Entity &player, WeaponSlotState &slot) {
    if (!w.active || (player.mounted && is_vehicle_control_seat(player.mount_type)))
        return false;
    const bool force = (w.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0;
    const bool no_card = (w.def.flags & weapon_flag::kNoCardSwitch) != 0 && !force;
    if (reloading_card_switch_weapon(w, &slot) && !no_card) return false;
    // The PROMOTED byte [orig: Player_IsEquippedWeaponScoped reads
    // g_WeaponScopeActive], never the target or the ease.
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

namespace {

// The toggle's tip events, by the equipped weapon's flags and name: a Scoped
// ShowElevation sight the elevation / binocular hint, a UseDesignator weapon
// the mortar hint, the designator itself the designator hint; each fades
// again on the way down [orig: Player_ToggleWeaponScope — up: 11 @0x4df39c,
// 13 @0x4df3b5, 15 @0x4df3d7 (stricmp "WPN_DESIGNATOR" @0x4df3cb); down: 12
// @0x4df241, 14 @0x4df25a, 16 @0x4df27b].
void raise_scope_tips(World &world, const LocalPlayerWeapon &w, bool engaged) {
    const uint32_t flags = w.def.flags;
    constexpr uint32_t kElevationSight = DEF_WEAPON_FLAG_SCOPED | DEF_WEAPON_FLAG_SHOWELEVATION;
    std::vector<uint8_t> &out = world.out.tip_events;
    if ((flags & kElevationSight) == kElevationSight)
        out.push_back(static_cast<uint8_t>(engaged ? hud::kTipEventScopeElevationOn
                                                   : hud::kTipEventScopeElevationOff));
    if ((flags & DEF_WEAPON_FLAG_USEDESIGNATOR) != 0)
        out.push_back(static_cast<uint8_t>(engaged ? hud::kTipEventDesignatorWeaponOn
                                                   : hud::kTipEventDesignatorWeaponOff));
    if (strutil::iequals(w.def_name, "WPN_DESIGNATOR"))
        out.push_back(static_cast<uint8_t>(engaged ? hud::kTipEventDesignatorOn
                                                   : hud::kTipEventDesignatorOff));
}

} // namespace

namespace {

// One toggle leg with its fov write and its tips, whatever the target.
// [orig: Player_ToggleWeaponScope @0x4df185..0x4df282 / @0x4df2a2..0x4df3de,
//  the Sighted fov @0x4df3ea..0x4df430]
void run_scope_leg(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                   WeaponSlotState &slot, bool engaged) {
    player_view_run_scope_leg(v, engaged, (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0);
    auto &target = world.weather.core.scalar_channels.camera_fov_target_fp;
    if (!engaged) target = 80 << 16; // [orig: @0x4DF218]
    if ((w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0)
        target = engaged && v.camera_mode == 0
            ? sighted_fov_target(local_player_scope_zoom(w, slot)) : 80 << 16;
    raise_scope_tips(world, w, engaged);
}

} // namespace

bool local_player_set_scope(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                            WeaponSlotState &slot, bool engaged) {
    if (!player_view_scope_request_pending(v, engaged)) return true;
    if (player_view_scope_ease_active(v)) return false; // [orig: @0x4df177]
    run_scope_leg(world, w, v, slot, engaged);
    return true;
}

namespace {

// Player_ToggleWeaponScope itself: its entry gates, then the leg the PROMOTED
// byte picks. The input toggle reaches it behind its own currentAction gate,
// the forced callers (the local death, the camera switch) without one.
// [orig: Player_ToggleWeaponScope @0x4df0c0]
bool toggle_weapon_scope(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                         WeaponSlotState &active_slot) {
    // The local player and its equipped slot's def [orig: @0x4df0cf,
    //  @0x4df0eb..0x4df0f6].
    const Entity *player = world.registry.get(world.cached.local_player);
    if (player == nullptr || !w.active) return false;
    // An airborne or swimming (the deep-water 0x8000) body refuses either way
    // [orig: (Flags & 0xA000) == 0 @0x4df0dc]. The airborne bit shares retail
    // storage with the motor's mirror, which may not be published back yet
    // this tick.
    const AiEntity *body = world.ai.for_handle(player->handle);
    const uint32_t flags = player->flags | player->engine_flags;
    if ((flags & (kEntityFlagInAir | kEntityFlagDrowning)) != 0 ||
        (body != nullptr && body->inf.airborne))
        return false;
    // The toggle branches on the PROMOTED byte, not the target: a promoted
    // sight disengages, anything else engages [orig: the g_WeaponScopeActive
    // branch @0x4df17f].
    const bool promoted = player_view_scope_settled(v);
    // ForceScoped pins the raised sight: un-scoping is refused once promoted
    // [orig: (flags1 & 0x20000000) == 0 || !g_WeaponScopeActive
    //  @0x4df104..0x4df115, the decompiler's @0x4df12d].
    if (promoted && (w.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0) return false;
    // Inset optics neither rise nor drop under NVG; the NVG toggle drops and
    // restores them around its own switch [orig: (flags2 & 0x200) == 0 ||
    //  !g_NVGActive @0x4df11b..0x4df12d].
    if (v.nvg_active && (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0) return false;
    // A vehicle control seat (parentSlot 2 or 5) refuses [orig: @0x4df133..0x4df145].
    if (player->mounted && is_vehicle_control_seat(player->mount_type)) return false;
    // A Scoped or Sighted def, and every toggle is refused while the previous
    // ease runs [orig: (flags & 3) && !g_FpCameraInterp.activeFlag @0x4df177].
    if ((w.def.flags & 3) == 0 || player_view_scope_ease_active(v)) return false;
    // Scope-UP is refused while a movement key is held on a Scoped weapon
    // [orig: the engage branch's g_MovementKeyHeld && (flags & 1) -> return @0x4df29c].
    if (!promoted && player_view_scope_up_blocked(v, w.def.flags)) return false;
    // The leg the promoted byte picks runs whatever the target: a camera reset
    // clears the target and keeps the promoted byte, and the disengage leg
    // still lowers that sight. It latches this ease's step count (7 for Inset
    // weapons, else 15; 1 on the hipfire-return leg) [orig: the branch
    // @0x4df17f, no early-out before Setup @0x4df1b3..0x4df36e].
    run_scope_leg(world, w, v, active_slot, !promoted);
    // The engage leg forces the promoted byte to 1 around its seat-flag
    // queries, so an OnlyScoped AbsorbPitch weapon levels the body pitch as
    // the sight comes up; the tube elevation then rides the offset alone.
    // [orig: g_WeaponScopeActive = 1 @0x4DF2A2; AbsorbPitch query @0x4DF302
    //  -> Pitch = 0 @0x4DF314; g_WeaponScopeActive = 0 @0x4DF31D]
    if (!promoted && (w.def.flags & DEF_WEAPON_FLAG_ABSORBPITCH) != 0)
        local_player_level_pitch(world);
    if (!promoted)
        weapon_fsm_queue_scope_up(active_slot);
    else
        weapon_fsm_queue_scope_down(active_slot);
    return true;
}

// Player_IsEquippedWeaponScoped: a promoted sight on a Scoped def
// [orig: @0x4dcc80 -- EquippedSlot and its Def @0x4dcc85..0x4dcc94, Def+8 & 1
//  @0x4dcc99, g_WeaponScopeActive @0x4dcca5].
bool equipped_weapon_scoped(const LocalPlayerWeapon &w, const PlayerViewState &v) {
    return w.active && (w.def.flags & DEF_WEAPON_FLAG_SCOPED) != 0 &&
           player_view_scope_settled(v);
}

} // namespace

bool local_player_scope_toggle(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                               WeaponSlotState &active_slot) {
    if (!w.active) return false;
    // Input case 6's own gate: currentAction not in {RELOAD, SWITCHFROM}
    // [orig: Input_HandleActionBinding_0 @0x4e052b..0x4e0537, the toggle call
    //  @0x4e053d].
    if (!weapon_fsm_scope_toggle_allowed(w.def, active_slot)) return false;
    return toggle_weapon_scope(world, w, v, active_slot);
}

bool local_player_forced_scope_toggle(World &world, LocalPlayerWeapon &w, PlayerViewState &v) {
    // Each forced caller toggles only while the sight is promoted, so the
    // toggle takes its disengage leg when its entry gates pass.
    if (!player_view_scope_settled(v)) return false;
    WeaponSlotState *active_slot = active_local_weapon_slot(world, w);
    return active_slot != nullptr && toggle_weapon_scope(world, w, v, *active_slot);
}

bool local_player_binoculars_toggle(World &world, const LocalPlayerWeapon &w,
                                    PlayerViewState &v, LocalPlayerViewTracker &) {
    const Entity *local = world.registry.get(world.cached.local_player);
    if (local == nullptr) return false;
    // Retail refuses binoculars while a PowerThrow charge is live. Allowing the
    // view to rise would suppress held weapon input and turn the charge into an
    // unintended release [orig: g_FireChargeStartTick @0xB76800; action 26 gate].
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

namespace {
// [orig: Sound_PlayInterfaceTriggerSet @0x527be0 -- the 2D interface play]
void play_nvg_interface_set(World &world, const char *set) {
    ScriptSoundEvent sound;
    sound.name = set;
    sound.kind = ScriptSoundEvent::Kind::Interface;
    world.out.script_sounds.push_back(std::move(sound));
}
} // namespace

bool local_player_nvg_toggle(World &world, LocalPlayerWeapon &w, PlayerViewState &v) {
    if (world.registry.get(world.cached.local_player) == nullptr) return false;
    WeaponSlotState &active_slot = *active_local_weapon_slot(world, w);
    if (!v.nvg_active) {
        // A promoted Scoped Inset sight runs the toggle itself, NVG still off,
        // and the restore latches whatever the toggle did [orig: case 41 --
        //  the Player_IsEquippedWeaponScoped call @0x4e06b3, the
        //  Player_IsVehicleHasAutoAim call (the def's Inset bit, flags2 0x200,
        //  Player_IsVehicleHasAutoAim @0x4dccb0) @0x4e06bc, the
        //  Player_ToggleWeaponScope call @0x4e06c5, dword_B76554 = 1 @0x4e06ca,
        //  g_NVGActive = 1 @0x4e06d7].
        if (equipped_weapon_scoped(w, v) && (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0) {
            toggle_weapon_scope(world, w, v, active_slot);
            w.nvg_scope_restore = true;
        }
        const bool on = player_view_toggle_nvg(v);
        // NV_ON after the scope drop [orig: case 41's on branch — `mov edx,
        // g_SndNvOn` @0x4e06d0, Sound_PlayInterfaceTriggerSet @0x4e06dd].
        play_nvg_interface_set(world, kNvgOnSoundset);
        // The NVG tip, once [orig: CTipSystem_HandleEvent(7) @0x4e06ec, after
        // the scope drop and the sound].
        world.out.tip_events.push_back(static_cast<uint8_t>(hud::kTipEventNvgOn));
        return on;
    }
    // NVG clears first, so the toggle's Inset refusal no longer applies; then
    // a latched restore runs the toggle whatever the sight's state, and the
    // latch clears [orig: g_NVGActive = 0 @0x4e067e; the latch test @0x4e0678 /
    // @0x4e0684; the Player_ToggleWeaponScope call @0x4e0686; dword_B76554 = 0
    // @0x4e068b].
    player_view_toggle_nvg(v);
    if (w.nvg_scope_restore) {
        toggle_weapon_scope(world, w, v, active_slot);
        w.nvg_scope_restore = false;
    }
    // NV_OFF after the scope restore [orig: case 41's off branch — `mov ecx,
    // g_SndNvOff` @0x4e0691, Sound_PlayInterfaceTriggerSet @0x4e0698].
    play_nvg_interface_set(world, kNvgOffSoundset);
    // The off branch fades it [orig: CTipSystem_HandleEvent(8) @0x4e06a7, after
    // the scope restore and the sound].
    world.out.tip_events.push_back(static_cast<uint8_t>(hud::kTipEventNvgOff));
    return false;
}

void local_player_view_tick(World *world, PlayerViewState &v,
                            LocalPlayerViewTracker &t, const LocalViewSessionInputs &s,
                            LocalPlayerWeapon *weapon) {
	t.hud_hit_feedback_frames = s.hud_hit_feedback_frames;
	t.hud_service = s.hud_service;
	t.hud_designations = s.hud_designations;
    if (world == nullptr || !world->cached.local_player.valid()) {
        // No seat without a player: the arbiter resolves to first person (or
        // the debug override) before the effective modes read the mode.
        v.mount = MountedCameraInput();
        player_view_resolve_mode(v);
        if (world != nullptr)
            world->cached.sound_listener_view_flags = audio::listener_view_flags_for_camera(v.camera_mode);
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
    // aircraft eye drop [orig: Camera_ComputeThirdPersonView @0x437D10 --
    // the +0x168 seat test, parentEntity +0x16C, boundRadius +0, the unitType
    // +0x196 in {3,4} test @0x43861D..0x43864C; see player_view.h]. The same
    // seat test is the arbiter's [orig: Render_ProcessMainSceneFrame
    // @0x5ca1e2..0x5ca1f2], so the read precedes the mode resolve and the
    // effective-mode refresh below.
    MountedCameraInput mount;
    // Every chase eye clears the water plane [orig: @0x438409..0x43841E].
    mount.water_z = static_cast<float>(world->env.water_z) / 65536.0f;
    const Entity *carrier = e->mounted ? world->registry.get(e->mount_target) : nullptr;
    if (carrier != nullptr && is_vehicle_control_seat(e->mount_type)) {
        int32_t pitch_bam = 0, roll_bam = 0;
        carrier_pose_fixed(*carrier, mount.carrier_pos_q16, mount.carrier_yaw_bam, pitch_bam,
                           roll_bam);
        mount.control_seat = true;
        // The look-ahead target: the carrier's own orientation matrix (the
        // mover rebuilds it from the entity euler at its tail) times (6.0, 0,
        // 0), rotation only, so a pitched hull tilts it too [orig:
        // Math_TransformPointFixedPoint22 @0x412E90 over parentMatrix(+0xB4),
        // called @0x438855].
        const int32_t zero[3] = {0, 0, 0};
        const int32_t ahead[3] = {to_fixed(kMountLookaheadDistance), 0, 0};
        collision_matrix_from_euler(mount.carrier_yaw_bam, pitch_bam, roll_bam, zero)
                .rotate_point(ahead, mount.lookahead_target_q16);
        mount.bound_radius = carrier->bound_radius;
        mount.aircraft = vehicle_unit_type_is_aircraft(carrier->item_unit_type);
    }
    v.mount = mount;
    // The remaining arbiter inputs [orig: Render_ProcessMainSceneFrame
    // @0x5ca1f4..0x5ca24b; see player_view.h]. The two g_RulesFlags bits are
    // admin `set` commands with no wire fold yet: carried false.
    v.local_dead = s.local_dead;
    v.death_screen_active = s.death_screen_active;
    v.death_screen_submode = s.death_screen_submode;
    v.round_ended = world->match.outcome().ended || s.end_round_known;
    v.end_round_winner_team = s.end_round_winner_team;
    v.on_foot = !e->mounted;
    v.in_session = s.in_session;
    v.view_tick = world->logic_tick;
    // The death stamp (retail: g_CameraLerpStartTick = current_tick on the
    // local death path @0x4b4d00 / the 0x13 self record @0x42ec0f): the local
    // dead EDGE. The same edge forces the scope toggle, below.
    const bool death_edge = v.local_dead && !t.camera_local_dead_seen;
    if (death_edge) v.death_cam.start_tick = world->logic_tick;
    t.camera_local_dead_seen = v.local_dead;
    const int mode_before = v.camera_mode;
    player_view_resolve_mode(v);
    // The arbiter's tracked entity: the spectate target while the death
    // screen's first-person sub-mode has one, else the local player
    // [orig: Camera_SetTrackedEntity @0x4391e0..0x4391f9, called
    //  @0x5ca262 when it or the mode changed @0x5ca250..0x5ca25e]. The
    // compose itself stays on the local entity, whose pose copies the
    // target's (docs/net/novaworld-net-re.md, D-NET-330).
    const bool tracks_target = s.death_screen_active && s.death_screen_submode == 2 &&
            s.spectate_target_key != 0;
    const uint32_t tracked = tracks_target ? s.spectate_target_key : 0u;
    const bool tracked_dead = tracks_target ? s.spectate_target_dead : v.local_dead;
    // A call into any mode but first person forces the scope toggle (the
    // tracked entity is the target by then), below
    // [orig: Camera_SetTrackedEntity @0x439248 (mode 0 skips), the test
    //  @0x4392a1..0x4392ac, Player_ToggleWeaponScope @0x4392ae].
    const bool camera_switch = (tracked != v.camera_tracked || v.camera_mode != mode_before) &&
            v.camera_mode != 0;
    player_view_track_entity(v, tracked, v.camera_mode != mode_before, tracked_dead);
    if (v.camera_mode == 4 && mode_before != 4) enter_death_camera(*world, *e, v, s);
    local_player_view_refresh(world, v);
    // The binocular tip on the raw toggle's edges: a toggle that went up with
    // the view up raises the range tip, any other change fades
    // [orig: Player_UpdatePerFrame @0x4de3e9..0x4de41a — `cmp edx,
    //  dword_B79438`, 9 when both the toggle (al) and the view byte (cl) are
    //  set @0x4de3f4..0x4de3fc, else 10 @0x4de400; the store @0x4de41a].
    if (!t.binocular_tip_seeded) {
        t.binocular_tip_seeded = true;
        t.binocular_tip_prev = v.binoculars_requested;
    }
    if (v.binoculars_requested != t.binocular_tip_prev)
        world->out.tip_events.push_back(static_cast<uint8_t>(
                v.binoculars_requested && v.binoculars_view_active ? hud::kTipEventBinocularsOn
                                                                   : hud::kTipEventBinocularsOff));
    t.binocular_tip_prev = v.binoculars_requested;
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
    // The same update's orbit legs, whatever the mode [orig:
    // ThirdPersonCamera_Update @0x437c1b..0x437d02].
    player_view_chase_tick(v, world->script.input_action_bits, tracked_dead);
    player_view_tick(v, eye);
    // The forced scope toggles see this tick's settle promoter, as retail's
    // do: Player_UpdatePerFrame steps the interp and promotes the sight first
    // [orig: Client_ProcessNetworkFrame @0x42c18e; the promoter @0x4de4c7..0x4de4f7],
    // ahead of the receive dispatch that runs the 0x13 death [orig:
    // CNapiNetwork_PumpClientProtocolRecv @0x42c228 -> Player_ToggleWeaponScope
    // @0x42ec15..0x42ec19], the body's death [orig: Entity_UpdateAllEntities
    // @0x52674b, after Client_ProcessNetworkFrame @0x526692; @0x4b4d1d..0x4b4d25]
    // and the render's camera switch. Each toggles only a promoted sight.
    if (weapon != nullptr) {
        if (death_edge) local_player_forced_scope_toggle(*world, *weapon, v);
        if (camera_switch) local_player_forced_scope_toggle(*world, *weapon, v);
    }
}

bool local_view_draws_virtual_display(const World &world, const PlayerViewState &v,
		const Entity &vehicle) {
    const VehicleTraits *traits = world.vehicles.traits.get(vehicle.item_id);
    return traits != nullptr && traits->render_family == VehicleRenderFamily::Tank &&
            world.cached.local_player.valid() &&
            vehicle.primary_occupant == world.cached.local_player && v.camera_mode == 0;
}

bool local_player_seat_bone_pose(World &world, const Entity &rider, int32_t out[6]) {
    if (!rider.mounted) return false;
    const Entity *parent = world.registry.get(rider.mount_target);
    if (parent == nullptr) return false;
    const AiEntity *body = world.ai.for_handle(rider.handle);
    const bool eweap = parent->has_item_def && (parent->item_attrib & kItemAttribEweap) != 0;
    const bool posable = eweap && world.collision != nullptr &&
            world.collision->entity_model_id(parent->handle) >= 0;
    if (parent->has_item_def && !posable) {
        // Not an EWEAP, or an EWEAP with no model to pose: the rider's own
        // Position + CameraOffset and its full-width rotation triple (the
        // seat carry's BAM32 Roll, never the whole-degree registry mirror).
        // [orig: not an EWEAP @0x545EB7..0x545EEA; the no-skeleton copy
        //  @0x545F17..0x545F4E]
        out[0] = io::bam_add(body ? body->pos[0] : to_fixed(rider.position.x),
                body ? body->inf.eye_offset_x : rider.eye_offset_x);
        out[1] = io::bam_add(body ? body->pos[1] : to_fixed(rider.position.y),
                body ? body->inf.eye_offset_y : rider.eye_offset_y);
        out[2] = io::bam_add(body ? body->pos[2] : to_fixed(rider.position.z),
                body ? body->inf.eye_offset_z : rider.eye_offset_z);
        out[3] = body ? body->heading : bam_heading_from_mission_yaw_deg(rider.yaw);
        out[4] = body ? body->pitch : bam_from_degrees_wrapped(rider.pitch);
        out[5] = body ? body->roll : bam_from_degrees_wrapped(rider.roll);
        // Only the EWEAP copy then adds the rider's +0x94 word (the body
        // attitude triple's roll slot, saved_live_roll) to the pitch
        // [orig: mov edx, [edi+94h]; add [eax+10h], edx @0x545F48..0x545F4E].
        if (eweap) out[4] = io::bam_add(out[4], rider.saved_live_roll);
        return true;
    }
    // [orig: CAMERA byte parent+0x318 @0x545F5B; the posed record and its
    //  part euler @0x545F69..0x546098; no byte (or no def) -> the gun's raw
    //  pose @0x5460AD..0x5460CC]
    if (parent->has_item_def && parent->camera_userpoint_byte != 0 &&
            world.pose_provider != nullptr &&
            world.pose_provider->resolve_userpoint_transform(
                    world, parent->handle, parent->camera_userpoint_byte, out))
        return true;
    out[0] = to_fixed(parent->position.x);
    out[1] = to_fixed(parent->position.y);
    out[2] = to_fixed(parent->position.z);
    out[3] = emplaced_gun_frame_heading(*parent);
    out[4] = emplaced_gun_frame_pitch(*parent);
    out[5] = parent->veh.yaw_seeded ? parent->veh.air_roll_bam
            : bam_from_degrees_wrapped(parent->roll);
    return true;
}

bool local_player_mounted_camera(World &world, const Entity &rider, int32_t out[6]) {
    if (!rider.mounted) return false;
    const Entity *parent = world.registry.get(rider.mount_target);
    if (parent == nullptr || !parent->has_item_def) return false;
    // Mode 0 opens with the rider's own Position and rotation triple; the
    // callbacks rewrite what they own. The Roll word is the seat carry's
    // full BAM32 roll, never the whole-degree registry mirror [orig:
    // @0x437D6F..0x437D9B, Roll `mov ecx, [esi+18h]` @0x437D86; the carry
    // writes it through Entity_AttachToBoneAndUpdateTransform @0x5463D0 ->
    // Math_FixedPointMatrixToEulerAngles @0x54656F].
    const AiEntity *body = world.ai.for_handle(rider.handle);
    out[0] = body ? body->pos[0] : to_fixed(rider.position.x);
    out[1] = body ? body->pos[1] : to_fixed(rider.position.y);
    out[2] = body ? body->pos[2] : to_fixed(rider.position.z);
    out[3] = body ? body->heading : bam_heading_from_mission_yaw_deg(rider.yaw);
    out[4] = body ? body->pitch : bam_from_degrees_wrapped(rider.pitch);
    out[5] = body ? body->roll : bam_from_degrees_wrapped(rider.roll);
    if (parent->virtual_display_camera) {
        // Every input row carries a camera callback, so def+0x174 never fails
        // the gate; def+0x1C0 is the virtual-display userpoint byte.
        // [orig: gates @0x437DC7 / @0x437DD3; vehicle arm @0x437DDF..0x437E2E;
        //  ground-entity arm @0x437E38..0x437E57 (slot flag +0x4D2 & 8);
        //  parent arm @0x437E61..0x437E72]
        const Entity *carrier = parent;
        if (parent->item_type != 1) {
            const Entity *ground = world.registry.get(parent->ground_target);
            if (ground != nullptr && ground->has_item_def &&
                    ground->primary_weapon_slot.redirect_to_parent_slot)
                carrier = ground;
        }
        if (carrier->input_class == 2) {
            // [orig: tank camera callback @0x44A190 -- record position through
            //  the carrier matrix @0x44A22D..0x44A24C, the view-rotation matrix
            //  @0x44A272, the (-0x3000, 0, 0) pull-back @0x44A27E..0x44A292]
            if (!carrier->virtual_display_camera) return false;
            int32_t eye[3];
            entity_placement_matrix(*carrier).transform_point(
                    carrier->virtual_display_camera_q16, eye);
            const int32_t back[3] = {-0x3000, 0, 0};
            collision_matrix_from_euler(out[3], out[4], out[5], eye).transform_point(back, out);
            return true;
        }
        // [orig: null / troop camera callback @0x4DC710 -- Position +
        //  CameraOffset (+0x6C) and the carrier's own rotation triple, all
        //  three full-width words @0x4DC732..0x4DC741]
        const AiEntity *carrier_body = world.ai.for_handle(carrier->handle);
        out[0] = io::bam_add(to_fixed(carrier->position.x), carrier->eye_offset_x);
        out[1] = io::bam_add(to_fixed(carrier->position.y), carrier->eye_offset_y);
        out[2] = io::bam_add(to_fixed(carrier->position.z), carrier->eye_offset_z);
        out[3] = carrier_body ? carrier_body->heading
                : carrier->veh.yaw_seeded ? carrier->veh.yaw_bam
                                          : bam_heading_from_mission_yaw_deg(carrier->yaw);
        out[4] = carrier_body ? carrier_body->pitch
                : carrier->veh.yaw_seeded ? carrier->veh.air_pitch_bam
                                          : bam_from_degrees_wrapped(carrier->pitch);
        out[5] = carrier_body ? carrier_body->roll
                : carrier->veh.yaw_seeded ? carrier->veh.air_roll_bam
                                          : bam_from_degrees_wrapped(carrier->roll);
        return true;
    }
    // The seat-bone leg: an EWEAP that is not PlayerControl, a Person rider.
    // [orig: attrib & 0x20 && !(attrib & 0x40) @0x437E7C..0x437E87; the
    //  Person gate @0x437E89; Entity_GetBoneWorldPosition @0x437EA8]
    if ((parent->item_attrib & kItemAttribEweap) == 0 ||
            (parent->item_attrib & kItemAttribPlayerControl) != 0)
        return false;
    if (!rider.has_item_def || rider.item_type != 3) return false;
    return local_player_seat_bone_pose(world, rider, out);
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
    // [orig: Player_IsVehicleHasAutoAim @0x4DCCB0..0x4DCCDA; its crosshair-gate
    //  call @0x592AE5 (the Sighted query Player_IsVehicleGunnerScoped @0x592AFA)]
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

namespace {

// The frame's reads of the current state: the effect states, the optics and
// HUD context, the chase anchor, the FP terms and the virtual-display
// verdict. Nothing here composes the camera.
void fill_view_context(World *world, LocalPlayerWeapon &w, const PlayerViewState &v,
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
    // [orig: g_CameraMode @0xA890C8; g_CameraThirdPersonSelected @0xA860DF].
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
    // The FrameFX dispatch facts: the monitor latch is the thermal latch's
    // sibling on flags2 & 8 (Player_IsHeldWeaponMonitor), and the dispatch
    // reads the RAW red word, the dead bit, the session, the death stamp and
    // g_NVGActive [orig: Render_ProcessMainSceneFrame @0x5ca2e8..0x5ca2f1;
    // @0x5ca8f6..0x5ca92e; @0x5ca9f5..0x5caa62; @0x5ca516..0x5ca554].
    const bool monitor_def = w.active && (w.def.flags2 & DEF_WEAPON_FLAG2_MONITOR) != 0;
    out.frame_fx.in_session = v.in_session;
    out.frame_fx.local_dead = v.local_dead;
    out.frame_fx.red_word = v.flash.red;
    out.frame_fx.camera_mode = v.camera_mode;
    out.frame_fx.death_elapsed_ticks = static_cast<int32_t>(v.view_tick - v.death_cam.start_tick);
    out.frame_fx.thermal_view = out.thermal_view;
    out.frame_fx.monitor_view = optical_view && monitor_def;
    out.frame_fx.nvg_active = v.nvg_active;
    out.frame_fx.death_screen_active = v.death_screen_active;
    const bool sighted = out.scope_card_active &&
                         (w.def.flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
                         active_slot->current != weapon_action::kSwitchFrom;
    const bool scoped = out.scope_card_active &&
                        (w.def.flags & DEF_WEAPON_FLAG_SCOPED) != 0 &&
                        (w.def.flags2 & DEF_WEAPON_FLAG2_INSET) == 0;
    // The NVG arms read the binocular byte and the frame's Scoped byte, which
    // the vehicle-attack context clears [orig: @0x5ca2ff..0x5ca304]; the HUD
    // takes the mask gate and the lens arm from the same planner.
    out.frame_fx.binoculars_view_active = v.binoculars_view_active;
    out.frame_fx.scoped_selector = scoped && !out.vehicle_attack_context;
    out.frame_fx.sighted_selector = sighted && !out.vehicle_attack_context;
    const renderer::FrameFxNvgPlan nvg = renderer::frame_fx_nvg_view(out.frame_fx);
    out.nvg_mask_visible = renderer::frame_fx_nvg_mask_visible(out.frame_fx);
    out.nvg_lens_active = nvg.lens;
    out.nvg_sights_in_scene = nvg.sighted;
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
	// The FP draw's own gates (player_present.h fp_viewmodel_retail_submit):
	// the alive gate reads the local dead bit and the decided winner; the draw
	// skips the showhud test for an Emplaced def and skips the model for a
	// scoped Inset def while CanFire holds [orig: Player_RenderViewModelIfAlive
	// @0x4E0145/@0x4E014B; Player_RenderFirstPersonViewModel @0x4DEDD9..0x4DEDF1
	// and @0x4DEDF7..0x4DEE19].
	out.fp_local_dead = v.local_dead;
	out.fp_round_winner_set = v.end_round_winner_team != 0;
	out.fp_def_emplaced = w.active && (w.def.flags & DEF_WEAPON_FLAG_EMPLACED) != 0;
	out.fp_inset_scoped = optical_view && out.scope_details_scoped &&
			(w.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0;
	out.inset_fov_over_zoom = out.inset_scope_active
			? float(current_fov) / 65536.0f / local_player_scope_zoom(w, *active_slot)
			: 0;
    out.aim_range_q16 = w.aim_range_q16;
	if (world)
		fill_hud_combat_view(*world, w, out, t, optical_view);
	out.hud_combat.state.death_screen = v.death_screen_active;
    out.tp_anchor[0] = v.tp_anchor[0];
    out.tp_anchor[1] = v.tp_anchor[1];
    out.tp_anchor[2] = v.tp_anchor[2];
    out.tp_anchor_valid = v.tp_anchor_valid;
    // The FP components of the composed camera, exported separately for
    // diagnostics/probes; authoritative look pitch never inherits the
    // camera-only doubling. [orig: recoil @0x437fc7, roll @0x437fe6]
    if (world == nullptr || !world->cached.local_player.valid()) return;
    const AiEntity *p = world->ai.for_handle(world->cached.local_player);
    if (p == nullptr) return;
    const Entity *e = world->registry.get(world->cached.local_player);
    out.fp_terms_valid = true;
    out.fp_pitch_recoil_deg = player_view_fp_pitch_recoil_deg(p->inf.recoil_pitch);
    out.fp_roll_deg = player_view_fp_roll_deg(p->inf.torso_roll, p->inf.lean_angle);
    if (e == nullptr) return;
    if (const Entity *vehicle = world->registry.get(e->mount_target)) {
        if (local_view_draws_virtual_display(*world, v, *vehicle)) {
            out.virtual_display_active = true;
            out.virtual_display_carrier = vehicle->handle;
            out.virtual_display_model = vehicle->virtual_display_model;
        }
    }
}

// THE GROUND-ENTITY LEG: a person whose groundEntity is a vehicle (def type
// 1) in its crashed or settled latch sets the eye along that vehicle's up
// axis, the longest CameraOffset seen as the lift, under its own rotation
// triple — neither the recoil doubling nor the lean roll nor the pull-back.
// [orig: Camera_ComputeThirdPersonView @0x437EB5..0x437F97 — groundEntity
//  +0x28 @0x437EB5, Entity_HasActiveParent @0x402BB0 via @0x437EC2, the
//  +0x2EC / +0x2F0 bytes @0x437ECF..0x437EDF]
bool compose_ground_leg(World &world, const AiEntity &p, const Entity &e,
                        PlayerViewState &v, PlayerCameraPose &out) {
    const Entity *ground = world.registry.get(e.ground_target);
    if (ground == nullptr || !ground->has_item_def || ground->item_type != 1 ||
            (ground->veh.crashed == 0 && ground->veh.settle_2f0 == 0))
        return false;
    // |CameraOffset| through the x87 (x² + y²) + z², fsqrt, ftol, kept as a
    // running maximum [orig: @0x437EE5..0x437F0B].
    const double ox = e.eye_offset_x;
    const double oy = e.eye_offset_y;
    const double oz = e.eye_offset_z;
    const int32_t length = static_cast<int32_t>(std::sqrt(ox * ox + oy * oy + oz * oz));
    if (length > v.ground_leg_lift_q16) v.ground_leg_lift_q16 = length;
    // The ground's up axis: its euler matrix's third column, >> 6 to 16.16
    // [orig: Math_BuildFixedPointMatrixFromEulerAngles via @0x437F1C,
    //  Math_ExtractRow2FromFixedPoint22 @0x6137A0 via @0x437F2B].
    int32_t ground_pos[3];
    int32_t ground_yaw = 0, ground_pitch = 0, ground_roll = 0;
    carrier_pose_fixed(*ground, ground_pos, ground_yaw, ground_pitch, ground_roll);
    const CollisionMatrix frame =
            collision_matrix_from_euler(ground_yaw, ground_pitch, ground_roll, ground_pos);
    const int32_t up[3] = {frame.m[2] >> 6, frame.m[6] >> 6, frame.m[10] >> 6};
    // Position plus the up axis scaled by the lift, each product rounded
    // [orig: @0x437F33..0x437F91].
    for (int i = 0; i < 3; ++i) {
        const int32_t lift = static_cast<int32_t>(
                (static_cast<int64_t>(up[i]) * v.ground_leg_lift_q16 + 0x8000) >> 16);
        out.eye[i] = static_cast<float>(from_fixed(io::bam_add(p.pos[i], lift)));
    }
    // The rotation stays the entity's own triple [orig: @0x437D86..0x437D9B].
    out.yaw_deg = static_cast<float>(mission_yaw_deg_from_bam_heading(p.heading));
    out.pitch_deg = static_cast<float>(static_cast<double>(p.pitch) * kDegreesPerBam);
    out.roll_deg = static_cast<float>(static_cast<double>(p.roll) * kDegreesPerBam);
    out.third_person = false;
    return true;
}

// The Inset scene's own slot offsets on its composed camera.
// [orig: Render_WeaponInsetScene @0x5C9841..0x5C9903]
void apply_inset_slot_offsets(World &world, LocalPlayerWeapon &w, LocalPlayerViewFrame &out) {
    const WeaponSlotState *slot = active_local_weapon_slot(world, w);
    constexpr double degrees_per_bam = 360.0 / 4294967296.0;
    out.inset_camera.yaw_deg -= float(slot->zero_yaw * degrees_per_bam);
    out.inset_camera.pitch_deg -= float(slot->zero_pitch * degrees_per_bam);
}

} // namespace

bool local_player_camera_compose(World &world, PlayerViewState &v, LocalPlayerViewTracker &t,
                                 PlayerCameraPose &out, bool &mounted_camera) {
    out = PlayerCameraPose();
    mounted_camera = false;
    const AiEntity *p = world.cached.local_player.valid()
            ? world.ai.for_handle(world.cached.local_player) : nullptr;
    const Entity *e = p != nullptr ? world.registry.get(world.cached.local_player) : nullptr;
    if (p == nullptr || e == nullptr) {
        // Nothing to compose over: the view words zero [orig: the tracked
        // entity / local player tests @0x437D42 / @0x437D4E ->
        //  @0x438B4A..0x438B68].
        t.composed = PlayerCameraPose();
        t.composed_valid = false;
        t.composed_mounted = false;
        return false;
    }
    // Compose from the body aim. The rendered view adds optical offsets
    // afterwards (LocalPlayer::present_view_frame).
    const float aim_yaw = static_cast<float>(mission_yaw_deg_from_bam_heading(p->heading));
    const float aim_pitch = static_cast<float>(static_cast<double>(p->pitch) * kDegreesPerBam);
    int32_t mounted[6];
    if (v.camera_mode == 0 && e->mounted && local_player_mounted_camera(world, *e, mounted)) {
        // A seated first-person view belongs to the carrier: its
        // virtual-display camera callback, or the gun's posed CAMERA
        // userpoint [orig: Camera_ComputeThirdPersonView mode 0
        //  @0x437DAC..0x437EAD].
        out.eye[0] = static_cast<float>(from_fixed(mounted[0]));
        out.eye[1] = static_cast<float>(from_fixed(mounted[1]));
        out.eye[2] = static_cast<float>(from_fixed(mounted[2]));
        out.yaw_deg = static_cast<float>(mission_yaw_deg_from_bam_heading(mounted[3]));
        out.pitch_deg = static_cast<float>(static_cast<double>(mounted[4]) * kDegreesPerBam);
        out.roll_deg = static_cast<float>(static_cast<double>(mounted[5]) * kDegreesPerBam);
        mounted_camera = true;
    } else if (!(v.camera_mode == 0 && p->inf.active && compose_ground_leg(world, *p, *e, v, out))) {
        const float position[3] = {e->position.x, e->position.y, e->position.z};
        // CameraOffset already includes the current motor's posed head and
        // its on-foot terrain floor. Re-anchor it after the motor's
        // translation. [orig: Camera_ComputeThirdPersonView @0x437FA5..0x437FB7]
        const Vec3 current_eye = player_eye_position(*e);
        const float anchor_eye[3] = {current_eye.x, current_eye.y, current_eye.z};
        // The chase march runs only with proximity candidates at hand
        // [orig: the entity +0x1C0 count @0x4381CB].
        const bool march_candidates =
                world.collision != nullptr && world.collision->candidate_count(e->handle) > 0;
        player_view_compose_camera(v, position, anchor_eye, p->inf.active, world.ai.terrain,
                                   (e->flags & kEntityFlagIndoors) != 0, aim_yaw, aim_pitch,
                                   p->inf.recoil_pitch, p->inf.torso_roll, p->inf.lean_angle,
                                   march_candidates,
                                   static_cast<float>(static_cast<double>(p->roll) * kDegreesPerBam),
                                   out);
    }
    // This call's shake. First person advances the three IIR filters from the
    // current weather PRNG word (unchanged between ticks) [orig: the mode-0
    // block @0x43803C..0x4380DF, the >> 6 applies @0x4380B0..0x4380D9]; the
    // chase applies the STATELESS sin/cos chain over the raw counter and the
    // engine tick [orig: the mode-1 block @0x438939..0x4389E5]; the mode-4
    // lerp takes neither. Both add to the BAM HEADING (`add g_ViewRotYaw`
    // @0x4380D9 / @0x43898B), so the mission yaw (90 - heading) takes the
    // negated delta.
    int32_t d_yaw = 0;
    int32_t d_pitch = 0;
    int32_t d_roll = 0;
    if (v.camera_mode == 0) {
        camera_shake_sample(v.shake, world.weather.core.oscillator.prng, d_yaw, d_pitch, d_roll);
    } else if (v.camera_mode == 1) {
        camera_shake_sample_chase(v.shake, world.weather.core.oscillator.prng, world.logic_tick,
                                  d_yaw, d_pitch, d_roll);
    }
    constexpr float kDegPerBam = 360.0f / 4294967296.0f;
    out.yaw_deg -= static_cast<float>(d_yaw) * kDegPerBam;
    out.pitch_deg += static_cast<float>(d_pitch) * kDegPerBam;
    out.roll_deg += static_cast<float>(d_roll) * kDegPerBam;
    // The call leaves its view in g_view_pos / g_view_rot for every reader
    // until the next call.
    t.composed = out;
    t.composed_valid = true;
    t.composed_mounted = mounted_camera;
    return true;
}

void local_player_view_frame(World *world, LocalPlayerWeapon &w, PlayerViewState &v,
                             LocalPlayerViewTracker &t, LocalPlayerViewFrame &out) {
    fill_view_context(world, w, v, t, out);
    if (world == nullptr) return;
    // The main scene composes its camera once per rendered frame
    // [orig: Render_ProcessMainSceneFrame @0x5CA34D].
    if (!local_player_camera_compose(*world, v, t, out.camera, out.mounted_camera)) return;
    out.inset_camera = out.camera;
    if (out.inset_scope_active) {
        // The Inset scene composes once more — a second shake step — then
        // takes its own slot offsets [orig: Render_WeaponInsetScene
        //  @0x5C9841].
        bool inset_mounted = false;
        local_player_camera_compose(*world, v, t, out.inset_camera, inset_mounted);
        apply_inset_slot_offsets(*world, w, out);
    }
    out.camera_pose_valid = true;
}

void local_player_view_observe(World *world, LocalPlayerWeapon &w, const PlayerViewState &v,
                               LocalPlayerViewTracker &t, LocalPlayerViewFrame &out) {
    fill_view_context(world, w, v, t, out);
    if (world == nullptr || !t.composed_valid) return;
    // Between composes the view is what the last one left behind.
    out.camera = t.composed;
    out.mounted_camera = t.composed_mounted;
    out.inset_camera = out.camera;
    if (out.inset_scope_active) apply_inset_slot_offsets(*world, w, out);
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

int local_player_health(const World &world) {
    if (!world.cached.local_player.valid()) return 0;
    const Entity *e = world.registry.get(world.cached.local_player);
    return e ? e->health : 0;
}

int local_player_max_health(const World &world) {
    if (!world.cached.local_player.valid()) return 100;
    const Entity *e = world.registry.get(world.cached.local_player);
    return e != nullptr ? max_health_with_difficulty(world, *e) : 100;
}

} // namespace opennova::world
