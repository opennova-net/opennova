// The LOCAL PLAYER's view cluster, orchestrated: the view-effect requests
// (scope / binoculars / NVG), the per-tick view promoter, the simulated eye,
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
#include <string>
#include <vector>

#include <runtime/renderer/frame_fx_effects.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/hud_combat_feed.h>

namespace opennova::world {

struct World;

// The per-local-player view trackers the view cluster keeps beside the
// PlayerViewState: the binocular aim displacement, the FP motion-lead sampler
// and its per-tick movement delta, and the local-dead edge the death stamp
// reads.
struct LocalPlayerViewTracker {
	uint8_t hud_hit_feedback_frames = 0;
	hud::HudServiceState hud_service;
	std::vector<hud::HudDesignationPoint> hud_designations;
	// A failed impact preview resets its point, but retains the last range.
	// [orig: Player_UpdatePerFrame @0x4DE9F2, miss @0x4DEADE..0x4DEB43]
	int32_t hud_impact_distance_q16 = 0;
    // The binocular ACTIVATION seeds one fixed-radius random aim displacement.
    // It survives movement/death/third-person suppression and is re-seeded on
    // the next activation, not on the raw toggle: retail runs the seed from the
    // RENDER frame behind a latch that is set on the first frame the optical
    // view is up and cleared on every frame it is down, so a raise that stays
    // suppressed (moving, dead, round over, third person) never draws
    // [orig: Render_ProcessMainSceneFrame @0x5ca3d3..0x5ca3f8 — the
    //  `if (!dword_29D6BA8) { dword_29D6BA8 = 1; Binoculars_RandomizeSwayOffsets(); }`
    //  arm, the clear @0x5ca4b0; the seeded pair is added to the view angles
    //  @0x5ca3f8..0x5ca407. Binoculars_RandomizeSwayOffsets @ 0x4dd830 writes
    //  the BINOCULAR SWAY pair: eight times truncated Q22 sin(angle)
    //  and cos(angle); the decompiler loses the x87 angle across ftol.
    //  The port stores mission-coordinate degree deltas (yaw sign reversed).]
    float binocular_yaw_offset_deg = 0.0f;
    float binocular_pitch_offset_deg = 0.0f;
    bool binocular_sway_latched = false; // dword_29D6BA8
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
    // The last composed view: retail's g_view_pos / g_view_rot, which every
    // Camera_ComputeThirdPersonView call overwrites (once per logic quantum,
    // once per rendered frame, once more for the Inset scene) and every reader
    // between calls takes as it stands: the aim acquisition's camera leg and
    // the between-frame observations. Zero with no local player to compose
    // over [orig: @0x438B4A..0x438B68].
    PlayerCameraPose composed;
    bool composed_valid = false;
    bool composed_mounted = false;
};

// What the camera arbiter reads from the SESSION (the net layer sits above
// this group, so its client state arrives as plain values): the client-local
// death-screen latch and sub-mode, the end-of-round knowledge, whether a live
// session owns the sim and whether this side is the joiner, plus the joiner's
// last received S2C 0x52 death-camera triple (zeros before any, like retail's
// globals). [orig: Render_ProcessMainSceneFrame @0x5ca1f4..0x5ca24b;
//  NapiNPClientMsg_0x00A @0x42ff88..0x43002b]
struct LocalViewSessionInputs {
	uint8_t hud_hit_feedback_frames = 0;
	hud::HudServiceState hud_service;
	std::vector<hud::HudDesignationPoint> hud_designations;
    bool in_session = false;
    bool joiner = false;
    bool death_screen_active = false;
    int death_screen_submode = 0;
    bool end_round_known = false;
    // The folded S2C 0x1D header's winner team (0 before any header)
    // [orig: NapiNPClientMsg_0x01D @0x430840 -> g_endround_winner_team].
    int32_t end_round_winner_team = 0;
    bool local_dead = false;
    // The joiner's death-camera triple is only meaningful with a live
    // client runtime; without one the anchor stays on the player.
    bool death_camera_target_known = false;
    int32_t death_camera_target[3] = {0, 0, 0}; // mission 16.16
};

// Reset the view effects for a fresh local player: binoculars down, NVG per
// the mission's StartWithNVGOn attribute, gain at the floor, the scope
// restore latch cleared; then the effective modes are refreshed.
// The category-handle / UseGun camera reset, shared by the admitted category
// request, the UseGun attach staging and the session-level view reset: the
// interp reset (player_view_weapon_switch_reset), the equipped-slot rebind
// (iff its def is optical, Flags & 3), the fov target back to 80 degrees, and
// the two binocular clears. `world` may be null (no fov channel to write).
// [orig: Player_ResetCameraAndMovementState @0x4DE1F0: fov @0x4de202, the
//  rebind @0x4de287..0x4de2a7, g_binocularsViewActive = 0 @0x4de2ad,
//  g_binocularsToggle = 0 @0x4de2b3]
void local_player_camera_reset(World *world, const LocalPlayerWeapon &w, PlayerViewState &v);

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
                                          const MountSlotSelectRequest &req, PlayerViewState &v);

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

