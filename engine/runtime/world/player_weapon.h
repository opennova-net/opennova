// The LOCAL PLAYER equipped-weapon cluster (S7a, ADR 0028): the weapon action
// FSM pump, install/def bake orchestration, clip-variant rings, the UseGun
// borrow, PowerThrow, switch outcomes, and the presentation-event queue —
// moved verbatim from the shell binding (net-re §5.62). The embedder feeds
// plain install data and inputs, drains presentation events, and routes the
// two wire request records; everything else runs here.
// [orig: the per-entity pump WeaponAction_ProcessAllEntities @ 0x542690; the
//  local player's action handlers WeaponAction_Fire @ 0x542bb0 /
//  WeaponAction_Reload @ 0x5430b0 / WeaponAction_Recoil @ 0x542dd0]
#pragma once

#include <formats/def/def.h>
#include <runtime/anim/adm_ring_table.h>
#include <runtime/anim/clip_timeline.h>
#include <runtime/world/player_view.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::world {

class World;

// The remaining WeaponDef flag bits the local pump consumes, mirrored from
// the def parser's token table (the binding static_asserts the pairing; world
// never consumes the format stack). [orig: the 16-byte flag rows @ 0x830BF0]
namespace weapon_flag {
enum : int32_t {
    kNoClipsNoDraw = 0x00000010,
    kBurst = 0x00000020,
    kAuto = 0x00000100,
    kPowerThrow = static_cast<int32_t>(0x80000000u),
};
} // namespace weapon_flag

// One drained presentation record — the per-tick anim/action payload the
// shell's weapon-effects presenter consumes. Records stay in logic-tick order
// until the embedder drains. world_position is the shooter's MISSION-space
// position (the embedder converts to its render space at drain).
struct WeaponPresentationEvent {
    uint32_t tick = 0;
    Vec3 world_position{};
    std::string anim_key;
    int32_t anim_variant = 0;
    int32_t action_started = -1;
    std::string action_soundset;
    std::string action_particle;
    std::string action_particle_userpoint;
    bool scope_settled = false;
    bool third_person = false;
    bool vehicle_attack_context = false;
    int32_t action_finished = -1;
    std::string action_end_soundset;
    int32_t action_effect = -1;
    std::string effect_particle;
    std::string effect_particle_userpoint;
    std::string switch_to_weapon;
    bool clear_weapon = false;
    bool preserve_slot_state = false;
    bool switch_denied = false;
};

// The UseGun borrow staging phases. [orig: Entity_AttachToUseGunSlot
// @ 0x546b80; the action-handler commit seams @ 0x543475 / @ 0x543539]
enum class LocalUseGunSwitch : uint8_t { kNone, kAttach, kSwap, kDetach };

// One pump tick of the local weapon FSM, recorded for the F3 Weapon window's
// trace pane. The FSM runs at the 62.5 Hz logic tick while the display frame
// does not, so a 1-tick action (SWITCHTO/SWITCHRANK) or a zero-length tail
// (RECOIL's delayend) can pass entirely between two frames — a frame-rate
// sampler would simply never see them. This is devtools instrumentation, not
// engine behaviour: nothing in the pump reads it back.
struct WeaponTraceSample {
    uint32_t tick = 0;
    int32_t current = 0;   // weapon_action::*
    int32_t next = 0;
    int32_t prev = 0;
    uint8_t phase = 0;     // weapon_phase::*
    int32_t counter = 0;
    int32_t clip = 0;
    int32_t reserve = 0;
    int32_t heat = 0;
    int32_t action_started = -1;
    int32_t action_finished = -1;
    int32_t action_effect = -1;
    bool fired = false;
    bool dry_fired = false;
    bool reload_requested = false;
    bool reload_applied = false;
    bool advance_anim = false;
    // The clip the FP channel is holding this tick (LocalPlayerWeapon::anim_key,
    // latched on the play edge) and its ring variant — NOT the play-edge key,
    // which is set on one tick per action. `advance_anim` says whether the
    // channel actually stepped this tick; retail clocks it from the counter,
    // so a held clip is a visible fact worth drawing.
    char anim_key[64] = {};
    int32_t anim_variant = 0;
};

// Samples the trace ring holds: 1024 ticks is ~16 s at 62.5 Hz, long enough
// for a whole magazine dump AND the reload that follows (~240 + ~220 ticks on
// the minimal rifle) to still be there when F3 is reopened a few seconds
// later. ~140 KB while armed, nothing while not.
inline constexpr size_t kWeaponTraceCapacity = 1024;


