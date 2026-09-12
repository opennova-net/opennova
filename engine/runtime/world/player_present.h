// The LOCAL PLAYER's presentation law (ADR 0043 slice G8): the pure policies
// the presenting shell applies over plain inputs when it draws the local
// player -- which camera modes present the body, when the first-person
// model submits, which lighting each submit takes, when a local action begin
// spawns its muzzle effect and which gun resolves its userpoint, how one
// owner-bound action effect binds, the order one fixed tick's weapon batch
// presents in, the per-submit CTRL register writers, the raw-key down latch,
// the switch-deny click, the equipped weapon.def precedence with its
// bring-up fallback and memo, and the spawn-loadout projection. The device
// half -- camera stamps, fov/cull-mask, ObjectModel builds and parenting,
// input sampling, the projection shader global, the userpoint bone remap --
// stays in the binding (godot/src/player).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

// --- the camera mode's presentation split -------------------------------------

// Every non-first-person mode presents the body and hides the FP arms -- the
// chase (1) and the death lerp camera (4) alike [orig: the FP viewmodel gate
// g_camera_mode == 0 in Entity_ComputeActionTransform @0x40133e].
bool presents_third_person(bool third_person, int camera_mode);

// --- the first-person submit ----------------------------------------------------

// The retail FP submission decision, ANDed from its four gates:
//   * the card switch: while the SIGHTS card is up, the FP model does not
//     draw -- the frame shows one or the other [orig: selectors/clear
//     @0x5ca299..0x5ca304; the card path @0x5caaf3..0x5cab15 and the
//     viewmodel candidate @0x5ca32c]; the binocular view takes the same
//     branch;
//   * the showhud bit-0 gate [orig: Player_RenderFirstPersonViewModel
//     @0x4DEDEA -- test g_FpWeaponViewFlags, 1 before the FP pass];
//   * the SEAT gate, evaluated inside the draw: a pilot/driver/gunner
//     carries no first-person weapon at all, so a helicopter cockpit shows a
//     clear screen instead of a rifle over the panel, while a PASSENGER keeps
//     his and can still shoot out [orig: Player_RenderFirstPersonViewModel
//     guards the whole draw on `!vehicle || parentSlot not in {2,3,5} ||
//     (attrib & EWEAP && !PLAYERCONTROL)`; the condition itself is
//     world::mount_hides_fp_viewmodel];
//   * third person presents the body instead (presents_third_person).
bool fp_viewmodel_retail_submit(bool third_person, bool scope_card_active,
                                bool binoculars_view_active, bool fp_gun_visible,
                                bool seat_hides_weapon);

// --- the local player's lighting contexts ---------------------------------------

// The third-person body and its held gun take the entity's outdoor sun factor
// like any sector-drawn entity (D-RLIT-3; the sim computes the local quality
// each occlusion frame). The FP submit deliberately keeps effectScale=1
// (retail computes then discards its outdoor sun sample) but still keys the
// interior group from blink_hits[0]. Updating on every presentation frame
// makes portal crossings live.
// [orig: Player_RenderFirstPersonViewModel @0x4DEEA4..0x4DEF52 -- the
//  setup_terrain_effect_for_entity return is dropped on the FP leg;
//  Terrain_RenderSectorEntities stacks it for the world body @0x5c7bff]
struct LocalPlayerLightingContext {
    bool interior = false;          // blink_hits[0] names a building
    float light_transfer = 0.0f;    // that building's items.def light transfer
    float body_effect_scale = 1.0f; // the sun-quality factor (body + held gun)
    float fp_effect_scale = 1.0f;   // the FP arms/gun keep 1
};
LocalPlayerLightingContext local_player_lighting_context(int interior_item_id,
                                                         float light_transfer,
                                                         float body_sun_factor);

// --- the local action-begin effect legs -----------------------------------------