// --- the scope ZOOM STEP (actions 212/214 on a raised scope, action 215) -------

// The two def facts the zoom writers read beside the equipped weapon's
// scope_max_mag [orig: WeaponDef +0x98 'scope_min_mag' (@0x544f7a; the record
// default 2 @0x53ff73), +0 'category' (3 = Primary)]. The session bit the
// class-6 sniper lock consults is World::rules.allow_sniper_scope_zoom
// (byte_A821F0). Carried by the caller until LocalPlayerWeapon grows the
// scope_min_mag field (WeaponInstallData::scope_min_mag <- DefWeaponDef::
// scope_min_mag); `local_player_scope_zoom_limits` fills the category from
// the weapon table by the equipped def name.
struct ScopeZoomLimits {
    int32_t scope_min_mag = 2;
    int32_t category = 0;
};
ScopeZoomLimits local_player_scope_zoom_limits(const World &world, const LocalPlayerWeapon &w,
                                               int32_t scope_min_mag);

// The zoom FLOOR both writers share: scope_min_mag, or scope_max_mag (the zoom
// locked at max) for a class-6 (sniper) local player on a Primary (category 3)
// def while the session forbids sniper scope zoom
// [orig: Player_AdjustWeaponElevation @0x4dbe29..0x4dbe3f;
//  Player_MountWeaponSlot @0x4dfadd..0x4dfb01].
int32_t local_player_scope_zoom_floor(const World &world, const ScopeZoomLimits &limits,
                                      int32_t scope_max_mag);

// The zoom-step click [orig: Sound_PlayInterfaceTriggerSet(dword_24E08B4)
// @0x4dbe64 -- entry 1 of the @0x82F590 resolver table (@0x82f5b4 -> 0x24e08b4,
// DialogSystem_Init @0x5275e0), the "GF_SCOPE" trigger set]. Raised as an
// Interface ScriptSoundEvent like the scope-zero click.
inline constexpr const char *kScopeZoomStepSoundset = "GF_SCOPE";

// Player_AdjustWeaponElevation @0x4dbdf0, the +/-2 step the weapon-cycle
// actions take instead of a cycle while the optical view is up on a def whose
// scope_min_mag != scope_max_mag (and action 215's own +/-2): gated on the
// CanFire verdict (the optical-view gate) and the equipped def
// (@0x4dbdfc..0x4dbe0e); next = slot zoom + delta; below the floor it is the
// floor, else capped at scope_max_mag (@0x4dbe47..0x4dbe57); a changed value
// clicks (@0x4dbe5b..0x4dbe64) and the slot stores it either way (@0x4dbe6c).
// Returns whether the zoom changed.
bool local_player_adjust_scope_zoom(World &world, LocalPlayerWeapon &w, const PlayerViewState &v,
                                    WeaponSlotState &slot, const ScopeZoomLimits &limits,
                                    int32_t delta);