// The local player's whole equipped-weapon state — the moved binding members,
// one aggregate the embedder holds beside the world.
struct LocalPlayerWeapon {
    WeaponFsmDef def{};
    std::string def_name;
    WeaponSlotState slot{};

    LocalUseGunSwitch usegun_switch = LocalUseGunSwitch::kNone;
    bool usegun_slot_active = false;
    EntityHandle usegun_mount{};
    EntityHandle usegun_pending_mount{};
    uint8_t usegun_weapon_adm = 0xFF;
    uint8_t usegun_pending_weapon_adm = 0xFF;
    uint8_t usegun_saved_adm = 0xFF;
    int32_t usegun_switch_action = -1;
    uint8_t first_person_model_adm = 0xFF;

    bool active = false;
    bool fire_held = false;
    bool fire_pressed = false;    // latched until the pump consumes the edge
    bool reload_pressed = false;
    uint32_t power_throw_start_tick = 0;
    uint8_t pending_throw_charge = 0;

    uint32_t play_serial = 0;
    // The FP animadm channel (WeaponDef+0x174), in retail's two halves. The
    // primary half is the clip on show: its key (the one it registered under:
    // `anim_reset` when the table fills the played slot with its reset clip),
    // its served ring variant, and its position in 62 Hz ticks, the count of
    // gated advances since it started (WeaponFsmEvents::advance_anim), NOT the
    // play's wall-clock age: ticks outside the counter window do not move it.
    // [orig: the channel t maintained by AnimChannel_AdvancePlayback @ 0x40b140]
    std::string anim_key;
    uint32_t anim_advance_ticks = 0;
    int32_t anim_variant = 0;
    // The latch: the slot the last play named, whose ring a loop wrap serves,
    // and the ring entry it holds (the entry's key and variant).
    // [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 latches S+0x3C / +0x40 / +0x44]
    std::string anim_slot_key;
    std::string anim_latched_key;
    int32_t anim_latched_variant = -1;
    // The blend half: at a loop wrap the slot's next ring entry fades in from
    // its start over eight gated advances while the outgoing clip runs on,
    // then replaces it. A play restarts only the primary half, so a fade in
    // flight still promotes its clip over the played one.
    // [orig: AnimMap_AdvanceToNextAnim @ 0x40BDF0 -> AnimChannel_InitFromParams
    //  @ 0x410640 (8, 0.125f); AnimChannel_AdvanceBlendedPlayback @ 0x40B1E0;
    //  PlayAnimBySlot re-inits only the primary half @ 0x40BDD1]
    bool anim_blending = false;
    std::string anim_blend_key;
    int32_t anim_blend_variant = 0;
    uint32_t anim_blend_ticks = 0;
    int32_t anim_fade_countdown = 0;
    float anim_blend_weight = 0.0f;
    // The primary half's clock, rebuilt when its clip changes: the loop wrap
    // is its step past 1 (0 fps = a channel that never steps).
    anim::ClipTimeline anim_clock;
    uint32_t fired_serial = 0;
    uint16_t round_sequence = 0;
    uint32_t dry_serial = 0;
    uint32_t reload_serial = 0;
    uint32_t reload_applied_serial = 0;
    uint32_t reload_received_serial = 0;
    uint16_t reload_received_entity = EntityHandle::kInvalid;
    uint16_t reload_received_param = 0;
    uint32_t unscope_serial = 0;
    uint32_t rescope_serial = 0;
    uint32_t action_serial = 0;
    int32_t action_started = -1;
    uint32_t action_end_serial = 0;
    int32_t action_finished = -1;

    float scope_max_mag = 0.0f;
    int32_t aim_range_q16 = 0;
    // AbsorbPitch: WeaponDef+316/+320 and local elevation dword_B79008.
    // [orig: Player_MountWeaponSlot @0x4DFA40]
    int32_t pitch_min_bam = 0, pitch_max_bam = 0, pitch_offset_bam = 0;
    int hud_category = 0, emplaced_stance = 0;
    int32_t attack_kind = 0;
    int32_t run_anim = 0;
    bool force_crouch = false;
    std::string anim_map;
    // Advanced by a mount whose weapon category differs from the one held before (the
    // arms-dip edge) [orig: Entity_UpdateInfantryPlayerBody @0x4b46e7..0x4b46f5].
    uint32_t category_serial = 0;

    bool switch_in_flight = false;
    int32_t switch_deferred_action = -1;
    bool start_in_switchto = false;
    bool presentation_pending = false;
    bool nvg_scope_restore = false;