// Particle gating is the witnessed local-player routing [orig:
// ActionSlot_ExecuteActionTick @0x541a70]: for the LOCAL player only the
// FIRE action takes the with-effect shim, and only in third person, from a
// vehicle, or un-scoped in first person (the FP muzzle-flash config
// dword_24D20C0 bit 0 rides that leg; treated always-on here) -- every other
// local begin routes through the no-effect shim @0x5419e0 (no casing ejects
// in your own FP view; remote views spawn them via the remote leg @0x541a83,
// an MP seam) [orig: @0x541b17]. The suppression reads the event's SETTLED
// scope state: retail promotes g_weaponScopeActive before weapon actions on
// every tick; one render frame can drain several ticks spanning that
// boundary, so the final render snapshot is not a valid substitute [orig:
// promoter @0x4de4f7 before weapon pump call @0x526786; gate @0x541aba
// !g_weaponScopeActive] -- settled-scoped FP fire shows no muzzle flash
// [orig: @0x541aba]. `fire_action` is the FIRE slot id (weapon_action::kFire).
bool local_fire_effect_admitted(int action_started, int fire_action, bool has_particle,
                                bool scope_settled, bool third_person,
                                bool vehicle_attack_context);

// Retail keeps two resolved indices for one authored userpoint name --
// ActionDef+56 against gfx1 and +57 against gfx3 -- and picks by the
// first-person bit, which requires the camera to be in first person at all
// [orig: the FP bit gate @0x540e8c..0x540eca requires g_camera_mode == 0;
// the gfx1/gfx3 resolvers @0x54039e/@0x54040f]. True = resolve the point
// against the third-person world gun (gfx3); false = the FP viewmodel (gfx1).
bool action_particle_uses_third_person_gun(bool third_person);

// The owner-bound action effect's binding policy. Weapon particles always
// enter the global effect world and render in the later world particle
// brackets, even when their position came from the first-person gun: retail
// flushes the viewmodel mini-scene, restores the world projection, then runs
// EffectWorld_RenderParticlePass; there is no separate FP particle pass
// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0; particle pass @ 0x5f7240;
// ActionSlot_SpawnEffect @ 0x401f20]. The live group follows the SPAWNING
// action's userpoint for its whole life: retail records the handle + action
// index on the slot and the pump re-anchors the emitter to that action's
// bone every tick, releasing it only on death [orig: ActionSlot_SpawnEffect
// handle/action record @ 0x40208f/0x402092 -> the +0x18 tracker leg in
// WeaponAction_ProcessFrame @ 0x540edf -> CEffectEmitter_UpdatePositionAndParams
// @ 0x5f6810]. The presenter analog is an owner-bound group whose anchor
// resolver re-reads the live userpoint pose; a re-fire while the group lives
// is suppressed by the slot (one live handle per action slot).
struct ActionEffectSpawnPolicy {
    bool suppress_while_owned = true; // one live handle per action slot
    bool follow_owner = true;         // the anchor re-read law above
    bool world_render_domain = true;  // never the first-person domain
};
inline constexpr ActionEffectSpawnPolicy kActionEffectSpawnPolicy{};

// --- one fixed tick's weapon batch, in presentation order -----------------------

// The facts of one drained presentation event the ordering reads (the
// binding derives them from WeaponPresentationEvent).
struct WeaponBatchEvent {
    bool starts_clip = false;      // a clip start (anim_key set)
    bool action_started = false;   // the ACTION begin leg (action_started >= 0)
    bool action_effect = false;    // the recoil-row DIRECT effect leg
    bool action_finished = false;  // the ACTION end leg
    bool clear_weapon = false;     // a committed clear
    bool switch_weapon = false;    // a committed switch (switch_to_weapon set)
    bool switch_denied = false;    // the switch-walk refusal
};

enum class WeaponPresentOp : uint8_t {
    kPoseChannel,   // pose the VIEW's current clip at its channel position (event -1)
    kPlayClip,      // start the event's clip at the view's channel position
    kActionBegin,   // the event's action-begin sound + muzzle legs
    kDirectEffect,  // the event's recoil-row direct effect
    kActionEnd,     // the event's action-end sound
    kClearWeapon,   // reinstall: clear the equipped weapon
    kSwitchWeapon,  // reinstall: the committed switch
    kSwitchDenied,  // the deny click
};

struct WeaponPresentStep {
    WeaponPresentOp op = WeaponPresentOp::kPoseChannel;
    int event = -1; // index into the batch, -1 for the view-wide pose
};

struct WeaponBatchPlan {
    std::vector<WeaponPresentStep> steps;
    int32_t play_serial = -1; // the presenter's clip serial after the batch
};