// The mount-time clamp of the slot's zoom into [floor, scope_max_mag] on a
// Scoped (Flags & 1) def with a nonzero scope_max_mag: a fresh slot (zoom 0)
// lands on the floor, an over-max carry-over on the max; Sighted-only defs and
// a zero max leave the slot alone. Runs at the slot install, before the view
// reset [orig: Player_MountWeaponSlot @0x4dfacf..0x4dfb16].
void local_player_scope_zoom_mount_clamp(const World &world, const ScopeZoomLimits &limits,
                                         int32_t def_flags, int32_t scope_max_mag,
                                         WeaponSlotState &slot);

// The weapon-cycle actions' dispatcher leg [orig: Input_HandleActionBinding_0
// cases 0xD4 (212, cycleweaponP "Cycle Weapon Prev": +1 / +2) / 0xD6 (214,
// cycleweaponN "Cycle Weapon Next": -1 / -2) @0x4e130c..0x4e13ae]: refused while
// the binocular view is up or a PowerThrow charge is live (g_fireChargeStartTick);
// on an equipped def whose scope_min_mag != scope_max_mag while the optical view
// is up (Player_CanFireWeapon) the action steps the zoom by +2 / -2 in place of a
// cycle; otherwise the caller runs Player_CycleWeaponSlot(direction)
// (weapon_cycle_slot). Action 215 (0xD7) is the bare step -- its fifth argument
// picks -2 / +2 -- and calls local_player_adjust_scope_zoom directly.
enum class WeaponCycleRoute : uint8_t { kRefused, kZoomStep, kCycle };
WeaponCycleRoute local_player_weapon_cycle_route(World &world, LocalPlayerWeapon &w,
                                                 const PlayerViewState &v,
                                                 const ScopeZoomLimits &limits,
                                                 int32_t direction);

// --- the USE-ITEM action's vehicle-loadout arm --------------------------------

// Action 177 (useitem) in a vehicle-loadout volume [orig: Input_HandleActionBinding_0
// @0x4e0420 case 0xB1 -- parentSlot == 0 @0x4e0a91, not in an armory volume or
// the MP preround @0x4e0aa1..0x4e0ab0, Flags & 0x800 @0x4e0ab2]: the press
// never latches the mount toggle; it opens vehicle.mnu when the ground
// entity's team byte is 0 or the player's (@0x4e0ad8..0x4e0aeb, the
// groundEntity +0x28 read has no null test) and does nothing otherwise.
// `local_player_in_vehicle_loadout_zone` is the arm's gate (an unmounted local
// player carrying the type-11 volume touch); `local_player_vehicle_zone_team_matches`
// the team test (a free-standing player reads team 0 = open).
bool local_player_in_vehicle_loadout_zone(const World &world);
bool local_player_vehicle_zone_team_matches(const World &world);

// The local player's health pair as the HUD bar, the F3/MCP card and the
// gamemus var pump all read it: the entity's current health (0 without a
// local player) and the body's authored max (100 without a body or an
// authored value). One home so the readers cannot drift (ADR 0042 d2).
int local_player_health(const World &world);
int local_player_max_health(const World &world);

// Action 26: toggle the persistent binocular request. Refused while a
// PowerThrow charge is live (the raised view would suppress the held weapon
// input and turn the charge into an unintended release) and while a scope is
// engaged in a gunner seat. The render latch in LocalPlayer::present_view_frame
// owns the displacement and PRNG draw. Returns the new requested state (false
// also = refused). [orig: g_fireChargeStartTick @0xB76800; the action 26 gate]
bool local_player_binoculars_toggle(World &world, const LocalPlayerWeapon &w,
                                    PlayerViewState &v, LocalPlayerViewTracker &t);

// The binocular sway's ONCE-PER-ACTIVATION seed and its clear (the tracker's
// `binocular_sway_latched` is retail's dword_29D6BA8). Runs when the rendered
// view is assembled; draws one PRNG_Next16 word on the activating frame
// only. [orig: Render_ProcessMainSceneFrame @0x5ca3d3..0x5ca3f8 / @0x5ca4b0 ->
//  Binoculars_RandomizeSwayOffsets @ 0x4dd830]
void local_player_binocular_sway_latch(World &world, const PlayerViewState &v,
                                       LocalPlayerViewTracker &t);

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
// @0x4de4f7; ThirdPersonCamera_Update @0x437b70..76]. The quantum's camera
// compose is LocalPlayer::tick_view's, after the aim acquisition.
void local_player_view_tick(World *world, PlayerViewState &v,
                            LocalPlayerViewTracker &t,
                            const LocalViewSessionInputs &session);