    std::vector<WeaponPresentationEvent> events;

    // The F3 Weapon window's tick trace (devtools only). Disarmed by default:
    // the pump pays one bool test per tick for it. `trace_head` is the next
    // write index once the ring has filled; `trace_wrapped` says it has.
    std::vector<WeaponTraceSample> trace;
    size_t trace_head = 0;
    bool trace_wrapped = false;
    bool trace_armed = false;
};

// The plain install payload: weapon_install_data_from_def fills the row half
// from the retained weapon.def parse; the feeder adds the clip rings.
struct WeaponInstallData {
    PlayerViewPose view_hip_pose;
    PlayerViewPose view_ads_pose;
    WeaponScopeZero scope_zero;
    int32_t ammo_cost = 0;
    int32_t pitch_min_bam = 0, pitch_max_bam = 0;
    int hud_category = 0, emplaced_stance = 0;
    std::string soundfireloop;
    std::string soundtrailoff;
    std::string soundhead;
    std::string soundlockedtone;

    std::string name;
    std::string animadm;
    int32_t flags = 0;
    int32_t flags2 = 0;
    int32_t heat_per_shot = 0;
    int32_t heat_decay_per_tick = 0;
    int32_t heat_glow_threshold = 0;
    float scope_max_mag = 0.0f;
    // The zoom seed's other two def words (weapon_inventory.h
    // weapon_slot_initial_zoom): scope_max_mag's second value (+0x94) and
    // scope_min_mag (+0x98, record default 2).
    int32_t scope_initial_mag = 0;
    int32_t scope_min_mag = 2;
    int32_t attack_anim = 0;
    int32_t run_anim = 0;
    int32_t clipsize = 0;
    int32_t startrounds = 0;
    std::vector<WeaponFsmActionRow> rows;
    // A mount by name: the weapon table's entry of that name carries the
    // descriptors its load baked, and the mount runs those, baking nothing.
    // Otherwise the install bakes `rows` itself (the seam a definition object
    // takes) against the table's shared rings.
    bool table_baked = false;
    // A definition object's own per-key clip-variant lengths in seconds (keys
    // any case), which stand in for its ANIMADM's rings when no table of that
    // name loaded.
    std::vector<std::pair<std::string, std::vector<float>>> clip_rings;
};

// The row half of a WeaponInstallData from a parsed weapon.def entry: the
// scalar slice the FSM bake reads plus the ACTION rows mirrored as
// WeaponFsmActionRow. The clip-variant rings are the caller's (the kernel's
// .adm clip index, or the embedder's own read).
WeaponInstallData weapon_install_data_from_def(const opennova::def::DefWeaponDef &row);

// The pump's wire-side outputs: the embedder's net layer consumes these — the
// joiner's C2S 0x06 fired descriptor inputs and the 0x25 reload request. The
// pump itself never touches the wire.
struct LocalWeaponFiredWire {
    bool valid = false;
    RoundEvent round;          // origin/dir/seq/flags exactly as spawned
    uint16_t shot_seq = 0;
    uint8_t adm_index = 0;
    int32_t ammo_index = -1;
    uint8_t charge = 0;
    uint16_t target_handle = EntityHandle::kInvalid; // C2S 0x06 +28, aiRuntime[3]
    int32_t shooter_pose[5] = {0, 0, 0, 0, 0}; // pos xyz (16.16) + heading/pitch BAM
};
struct LocalWeaponReloadWire {
    bool valid = false;
    uint16_t entity_handle = 0xFFFF;
    uint16_t reload_param = 0;
};

struct LocalWeaponPumpIO {
	int8_t map_command = 0; // authored scope/holster map callback for the local HUD
    PlayerViewState *view = nullptr;      // required
    WeaponInventory *inventory = nullptr; // null = no inventory installed
    bool is_authority = true;             // the joiner defers refills/rounds
    uint16_t self_wire_handle = 0;        // joiner wire handle; 0 = local packed
    // Invoked only on a joiner fire: the shooter's carrier-exclusion handle
    // resolved from the decoded net state (0xFFFF = none).
    std::function<uint16_t()> carrier_exclusion;
    LocalWeaponFiredWire fired;
    LocalWeaponReloadWire reload;
};

// The active slot: the borrowed UseGun parent slot when engaged, else the
// personal slot. [orig: the parentSlot==3 slot redirect the action handlers
// perform via the shared route helper @ 0x5460e0]
WeaponSlotState *active_local_weapon_slot(World &world, LocalPlayerWeapon &w);
const WeaponSlotState *active_local_weapon_slot(const World &world,
                                               const LocalPlayerWeapon &w);

