// First-person weapon action FSM — see weapon_fsm.h.
// [orig: WeaponAction_ProcessFrame @ 0x540e60 + the wpn_std_* handlers @ 0x542920..
//  0x543500; Anim_InitActions @ 0x541fa0; docs/net/novaworld-net-re.md §5.62]

#include "world/weapon_fsm.h"

#include <cstring>

namespace opennova::world {

// [orig: WeaponSlot_CalcAccumulatedHeat @ 0x53f780] — heat = rate x ticks remaining.
// Gated on the DEF field, not the slot's, so a weapon with no heat model reads 0 even
// if a stale window survived a def swap.
int32_t weapon_slot_accumulated_heat(const WeaponFsmDef &def, const WeaponSlotState &slot,
                                     int32_t current_tick) {
    if (def.heat_per_shot == 0) return 0;
    if (slot.heat_window_end_tick <= current_tick) return 0;
    // Retail's two-operand IMUL keeps only the low dword. Express the
    // subtraction and multiplication in unsigned bits, then preserve those
    // bits in the signed return value; signed C++ overflow would be undefined.
    // [orig: WeaponSlot_CalcAccumulatedHeat @ 0x53F7A9..0x53F7B1]
    const uint32_t ticks_remaining =
            static_cast<uint32_t>(slot.heat_window_end_tick) -
            static_cast<uint32_t>(current_tick);
    const uint32_t product =
            static_cast<uint32_t>(def.heat_decay_per_tick) * ticks_remaining;
    int32_t result = 0;
    std::memcpy(&result, &product, sizeof(result));
    return result;
}

int32_t weapon_slot_world_heat_glow(const WeaponFsmDef &def,
                                    const WeaponSlotState &slot,
                                    int32_t current_tick) {
    // Retail tests the inline slot's deadline before calling the accumulator,
    // writes a literal zero on the cold leg, then caps the hot result at the
    // largest unsigned word before storing it on the signed-dword CTRL bus.
    // Preserve the final zero-extension as well as the 0xFFFF ceiling.
    // [orig: HUD_CacheWeaponSlotInfo cold store @ 0x440969;
    //  accumulator call/cap/store @ 0x440974..0x440991]
    if (slot.heat_window_end_tick <= current_tick) return 0;
    int32_t heat = weapon_slot_accumulated_heat(def, slot, current_tick);
    if (heat > 0xFFFF) heat = 0xFFFF;
    return static_cast<uint16_t>(heat);
}

namespace {

// ASCII case-insensitive compare (the original binds action names via stricmp
// [orig: ActionDef_FindByNameInTable @ 0x402360 / ActionDef_ParseScriptLine @ 0x4023f3]).
bool name_equals_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

void copy_key(char (&dst)[64], const char *src) {
    std::strncpy(dst, src, sizeof(dst) - 1);
    dst[sizeof(dst) - 1] = '\0';
}

void copy_str128(char (&dst)[128], const char *src) {
    std::strncpy(dst, src, sizeof(dst) - 1);
    dst[sizeof(dst) - 1] = '\0';
}

// Recoil kick accumulation, capped at 20 (signed-char compare in the original).
// [orig: @ 0x53f7f0..0x53f805 / @ 0x542cf1..0x542d0f]
void kick_add(WeaponSlotState &slot, int32_t amount) {
    int32_t v = static_cast<int32_t>(slot.kick) + amount;
    if (v > 20) v = 20;
    if (v < 0) v = 0;
    slot.kick = static_cast<uint8_t>(v);
}

bool has_rounds(const WeaponFsmDef &def, const WeaponSlotState &slot) {
    // Infinite-clip weapons (the def clip field == -1 paths [orig: @ 0x542deb /
    // @ 0x54296c]) always pass; otherwise the magazine u16.
    return def.clip_capacity < 0 || slot.clip > 0;
}

// The witnessed ammo gate the fire handler re-checks on its entry tick. On an empty
// magazine it WRITES the queued next action itself: RECOIL(3) when the carried reserve
// has rounds (routing the empty trigger into the recoil arbiter's auto-reload
// decision), else EMPTYIDLE(1) [orig: WeaponSlot_CanFire @ 0x541ba0, the empty leg
// @ 0x541c8b..0x541cb9]. The busy-weapon-child, underwater-fire, and score-lock legs
// need entity/env state this port does not model yet (divergence D-WPN-3).
bool can_fire_ammo(const WeaponFsmDef &def, WeaponSlotState &slot) {
    if (def.clip_capacity < 0) return true; // no clip tracking (knife/thrown legs)
    if (slot.clip > 0) return true;
    slot.next = slot.reserve > 0 ? weapon_action::kRecoil : weapon_action::kEmptyIdle;
    return false;
}

// The begin-active shim shared by every handler's tick path: the first tick after a
// transition (phase 1, or the held-ready 0x40) flips the slot ACTIVE and starts the
// action's clip on the owner's animadm channel — LOCAL PLAYER ONLY in the original.
// Only a real phase-1 entry runs the begin sound/effect leg: held-ready resumes at
// the animation call inside BeginActivePhase, after that leg has already been skipped.
// [orig: ActionSlot_BeginActivePhase @ 0x53f830; the FP-routing variants
//  ActionSlot_ExecuteActionWithEffect @ 0x541860 / ..NoEffect @ 0x5419e0 write the same
//  phase protocol; held-ready animation branch @ 0x53f88b]
void begin_active(const WeaponFsmAction &desc, WeaponSlotState &slot,
                  const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    const bool entered = (slot.phase & 1) != 0;
    const bool held = (slot.phase & weapon_phase::kHeld) != 0;
    if (entered || held) {
        slot.phase = weapon_phase::kActive;
        // The begin leg's sound/muzzle seam belongs only to phase-1 entry. The
        // held-ready 0x40 path rejoins below at animation playback.
        // [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860 ->
        //  ActionSlot_SpawnEffect @ 0x401f20; held branch @ 0x53f88b].
        if (entered) {
            out.action_started = desc.id;
        }
        if (desc.has_anim && in.is_local) {
            out.play_anim = true;
            copy_key(out.anim_key, desc.anim_key);
        }
    }
    // else: the anim channel keeps advancing on its own (the embedder owns clip playback).
}

// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 (desc, slot, entity, nextAction)]:
// counter = delayEnd, nextAction = the passed value, the END-leg sound (ACTIVE only),
// the ACTIVE->DONE kick bump (skipped for RELOAD), phase = DONE. The original gates the
// kick on the weapon's fire-sound id being set (Def+0x294) — every shipped weapon
// carries one (D-WPN-3).
void finish_active(const WeaponFsmAction &desc, WeaponSlotState &slot, int32_t next,
                   WeaponFsmEvents &out) {
    const bool was_active = slot.phase == weapon_phase::kActive;
    slot.counter = desc.delay_end;
    slot.next = next;
    if (was_active) {
        // The end-leg sound plays only when the phase byte was 2 (ACTIVE) at entry —
        // a phase-1 abort (the fire CanFire refusal) finishes silently.
        // [orig: @ 0x53f7b9 phase==2 latch -> the end shim @ 0x53f7d6 (sub_401100
        //  plays ActionDef+12); the shim's dupsound repeat loop (+44 count / +48
        //  interval) is data-dead in the JOX/REVX corpora]
        out.action_finished = desc.id;
        if (slot.current != weapon_action::kReload)
            kick_add(slot, desc.delay_start + desc.delay_end + slot.counter + 10);
    }
    slot.phase = weapon_phase::kDone;
}

// The authority-side clip refill: refund the remaining magazine into the pool, then
// refill to capacity clamped by what the pool affords. Single-class round pool, one
// pool unit per round (D-WPN-2). [orig: WeaponSlot_ReloadAmmo @ 0x541720 — refund
// @ 0x541811, refill clamp @ 0x541850; net-re §5.58]
void apply_reload_ammo(const WeaponFsmDef &def, WeaponSlotState &slot) {
    if (def.clip_capacity < 0) return;
    slot.reserve += slot.clip;
    slot.clip = def.clip_capacity < slot.reserve ? def.clip_capacity : slot.reserve;
    slot.reserve -= slot.clip;
    slot.phase = static_cast<uint8_t>(slot.phase & ~weapon_phase::kReloadPendingBit);
}

// --- handlers -------------------------------------------------------------------

// [orig: WeaponAction_Idle @ 0x542920] The idle LOOP: the enter/replay branch restarts
// the global wpn_idle clip (slot 241 -> the .adm 'anim_wpn_idle' key) and runs the
// empty-magazine decision; the tick branch is the begin-active shim.
void handler_idle(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                  WeaponSlotState &slot, const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    if (slot.counter != 0 || slot.phase == weapon_phase::kDone) {
        begin_active(desc, slot, in, out);
        return;
    }
    // UNGUARDED, deliberately: the original plays this on EVERY owner, local or not
    // [orig: AnimMap_PlayAnimBySlot(weaponDefPtr->field_174, 241) @ 0x542955 — no
    // owner test]. Every sibling play onto that object IS gated on
    // `ownerEntity == g_local_player_entity` [orig: ActionSlot_ExecuteActionWithEffect
    // @0x541893/@0x54195A/@0x5419B8], so retail's omission here is a defect — and a
    // live A/B on a stock host confirmed it: with two retail players on the SAME
    // weapon, the host's own first-person gun visibly reacted to the other player's
    // shots, stopped the instant either switched weapon, and never affected the
    // non-authority side. That is D-NET-184; the shared object it corrupts is the
    // per-WeaponDef field_174. We keep the original's shape rather than "improving"
    // it, but the symptom cannot follow: our anim state is per-entity, and the only
    // consumer of play_anim is the LOCAL viewmodel pump.
    out.play_anim = true;
    copy_key(out.anim_key, "anim_wpn_idle");
    slot.phase = weapon_phase::kDone;
    if (def.clip_capacity < 0) return;   // [orig: @ 0x54296c infinite -> effects only]
    if (has_rounds(def, slot)) return;   // [orig: @ 0x542981]
    if (slot.reserve > 0 && in.auto_reload) { // [orig: @ 0x5429ac g_autoReloadEnabled]
        weapon_fsm_request_reload(slot);
        return;
    }
    slot.next = weapon_action::kEmptyIdle; // [orig: @ 0x5429cf]
    // One-shot weapons drop the scope with the last round — unless ForceScoped
    // (0x20000000) pins the sight view. [orig: @ 0x5429ee g_weaponScopeActive = 0]
    if (in.is_local && def.clip_capacity == 1 && (def.flags & weapon_flag::kForceScoped) == 0)
        out.unscope = true;
}

// [orig: WeaponAction_EmptyIdle @ 0x542a20] Same LOOP shape on the global
// wpn_empty_idle clip (slot 242); reserve available -> auto reload (unconditional,
// no g_autoReloadEnabled gate here); else keep holding (next = self).
void handler_emptyidle(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                       WeaponSlotState &slot, const WeaponFsmInputs &in,
                       WeaponFsmEvents &out) {
    if (slot.counter != 0 || slot.phase == weapon_phase::kDone) {
        begin_active(desc, slot, in, out);
        return;
    }
    // UNGUARDED for the same reason as handler_idle's slot-241 play — the original
    // has no owner test here either [orig: AnimMap_PlayAnimBySlot(weaponDefPtr->
    // field_174, 242) @ 0x542a53]. See D-NET-184.
    out.play_anim = true;
    copy_key(out.anim_key, "anim_wpn_empty_idle");
    if (def.clip_capacity >= 0 && !has_rounds(def, slot)) {
        if (slot.reserve > 0) {
            weapon_fsm_request_reload(slot); // [orig: @ 0x542aa3]
        } else {
            slot.next = weapon_action::kEmptyIdle; // hold [orig: @ 0x542ab2]
            if (in.is_local && def.clip_capacity == 1 && (def.flags & weapon_flag::kForceScoped) == 0)
                out.unscope = true; // [orig: @ 0x542ad1; ForceScoped pins the view]
        }
    }
    slot.phase = weapon_phase::kDone; // [orig: @ 0x542ae1]
}

// [orig: WeaponAction_Fire @ 0x542b10] The shot: phase-1 ammo recheck (abort keeps the
// queued next), the fire seam, ammo consume, 3-round-burst cycling, the UNCONDITIONAL
// chain to RECOIL, and the kick bump sized by the recoil action.
void handler_fire(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                  WeaponSlotState &slot, const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    if (slot.phase == weapon_phase::kEntered && !can_fire_ammo(def, slot)) {
        // The abort adopts whatever CanFire queued (RECOIL toward auto-reload, or
        // EMPTYIDLE) — the [esi+30h] read happens AFTER the CanFire call. Phase is
        // still 1 here, so the finish plays no end-leg sound.
        // [orig: @ 0x542b44..0x542b5e]
        finish_active(desc, slot, slot.next, out);
        slot.counter = 0;
        return;
    }
    if (slot.counter != 0 || slot.phase == weapon_phase::kDone) {
        begin_active(desc, slot, in, out); // [orig: @ 0x542db9]
        return;
    }
    out.fired_clip_before_consume = slot.clip;
    out.fired = true; // Entity_FireWeaponAndSendPacket seam [orig: @ 0x542c5e]
    if (def.clip_capacity >= 0) { // [orig: consume_weapon_ammo @ 0x542c75, clip leg]
        if (slot.clip > 0) --slot.clip;
    }
    if (def.burst3) // Flags & 0x20 [orig: @ 0x542c8a]
        slot.burst = slot.burst != 0 ? static_cast<uint8_t>(slot.burst - 1) : 2;
    slot.next = weapon_action::kRecoil; // [orig: @ 0x542c9e — hardcoded]
    begin_active(desc, slot, in, out);  // [orig: ExecuteActionTick @ 0x542cb4 runs the
                                        //  same phase-1 play on the fire desc]
    const WeaponFsmAction &recoil = def.actions[weapon_action::kRecoil];
    // [orig: @ 0x542cf1..0x542d0f — recoil ds + de + counter + 10, cap 20]
    kick_add(slot, recoil.delay_start + recoil.delay_end + slot.counter + 10);
    finish_active(desc, slot, slot.next, out); // [orig: @ 0x542d13 push [esi+30h] — keeps
                                               //  3; the finish plays the fire row's
                                               //  soundsetend = the per-shot gunshot]
}

// [orig: WpnAction_Recoil @ 0x542dd0] THE ARBITER: when the recoil clip ends, decide
// refire (burst), idle, auto-reload, or emptyidle; the held-trigger auto refire is the
// deferred re-queue of input binding 149 in the window below [orig: @ 0x542e9d].
void handler_recoil(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                    WeaponSlotState &slot, const WeaponFsmInputs &in,
                    WeaponFsmEvents &out) {
    const bool rounds = has_rounds(def, slot); // [orig: isLocalWeapon calc @ 0x542deb]
    begin_active(desc, slot, in, out);         // [orig: ExecuteActionTick @ 0x542e2a]
    if (slot.phase == weapon_phase::kDone ||
        (desc.delay_start == 0 && desc.delay_end == 0)) {
        // The deferred-refire window — the auto-fire sustainer: the closing ticks of
        // the recoil (counter <= 1 on the post-arbitration delayend ticks, or any tick
        // of a zero-length recoil) re-queue the still-held fire binding as a deferred
        // input event; the next tick's dispatch routes it into RequestFire (RECOIL ->
        // next = FIRE). The ROUNDS gate kills the chain on an empty magazine, so the
        // arbiter's queued RELOAD stands and the volley does NOT resume after the
        // auto-reload without a fresh press.
        // [orig: @ 0x542e7f..0x542e9d Input_QueueDeferredEvent(149, current_tick)]
        if (slot.counter <= 1 && in.is_local && rounds && def.auto_fire &&
            static_cast<int8_t>(slot.burst) <= 0 && in.fire_held)
            slot.refire_queued = true;
        if (desc.delay_start != 0 || desc.delay_end != 0) return; // [orig: @ 0x542eae]
    }
    if (slot.counter != 0) return; // [orig: @ 0x542eb7]
    // The recoil-row DIRECT effect leg (casing eject / bolt smoke) at the arbiter tick:
    // emitted when the row authors a particle. For the local player the original's only
    // extra gate is the FP weapon-view flag (embedder-side; treated always-on) — NOT the
    // scope state — and the spawn never records a live handle (param7=0), so it is
    // never suppressed by a previous casing group still alive.
    // [orig: WeaponAction_Recoil gate @ 0x542efa -> ActionSlot_SpawnEffect @ 0x542f64]
    if (in.is_local && desc.particle[0] != '\0')
        out.action_effect = weapon_action::kRecoil;
    slot.phase = weapon_phase::kDone; // [orig: @ 0x542f74]
    // The per-shot heat stamp. The window is pushed out by however many ticks it
    // takes to shed one shot's worth of heat, so the "accumulator" is really a
    // deadline. A cold slot (or one whose window already lapsed) restarts from now
    // rather than crediting the time it spent cold.
    //
    // The `heat_decay_per_tick > 0` term is ours: the original gates on def+876 alone
    // and would divide by zero on a hypothetical `heat_values <n>,0`. No shipped def
    // authors that, and reproducing a hardware exception is not parity — treat a zero
    // decay as no heat model.
    // [orig: @ 0x542f8b..0x542fdc]
    if (def.heat_per_shot != 0 && def.heat_decay_per_tick > 0) {
        if (slot.heat_window_end_tick < in.current_tick)
            slot.heat_window_end_tick = in.current_tick; // [orig: @ 0x542fa0]
        slot.heat_window_end_tick +=
                def.heat_per_shot / def.heat_decay_per_tick + 1; // [orig: @ 0x542fb4]
        // The ceiling clamp: past it the window is re-stamped to a fixed overrun, so
        // a held trigger cannot bank heat beyond one lockout's worth.
        // [orig: @ 0x542fc4 — the compare is > kCeiling-1, i.e. >= kCeiling]
        if (weapon_slot_accumulated_heat(def, slot, in.current_tick) >
            weapon_heat::kCeiling - 1)
            slot.heat_window_end_tick =
                    in.current_tick +
                    weapon_heat::kCeiling / def.heat_decay_per_tick + 1; // [orig: @ 0x542fdc]
    }
    // (the overheat glow emitter — actionTable[11] muzzle FX @ 0x54109e..0x54122c —
    //  is an embedder effect seam, D-WPN-28)
    if (!in.is_local) { // [orig: @ 0x542fe9 -> LABEL_59]
        slot.counter = desc.delay_end;
        return;
    }
    if (def.clip_capacity < 0 || rounds) {
        // [orig: @ 0x543080] burst continues the volley; else back to idle.
        slot.next = slot.burst != 0 ? weapon_action::kFire : weapon_action::kIdle;
        slot.counter = desc.delay_end; // [orig: @ 0x54309b]
        return;
    }
    // reserve >= a FULL clip (units-per-round folded to 1, D-WPN-2) and auto-reload on.
    // [orig: @ 0x54300f..0x54301d]
    if (slot.reserve >= def.clip_capacity && in.auto_reload) {
        slot.next = weapon_action::kReload;
        return;
    }
    slot.next = weapon_action::kEmptyIdle; // [orig: @ 0x543036]
    if (in.is_local && def.clip_capacity == 1 && (def.flags & weapon_flag::kForceScoped) == 0)
        out.unscope = true; // [orig: @ 0x543053; ForceScoped pins the view]
    // (auto-switch to the def+0x168 follow-up weapon — Player_SwitchToWeaponByHandle
    //  @ 0x54307c — is the weapon-switch seam, D-WPN-5)
}

// [orig: WeaponAction_Reload @ 0x5430b0] First tick (phase 1, no pending bit): emit the
// reload request (C2S 0x25 on a joiner; immediate authority apply on the listen host —
// net-re §5.58), latch the pending bit, stash-and-drop the scope. Clip end: finish with
// next = IDLE and reset the burst counter.
void handler_reload(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                    WeaponSlotState &slot, const WeaponFsmInputs &in,
                    WeaponFsmEvents &out) {
    const bool pending = (slot.phase & weapon_phase::kReloadPendingBit) != 0;
    if (!pending && (slot.phase & 1) != 0 && (in.is_local || in.is_authority)) {
        out.reload_requested = true; // [orig: NetPacket send @ 0x5430ff]
        slot.phase = static_cast<uint8_t>(slot.phase | weapon_phase::kReloadPendingBit);
        if (in.is_local) {
            if ((def.flags & weapon_flag::kForceCrouch) != 0) {
                // The keep-scope reload class (ForceCrouch 0x40000 — the mortars):
                // no stash, no unscope; the sight view rides through the reload.
                // [orig: @ 0x543126 -> g_rescopeAfterReload = 0 @ 0x54313d]
                slot.rescope_after_reload = false;
            } else {
                // Stash the scope across the reload; the pump rescopes on completion.
                // [orig: @ 0x54312f g_rescopeAfterReload = g_weaponScopeActive]
                slot.rescope_after_reload = in.scope_active;
                if (in.scope_active) out.unscope = true; // [orig: Player_ToggleWeaponScope @ 0x543136]
            }
        }
        if (in.is_authority) {
            // Listen-host/SP zero-latency loopback of the §5.58 round-trip: the
            // authority applies WeaponSlot_ReloadAmmo when it relays the request.
            apply_reload_ammo(def, slot);
            out.reload_applied = true;
        }
    }
    begin_active(desc, slot, in, out); // [orig: ExecuteActionTick @ 0x543150]
    if (slot.counter == 0 && slot.phase != weapon_phase::kDone) {
        finish_active(desc, slot, weapon_action::kIdle, out); // [orig: @ 0x54316e push 0]
        slot.burst = 0; // [orig: @ 0x543176]
    }
}

// [orig: WeaponAction_Empty @ 0x543180] The dry-fire click one-shot.
void handler_empty(const WeaponFsmDef &, const WeaponFsmAction &desc,
                   WeaponSlotState &slot, const WeaponFsmInputs &in,
                   WeaponFsmEvents &out) {
    begin_active(desc, slot, in, out); // [orig: ExecuteActionTick @ 0x543193]
    if (slot.counter == 0 && slot.phase != weapon_phase::kDone) {
        slot.counter = desc.delay_end;         // [orig: @ 0x5431b0]
        slot.next = weapon_action::kEmptyIdle; // [orig: @ 0x5431b2]
        slot.phase = weapon_phase::kDone;
    }
}

// [orig: WeaponAction_SwitchTo @ 0x5431d0] The draw: seeds the -900 switch timer,
// +30/tick until positive (~30 ticks = 0.48 s at 62.5 Hz), then DONE. The pending-slot
// swap itself happens in SWITCHFROM; the priority-3 weapon-switch wiring drives these.
void handler_switchto(const WeaponFsmDef &, const WeaponFsmAction &desc,
                      WeaponSlotState &slot, const WeaponFsmInputs &in,
                      WeaponFsmEvents &out) {
    begin_active(desc, slot, in, out); // [orig: ExecuteActionTick @ 0x5431e4]
    if (slot.counter == 0 && slot.phase != weapon_phase::kDone &&
        slot.phase != weapon_phase::kHeld) {
        slot.switch_timer = -900;      // [orig: @ 0x543207]
        slot.counter = desc.delay_end; // [orig: @ 0x543211]
        slot.next = slot.prev;         // [orig: @ 0x543213 — resume the pre-switch state]
        slot.phase = weapon_phase::kDone;
        return;
    }
    // (the instant-switch -901 branch reads the pending slot's def Flags & 0x80 —
    //  weapon-switch seam [orig: @ 0x54323e])
    if (slot.switch_timer <= 0) {
        slot.switch_timer = static_cast<int16_t>(slot.switch_timer + 30); // [orig: @ 0x54327b]
        slot.counter = desc.delay_end;
    } else {
        slot.switch_timer = 0; // [orig: @ 0x54324f]
        slot.counter = 0;
        slot.phase = weapon_phase::kDone;
    }
}

// [orig: WeaponAction_SwitchFrom @ 0x5433b0] The holster: timer runs 0 -> -930 by
// -30/tick; past -900 the original swaps EquippedSlot from g_pendingWeaponSlot and
// queues SWITCHTO on the NEW slot. The swap itself is the weapon-switch seam
// (priority-3 loadout work); this port runs the timing shape on the one slot.
void handler_switchfrom(const WeaponFsmDef &, const WeaponFsmAction &desc,
                        WeaponSlotState &slot, const WeaponFsmInputs &in,
                        WeaponFsmEvents &out) {
    begin_active(desc, slot, in, out); // [orig: ExecuteActionTick @ 0x5433c6]
    if (slot.counter == 0 && slot.phase != weapon_phase::kDone &&
        slot.phase != weapon_phase::kHeld) {
        slot.switch_timer = 0;         // [orig: @ 0x5433e6]
        slot.counter = desc.delay_end; // [orig: @ 0x5433ef]
        slot.next = slot.prev;         // [orig: @ 0x5433f1]
        slot.phase = weapon_phase::kDone;
        return;
    }
    if (in.instant_emplaced_switch) {
        // Either side of an Emplaced handoff uses the -901 sentinel, crossing
        // the ordinary >= -900 timer gate on this handler call.
        // [orig: outgoing/pending Def Flags 0x80 @0x543417..0x54344a]
        slot.switch_timer = -901;
    }
    if (slot.switch_timer >= -900) {
        slot.switch_timer = static_cast<int16_t>(slot.switch_timer - 30); // [orig: @ 0x5434c2]
        slot.counter = desc.delay_end;
    } else {
        slot.switch_timer = 0; // [orig: @ 0x54345a; the swap + TryQueueSwitchTo(new)
                               //  @ 0x543475..0x5434a3 is the weapon-switch seam]
        slot.counter = 0;
        slot.phase = weapon_phase::kDone;
        out.switch_completed = true;
    }
}

// [orig: WeaponAction_SwitchRank @ 0x543500] Fire-mode / same-category swap: instant
// (no holster) — the slot swap is the weapon-switch seam; timing shape ported.
void handler_switchrank(const WeaponFsmDef &, const WeaponFsmAction &desc,
                        WeaponSlotState &slot, const WeaponFsmInputs &in,
                        WeaponFsmEvents &out) {
    if (slot.counter != 0 || slot.phase == weapon_phase::kDone) {
        begin_active(desc, slot, in, out); // [orig: @ 0x5435b0]
        return;
    }
    slot.switch_timer = 0;         // [orig: @ 0x543560]
    slot.counter = desc.delay_end; // [orig: @ 0x543596]
    slot.next = weapon_action::kIdle;
    slot.phase = weapon_phase::kDone;
    out.switch_completed = true;
}

// [orig: WeaponAction_ScopeUp @ 0x543290 / ..ScopeDown @ 0x543320] Timed one-shots —
// the ADS easing states. JOX/REVX ship no scopeup/scopedown ACTION rows, so both bake
// to zero-length pass-throughs; the camera easing lives embedder-side (§5.41 interp).
void handler_scope(const WeaponFsmDef &, const WeaponFsmAction &desc,
                   WeaponSlotState &slot, const WeaponFsmInputs &in,
                   WeaponFsmEvents &out) {
    begin_active(desc, slot, in, out); // [orig: ExecuteActionTick @ 0x5432a3/@ 0x543333]
    if (slot.counter == 0 && slot.phase != weapon_phase::kDone) {
        slot.counter = desc.delay_end; // [orig: @ 0x5432c0/@ 0x543350]
        slot.phase = weapon_phase::kDone;
    }
}

void run_handler(const WeaponFsmDef &def, WeaponSlotState &slot,
                 const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    const WeaponFsmAction &desc = def.actions[slot.current];
    switch (slot.current) {
        case weapon_action::kIdle:
        case weapon_action::kOverheated: // idle handler on the default table
            handler_idle(def, desc, slot, in, out);
            break;
        case weapon_action::kEmptyIdle:
            handler_emptyidle(def, desc, slot, in, out);
            break;
        case weapon_action::kFire:
            handler_fire(def, desc, slot, in, out);
            break;
        case weapon_action::kRecoil:
            handler_recoil(def, desc, slot, in, out);
            break;
        case weapon_action::kReload:
            handler_reload(def, desc, slot, in, out);
            break;
        case weapon_action::kEmpty:
            handler_empty(def, desc, slot, in, out);
            break;
        case weapon_action::kSwitchTo:
            handler_switchto(def, desc, slot, in, out);
            break;
        case weapon_action::kSwitchFrom:
            handler_switchfrom(def, desc, slot, in, out);
            break;
        case weapon_action::kSwitchRank:
            handler_switchrank(def, desc, slot, in, out);
            break;
        case weapon_action::kScopeUp:
        case weapon_action::kScopeDown:
            handler_scope(def, desc, slot, in, out);
            break;
        default:
            break;
    }
}

} // namespace

bool weapon_sights_card_eligible(const WeaponFsmDef &def,
                                 const WeaponSlotState &slot) {
    const bool scoped =
        (def.flags & weapon_flag::kScoped) != 0 &&
        (def.flags2 & weapon_flag2::kInset) == 0;
    const bool sighted =
        (def.flags & weapon_flag::kSighted) != 0 &&
        slot.current != weapon_action::kSwitchFrom;
    const bool card_switch_allowed =
        (def.flags & weapon_flag::kNoCardSwitch) == 0 ||
        (def.flags & weapon_flag::kForceScoped) != 0;
    return (scoped || sighted) && card_switch_allowed;
}

const char *const kWeaponActionSuffixes[weapon_action::kCount] = {
    "idle",       "emptyidle",  "fire",    "recoil",  "reload",    "empty",
    "switchto",   "switchfrom", "switchrank", "scopeup", "scopedown", "overheated",
};

int32_t weapon_anim_ticks_from_ms(int32_t ms) {
    // [orig: Anim_GetDurationTicks @ 0x53ee10 — trunc(ms * 62.5 (flt_7C3B3C)
    // / 1000 + 0.5 (flt_7C3B94)) + 1: ROUND-to-nearest, then +1. The prior
    // port truncated without the +0.5 (re-grilled 2026-07-10 at the oscarmike
    // adjudication — one tick short whenever the fraction reached .5).]
    return static_cast<int32_t>(static_cast<float>(ms) * 62.5f / 1000.0f + 0.5f) + 1;
}

void weapon_fsm_bake(const WeaponFsmActionRow *rows, size_t row_count,
                     WeaponClipResolvesFn clip_resolves, WeaponClipSecondsFn clip_seconds,
                     void *ctx, WeaponFsmDef &out) {
    for (int i = 0; i < weapon_action::kCount; ++i) {
        WeaponFsmAction &a = out.actions[i];
        a.id = i;
        // Absent rows are generated defaults: zeroed fields, unresolved anim.
        // [orig: ActionDef_InitDefaults @ 0x4022b0 memsets the record]
        int32_t ds = 0;
        int32_t de = 0;
        const char *anim = nullptr;
        for (size_t r = 0; r < row_count; ++r) {
            if (!name_equals_ci(rows[r].name, kWeaponActionSuffixes[i])) continue;
            ds = rows[r].delaystart;
            de = rows[r].delayend;
            if (rows[r].anim[0] != '\0') anim = rows[r].anim;
            // The row's audio/effect legs ride the baked pool entry [orig: the
            // ActionDef record carries the resolved references].
            copy_str128(a.soundset, rows[r].soundset);
            copy_str128(a.soundsetend, rows[r].soundsetend);
            copy_str128(a.particle, rows[r].particle);
            copy_str128(a.particle_userpoint, rows[r].particleuserpoint);
            break;
        }
        a.has_anim = false;
        a.anim_key[0] = '\0';
        // Existence is a pure LOOKUP [orig: AnimMap_FindSlotByName @ 0x40cfa0
        // checked @ 0x5421ae]; durations are consuming ring READS below.
        const bool resolves = anim != nullptr && clip_resolves != nullptr &&
                clip_resolves(ctx, anim) != 0;
        if (resolves) {
            a.has_anim = true;
            copy_key(a.anim_key, anim);
            // ONE Anim_GetDurationTicks read per 'auto' field — each read serves
            // the slot ring's head and advances it, so a both-auto action consumes
            // TWO ring entries and the reads can serve different variants
            // [orig: Anim_InitActions @ 0x5421b3..0x5421ec, the two calls
            //  @ 0x5421c5 / @ 0x5421d8].
            if (ds == -1) {
                const float s = clip_seconds != nullptr ? clip_seconds(ctx, anim) : -1.0f;
                ds = s >= 0.0f
                        ? weapon_anim_ticks_from_ms(static_cast<int32_t>(s * 1000.0f))
                        : 0;
            }
            if (de == -1) {
                const float s = clip_seconds != nullptr ? clip_seconds(ctx, anim) : -1.0f;
                const int32_t ticks = s >= 0.0f
                        ? weapon_anim_ticks_from_ms(static_cast<int32_t>(s * 1000.0f))
                        : 0;
                de = ticks;
                if (ticks > ds) de = ticks - ds;
            }
        } else {
            // No anim (or the clip did not resolve): 'auto' collapses to zero.
            // [orig: @ 0x542152..0x542164 / @ 0x542202..0x542210]
            if (ds == -1) ds = 0;
            if (de == -1) de = 0;
        }
        a.delay_start = ds;
        a.delay_end = de;
    }
}

bool weapon_fsm_request_fire(const WeaponFsmDef &def, WeaponSlotState &slot) {
    // [orig: WeaponSlot_RequestFire @ 0x53efa0]
    if (def.auto_fire) {
        switch (slot.current) {
            case weapon_action::kIdle:
            case weapon_action::kRecoil:
            case weapon_action::kScopeUp:
            case weapon_action::kScopeDown:
                slot.next = weapon_action::kFire;
                return true;
            case weapon_action::kEmptyIdle:
                slot.next = weapon_action::kEmpty;
                return false;
            case weapon_action::kFire:
                // A fire request landing mid-FIRE re-queues itself as a deferred
                // event — the loop self-sustains until a state that accepts the
                // dispatch consumes it (RECOIL banks the shot, even if the trigger
                // was already released). [orig: @ 0x53effd Input_QueueDeferredEvent]
                slot.refire_queued = true;
                return false;
            default:
                return false;
        }
    }
    if (slot.current == weapon_action::kIdle) {
        slot.next = weapon_action::kFire;
        return true;
    }
    if (slot.current == weapon_action::kEmptyIdle) slot.next = weapon_action::kEmpty;
    return false;
}

void weapon_fsm_request_reload(WeaponSlotState &slot) {
    // [orig: WeaponSlot_RequestReload @ 0x53f110 — phase sign bit clear, queued next
    //  in {IDLE, EMPTYIDLE, OVERHEATED}]
    if ((slot.phase & weapon_phase::kReloadPendingBit) != 0) return;
    if (slot.next < 2 || slot.next == weapon_action::kOverheated)
        slot.next = weapon_action::kReload;
}

void weapon_fsm_queue_scope_up(WeaponSlotState &slot) {
    // [orig: WeaponSlot_TryQueueScopeUp @ 0x53f050]
    if (slot.current == weapon_action::kScopeUp) return;
    if (slot.phase == weapon_phase::kDone || slot.phase == weapon_phase::kNone)
        slot.next = weapon_action::kScopeUp;
    else
        slot.next = weapon_action::kIdle;
}

bool weapon_fsm_reload_allowed(const WeaponFsmDef &def, const WeaponSlotState &slot) {
    // [orig: the reload input case 0xD3 @ 0x4e0420 compares the clip against
    // clipsize and the reserve against zero before WeaponSlot_RequestReload]
    if (def.clip_capacity <= 0) return false;
    return slot.clip != def.clip_capacity && slot.reserve > 0;
}

bool weapon_fsm_scope_toggle_allowed(const WeaponFsmDef &def, const WeaponSlotState &slot) {
    // [orig: input case 6 @ 0x4e0420 gates currentAction not in {RELOAD,
    // SWITCHFROM}; Player_ToggleWeaponScope @ 0x4df0c0 gates def Flags & 3]
    if (slot.current == weapon_action::kReload || slot.current == weapon_action::kSwitchFrom)
        return false;
    return (def.flags & 3) != 0;
}

void weapon_fsm_queue_scope_down(WeaponSlotState &slot) {
    // [orig: WeaponSlot_TryQueueScopeDown @ 0x53f080]
    if (slot.current == weapon_action::kScopeDown) return;
    if (slot.phase == weapon_phase::kDone || slot.phase == weapon_phase::kNone)
        slot.next = weapon_action::kScopeDown;
    else
        slot.next = weapon_action::kIdle;
}

void weapon_fsm_queue_switch_from(WeaponSlotState &slot) {
    // [orig: WeaponSlot_ForceQueueSwitchFrom @ 0x53f170 — refused while SWITCHFROM
    //  or SWITCHTO is current; a complete phase resets the slot and queues
    //  SWITCHFROM, otherwise the action is forced back to idle first]
    if (slot.current == weapon_action::kSwitchFrom ||
        slot.current == weapon_action::kSwitchTo)
        return;
    if (slot.phase == weapon_phase::kDone || slot.phase == weapon_phase::kHeld ||
        slot.phase == weapon_phase::kNone) {
        slot.counter = 0;       // [orig: *(slot+0) = 0]
        slot.burst = 0;         // [orig: slot+98 = 0]
        slot.switch_timer = 0;  // [orig: slot+88 = 0]
        slot.next = weapon_action::kSwitchFrom;
        slot.prev = weapon_action::kIdle; // [orig: slot+52 = 0]
        slot.current = weapon_action::kIdle; // [orig: slot+44 = 0]
    } else {
        slot.next = weapon_action::kIdle;
    }
}

void weapon_fsm_queue_switch_rank(WeaponSlotState &slot) {
    // [orig: WeaponSlot_TryQueueSwitchRank @ 0x53f1c0 — refused while SWITCHRANK is
    //  current; a complete phase queues it, otherwise idle]
    if (slot.current == weapon_action::kSwitchRank) return;
    if (slot.phase == weapon_phase::kDone || slot.phase == weapon_phase::kHeld ||
        slot.phase == weapon_phase::kNone) {
        slot.counter = 0; // [orig: *(slot+0) = 0]
        slot.next = weapon_action::kSwitchRank;
    } else {
        slot.next = weapon_action::kIdle;
    }
}

void weapon_fsm_try_queue_switch_to(WeaponSlotState &slot) {
    // [orig: WeaponSlot_TryQueueSwitchTo @0x53f140]
    if (slot.current == weapon_action::kSwitchTo) return;
    if (slot.phase == weapon_phase::kDone || slot.phase == weapon_phase::kNone)
        slot.next = weapon_action::kSwitchTo;
    else
        slot.next = weapon_action::kIdle;
}

void weapon_fsm_tick(const WeaponFsmDef &def, WeaponSlotState &slot,
                     const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    out = WeaponFsmEvents{};

    // Input-dispatcher writers run before the frame pump [orig: the input layer
    // dispatches binding events ahead of WeaponAction_ProcessAllEntities in the
    // frame]. Held-trigger auto fire is NOT a per-tick re-request: the press edge
    // starts the volley and the recoil window's deferred re-queue sustains it,
    // consumed here one tick after it was queued — the deferred dispatch
    // [orig: Input_QueueDeferredEvent @ 0x4993e0 -> WeaponSlot_RequestFire @ 0x53efa0].
    if (in.reload_pressed) weapon_fsm_request_reload(slot);
    const bool fire_request = in.fire_pressed || slot.refire_queued;
    slot.refire_queued = false; // consumed (RequestFire may re-queue it mid-FIRE)
    if (fire_request) weapon_fsm_request_fire(def, slot);

    // --- the pump tail [orig: WeaponAction_ProcessFrame @ 0x541262..0x5414ac] -----

    // Kick decay [orig: @ 0x541262..0x54129b; the kick-end sound is a host seam].
    if (slot.kick != 0) {
        --slot.kick;
        if ((slot.kick == 0 || slot.current == weapon_action::kIdle) &&
            slot.current != weapon_action::kReload)
            slot.kick = 0;
    }

    // The heat window [orig: @ 0x540fed..0x541262]. A live window either denies the
    // next shot or, once it lapses (or the owner submerges with a def that is not
    // Underwater), clears itself back to cold.
    if (slot.heat_window_end_tick != 0) {
        const bool window_live = slot.heat_window_end_tick > in.current_tick;
        const bool water_ok = !in.submerged || (def.flags & weapon_flag::kUnderwater) != 0;
        if (window_live && water_ok) {
            // The overheat deny: a queued FIRE becomes the dry-fire click. Note it
            // does NOT cancel an in-flight FIRE, and nothing stops the heat from
            // climbing past kFull — the gun coughs until the level falls back under.
            // [orig: @ 0x541046]
            if (weapon_slot_accumulated_heat(def, slot, in.current_tick) >
                        weapon_heat::kFull &&
                slot.next == weapon_action::kFire)
                slot.next = weapon_action::kEmpty;
        } else {
            slot.heat_window_end_tick = 0; // [orig: @ 0x54125f]
        }
    }

    const WeaponFsmAction *desc = &def.actions[slot.current];
    if (slot.counter == 0) {
        if (slot.current == weapon_action::kIdle && slot.next == weapon_action::kIdle) {
            // The idle reseed [orig: @ 0x54134c..0x54135d].
            slot.counter = desc->delay_start + desc->delay_end;
        }
    }
    if (slot.current != weapon_action::kIdle || slot.next == weapon_action::kIdle) {
        if (slot.counter > 0) {
            --slot.counter; // [orig: @ 0x541424]
            run_handler(def, slot, in, out);
            return;
        }
    } else {
        slot.counter = 0; // idle with a pending action transitions now [orig: @ 0x541370]
    }

    // Rescope after a completed reload [orig: @ 0x54139e..0x5413ab — the pump toggles
    // the scope back on and clears g_rescopeAfterReload].
    if (slot.current == weapon_action::kReload && slot.next == weapon_action::kIdle &&
        in.is_local && slot.rescope_after_reload) {
        out.rescope = true;
        slot.rescope_after_reload = false;
    }

    if (slot.current != slot.next) {
        const bool instant = slot.phase == weapon_phase::kDone ||
                             slot.phase == weapon_phase::kNone;
        const bool held_variant = slot.phase == weapon_phase::kHeld;
        if (instant || held_variant) {
            // [orig: the transition @ 0x5413d0.. / the held variant @ 0x541453..]
            const int32_t target = slot.next;
            if (slot.current != weapon_action::kSwitchTo &&
                slot.current != weapon_action::kSwitchFrom)
                slot.prev = slot.current; // [orig: @ 0x5413de]
            slot.current = target;
            slot.next = weapon_action::kIdle;
            slot.counter = def.actions[target].delay_start; // [orig: @ 0x5413ea]
            slot.phase = weapon_phase::kEntered;
            if (target == weapon_action::kEmpty) out.dry_fired = true;
            run_handler(def, slot, in, out);
            if (held_variant) slot.phase = weapon_phase::kHeld; // [orig: @ 0x54148e]
            return;
        }
    }
    run_handler(def, slot, in, out); // [orig: LABEL_118 @ 0x5414a2]
}

} // namespace opennova::world