// Present one tick's ordered batch. Every clip/begin/end payload survives;
// clips are POSED at the sim's gated channel position (anim_advance_ticks) --
// nothing free-runs the FP playhead. Clip starts land on BOTH viewmodel
// parts; scope side effects (forced unscope, rescope-after-reload) flip the
// SIM's own engaged bit -- they arrive already folded into the view snapshot.
// [orig: ActionSlot_BeginActivePhase @0x53f830 plays the action clip on the
//  owner's animadm channel; the rescope block @0x54139e]
// The playhead is PINNED (external phase): retail's channel moves only in the
// weapon pump's gated tick shim, never per render frame [orig: the gate
// ActionSlot_ExecuteActionNoEffect @ 0x541a4d].
//   * no view (an unarmed player has no view until UseGun installs one):
//     slot selection is control state, not viewmodel presentation -- only the
//     clear/switch reinstalls run, and the serial resets;
//   * a batch with no clip start first poses the already-playing clip at the
//     channel position, so same-tick direct effects sample a posed userpoint;
//     a clip event replaces that pose first;
//   * per event, in order: clip, action begin, direct effect, action end,
//     clear/switch, deny; a batch's clip events are superseded by later ones;
//   * a batch that started a clip adopts the view's play serial; a serial the
//     presenter has not seen (first adoption, a fresh viewmodel) poses the
//     latest snapshot's clip but never replays its historical sound/effect
//     payloads.
void weapon_batch_plan(bool view_active, int32_t view_play_serial, int32_t play_serial,
                       const WeaponBatchEvent *events, size_t count,
                       WeaponBatchPlan &out);

// --- the per-submit first-person CTRL register writers --------------------------

// Which FP CTRL writers execute for one viewmodel part on one frame:
//   * TEX_TEAM is a signed-byte store immediately before the FP lighting,
//     heat and model-submit path; hidden/carded/binocular/third-person frames
//     never execute that retail writer [orig: Player_RenderFirstPersonViewModel
//     @0x4DEE96..0x4DEE9F];
//   * retail publishes accumulated heat independently for every FP model
//     submit, clamped through the exact 0x10000 endpoint [orig:
//     Player_RenderFirstPersonViewModel @0x4DEEC2..0x4DEEF5];
//   * the emplaced gun's controls ride the same submit when the view carries
//     them;
//   * the character arms' own raw camo triplet is stored immediately before
//     each arms submit -- the same per-submit writer family, arms part only
//     [orig: Avatar_SetArmsCamoCtrl @0x57a3b0 at @0x4df008/@0x4df070].
// A false member means the writer's registers are CLEARED for the frame.
struct FpCtrlRegisterWrites {
    bool team = false;
    bool heat = false;
    bool emplaced = false;
    bool arms_camo = false;
};
FpCtrlRegisterWrites fp_ctrl_register_writes(bool submit, bool has_weapon_view,
                                             bool emplaced_controls_valid, bool arms_part);

// --- the raw-key down latch -----------------------------------------------------

// The down-edge latches ride the RAW key state -- retail's key scan latches
// the device state and the context only gates which dispatcher arm runs, so
// a key held across an armory/F3 window must NOT re-fire when the gate
// reopens (the same rule the hudcolor poll follows) [orig: the @0x49d1f0
// scan's per-row down latch reads raw key state]. Returns the gated edge and
// updates the latch regardless of the gate.
bool latched_key_edge(bool down, bool active, bool &was_down);

// --- the switch/equip deny click ------------------------------------------------

// The switch/equip DENY click (D-WPN-22): a refused weapon switch plays the
// "DRY_CLAYSATCH" trigger set as a non-positional interface one-shot. The
// deny legs tail-call the interface play with the mission-load-resolved
// handle at dword_24E08C4 [orig: Player_SwitchToWeaponByHandle @ 0x4e0344 /
// Player_EquipWeaponByEntity @ 0x4e037e -> Sound_PlayInterfaceTriggerSet
// @ 0x527be0]; the name->slot row lives in the @ 0x82F590 resolver table
// (DialogSystem_Init @ 0x527687). game.lwf ships the set. The play's
// is_mp_session_peer gate (@ 0x527be5) is the is_client bit -- TRUE in SP
// mode 3, false only on dedicated hosts (correspondence.md
// Sound_PlayWithDistanceAttenuation) -- and the local presenter only exists
// on a client, so playing unconditionally there IS the gate.
inline constexpr const char *kWeaponSwitchDenySoundset = "DRY_CLAYSATCH";

// --- the scope-zero click -------------------------------------------------------

