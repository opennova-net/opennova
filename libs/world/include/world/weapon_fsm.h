// First-person weapon action FSM: the 12-state action queue on the equipped weapon
// slot, driven at the 62.5 Hz world tick. Structural translation of the witnessed
// original — the weapon.def ACTION rows bind into a per-weapon 12-slot action table
// (suffix-keyed), the per-tick pump advances {current, next, counter, phase}, and the
// handlers make the witnessed decisions (recoil is the arbiter).
// [orig: pump WeaponAction_ProcessFrame @ 0x540e60 (driver WeaponAction_ProcessAllEntities
//  @ 0x542690); bind/bake Anim_InitActions @ 0x541fa0; row parse ActionDef_ParseScriptLine
//  @ 0x4023c0; suffix/default table @ 0x830B90; handlers 0x542920..0x543500;
//  witness record docs/net/novaworld-net-re.md §5.62]
//
// Deliberately def-lib-agnostic (ADR 0020 shape: libs/world consumes no format stack):
// callers feed plain WeaponFsmActionRow mirrors of the parsed DefWeaponAction rows plus a
// clip-duration callback; the bake produces the runtime table exactly as the original does.
#ifndef OPENNOVA_WORLD_WEAPON_FSM_H
#define OPENNOVA_WORLD_WEAPON_FSM_H

#include <cstddef>
#include <cstdint>