// g_LocalPlayerEntity->Pitch = 0, shared by the AbsorbPitch mount stamp and
// the scope-up leg. [orig: @0x4DFAB7; @0x4DF314]
void local_player_level_pitch(World &world);

// The equipped weapon's flag query. An OnlyScoped weapon in the local
// player's hands answers no mask until the scope is PROMOTED, so AbsorbPitch
// (and every other seat flag) is inert while the mortar is carried unscoped.
// [orig: Entity_CheckWeaponSeatFlags @0x540D00]
bool local_weapon_seat_flag(const LocalPlayerWeapon &, bool scope_settled, uint32_t mask);

// Shared local fire/aim pose, including AbsorbPitch and mounted barrels.
// clip_before_consume selects the barrel before the FSM spends ammo;
// scope_settled is the promoted byte the seat-flag query reads.
// [orig: Entity_CalcWeaponFirePosition @0x4DC750]
void local_weapon_fire_pose(World &, const LocalPlayerWeapon &,
                           int32_t clip_before_consume, bool scope_settled, int32_t out[6]);

// The weapon point of a fired slot on `carrier`, the half both userpoint
// transforms share: a def carrying a third-person model (gfx3) poses its
// resolved launch userpoint on that model through the carrier; any other
// def poses the carrier's own slot byte for barrel `clip & 3` in `column`.
// `out_direction`, when set, also receives the point's authored direction
// through the posed bone. False is retail's raw leg (no def, a zero index,
// no posed model): nothing is written and the caller copies the raw pose.
// [orig: Entity_ComputeUserpointWorldTransform @0x545CC6..0x545D85;
//  Entity_ComputeUserpointTransform @0x545AAC..0x545BA5]
bool carrier_weapon_userpoint(World &, const Entity &carrier, const WeaponTableEntry *fired,
                              int32_t clip, int column, int32_t out[6],
                              int32_t out_direction[3] = nullptr);

// Entity_ComputeUserpointWorldTransform for a fired slot: the weapon point,
// else the carrier's raw pose (out_direction then stays unwritten).
// [orig: Entity_ComputeUserpointWorldTransform @0x545C60; raw copy @0x545E1F]
bool carrier_weapon_world_pose(World &, const Entity &carrier, const WeaponTableEntry *fired,
                               int32_t clip, int column, int32_t out[6],
                               int32_t out_direction[3] = nullptr);

// The UseGun gunner branch of the fire position, shared by every mounted
// shooter's fire tick: a G-attached gun routed to its parent slot fires from
// the HULL through the local-space helper, any other gun through its own
// world-space helper. The fired slot's def picks the gfx3 launch point;
// clip_before_consume the barrel, in column 0.
// [orig: Entity_CalcWeaponFirePosition gunner branch @0x4DC7A0..0x4DC802]
void usegun_fire_pose(World &, const Entity &gun, const WeaponTableEntry *fired,
                      int32_t clip_before_consume, int32_t out[6]);
// The ctrlx seat's shot pose: the EWeap carrier's own weapon userpoint.
// [orig: Entity_CalcWeaponFirePosition @0x4DC803..0x4DC846]
void controller_fire_pose(World &, Entity &carrier, const WeaponTableEntry *fired,
                          int32_t clip_before_consume, int32_t out[6]);

// The seat/equip gates the switch walks consume.
// [orig: the parentSlot {2,3,5} stance gate @ 0x4e0192; the equip-commit
//  defer gates {2,3} @ 0x4dd6fc]
WeaponSwitchGates local_weapon_switch_gates(const World &world,
        const LocalPlayerWeapon &w, const WeaponInventory *inventory);

// UseGun borrow staging/commit + the emplaced instant-switch predicate.
bool local_usegun_switch_is_instant(const World &world,
                                    const LocalPlayerWeapon &w);
void queue_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w,
                                      bool same_category);
void commit_local_usegun_weapon_switch(World &world, LocalPlayerWeapon &w);
void sync_local_usegun_weapon_transition(World &world, LocalPlayerWeapon &w,
                                         PlayerViewState &view);

// The pending -> equipped commit and the shared switch-outcome routing.
void commit_pending_weapon_switch(World &world, LocalPlayerWeapon &w,
                                  WeaponInventory *inventory);
void handle_weapon_switch_outcome(World &world, LocalPlayerWeapon &w,
                                  WeaponInventory *inventory,
                                  const WeaponSwitchOutcome &out, PlayerViewState &view);


