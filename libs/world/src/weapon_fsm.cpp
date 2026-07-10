// First-person weapon action FSM — see weapon_fsm.h.
// [orig: WeaponAction_ProcessFrame @ 0x540e60 + the wpn_std_* handlers @ 0x542920..
//  0x543500; Anim_InitActions @ 0x541fa0; docs/net/novaworld-net-re.md §5.62]

#include "world/weapon_fsm.h"

#include <cstring>

namespace opennova::world {

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
// [orig: ActionSlot_BeginActivePhase @ 0x53f830; the FP-routing variants
//  ActionSlot_ExecuteActionWithEffect @ 0x541860 / ..NoEffect @ 0x5419e0 write the same
//  phase protocol; sound/ctrlreg/muzzle legs are host seams]
void begin_active(const WeaponFsmAction &desc, WeaponSlotState &slot,
                  const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    if ((slot.phase & 1) != 0 || (slot.phase & weapon_phase::kHeld) != 0) {
        slot.phase = weapon_phase::kActive;
        if (desc.has_anim && in.is_local) {
            out.play_anim = true;
            copy_key(out.anim_key, desc.anim_key);
        }
    }
    // else: the anim channel keeps advancing on its own (the host owns clip playback).
}

// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 (desc, slot, entity, nextAction)]:
// counter = delayEnd, nextAction = the passed value, the ACTIVE->DONE kick bump
// (skipped for RELOAD), phase = DONE. The original gates the kick on the weapon's
// fire-sound id being set (Def+0x294) — every shipped weapon carries one (D-WPN-3).
void finish_active(const WeaponFsmAction &desc, WeaponSlotState &slot, int32_t next) {
    const bool was_active = slot.phase == weapon_phase::kActive;
    slot.counter = desc.delay_end;
    slot.next = next;
    if (was_active && slot.current != weapon_action::kReload)
        kick_add(slot, desc.delay_start + desc.delay_end + slot.counter + 10);
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
    if (in.is_local) { // [orig: AnimMap_PlayAnimBySlot(adm, 241) @ 0x542955]
        out.play_anim = true;
        copy_key(out.anim_key, "anim_wpn_idle");
    }
    slot.phase = weapon_phase::kDone;
    if (def.clip_capacity < 0) return;   // [orig: @ 0x54296c infinite -> effects only]
    if (has_rounds(def, slot)) return;   // [orig: @ 0x542981]
    if (slot.reserve > 0 && in.auto_reload) { // [orig: @ 0x5429ac g_autoReloadEnabled]
        weapon_fsm_request_reload(slot);
        return;
    }
    slot.next = weapon_action::kEmptyIdle; // [orig: @ 0x5429cf]
    // One-shot weapons drop the scope with the last round (the Flags & 0x20000000
    // stay-scoped exception is unmapped in our flag table; no JOX/REVX token sets it).
    // [orig: @ 0x5429ee g_weaponScopeActive = 0]
    if (in.is_local && def.clip_capacity == 1) out.unscope = true;
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
    if (in.is_local) { // [orig: AnimMap_PlayAnimBySlot(adm, 242) @ 0x542a53]
        out.play_anim = true;
        copy_key(out.anim_key, "anim_wpn_empty_idle");
    }
    if (def.clip_capacity >= 0 && !has_rounds(def, slot)) {
        if (slot.reserve > 0) {
            weapon_fsm_request_reload(slot); // [orig: @ 0x542aa3]
        } else {
            slot.next = weapon_action::kEmptyIdle; // hold [orig: @ 0x542ab2]
            if (in.is_local && def.clip_capacity == 1) out.unscope = true; // [orig: @ 0x542ad1]
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
        // EMPTYIDLE) — the [esi+30h] read happens AFTER the CanFire call.
        // [orig: @ 0x542b44..0x542b5e]
        finish_active(desc, slot, slot.next);
        slot.counter = 0;
        return;
    }
    if (slot.counter != 0 || slot.phase == weapon_phase::kDone) {
        begin_active(desc, slot, in, out); // [orig: @ 0x542db9]
        return;
    }
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
    finish_active(desc, slot, slot.next); // [orig: @ 0x542d13 push [esi+30h] — keeps 3]
}

// [orig: WpnAction_Recoil @ 0x542dd0] THE ARBITER: when the recoil clip ends, decide
// refire (burst), idle, auto-reload, or emptyidle; the held-trigger auto refire is the
// per-tick fire re-request (the original re-queues input binding 149 here
// [orig: @ 0x542e9d]).
void handler_recoil(const WeaponFsmDef &def, const WeaponFsmAction &desc,
                    WeaponSlotState &slot, const WeaponFsmInputs &in,
                    WeaponFsmEvents &out) {
    const bool rounds = has_rounds(def, slot); // [orig: isLocalWeapon calc @ 0x542deb]
    begin_active(desc, slot, in, out);         // [orig: ExecuteActionTick @ 0x542e2a]
    if (slot.phase == weapon_phase::kDone ||
        (desc.delay_start == 0 && desc.delay_end == 0)) {
        // (the deferred-event refire block sits here in the original; the port's
        //  held re-request covers it)
        if (desc.delay_start != 0 || desc.delay_end != 0) return; // [orig: @ 0x542eae]
    }
    if (slot.counter != 0) return; // [orig: @ 0x542eb7]
    slot.phase = weapon_phase::kDone; // [orig: @ 0x542f74]
    // (heat-window stamp @ 0x542f8b and the muzzle-effect leg are host seams — D-WPN-4)
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
    if (in.is_local && def.clip_capacity == 1) out.unscope = true; // [orig: @ 0x543053]
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
            // Stash the scope across the reload; the pump rescopes when it completes.
            // (The Flags & 0x40000 keep-scoped class is unmapped — no JOX/REVX token.)
            // [orig: @ 0x54312f g_rescopeAfterReload = g_weaponScopeActive]
            slot.rescope_after_reload = in.scope_active;
            if (in.scope_active) out.unscope = true; // [orig: Player_ToggleWeaponScope @ 0x543136]
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
        finish_active(desc, slot, weapon_action::kIdle); // [orig: @ 0x54316e push 0]
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
    if (slot.switch_timer >= -900) {
        slot.switch_timer = static_cast<int16_t>(slot.switch_timer - 30); // [orig: @ 0x5434c2]
        slot.counter = desc.delay_end;
    } else {
        slot.switch_timer = 0; // [orig: @ 0x54345a; the swap + TryQueueSwitchTo(new)
                               //  @ 0x543475..0x5434a3 is the weapon-switch seam]
        slot.counter = 0;
        slot.phase = weapon_phase::kDone;
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
}

// [orig: WeaponAction_ScopeUp @ 0x543290 / ..ScopeDown @ 0x543320] Timed one-shots —
// the ADS easing states. JOX/REVX ship no scopeup/scopedown ACTION rows, so both bake
// to zero-length pass-throughs; the camera easing lives host-side (§5.41 interp).
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
                     WeaponClipSecondsFn clip_seconds, void *ctx, WeaponFsmDef &out) {
    for (int i = 0; i < weapon_action::kCount; ++i) {
        WeaponFsmAction &a = out.actions[i];
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
            break;
        }
        a.has_anim = false;
        a.anim_key[0] = '\0';
        float seconds = -1.0f;
        if (anim != nullptr && clip_seconds != nullptr)
            seconds = clip_seconds(ctx, anim);
        if (anim != nullptr && seconds >= 0.0f) {
            a.has_anim = true;
            copy_key(a.anim_key, anim);
            const int32_t ticks =
                    weapon_anim_ticks_from_ms(static_cast<int32_t>(seconds * 1000.0f));
            // [orig: Anim_InitActions @ 0x5421b3..0x5421ec]
            if (ds == -1) ds = ticks;
            if (de == -1) {
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
                // deferred re-queue in the original; the per-tick held re-request
                // covers it.
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

void weapon_fsm_tick(const WeaponFsmDef &def, WeaponSlotState &slot,
                     const WeaponFsmInputs &in, WeaponFsmEvents &out) {
    out = WeaponFsmEvents{};

    // Input-dispatcher writers run before the frame pump [orig: the input layer
    // dispatches binding events ahead of WeaponAction_ProcessAllEntities in the frame].
    if (in.reload_pressed) weapon_fsm_request_reload(slot);
    if (in.fire_pressed || (def.auto_fire && in.fire_held))
        weapon_fsm_request_fire(def, slot);

    // --- the pump tail [orig: WeaponAction_ProcessFrame @ 0x541262..0x5414ac] -----

    // Kick decay [orig: @ 0x541262..0x54129b; the kick-end sound is a host seam].
    if (slot.kick != 0) {
        --slot.kick;
        if ((slot.kick == 0 || slot.current == weapon_action::kIdle) &&
            slot.current != weapon_action::kReload)
            slot.kick = 0;
    }

    // (the overheat deny — heat > 0xFFFF converting a queued FIRE to EMPTY
    //  [orig: @ 0x541046] — needs the heat model, D-WPN-4)

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