namespace opennova::world {

// Action ids, in suffix-table order. [orig: {suffix, default handler} pairs @ 0x830B90:
// idle emptyidle fire recoil reload empty switchto switchfrom switchrank scopeup
// scopedown overheated]
namespace weapon_action {
enum : int32_t {
    kIdle = 0,
    kEmptyIdle = 1,
    kFire = 2,
    kRecoil = 3,
    kReload = 4,
    kEmpty = 5,       // dry-fire click (distinct from the emptyidle LOOP)
    kSwitchTo = 6,
    kSwitchFrom = 7,
    kSwitchRank = 8,
    kScopeUp = 9,
    kScopeDown = 10,
    kOverheated = 11, // idle handler on the default table [orig: @ 0x830BE8]
    kCount = 12,
};
} // namespace weapon_action

// The 12 action-name suffixes, index == action id. [orig: strings @ 0x7C2FB4 etc.,
// paired in the default table @ 0x830B90]
extern const char *const kWeaponActionSuffixes[weapon_action::kCount];

// Phase byte values (MountSlot+0x5A). [orig: transition write @ 0x5413ec; the
// begin-active shims @ 0x53f830 / 0x541860 / 0x5419e0; finish @ 0x53f7b0]
namespace weapon_phase {
enum : uint8_t {
    kNone = 0,      // fresh slot
    kEntered = 1,   // set by the pump transition; the first handler tick plays the anim
    kActive = 2,    // anim playing / counter ticking
    kDone = 4,      // handler finished; pending action may transition in
    kHeld = 0x40,   // held-ready variant (the pump's second transition path)
    kReloadPendingBit = 0x80, // OR'd while the reload request is in flight
                              // [orig: @ 0x543108; cleared by the first shim tick's
                              //  phase=2 write and by WeaponSlot_ReloadAmmo @ 0x5417a2]
};
} // namespace weapon_phase

// One parsed weapon.def ACTION block, def-lib-agnostic (mirror of DefWeaponAction's
// FSM-relevant fields; name/anim/function semantics per ActionDef_ParseScriptLine
// @ 0x4023c0). delaystart/delayend: ticks, -1 = 'auto' (bake from the clip).
struct WeaponFsmActionRow {
    char name[64] = {};
    char anim[128] = {};
    char function[128] = {};
    int32_t delaystart = -1;
    int32_t delayend = -1;
};

// A baked runtime action slot. [orig: ActionDef pool entry — delayStart/+0x24,
// delayEnd/+0x28 (dwords +9/+10), anim name +58, resolved anim slot +24]
struct WeaponFsmAction {
    int32_t delay_start = 0;
    int32_t delay_end = 0;
    bool has_anim = false;   // anim name present AND the clip resolved
    char anim_key[64] = {};  // the .adm clip key (ACTION rows name them directly,
                             // e.g. "anim_wpn_fire")
};

// The per-weapon def slice the FSM consumes. Flag bits are the witnessed WeaponDef+8
// bits (libs/def flag_table maps the file tokens): auto = 0x100
// [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0], burst = 0x20
// [orig: WeaponAction_Fire burst block @ 0x542c8a].
struct WeaponFsmDef {
    WeaponFsmAction actions[weapon_action::kCount];
    bool auto_fire = false;
    bool burst3 = false;
    int32_t clip_capacity = 0; // rounds per clip; < 0 = infinite (the def+0x58 == -1 paths)
    int32_t flags = 0;         // the raw weapon.def flag mask (scoped 1 / sighted 2 / ...)
};

// ms -> 62.5 Hz ticks. [orig: Anim_GetDurationTicks @ 0x53ee10 = ms * 62.5 / 1000 + 1
// (flt_7C3B3C)]
int32_t weapon_anim_ticks_from_ms(int32_t ms);

// Clip-duration source for the bake: clip length in SECONDS for an .adm key,
// < 0 when the key does not resolve.
using WeaponClipSecondsFn = float (*)(void *ctx, const char *anim_key);

// Bind the 12 action slots from the weapon's parsed ACTION rows — the Anim_InitActions
// structural translation. Rows bind by suffix name (the original registers each row as
// "<weaponName>_<suffix>" in a global pool and looks the composite back up per slot
// [orig: @ 0x4023d5 prefix concat / @ 0x5420c6 lookup]; per-weapon rows + bare-suffix
// match is the same binding). Missing rows become generated defaults. 'auto' (-1)
// delays bake from the clip: delaystart = ticks; delayend = ticks, or ticks -
// delaystart when ticks > delaystart; no anim / unresolved clip -> 0.
// [orig: @ 0x5421b3..0x5421ec / 0x542152..0x542164]
// FUNCTION rows are not consulted: every shipped row names the standard handler for its
// own suffix (wpn_std_<suffix>, JOX + REVX corpora), so the per-state behavior is fixed
// (divergence D-WPN-1).
void weapon_fsm_bake(const WeaponFsmActionRow *rows, size_t row_count,
                     WeaponClipSecondsFn clip_seconds, void *ctx, WeaponFsmDef &out);

// The MountSlot FSM fields. [orig: MountSlot (100 B): counter +0, clip u16 +0x10,
// currentAction +0x2C, nextAction +0x30, prevAction +0x34, switchTimer +0x58,
// phase +0x5A, kickIntensity +0x5B, burstCounter +0x62]
struct WeaponSlotState {
    int32_t counter = 0;
    int32_t current = weapon_action::kIdle;
    int32_t next = weapon_action::kIdle;
    int32_t prev = weapon_action::kIdle;
    uint8_t phase = weapon_phase::kNone;
    int16_t switch_timer = 0;
    uint8_t kick = 0;
    uint8_t burst = 0;
    int32_t clip = 0;          // rounds in the magazine (MountSlot+0x10 low u16)
    int32_t reserve = 0;       // carried pool, in rounds (the per-class pool via
                               // Entity_GetScoreValueBySlotType @ 0x5406e0; single-class
                               // model, D-WPN-2)
    // Scope stash across a reload: reload unscoped us, rescope when it completes.
    // [orig: g_rescopeAfterReload @ 0xB7647C; write @ 0x54312f, consume @ 0x54139e]
    bool rescope_after_reload = false;
};

// Per-tick inputs (the input-dispatcher writers run before the pump).
struct WeaponFsmInputs {
    bool fire_pressed = false;  // the binding-149 activation edge
                                // [orig: Player_RequestPrimaryFire @ 0x5414c0]
    bool fire_held = false;     // held state; autos re-request per tick and the recoil
                                // arbiter re-queues on it [orig: Input_IsBindingActive(149)
                                // @ 0x542e7f]
    bool reload_pressed = false; // the reload-key edge (case 0xD3 gates applied by caller)
    bool is_local = true;        // owner == g_local_player_entity paths
    bool is_authority = true;    // listen-host/SP: reload requests apply immediately
    bool auto_reload = true;     // [orig: g_autoReloadEnabled @ 0x24D2118]
    bool scope_active = false;   // g_weaponScopeActive at reload time (the stash source)
};

// Per-tick outputs for the host. anim events carry the .adm clip key to start on the
// viewmodel parts (arms + gun share the animadm channel).
struct WeaponFsmEvents {
    bool play_anim = false;
    char anim_key[64] = {};
    bool fired = false;            // Entity_FireWeaponAndSendPacket seam [orig: @ 0x542c5e]
    bool dry_fired = false;        // the EMPTY one-shot entered
    bool reload_requested = false; // C2S 0x25 seam [orig: @ 0x5430ff; net-re §5.58]
    bool reload_applied = false;   // authority refill ran (WeaponSlot_ReloadAmmo shape)
    bool unscope = false;          // g_weaponScopeActive = 0 writes (one-shot/empty paths)
    bool rescope = false;          // the pump's rescope-after-reload block [orig: @ 0x54139e]
};

// Request writers (the input-dispatcher sites).
// [orig: WeaponSlot_RequestFire @ 0x53efa0] AUTO (Flags&0x100): current {0,3,9,10} ->
// next=FIRE, {1} -> next=EMPTY, {2} -> deferred re-queue (the per-tick held re-request
// covers it); SEMI: {0} -> FIRE, {1} -> EMPTY. Returns true when a fire was queued.
bool weapon_fsm_request_fire(const WeaponFsmDef &def, WeaponSlotState &slot);
// [orig: WeaponSlot_RequestReload @ 0x53f110] next {0,1,11} and no reload pending
// (phase sign bit) -> next = RELOAD.
void weapon_fsm_request_reload(WeaponSlotState &slot);
// [orig: WeaponSlot_TryQueueScopeUp @ 0x53f050 / ..ScopeDown @ 0x53f080] queue 9/10 when
// phase is {0,4} and the state isn't already current; else queue idle.
void weapon_fsm_queue_scope_up(WeaponSlotState &slot);
void weapon_fsm_queue_scope_down(WeaponSlotState &slot);

// The input-dispatcher gates in front of the requests
// [orig: Input_HandleActionBinding_0 @ 0x4e0420]:
// reload (case 0xD3) is refused on a full magazine or an empty reserve (and for
// defs without a clip);
bool weapon_fsm_reload_allowed(const WeaponFsmDef &def, const WeaponSlotState &slot);
// the scope toggle (case 6) is refused while RELOAD or SWITCHFROM is current and
// for defs without the scoped/sighted flags
// [orig: + Player_ToggleWeaponScope @ 0x4df0c0 gates def Flags & 3].
bool weapon_fsm_scope_toggle_allowed(const WeaponFsmDef &def, const WeaponSlotState &slot);

// One 62.5 Hz tick of the pump + the current action's handler.
void weapon_fsm_tick(const WeaponFsmDef &def, WeaponSlotState &slot,
                     const WeaponFsmInputs &in, WeaponFsmEvents &out);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WEAPON_FSM_H