// The FP channel's two legs, run by the pump on the FSM's events against the
// weapon table's shared rings: a play of `key` (false for slot 0, whose play
// does nothing) and one gated advance. [orig: AnimMap_PlayAnimBySlot
// @ 0x40bda0; AnimChannel_AdvanceDispatch @ 0x40b960 ->
// AnimChannel_AdvancePlayback @ 0x40b140 / AdvanceBlendedPlayback @ 0x40B1E0]
bool fp_channel_play(anim::AdmRingTable &rings, LocalPlayerWeapon &w, const std::string &key);
void fp_channel_advance(anim::AdmRingTable &rings, LocalPlayerWeapon &w);

// Install/rebake, clear, and the latching input writer.
void local_weapon_install(World &world, LocalPlayerWeapon &w,
                          const WeaponInstallData &data,
                          bool preserve_slot_state,
                          bool allow_same_weapon_rebake,
                          WeaponInventory *inventory, PlayerViewState &view);
// A zoom step on the personal slot lands in its inventory entry, the storage
// the next mount reads back: retail's EquippedSlot IS that slot-table entry,
// so the step's store is the entry's own zoom [orig: Player_AdjustWeaponElevation
// stores MountSlot+0xC @0x4DBE6C]. A borrowed UseGun slot, and an install the
// equipped entry does not back (a shell-installed def), store nothing.
void local_weapon_store_scope_zoom(const World &world, const LocalPlayerWeapon &w,
                                   WeaponInventory &inventory);
void local_weapon_clear(LocalPlayerWeapon &w, PlayerViewState &view);

// The LOCAL branch of retail's held-weapon draw gate: the weapon model is
// drawn iff the soldier may FIRE it — one predicate serves both, which is why
// a dead, seated or dry-magazine player simply has no gun in his hands.
// Seat rule, LOCAL flavour: control/driver always hide; the gunner seat hides
// only while the third-person camera is up (the remote flavour hides all
// three — seat_type_blocks_weapon_channel). A weapon with no FIRST-person
// model hides on your OWN body even though every observer still sees it —
// retail asymmetry, not a bug.
// [orig: Entity_CanFireWeapon @ 0x4dcb10 — the local branch
//  @ 0x4dcbcf..0x4dcc5d; dead @0x4dcb22; no EquippedSlot @0x4dcbcf; no Def
//  @0x4dcbda; the ammo leg @0x4dcbea -> Entity_GetScoreValueBySlotType
//  @0x5406E0 (weapon_pool_get); no-1P-model @0x4dcc32; the mount split
//  @0x4dcc42..0x4dcc5d]
bool local_held_weapon_visible(const World &world, const Entity &entity,
		const LocalPlayerWeapon &weapon, const WeaponInventory &inventory,
		bool third_person);
void local_weapon_set_input(LocalPlayerWeapon &w, const PlayerViewState &view,
                            bool fire_held, bool fire_pressed,
                            bool reload_pressed);

// Why the pump would drop this tick's weapon input, if it would. The pump's
// own gate (dead, a pending UseGun switch, a Controller/Driver seat) evaluated
// as a value, so a tool can refuse a trigger with the reason instead of
// queueing input the pump silently zeroes.
// [orig: Player_IsOpticalViewVisible @0x5cf780 rejects a Controller or Driver seat;
//  the dead/switch legs are the pump's own early-outs]
enum class LocalWeaponInputBlock : uint8_t {
    kNone,
    kInactive,      // no weapon installed
    kDead,
    kUseGunSwitch,  // a UseGun attach/swap/detach is staged
    kSeat,          // the occupied seat blocks firing
};
LocalWeaponInputBlock local_weapon_input_block(const World &world,
                                               const LocalPlayerWeapon &w);
// A short reason for each block, "" for kNone.
const char *local_weapon_input_block_name(LocalWeaponInputBlock block);

// One 62.5 Hz pump of the local player's slot: the local player's visit in
// the world's weapon-action walk (LocalPlayer::pump_local_weapon; the AI pump
// never advances L's slot as well).
void local_weapon_pump_tick(World &world, LocalPlayerWeapon &w,
                            LocalWeaponPumpIO &io);