// The motor writes CameraOffset; every later consumer re-anchors it to the
// current position. [orig: Camera_ComputeThirdPersonView @0x437FA5..0x437FB7]
inline Vec3 player_eye_position(const Entity &entity) {
    return {entity.position.x + static_cast<float>(entity.eye_offset_x) / 65536.0f,
            entity.position.y + static_cast<float>(entity.eye_offset_y) / 65536.0f,
            entity.position.z + static_cast<float>(entity.eye_offset_z) / 65536.0f};
}

// The composed per-frame view read, mission space: the effect states, the
// resolved camera mode and its gates, the NoCardSwitch bias suppression, the
// SIGHTS-card selector, the fov, the chase anchor, and the composed camera
// pose (the shell converts frames and stamps the Camera3D node).
// [orig: g_camera_mode @0xA890C8; Player_UpdateFirstPersonCamera @0x4dd439;
//  Player_IsReloadingCardSwitchWeapon @0x4dcdd0; Render_ProcessMainSceneFrame
//  @0x5ca299..0x5ca304; Camera_ComputeThirdPersonView @0x437d10 — the
//  MOUNTED local eye leg @0x4b6908 re-anchored to the live position]
struct LocalPlayerViewFrame {
	HudCombatView hud_combat;
	// Separate scene camera: Inset consumes another shake sample and
	// its own slot-zero offsets after the main scene has sampled its camera.
	// [orig: Render_RadarCompassOverlay @0x5C9841..0x5C9903]
	bool inset_scope_active = false;
	PlayerCameraPose inset_camera;
	float inset_fov_over_zoom = 0;
    bool scope_camera_zero_active = false;
    bool scope_details_active = false;
    bool scope_details_scoped = false;
    uint32_t scope_weapon_flags = 0;
    int32_t scope_magnification = 1;
    int32_t scope_max_range_q16 = 0;
    int16_t scope_zero_word = 0;
    int32_t scope_zero_max = 0;
    int32_t scope_zero_step = 0;
    int32_t scope_zero_default = 0;
    int32_t aim_range_q16 = 0;
    // HUD_BuildEntityInfo @0x4B8440 and HUD_RenderOverlays @0x5A7BB0.
    int hud_stance = 0, hud_mount_slot = 0, hud_weapon_category = 0;
    bool hud_keep_crosshair_while_aimed = false;
    bool scope_engaged = false; // the TARGET (g_scopeEngaged)
    bool scope_settled = false; // the PROMOTED byte (g_weaponScopeActive)
    bool binoculars_requested = false;
    bool binoculars_raised = false;
    bool binoculars_view_active = false;
    float binocular_yaw_offset_deg = 0.0f;
    float binocular_pitch_offset_deg = 0.0f;
    bool nvg_active = false;
    bool nvg_visible = false;
    int32_t nvg_gain = 0;
    // The NVG composite's HUD half (renderer/frame_fx_effects.h): NVG.tga and
    // its gain scale draw only under the full-screen composite, and the Scoped
    // arm's lens takes the circle mask's place with its unit-scale reticle.
    bool nvg_mask_visible = false;
    bool nvg_lens_active = false;
    // The Sighted arm: the SIGHTS card draws into the NVG scene, not over it.
    bool nvg_sights_in_scene = false;
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
    // The frame's FrameFX dispatch facts (runtime/renderer/frame_fx_effects.h):
    // the raw red word, the camera mode, the dead/session bits, the ticks since
    // the death stamp, the thermal and monitor latches (CanFire && flags2 & 4
    // / & 8) and the NVG / death-screen state.
    // [orig: Render_ProcessMainSceneFrame @0x5ca2da..0x5ca2f1 (the latches),
    //  @0x5ca8f6..0x5caad5 (the dispatch)]
    renderer::FrameFxViewInputs frame_fx;
    // The three fullscreen damage-feedback quads, already reduced to what the
    // presenting shell draws (player_view.h carries the arms/decays/colours):
    // `screen_flash_white_alpha` is the raw word, `screen_flash_red_alpha` the
    // capped DRAW alpha with the camera-mode-3 suppression applied, and
    // `screen_flash_revive_channel` the red/green byte of the revive tint
    // (blue 255) with `screen_flash_revive` as its non-zero gate.
    // `hud_overlays_suppressed` is retail's whole-HUD early return while the
    // white word burns.
    // [orig: Render_ProcessMainSceneFrame @0x5CAB9A..0x5CAC48;
    //  HUD_RenderAllOverlays @0x5a8098]
    int32_t screen_flash_white_alpha = 0;
    int32_t screen_flash_red_alpha = 0;
    int32_t screen_flash_revive = 0;
    int32_t screen_flash_revive_channel = 255;
    bool hud_overlays_suppressed = false;
    bool mounted = false;
    bool third_person = false;
    bool third_person_selected = false;
    int camera_mode = 0;
    bool camera_mounted = false;
    bool vehicle_attack_context = false;
    float scope_fraction = 0.0f;
    bool suppress_view_bias = false;
    bool scope_card_active = false;
    // The FP draw's own gates beyond the card switch (player_present.h
    // fp_viewmodel_retail_submit composes them): the local dead bit, a
    // decided round winner, the equipped def's Emplaced bit (skips the
    // showhud test) and the scoped Inset skip [orig:
    // Player_RenderViewModelIfAlive @0x4E0145/@0x4E014B;
    // Player_RenderFirstPersonViewModel @0x4DEDD9..0x4DEE19].
    bool fp_local_dead = false;
    bool fp_round_winner_set = false;
    bool fp_def_emplaced = false;
    bool fp_inset_scoped = false;
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
    // The carrier owns this frame's first-person view (see
    // local_player_mounted_camera).
    bool mounted_camera = false;
    // local_view_draws_virtual_display holds for the local player's vehicle:
    // its hull row is local-view suppressed and the shell draws this graphic
    // key at the hull's transform instead (empty = the def authors none, so
    // nothing draws).
    bool virtual_display_active = false;
    EntityHandle virtual_display_carrier;
    std::string virtual_display_model;
};