// A changed scope zero (the zero-step keys) clicks the "GF_SCOPE_ZERO" trigger
// set through the same non-positional interface play [orig:
// Player_AdjustWeaponZoomLevel @0x4dbd47..0x4dbd50 ->
// Sound_PlayInterfaceTriggerSet(dword_24E08B8) @0x527be0; the name->handle row
// is entry 2 of the @0x82F590 resolver table (@0x82f5d8 -> 0x24e08b8,
// DialogSystem_Init @0x5275e0)]. The play's is_mp_session_peer gate (@0x527beb)
// holds by construction: the request runs only over a live local player,
// which a dedicated host never has. The sim raises it as an Interface
// ScriptSoundEvent on world.out.script_sounds, the shell's ui_soundset channel.
inline constexpr const char *kScopeZeroSoundset = "GF_SCOPE_ZERO";

// --- the equipped weapon.def precedence -----------------------------------------

// Which weapon.def row drives the FP viewmodel: the armory-equipped (or
// debug-selected) weapon overrides the bring-up fallback once the player
// accepts a loadout [orig: the equipped AdmDef drives the FP model pick,
// Player_RenderFirstPersonViewModel @0x4ded60 via the mounted slot]; the
// authored NONE row (`cleared`) is distinct from the pre-armory empty
// override, which falls back to the witnessed bring-up default until an
// equipped weapon is resolved. `resolves` false = no viewmodel def at all.
struct ViewmodelDefPick {
    bool resolves = false;
    std::string name;
};
ViewmodelDefPick viewmodel_def_pick(bool cleared, const std::string &override_name,
                                    const char *bringup_fallback);

// Retail reads the equipped slot's def pointer, resolved when the slot was
// mounted; the decoded record is keyed on the name it resolved from, so the
// presenter's per-frame read costs one string compare. The memo is dropped
// with the mission (a reload re-decodes from ITS weapon.def even when the
// name repeats).
bool viewmodel_def_memo_hit(const std::string &name, const std::string &memo_name,
                            bool memo_present);

// --- the spawn-loadout projection -----------------------------------------------

// The PLAYER_INFO kit slots the spawn projection walks, in kit order.
inline constexpr const char *kSpawnLoadoutSlotKeys[3] = {"primary", "secondary", "accessory"};
inline constexpr int kSpawnLoadoutSlotCount = 3;

struct SpawnLoadoutSlot {
    bool present = false; // the profile carried the slot key (an empty name = NONE)
    std::string name;
    int32_t clips = -1;   // -1 = the untouched default
};

struct SpawnLoadoutInput {
    SpawnLoadoutSlot slots[kSpawnLoadoutSlotCount];
    bool has_player_class = false;
    int32_t player_class = 0;
};

struct SpawnLoadoutKitRow {
    std::string name;
    int32_t clips = -1;
};

enum class SpawnLoadoutAction : uint8_t {
    kNone,          // nothing staged: the engine's default kit stands
    kSyncInventory, // adopt the authoritative inventory's equipped weapon
    kApplyKit,      // apply `kit` (then `after_apply`)
};

enum class SpawnLoadoutAfterApply : uint8_t {
    kClearWeapon,   // an empty kit: no slots, nothing equipped
    kSyncInventory, // a syntactically nonempty kit: follow the inventory
};

struct SpawnLoadoutPlan {
    bool set_player_class = false;
    int32_t player_class = 0;       // the class to commit / apply (0 = unclassed)
    SpawnLoadoutAction action = SpawnLoadoutAction::kNone;
    std::vector<SpawnLoadoutKitRow> kit;
    SpawnLoadoutAfterApply after_apply = SpawnLoadoutAfterApply::kSyncInventory;
};

// The mission-start projection of the staged PLAYER_INFO kit onto the local
// player: the profile class commits whenever it was carried; mission-authored
// kits outrank the profile selection (unlike the inventory itself, that
// source bit stays false for load_weapon_table's WPN_M4AUTO fallback, so a
// real default weapon cannot masquerade as mission policy); a staged kit
// applies its non-empty slot names in kit order with their clip counts, then
// an empty kit clears the equipped weapon and a nonempty one follows the
// authoritative inventory (a syntactically nonempty kit can still be
// rejected by mission/class rules).
SpawnLoadoutPlan spawn_loadout_plan(const SpawnLoadoutInput &input,
                                    bool has_explicit_spawn_loadout);

} // namespace opennova::world
