// The LOCAL PLAYER's view cluster, orchestrated: the view-effect requests
// (scope / binoculars / NVG), the per-tick view promoter, the shell-fed eye,
// and the composed per-frame view read. player_view.h owns the witnessed
// primitives (the ease, the arbiter, the camera composition); this module
// owns the ORDER retail runs them in and the gates in front of them, so the
// presenting shell only converts frames and routes wire requests.
//
// [orig: Input_HandleActionBinding_0 @0x4e0420 case 6; Player_ToggleWeaponScope
//  @0x4df0c0; the binocular action 26 / NVG action 41 legs; Player_UpdatePerFrame
//  call @0x42c18e; Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b;
//  Camera_SetTrackedEntity @0x439257; ThirdPersonCamera_Update @0x437b70;
//  Player_UpdateFirstPersonCamera @0x4dd380]
#pragma once

#include <cstdint>
#include <functional>

#include <runtime/world/entity.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>

namespace opennova::world {

struct World;

// The per-local-player view trackers the view cluster keeps beside the
// PlayerViewState: the binocular aim displacement, the FP motion-lead sampler
// and its per-tick movement delta, and the local-dead edge the death stamp
// reads.
struct LocalPlayerViewTracker {
    // The binocular toggle seeds one fixed-radius random aim displacement. It
    // survives movement/death/third-person suppression until the raw toggle
    // drops [orig: the binocular-raise offset beside g_binocularsToggle].
    float binocular_yaw_offset_deg = 0.0f;
    float binocular_pitch_offset_deg = 0.0f;
    // The FP viewmodel motion-lead tracker (per render frame) and the local
    // entity's per-62.5 Hz-tick movement delta it samples
    // [orig: the (position - entity+0x80) samples @0x437bb2/0x437b92/0x437ba2].
    PlayerViewMotionLead motion_lead;
    float tick_delta[3] = {0.0f, 0.0f, 0.0f};
    float tick_prev_pos[3] = {0.0f, 0.0f, 0.0f};
    bool tick_prev_valid = false;
    // The death stamp's edge detector [orig: g_camera_lerp_start_tick =
    // current_tick on the local death path @0x4b4d00 / @0x42ec0f].
    bool camera_local_dead_seen = false;
};

// What the camera arbiter reads from the SESSION (the net layer sits above
// this group, so its client state arrives as plain values): the client-local
// death-screen latch and sub-mode, the end-of-round knowledge, whether a live
// session owns the sim and whether this side is the joiner, plus the joiner's
// last received S2C 0x52 death-camera triple (zeros before any, like retail's
// globals). [orig: Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b;
//  NapiNPClientMsg_0x00A @0x42ff88..0x43002b]
struct LocalViewSessionInputs {
    bool in_session = false;
    bool joiner = false;
    bool death_screen_active = false;
    int death_screen_submode = 0;
    bool end_round_known = false;
    bool local_dead = false;
    // The joiner's death-camera triple is only meaningful with a live
    // client runtime; without one the anchor stays on the player.
    bool death_camera_target_known = false;
    int32_t death_camera_target[3] = {0, 0, 0}; // mission 16.16
};

// Reset the view effects for a fresh local player: binoculars down, NVG per
// the mission's StartWithNVGOn attribute, gain at the floor, the scope
// restore latch cleared; then the effective modes are refreshed.
void local_player_view_reset(World *world, LocalPlayerWeapon &w,
                             PlayerViewState &v, LocalPlayerViewTracker &t);

// Recompute the binocular body pose / optical view against the local player's
// life and the round state (player_view_update_effective_modes).
void local_player_view_refresh(World *world, PlayerViewState &v);

// Action 6's FIRST leg: on a designated-G carried EWeap the action toggles the
// selected MountSlot instead of the scope. `applies` = the branch matched a
// valid route; the caller routes the selection (the joiner queues it toward
// the authority, a serving host sends it to its own loopback, a standalone
// world applies it at once through local_player_apply_mount_slot_select).
// [orig: Input_HandleActionBinding_0 @0x4e0420, case 6 @0x4e0492]
struct MountSlotSelectRequest {
    bool applies = false;
    bool use_parent_slot = false;
    EntityHandle mount;
    EntityHandle parent; // valid only on the parent-slot route
};
bool local_player_mount_slot_select(World &world, const LocalPlayerWeapon &w,
                                    MountSlotSelectRequest &out);
// The standalone/tool-world apply of a validated selection: the same
// transition the authoritative compact seat_type 1/2 echo performs.
void local_player_apply_mount_slot_select(World &world, LocalPlayerWeapon &w,
                                          const MountSlotSelectRequest &req);

// The ordinary scope toggle, in the witnessed order: the dispatcher gates
// (no toggle during RELOAD/SWITCHFROM, def flags), the movement-held refusal
// of a Scoped weapon's scope-UP, the Inset-under-NVG refusal, the ForceScoped
// pin, the mid-ease refusal with the per-toggle ease latch, then the FSM's
// scopeup/scopedown queue. Returns whether it toggled.
// [orig: Player_ToggleWeaponScope @0x4df0c0 — @0x4df29c, @0x4df12d,
//  @0x4df177, @0x4df1b3..0x4df36e; WeaponSlot_TryQueueScopeUp @0x53f050 /
//  ..ScopeDown @0x53f080]
bool local_player_scope_toggle(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                               WeaponSlotState &active_slot);

// The one optical visibility query used by body, weapon, HUD and camera paths.
// Its retail FOV target writes are synchronous, even without a weather tick.
bool local_player_scope_view_visible(World &world, LocalPlayerWeapon &w,
                                      const PlayerViewState &v);

// The shared pose/FOV transition after a caller's refusal gates. The weapon
// pump and movement unscope use the same transition as the input toggle.
bool local_player_set_scope(World &world, const LocalPlayerWeapon &w, PlayerViewState &v,
                            WeaponSlotState &slot, bool engaged);
// The current slot zoom, lazily initialized and clamped by the retail getter.
int32_t local_player_scope_zoom(const LocalPlayerWeapon &w, WeaponSlotState &slot);

// Action 26: toggle the persistent binocular request. Refused while a
// PowerThrow charge is live (the raised view would suppress the held weapon
// input and turn the charge into an unintended release) and while a scope is
// engaged in a gunner seat. A raise seeds the aim displacement from
// `unit_random` in [0, 1); dropping the request zeroes it. Returns the new
// requested state (false also = refused). `unit_random` is sampled once, only
// on a raise. [orig: g_fireChargeStartTick @0xB76800; the action 26 gate; the
//  offset seed beside g_binocularsToggle]
bool local_player_binoculars_toggle(World &world, const LocalPlayerWeapon &w,
                                    PlayerViewState &v, LocalPlayerViewTracker &t,
                                    const std::function<float()> &unit_random);

// Action 41: toggle NVG. Raising NVG over a settled Inset scope first drops the
// scope through `scope_toggle` and latches a one-shot restore; clearing NVG
// consumes the latch and re-raises the scope after the Inset refusal no
// longer applies. `scope_toggle` is the full scope request (it may route to
// the wire). Returns the new NVG state.
bool local_player_nvg_toggle(World &world, LocalPlayerWeapon &w,
                             PlayerViewState &v,
                             const std::function<bool()> &scope_toggle);

// One 62.5 Hz tick of the view state, before the weapon pump: the mounted
// carrier read, the arbiter inputs, the mode resolve with the mode-4 lerp
// camera computed on entry, the effective modes, the per-tick movement delta
// the FP motion lead samples, and the anchor chase over the terrain-floored
// eye. Retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call, so this tick's settle promoter is
// visible to action routing while an action's unscope/rescope begins easing
// on the next tick [orig: call sites @0x42c18e / @0x526786; promoter
// @0x4de4f7; Camera_ComputeThirdPersonView @0x437D10; ThirdPersonCamera_Update
// @0x437b70..76; the non-person bump @0x437e8f].
void local_player_view_tick(World *world, const LocalPlayerWeapon &w,
                            PlayerViewState &v, LocalPlayerViewTracker &t,
                            const LocalViewSessionInputs &session);

// The shell-fed head-bone eye (mission space) for the 3P anchor chase; also
// mirrored into the world so the infantry body tick can restamp the local
// eye-offset triple from the exact posed head (the D-HUD-20 local leg).
void local_player_set_eye(World *world, LocalPlayerWeapon &w,
                          const float eye_mission[3], bool valid);
// The posed head as a BODY-RELATIVE delta (head minus the skeleton origin)
// [orig: Entity_UpdateInfantryPlayerBody @0x4b6908 -- the mounted local eye
//  leg stores head - Position from a skeleton posed in the SAME tick].
void local_player_set_eye_offset(World *world, const float offset_mission[3],
                                 bool valid);

// The composed per-frame view read, mission space: the effect states, the
// resolved camera mode and its gates, the NoCardSwitch bias suppression, the
// SIGHTS-card selector, the fov, the chase anchor, and the composed camera
// pose (the shell converts frames and stamps the Camera3D node).
// [orig: g_camera_mode @0xA890C8; Player_UpdateFirstPersonCamera @0x4dd439;
//  Player_IsReloadingCardSwitchWeapon @0x4dcdd0; Render_ProcessMainSceneFrame
//  @0x5ca299..0x5ca304; Camera_ComputeThirdPersonView @0x437d10 — the
//  MOUNTED local eye leg @0x4b6908 re-anchored to the live position]
struct LocalPlayerViewFrame {
    int16_t scope_zero_word = 0;
    int32_t scope_zero_max = 0;
    int32_t scope_zero_step = 0;
    int32_t scope_zero_default = 0;
    int32_t aim_range_q16 = 0;
    bool scope_engaged = false;
    bool binoculars_requested = false;
    bool binoculars_raised = false;
    bool binoculars_view_active = false;
    float binocular_yaw_offset_deg = 0.0f;
    float binocular_pitch_offset_deg = 0.0f;
    bool nvg_active = false;
    bool nvg_visible = false;
    int32_t nvg_gain = 0;
    // The thermal-imaging view of a Thermal-flagged weapon def (flags2 & 4;
    // the IDB's Player_IsVehicleSeatHasFlag4 @0x4dcd70 reads EquippedSlot
    // (+0x118)->Def(+0x20)->flags2(+0x0C) & 4). `thermal_view` is the frame's
    // latched byte -- the CanFire verdict AND the def bit -- that greys the
    // world lighting block and selects the 0x808080 device fog and clear;
    // `thermal_terrain_view` is the def bit in first person alone, the
    // terrain-ramp gate. The only shipped trigger is WPN_EMP50BD (Emplaced +
    // Thermal + ForceScoped), on which the two coincide.
    // [orig: Render_ProcessMainSceneFrame @0x5ca290 (Player_CanFireWeapon)
    //  -> @0x5ca2da..0x5ca2e3 (the latch), read @0x5ca363;
    //  CTerrainRenderer_BuildLightingShaderConstants @0x5c837c..0x5c8389;
    //  Render_TerrainScene @0x610e51..0x610e5b]
    bool thermal_view = false;
    bool thermal_terrain_view = false;
    bool mounted = false;
    bool third_person = false;
    bool third_person_selected = false;
    int camera_mode = 0;
    bool camera_mounted = false;
    bool vehicle_attack_context = false;
    float scope_fraction = 0.0f;
    bool suppress_view_bias = false;
    bool scope_card_active = false;
    float fov_h_deg = 0.0f;
    float tp_anchor[3] = {0.0f, 0.0f, 0.0f}; // mission space
    bool tp_anchor_valid = false;
    // The composed camera + its FP components (valid only with a local AI
    // entity to compose over).
    bool fp_terms_valid = false;
    bool camera_pose_valid = false;
    float fp_pitch_recoil_deg = 0.0f;
    float fp_roll_deg = 0.0f;
    PlayerCameraPose camera;
};
void local_player_view_frame(World *world, LocalPlayerWeapon &w,
                             const PlayerViewState &v, const LocalPlayerViewTracker &t,
                             LocalPlayerViewFrame &out);

// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=forward,
// Y=left, Z=up): the raw weapon.def `pos`/`tpos` blend over the /256 scale with
// the NoCardSwitch reload suppression applied, plus the per-frame motion lead
// and the 4:3 framing drop. Advances the motion-lead tracker.
// [orig: Player_UpdateFirstPersonCamera @0x4dd380 — lead @0x4dd4f2..0x4dd56c,
//  narrow-aspect drop @0x4dd571]
void local_player_viewmodel_bias(World *world, const LocalPlayerWeapon &w,
                                 const PlayerViewState &v, LocalPlayerViewTracker &t,
                                 const float pos_raw_units[3],
                                 const float tpos_raw_units[3],
                                 int viewport_w, int viewport_h, float out[3]);

} // namespace opennova::world