// ONE Camera_ComputeThirdPersonView call over the local player: the pose
// (the mode-0 carrier legs, the ground-entity leg, else the person / chase /
// death legs of player_view_compose_camera), then this call's shake — the
// mode-0 IIR step or the stateless chase chain, both added to the BAM heading
// — recorded as the last composed view. It advances the composition's own
// state (the shake filters, the chase look-ahead, the ground-entity lift), so
// it runs exactly where retail calls it: once per logic quantum
// (LocalPlayer::tick_view), once per rendered frame and once more for the
// Inset scene (local_player_view_frame). False with no local player, whose
// view words zero.
// [orig: Camera_ComputeThirdPersonView @0x437D10, called @0x526781 /
//  @0x5CA34D / @0x5C9841]
bool local_player_camera_compose(World &world, PlayerViewState &v, LocalPlayerViewTracker &t,
                                 PlayerCameraPose &out, bool &mounted_camera);

// The RENDERED frame: the view read plus the main scene's compose and, with
// the Inset scene up, its second compose and slot offsets. Advances the
// composition state; the presenter's per-frame leg is its one live caller.
// [orig: Render_ProcessMainSceneFrame @0x5CA34D; Render_RadarCompassOverlay
//  @0x5C9841..0x5C9903]
void local_player_view_frame(World *world, LocalPlayerWeapon &w, PlayerViewState &v,
                             LocalPlayerViewTracker &t, LocalPlayerViewFrame &out);

