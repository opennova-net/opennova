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

// A per-key clip-variant ring: playback serves the head then advances — the
// authored duplication is the rotation weighting. Keys are stored lowercased.
// [orig: the animState slot heads (+72) built by AnimMap_RegisterBoneNode
//  @ 0x40c2d0; Anim_GetDurationTicks @ 0x53ee10]
struct WeaponClipRing {
    std::vector<float> lengths;
    int head = 0;
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
    std::vector<std::pair<std::string, WeaponClipRing>> clip_rings;

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
    std::string anim_key;
    // The FP animadm channel position in 62 Hz ticks: the count of gated
    // advances since the play (WeaponFsmEvents::advance_anim), NOT the play's
    // wall-clock age — ticks outside the counter window do not move the clip.
    // [orig: the channel t maintained by AnimChannel_AdvancePlayback @ 0x40b140]
    uint32_t anim_advance_ticks = 0;
    int32_t anim_variant = 0;
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
    uint32_t anim_map_serial = 0;

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
    int32_t attack_anim = 0;
    int32_t run_anim = 0;
    int32_t clipsize = 0;
    int32_t startrounds = 0;
    std::vector<WeaponFsmActionRow> rows;
    // Per-key clip-variant lengths in seconds (keys any case; stored lowered).
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

// Shared local fire/aim pose, including AbsorbPitch and mounted barrels.
// clip_before_consume selects the barrel before the FSM spends ammo.
// [orig: Entity_CalcWeaponFirePosition @0x4DC750]
void local_weapon_fire_pose(World &, const LocalPlayerWeapon &,
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

// Ring reads: serve the head then advance (the consuming duration read the
// bake performs, and the play latch the anim events record).
WeaponClipRing *weapon_ring_for(LocalPlayerWeapon &w,
                                const std::string &key_lower);
float weapon_ring_take_length(LocalPlayerWeapon &w, const char *key);
int weapon_ring_take_variant(LocalPlayerWeapon &w, const std::string &key);

// Install/rebake, clear, and the latching input writer.
void local_weapon_install(World &world, LocalPlayerWeapon &w,
                          const WeaponInstallData &data,
                          bool preserve_slot_state,
                          bool allow_same_weapon_rebake,
                          WeaponInventory *inventory, PlayerViewState &view);
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
// [orig: Player_CanFireWeapon @0x5cf780 rejects a Controller or Driver seat;
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

// One 62.5 Hz pump of the local player's slot, after the world logic tick
// (the world's own pump skips L — external_local_mounted_weapon_pump).
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


// The local player's equipped-weapon FSM view for one tick, as one value the
// embedder fills (Simulation::get_local_player_weapon_state carries the
// field witnesses): the action ladder position, the FP clip channel, the last
// action's audio/effect legs, the event serials, the magazine, heat and
// recoil, the HUD crosshair spread in retail's integer domains, the
// PowerThrow windup, the emplaced-gun controls, the round-ring diagnostics
// and the 3P body weapon channel. `active` false = no weapon FSM installed
// (every other field reads its default). Its Godot record wraps it by value
// (ADR 0043 d10).
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
    bool emplaced_controls_valid = false;
    int32_t emplaced_gun_yaw = 0;
    int32_t emplaced_gun_pitch = 0;
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

} // namespace opennova::world