// Arm or disarm the devtools tick trace. Arming sizes the ring and starts it
// empty; disarming releases it, so a closed Weapon window costs the pump one
// bool test and no memory.
void weapon_trace_arm(LocalPlayerWeapon &w, bool armed);
// Drop every recorded sample, keeping the ring armed.
void weapon_trace_clear(LocalPlayerWeapon &w);
// The recorded samples oldest-first (empty when disarmed).
std::vector<WeaponTraceSample> weapon_trace_samples(const LocalPlayerWeapon &w);
// The incremental read a per-frame consumer wants: appends to `out`, oldest
// first, only the samples newer than `after_tick` (every sample when
// `take_all`), walking back from the write head so the common empty delta
// touches nothing. Returns the newest recorded tick (0 when empty), which a
// consumer compares against its cursor to notice a restarted logic clock.
uint32_t weapon_trace_samples_since(const LocalPlayerWeapon &w, uint32_t after_tick,
                                    bool take_all, std::vector<WeaponTraceSample> &out);


// The local player's equipped-weapon FSM view for one tick, as one value
// (the fill below carries the field witnesses): the action ladder position,
// the FP clip channel, the last action's audio/effect legs, the event
// serials, the magazine, heat and recoil, the HUD crosshair spread in
// retail's integer domains, the PowerThrow windup, the emplaced-gun
// controls, the round-ring diagnostics and the 3P body weapon channel.
// `active` false = no weapon FSM installed (every other field reads its
// default). Its Godot record wraps it by value (ADR 0043 d10).
struct LocalPlayerWeaponView {
    bool active = false;
    int32_t current_action = 0;
    int32_t next_action = 0;
    int32_t phase = 0;
    int32_t switch_deferred_action = -1;
    bool switch_in_flight = false;
    int32_t pending_combo = 0;
    std::string anim_key;
    int32_t anim_variant = 0;
    int32_t anim_advance_ticks = 0;
    // The channel's blend half while a loop wrap fades the next ring entry
    // in: the pose is the primary clip slerped toward it by the weight.
    // [orig: AnimChannel_BlendTwoChannels @ 0x410DBD]
    bool anim_blending = false;
    std::string anim_blend_key;
    int32_t anim_blend_variant = 0;
    int32_t anim_blend_ticks = 0;
    float anim_blend_weight = 0.0f;
    int32_t play_serial = 0;
    int32_t action_serial = 0;
    int32_t action_started = -1;
    std::string action_soundset;
    std::string action_particle;
    std::string action_particle_userpoint;
    int32_t action_end_serial = 0;
    std::string action_end_soundset;
    bool windup_active = false;
    int32_t windup_held_ticks = 0;
    int32_t fired_serial = 0;
    int32_t tracer_counter = 0;
    int32_t dry_serial = 0;
    int32_t reload_serial = 0;
    int32_t reload_applied_serial = 0;
    int32_t reload_received_serial = 0;
    int32_t reload_received_entity = 0;
    int32_t reload_received_param = 0;
    int32_t unscope_serial = 0;
    int32_t rescope_serial = 0;
    int32_t clip = 0;
    int32_t reserve = 0;
    int32_t kick = 0;
    int32_t recoil_pitch_bam = 0;
    int32_t weapon_weight_spread_bam = 0;
    bool aimed_shot_available = false;
    int32_t hud_spread_row = 0;
    int32_t hud_spread_fp16 = 0;
    int32_t heat = 0;
    int32_t heat_glow = 0;
    bool borrowed_usegun_slot = false;
    int32_t usegun_mount_handle = EntityHandle::kInvalid;
    // The def half of the pump's FP bit: the gfx1 model loaded and its
    // animadm installed (player_weapon_view.cpp carries the witness).
    bool first_person_action_model = false;
    bool emplaced_controls_valid = false;
    int32_t emplaced_gun_yaw = 0;
    int32_t emplaced_gun_pitch = 0;
    int32_t emplaced_spin_phase = 0;
    int32_t round_ring_count = 0;
    int32_t last_round_flags = 0;
    int32_t last_round_subtype = 0;
    int32_t last_round_slot_byte = 0;
    int32_t last_round_seq = 0;
    std::string body_anim_key;
    int32_t body_anim_phase = 0;
    std::string body_anim_prev_key;
    int32_t body_anim_prev_phase = 0;
    float body_anim_blend_weight = 1.0f;
    int32_t body_anim_variant = 0;
    int32_t body_anim_prev_variant = 0;
};

// The fill (player_weapon_view.cpp), from the live world, the local weapon
// aggregate and the inventory's staged combo slot.
LocalPlayerWeaponView local_player_weapon_view(const World &world, const LocalPlayerWeapon &w,
                                               const WeaponInventory &inventory);

} // namespace opennova::world