// The same read OBSERVED between composes: the camera is the last composed
// view (g_view_pos / g_view_rot) and nothing advances — the weapon-event
// placement, the mode refresh and every diagnostic read this one.
void local_player_view_observe(World *world, LocalPlayerWeapon &w, const PlayerViewState &v,
                               LocalPlayerViewTracker &t, LocalPlayerViewFrame &out);

// THE MOUNTED FIRST-PERSON CAMERA. Mode 0 hands a seated rider's view to its
// carrier before any person leg runs:
//  - a carrier whose def authors `virtualdisplay <model> <userpoint>` calls
//    its input class's camera callback. `tank` places the eye at the carrier
//    matrix x that userpoint and pulls it 0.1875u back along the rider's view
//    rotation; null / troop take the carrier's own Position + CameraOffset and
//    rotation triple. A non-vehicle parent defers to its ground entity while
//    that entity's weapon slot is parent-routed;
//  - else an EWEAP that is not PlayerControl poses its own "CAMERA" userpoint:
//    the view POSITION AND ROTATION both come from the posed gun part, so a
//    turret gunner's view follows the lagging gun. Without the userpoint the
//    view is the gun's raw pose.
// out = position (Q16) + yaw / pitch / roll (BAM). False leaves the person
// legs to run (no carrier, or a carrier neither leg admits).
// [orig: Camera_ComputeThirdPersonView @0x437D10 -- callback leg
//  @0x437DAC..0x437E77, EWEAP leg @0x437E7C..0x437EAD; the tank callback
//  @0x44A190 (row @0x829DC8), the null/troop callback @0x4DC710;
//  Entity_GetBoneWorldPosition @0x545E60 (CAMERA byte +0x318 @0x545F5B)]
bool local_player_mounted_camera(World &world, const Entity &rider, int32_t out[6]);

// THE `tank` RENDER CLASS'S FIRST-PERSON SWAP. While this machine's player is
// the vehicle's claimant (the +0x170 primary occupant) and the camera is in
// mode 0, the class draws the def's virtual-display model INSTEAD of the hull
// -- and nothing at all when the def authors no virtual display. Every other
// render class, seat and camera mode draws the hull as usual.
// [orig: render class row `tank` @0x82CFF0 -> 0x449EF0 -- claimant / mode gate
//  @0x449F12..0x449F27, the def+0x12C swap @0x449F29..0x449F45]
bool local_view_draws_virtual_display(const World &world, const PlayerViewState &v,
		const Entity &vehicle);

// The seat-bone pose alone: the carrier's posed CAMERA userpoint for an EWEAP
// with a model, the gun's raw pose without the userpoint, else the rider's own
// Position + CameraOffset. The mounted camera's EWEAP leg and a gunner's
// aim-ray START both read it. [orig: Entity_GetBoneWorldPosition @0x545E60]
bool local_player_seat_bone_pose(World &world, const Entity &rider, int32_t out[6]);

// The authored pose interpolation's additive rotation bias. The same airborne
// and reload gate as the position leg selects zero while interpolation keeps
// advancing. [orig: Player_UpdateFirstPersonCamera @0x4DD40D..0x4DD456]
void local_player_viewmodel_rotation_bias(World *world, const LocalPlayerWeapon &w,
                                         const PlayerViewState &v, int32_t out_bam[3]);

// The FP viewmodel view-offset in VIEW-FRAME world units (X=forward, Y=left,
// Z=up): the raw weapon.def `pos` plus the interp's published position bias
// over the /256 scale with the NoCardSwitch reload suppression applied, plus
// the per-frame motion lead and the 4:3 framing drop. Advances the
// motion-lead tracker. (The ADS endpoint is the bound pose, not an input.)
// [orig: Player_UpdateFirstPersonCamera @0x4dd380 — lead @0x4dd4f2..0x4dd56c,
//  narrow-aspect drop @0x4dd571]
void local_player_viewmodel_bias(World *world, const LocalPlayerWeapon &w,
                                 const PlayerViewState &v, LocalPlayerViewTracker &t,
                                 const float pos_raw_units[3],
                                 int viewport_w, int viewport_h, float out[3]);

} // namespace opennova::world
