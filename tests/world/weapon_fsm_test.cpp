// Weapon action FSM unit tests [orig: WeaponAction_ProcessFrame @ 0x540e60 + handlers;
// Anim_InitActions @ 0x541fa0; docs/net/novaworld-net-re.md §5.62]: the bake (auto
// delays from clip ticks, absent rows, suffix binding), the pump protocol (transition
// on DONE, counter tick-down, idle reseed), and the witnessed decision edges — fire
// chains recoil unconditionally, recoil arbitrates refire/reload/emptyidle, reload
// stashes and restores the scope, dry-fire from emptyidle.
#include <cstdio>
#include <cstring>
#include <string>

#include "world/weapon_fsm.h"

using namespace opennova::world;
namespace wa = opennova::world::weapon_action;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// Clip table double: every anim_wpn_* key resolves to a fixed length.
float clip_seconds(void *, const char *key) {
    if (std::strcmp(key, "anim_wpn_fire") == 0) return 0.096f;   // -> 7 ticks
    if (std::strcmp(key, "anim_wpn_idle") == 0) return 1.0f;     // -> 63 ticks
    if (std::strcmp(key, "anim_wpn_reload") == 0) return 0.5f;   // -> 32 ticks
    if (std::strcmp(key, "anim_wpn_empty") == 0) return 0.2f;    // -> 13 ticks
    if (std::strcmp(key, "missing") == 0) return -1.0f;
    return 0.1f;
}

// Existence probe paired with the table above — a lookup, never a read
// [orig: AnimMap_FindSlotByName @ 0x40cfa0].
int clip_resolves(void *, const char *key) {
    return std::strcmp(key, "missing") != 0;
}

void set_row(WeaponFsmActionRow &row, const char *name, const char *anim, int32_t ds,
             int32_t de) {
    std::snprintf(row.name, sizeof(row.name), "%s", name);
    std::snprintf(row.anim, sizeof(row.anim), "%s", anim);
    row.delaystart = ds;
    row.delayend = de;
}

// The JOX AK-47 def shape (weapon "WPN_AK47AUTO" rows): idle/emptyidle {0, auto},
// fire {0, 6} carrying the cadence, recoil {0, 0} — the zero-length pass-through
// arbiter with a real anim (retail recoils carry no delays; a delaystart-carried
// recoil never ships and would not sustain the refire chain) — reload/empty
// {0, auto}, auto-fire flags.
WeaponFsmDef make_ak_def() {
    WeaponFsmActionRow rows[6];
    set_row(rows[0], "idle", "anim_wpn_idle", 0, -1);
    set_row(rows[1], "emptyidle", "anim_wpn_idle", 0, -1);
    set_row(rows[2], "fire", "anim_wpn_fire", 0, 6);
    set_row(rows[3], "recoil", "anim_wpn_recoil", 0, 0);
    set_row(rows[4], "reload", "anim_wpn_reload", 0, -1);
    set_row(rows[5], "empty", "anim_wpn_empty", 0, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 6, clip_resolves, clip_seconds, nullptr, def);
    def.auto_fire = true;
    def.clip_capacity = 30;
    return def;
}

WeaponSlotState make_ak_slot() {
    WeaponSlotState s;
    s.clip = 30;
    s.reserve = 300;
    return s;
}

void test_ticks_from_ms() {
    // [orig: Anim_GetDurationTicks @ 0x53ee10 = trunc(ms*62.5/1000 + 0.5) + 1 —
    // ROUND-to-nearest then +1 (0.5 = flt_7C3B94; re-grilled 2026-07-10)]
    CHECK(weapon_anim_ticks_from_ms(0) == 1);
    CHECK(weapon_anim_ticks_from_ms(96) == 7);    // 6.0 -> trunc(6.5)+1
    CHECK(weapon_anim_ticks_from_ms(1000) == 64); // 62.5 -> trunc(63.0)+1 (was 63 pre-rounding)
    CHECK(weapon_anim_ticks_from_ms(16) == 2);    // 1.0 -> trunc(1.5)+1
}

void test_sights_card_eligibility() {
    WeaponFsmDef def;
    WeaponSlotState slot;

    def.flags = weapon_flag::kScoped;
    CHECK(weapon_sights_card_eligible(def, slot));

    slot.current = wa::kSwitchFrom;
    CHECK(weapon_sights_card_eligible(def, slot));

    slot.current = wa::kIdle;

    def.flags2 = weapon_flag2::kInset;
    CHECK(!weapon_sights_card_eligible(def, slot));

    def.flags = weapon_flag::kSighted;
    CHECK(weapon_sights_card_eligible(def, slot));

    def.flags2 = 0;
    CHECK(weapon_sights_card_eligible(def, slot));

    slot.current = wa::kSwitchFrom;
    CHECK(!weapon_sights_card_eligible(def, slot));

    slot.current = wa::kIdle;
    def.flags = weapon_flag::kSighted | weapon_flag::kNoCardSwitch;
    CHECK(!weapon_sights_card_eligible(def, slot));

    def.flags |= weapon_flag::kForceScoped;
    CHECK(weapon_sights_card_eligible(def, slot));

    def.flags = weapon_flag::kScoped | weapon_flag::kSighted;
    def.flags2 = weapon_flag2::kInset;
    slot.current = wa::kSwitchFrom;
    CHECK(!weapon_sights_card_eligible(def, slot));

    def.flags = weapon_flag::kScoped | weapon_flag::kNoCardSwitch;
    def.flags2 = 0;
    slot.current = wa::kIdle;
    CHECK(!weapon_sights_card_eligible(def, slot));

    def.flags = weapon_flag::kForceScoped;
    CHECK(!weapon_sights_card_eligible(def, slot));
}

void test_bake() {
    WeaponFsmDef def = make_ak_def();
    // idle: ds explicit 0; de auto -> full clip ticks (64; ticks > ds -> ticks - 0).
    CHECK(def.actions[wa::kIdle].delay_start == 0);
    CHECK(def.actions[wa::kIdle].delay_end == 64);
    CHECK(def.actions[wa::kIdle].has_anim);
    // recoil: explicit {0, 0} — the witnessed retail shape; the anim still binds.
    CHECK(def.actions[wa::kRecoil].delay_start == 0);
    CHECK(def.actions[wa::kRecoil].delay_end == 0);
    CHECK(std::strcmp(def.actions[wa::kRecoil].anim_key, "anim_wpn_recoil") == 0);
    // absent rows (scopeup/scopedown/overheated...) bake to zero-length.
    CHECK(def.actions[wa::kScopeUp].delay_start == 0);
    CHECK(def.actions[wa::kScopeUp].delay_end == 0);
    CHECK(!def.actions[wa::kScopeUp].has_anim);
    // 'auto' with an unresolvable clip collapses to 0 [orig: @ 0x542202..0x542210].
    WeaponFsmActionRow bad;
    set_row(bad, "reload", "missing", -1, -1);
    WeaponFsmDef def2;
    weapon_fsm_bake(&bad, 1, clip_resolves, clip_seconds, nullptr, def2);
    CHECK(def2.actions[wa::kReload].delay_start == 0);
    CHECK(def2.actions[wa::kReload].delay_end == 0);
    CHECK(!def2.actions[wa::kReload].has_anim);
    // delayend auto with ticks > delaystart -> ticks - delaystart
    // [orig: @ 0x5421e8..0x5421ec].
    WeaponFsmActionRow part;
    set_row(part, "reload", "anim_wpn_reload", 10, -1);
    WeaponFsmDef def3;
    weapon_fsm_bake(&part, 1, clip_resolves, clip_seconds, nullptr, def3);
    CHECK(def3.actions[wa::kReload].delay_start == 10);
    CHECK(def3.actions[wa::kReload].delay_end == 22); // 32 - 10
    // suffix binding is case-insensitive [orig: stricmp @ 0x402360].
    WeaponFsmActionRow upper;
    set_row(upper, "RELOAD", "anim_wpn_reload", 3, 4);
    WeaponFsmDef def4;
    weapon_fsm_bake(&upper, 1, clip_resolves, clip_seconds, nullptr, def4);
    CHECK(def4.actions[wa::kReload].delay_start == 3);
    CHECK(def4.actions[wa::kReload].delay_end == 4);
}

// Drive N ticks with fixed inputs, collecting the last events.
WeaponFsmEvents run_ticks(const WeaponFsmDef &def, WeaponSlotState &s,
                          const WeaponFsmInputs &in, int n) {
    WeaponFsmEvents ev;
    for (int i = 0; i < n; ++i) weapon_fsm_tick(def, s, in, ev);
    return ev;
}

void test_fire_chains_recoil() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_pressed = true;
    in.fire_held = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev); // request lands; idle transitions to FIRE
    CHECK(s.current == wa::kFire);
    CHECK(ev.fired);                 // fire ds == 0 -> the shot on the entry tick
    CHECK(ev.fired_clip_before_consume == 30); // mode byte samples MountSlot+0x10 first
    CHECK(s.clip == 29);             // ammo consumed [orig: consume_weapon_ammo]
    CHECK(s.next == wa::kRecoil);    // [orig: @ 0x542c9e unconditional]
    CHECK(s.kick > 0);
    in.fire_pressed = false;
    in.fire_held = false;
    for (int t = 0; t < 6; ++t) weapon_fsm_tick(def, s, in, ev); // the delayend window
    CHECK(s.current == wa::kFire);   // still counting the fire row's delayend
    weapon_fsm_tick(def, s, in, ev); // FIRE(done) -> RECOIL
    CHECK(s.current == wa::kRecoil);
    // The recoil anim starts on its entry tick.
    CHECK(ev.play_anim);
    CHECK(std::strcmp(ev.anim_key, "anim_wpn_recoil") == 0);
}

void test_auto_refire_cadence() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_pressed = true;
    in.fire_held = true;
    int fired = 0;
    for (int t = 0; t < 100; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (ev.fired) ++fired;
    }
    // fire {0, 6}: the shot tick + 6 delayend ticks + the recoil pass whose deferred
    // re-queue dispatches on the following tick = a deterministic 8-tick cycle ->
    // shots at t=0,8,...,96 = 13 in a 100-tick hold. The pin is the witnessed refire
    // chain (recoil re-queue -> next-tick dispatch), not an invented rate.
    CHECK(fired == 13);
    CHECK(s.clip == 30 - fired);
}

void test_revx_m4_zero_recoil_auto_cadence() {
    // The REVX02 M4AUTO shape: FIRE {0, 3} carries the cadence and RECOIL is a
    // ZERO-LENGTH pass-through arbiter (DELAYEND 0, ANIM authored as a comment ->
    // empty). Retail runs the {0,0} action on its entry tick (LABEL_118
    // @ 0x5414a2) and the held trigger re-fires through the recoil arbiter
    // [orig: @ 0x542e9d family] — the volley must run at the FIRE row's cadence
    // with the per-shot end leg (SOUNDSETEND GS_M4) on every shot.
    WeaponFsmActionRow rows[5];
    set_row(rows[0], "idle", "anim_wpn_idle", 0, -1);
    set_row(rows[1], "fire", "anim_wpn_fire", 0, 3);
    set_row(rows[2], "recoil", "", 0, 0);
    set_row(rows[3], "reload", "anim_wpn_reload", 200, -1);
    set_row(rows[4], "empty", "anim_wpn_empty", -1, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 5, clip_resolves, clip_seconds, nullptr, def);
    def.auto_fire = true;
    def.clip_capacity = 30;
    WeaponSlotState s;
    s.clip = 30;
    s.reserve = 300;
    WeaponFsmInputs in;
    in.fire_pressed = true;
    in.fire_held = true;
    int fired = 0;
    int fire_ends = 0;
    for (int t = 0; t < 100; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (ev.fired) ++fired;
        if (ev.action_finished == wa::kFire) ++fire_ends;
    }
    // fire entry + 3 counter ticks + the zero-length recoil pass (refire re-queued on
    // its entry tick, dispatched the next) = a deterministic 5-tick cycle -> shots at
    // t=0,5,...,95 = 20 in 100 ticks, each with the per-shot end leg.
    CHECK(fired == 20);
    CHECK(fire_ends == fired);
    CHECK(s.clip == 30 - fired);
}

void test_semi_no_auto_refire() {
    WeaponFsmDef def = make_ak_def();
    def.auto_fire = false;
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_pressed = true; // one edge
    in.fire_held = true;
    int fired = 0;
    for (int t = 0; t < 60; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false; // held but no new edge
        if (ev.fired) ++fired;
    }
    CHECK(fired == 1); // SEMI fires once per edge [orig: WeaponSlot_RequestFire @ 0x53efa0]
}

void test_burst3() {
    WeaponFsmDef def = make_ak_def();
    def.auto_fire = false;
    def.burst3 = true; // Flags & 0x20
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_pressed = true; // ONE edge; the burst carries the volley
    in.fire_held = false;
    int fired = 0;
    for (int t = 0; t < 60; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (ev.fired) ++fired;
    }
    // burst cycles 0 -> 2 -> 1 -> 0: exactly three shots [orig: @ 0x542c8a-0x542c9b +
    // the recoil burst refire @ 0x543080].
    CHECK(fired == 3);
    CHECK(s.burst == 0);
}

void test_empty_clip_paths() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 1;
    s.reserve = 0; // last round, nothing carried
    WeaponFsmInputs in;
    in.auto_reload = true;
    in.fire_pressed = true;
    in.fire_held = true;
    bool saw_emptyidle = false;
    for (int t = 0; t < 40; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (s.current == wa::kEmptyIdle) saw_emptyidle = true;
    }
    // Clip spent, no reserve -> EMPTYIDLE (the fire-abort adopts CanFire's queued 1
    // [orig: @ 0x541cb9]; the recoil arbiter lands the same [orig: @ 0x543036]). The
    // refire chain died with the rounds [orig: the re-queue @ 0x542e9d is
    // rounds-gated], so the held trigger goes SILENT — dry clicks need fresh press
    // edges (binding 149 dispatches on edges, not per-tick).
    CHECK(s.clip == 0);
    CHECK(saw_emptyidle);
    CHECK(s.current == wa::kEmptyIdle || s.current == wa::kEmpty);
    bool held_click = false;
    for (int t = 0; t < 20; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev); // still held, no new edge
        held_click |= ev.dry_fired;
    }
    CHECK(!held_click);
    // A fresh press edge lands ONE dry click [orig: WeaponSlot_RequestFire {1} -> 5].
    int clicks = 0;
    for (int t = 0; t < 20; ++t) {
        WeaponFsmEvents ev;
        in.fire_pressed = (t == 0);
        weapon_fsm_tick(def, s, in, ev);
        if (ev.dry_fired) ++clicks;
    }
    CHECK(clicks == 1);
}

void test_empty_clip_held_auto_reload() {
    // THE REVVY M4 wedge (grilled 2026-07-12): hold the trigger through the whole
    // magazine. The last shot's recoil arbitration queues RELOAD; the refire chain is
    // dead (the ROUNDS gate [orig: @ 0x542e7f]), so nothing overwrites it — the
    // auto-reload runs WHILE the trigger is held, with no FIRE<->RECOIL thrash. (The
    // pre-fix port's per-tick held re-request overwrote the queued RELOAD every tick:
    // the gun never reloaded and the recoil row's soundset/particle spammed at 31 Hz.)
    // After the reload the volley does NOT resume until a fresh press edge.
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 2;
    s.reserve = 300;
    WeaponFsmInputs in;
    in.auto_reload = true;
    in.fire_pressed = true;
    in.fire_held = true;
    int fired = 0, recoil_starts = 0;
    bool applied = false;
    for (int t = 0; t < 200; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (ev.fired) ++fired;
        if (ev.action_started == wa::kRecoil) ++recoil_starts;
        applied |= ev.reload_applied;
    }
    CHECK(fired == 2);             // the volley stopped at the empty clip
    CHECK(applied);                // the auto-reload ran despite the held trigger
    CHECK(recoil_starts == 2);     // one recoil per shot — no empty-mag thrash
    CHECK(s.clip == 30);           // refilled
    CHECK(s.current == wa::kIdle); // settled idle, NOT firing
    // A fresh press edge restarts the volley from the refilled magazine.
    WeaponFsmEvents ev;
    in.fire_pressed = true;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(ev.fired);
    CHECK(s.current == wa::kFire);
}

void test_tap_during_fire_banks_one() {
    // A fire press landing DURING the FIRE delayend window re-queues as a deferred
    // event; the loop self-sustains through the FIRE ticks and the recoil dispatch
    // banks exactly ONE follow-up shot — even though the trigger is already released
    // when the recoil runs. [orig: WeaponSlot_RequestFire case FIRE @ 0x53effd
    // re-queues; the RECOIL dispatch consumes -> next = FIRE]
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    int fired = 0;
    for (int t = 0; t < 60; ++t) {
        WeaponFsmEvents ev;
        in.fire_pressed = (t == 0 || t == 3); // tap, tap-mid-FIRE-window
        weapon_fsm_tick(def, s, in, ev);
        if (ev.fired) ++fired;
    }
    CHECK(fired == 2);
    CHECK(s.current == wa::kIdle); // the bank is one shot deep, then the volley dies
}

void test_auto_reload_from_recoil() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 1;
    s.reserve = 300;
    WeaponFsmInputs in;
    in.auto_reload = true; // [orig: g_autoReloadEnabled @ 0x24D2118]
    in.fire_pressed = true;
    in.fire_held = false;
    bool requested = false, applied = false;
    for (int t = 0; t < 80 && !applied; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        requested |= ev.reload_requested;
        applied |= ev.reload_applied;
    }
    // Last round fired -> recoil arbiter queues RELOAD (reserve >= a full clip)
    // [orig: @ 0x54301d]; the authority applies the §5.58 refill.
    CHECK(requested);
    CHECK(applied);
    CHECK(s.current == wa::kReload);
    CHECK(s.clip == 30);
    CHECK(s.reserve == 270);
    // Reload runs its clip then lands IDLE with the burst reset [orig: @ 0x54316e].
    for (int t = 0; t < 80 && s.current != wa::kIdle; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
    }
    CHECK(s.current == wa::kIdle);
    CHECK(s.burst == 0);
}

void test_reload_scope_stash() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 5;
    WeaponFsmInputs in;
    in.scope_active = true; // scoped when the reload starts
    in.reload_pressed = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev); // request lands, idle -> RELOAD transition
    in.reload_pressed = false;
    CHECK(s.current == wa::kReload);
    bool unscoped = ev.unscope;
    bool rescoped = false;
    for (int t = 0; t < 80 && s.current == wa::kReload; ++t) {
        weapon_fsm_tick(def, s, in, ev);
        unscoped |= ev.unscope;
        rescoped |= ev.rescope;
    }
    // The reload dropped the scope and the pump rescoped on completion
    // [orig: @ 0x54312f stash / @ 0x54139e rescope].
    CHECK(unscoped);
    CHECK(rescoped);
    CHECK(!s.rescope_after_reload);
}

void test_reload_request_gate() {
    WeaponSlotState s;
    s.next = wa::kFire; // something else queued
    weapon_fsm_request_reload(s);
    CHECK(s.next == wa::kFire); // refused [orig: next in {0,1,11} @ 0x53f12d]
    s.next = wa::kOverheated;
    weapon_fsm_request_reload(s);
    CHECK(s.next == wa::kReload); // 11 allowed
}

void test_scope_queue() {
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    run_ticks(def, s, in, 2); // settle idle (phase DONE)
    weapon_fsm_queue_scope_up(s); // [orig: WeaponSlot_TryQueueScopeUp @ 0x53f050]
    CHECK(s.next == wa::kScopeUp);
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.current == wa::kScopeUp);
    // Zero-length pass-through (no shipped scopeup rows) -> back to idle.
    run_ticks(def, s, in, 3);
    CHECK(s.current == wa::kIdle);
    // Firing is legal from the scope states (the auto can-fire set {0,2,3,9,10})
    // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0].
    run_ticks(def, s, in, 2);
    weapon_fsm_queue_scope_up(s);
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.current == wa::kScopeUp);
    CHECK(weapon_fsm_request_fire(def, s));
}

void test_idle_plays_once_per_entry() {
    // The idle handler's enter branch plays the global wpn_idle clip ONCE per idle
    // ENTRY (phase then stays DONE and the shim only advances the channel — the clip
    // loops at the anim layer); the reseed keeps the counter cycling without replays.
    // [orig: WeaponAction_Idle @ 0x542920 — the phase==4 guard; reseed @ 0x54135d]
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    // One semi shot, then release: fire -> recoil -> idle re-entry.
    def.auto_fire = false;
    in.fire_pressed = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    in.fire_pressed = false;
    int idle_plays = 0;
    for (int t = 0; t < 260; ++t) {
        weapon_fsm_tick(def, s, in, ev);
        if (ev.play_anim && std::strcmp(ev.anim_key, "anim_wpn_idle") == 0) ++idle_plays;
    }
    CHECK(s.current == wa::kIdle);
    CHECK(idle_plays == 1);
}

void test_action_sound_legs() {
    // The corpus split: fire rows carry the gunshot in soundsetEND (118/130 REVX,
    // 83/89 JOX), reload rows in soundset (start). The shot tick begins AND finishes
    // the fire action's ACTIVE phase, so both legs land on that tick — begin plays
    // ActionDef+8, finish plays ActionDef+12. [orig: ActionSlot_PlaySound @ 0x4010c0
    // from the begin shims; ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim
    // @ 0x401100, reached from WeaponAction_Fire @ 0x542d1a]
    WeaponFsmActionRow rows[3];
    set_row(rows[0], "fire", "anim_wpn_fire", 0, 6);
    std::snprintf(rows[0].soundsetend, sizeof(rows[0].soundsetend), "%s", "GS_TEST");
    set_row(rows[1], "reload", "anim_wpn_reload", 0, -1);
    std::snprintf(rows[1].soundset, sizeof(rows[1].soundset), "%s", "GF_RL_TEST");
    set_row(rows[2], "idle", "anim_wpn_idle", 0, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 3, clip_resolves, clip_seconds, nullptr, def);
    def.auto_fire = true;
    def.clip_capacity = 10;
    CHECK(std::strcmp(def.actions[wa::kFire].soundsetend, "GS_TEST") == 0);
    CHECK(std::strcmp(def.actions[wa::kFire].soundset, "") == 0);
    CHECK(std::strcmp(def.actions[wa::kReload].soundset, "GF_RL_TEST") == 0);

    WeaponSlotState s;
    s.clip = 10;
    s.reserve = 20;
    WeaponFsmInputs in;
    in.fire_pressed = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(ev.fired);
    CHECK(ev.action_started == wa::kFire);
    CHECK(ev.action_finished == wa::kFire); // the per-shot GS_* leg
}

void test_held_replay_skips_the_begin_sound_leg() {
    // The held-ready branch replays only the animation. It deliberately skips the
    // begin sound/ctrlreg legs [orig: ActionSlot_BeginActivePhase @ 0x53f88b].
    WeaponFsmDef def = make_ak_def();
    std::snprintf(def.actions[wa::kIdle].soundset,
                  sizeof(def.actions[wa::kIdle].soundset), "%s", "GS_MUST_NOT_PLAY");
    WeaponSlotState s = make_ak_slot();
    s.current = wa::kIdle;
    s.next = wa::kIdle;
    s.phase = weapon_phase::kHeld;
    s.counter = 2;
    WeaponFsmInputs in;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(ev.play_anim);
    CHECK(ev.action_started == -1);
}

void test_fire_abort_finishes_silently() {
    // A CanFire refusal finishes from phase 1 (never ACTIVE) -> no end-leg sound.
    // [orig: @ 0x542b5e finish; the phase==2 latch @ 0x53f7b9 stays false]
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 0;
    s.reserve = 0;
    WeaponFsmInputs in;
    in.fire_pressed = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.current == wa::kFire);
    CHECK(!ev.fired);
    CHECK(ev.action_finished == -1);
}

void test_reload_end_leg() {
    // Reload completion runs the finish shim -> the end leg fires exactly once.
    // [orig: WeaponAction_Reload @ 0x54316e]
    WeaponFsmDef def = make_ak_def();
    WeaponSlotState s = make_ak_slot();
    s.clip = 0;
    s.reserve = 300;
    WeaponFsmInputs in;
    in.reload_pressed = true;
    int end_legs = 0;
    for (int t = 0; t < 80; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.reload_pressed = false;
        if (ev.action_finished == wa::kReload) ++end_legs;
    }
    CHECK(end_legs == 1);
    CHECK(s.clip == 30);
}

void test_keep_scope_reload_class() {
    // ForceCrouch (0x40000 — the mortars) keeps the sight view through a reload:
    // no stash, no unscope, no rescope. [orig: @ 0x543126 -> g_rescopeAfterReload
    // = 0 @ 0x54313d; every other weapon stashes @ 0x54312f]
    WeaponFsmDef def = make_ak_def();
    def.flags |= 0x40000;
    WeaponSlotState s = make_ak_slot();
    s.clip = 5;
    WeaponFsmInputs in;
    in.scope_active = true;
    in.reload_pressed = true;
    bool unscoped = false, rescoped = false;
    for (int t = 0; t < 80; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.reload_pressed = false;
        unscoped |= ev.unscope;
        rescoped |= ev.rescope;
    }
    CHECK(!unscoped);
    CHECK(!rescoped);
    CHECK(!s.rescope_after_reload);
    CHECK(s.clip == 30); // the reload itself still applied
}

void test_non_local_recoil_makes_no_decision() {
    WeaponFsmDef def = make_ak_def();
    std::snprintf(def.actions[wa::kRecoil].particle,
                  sizeof(def.actions[wa::kRecoil].particle), "Effect_RemoteCas");
    WeaponSlotState s = make_ak_slot();
    s.clip = 0;
    s.reserve = 300;
    s.current = wa::kRecoil;
    s.next = wa::kIdle;
    s.phase = weapon_phase::kActive;
    s.counter = 0;
    WeaponFsmInputs in;
    in.is_local = false; // remote entities skip the arbiter [orig: @ 0x542fe9]
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.next == wa::kIdle); // no reload/emptyidle decision made
    CHECK(ev.action_effect < 0); // direct leg is local-player-only [orig: @0x542efa]
}

// Multi-clip variant rows: each 'auto' field is its OWN consuming ring read --
// serve-then-advance -- so a both-auto action reads TWO entries (possibly different
// durations), one-auto reads one, explicit delays read none, and existence stays a
// pure lookup [orig: Anim_InitActions @ 0x5421b3..0x5421ec -- Anim_GetDurationTicks
// @ 0x5421c5 (delaystart) / @ 0x5421d8 (delayend), each serving *slot then
// advancing *slot = next(+36) @ 0x53ee26; the lookup @ 0x5421ae].
void test_bake_ring_read_multiplicity() {
    struct RingCtx {
        float lengths[3];
        int head;
        int reads;
    } ring{{0.5f, 1.0f, 0.25f}, 0, 0};
    const auto ring_seconds = [](void *p, const char *) -> float {
        RingCtx *r = static_cast<RingCtx *>(p);
        ++r->reads;
        const float s = r->lengths[r->head];
        r->head = (r->head + 1) % 3;
        return s;
    };
    const auto always_resolves = [](void *, const char *) -> int { return 1; };

    // Both auto: read1 (0.5s -> 32) bakes delaystart, read2 (1.0s -> 64) bakes
    // delayend = 64 - 32 (ticks > ds) -- the two reads served DIFFERENT variants.
    WeaponFsmActionRow both;
    set_row(both, "reload", "anim_wpn_reload", -1, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(&both, 1, always_resolves, ring_seconds, &ring, def);
    CHECK(ring.reads == 2);
    CHECK(def.actions[wa::kReload].delay_start == 32);
    CHECK(def.actions[wa::kReload].delay_end == 32); // 64 - 32
    CHECK(def.actions[wa::kReload].has_anim);

    // One auto: exactly one more read, serving the NEXT ring entry (0.25s -> 17).
    ring.reads = 0;
    WeaponFsmActionRow one;
    set_row(one, "fire", "anim_wpn_fire", -1, 0);
    WeaponFsmDef def2;
    weapon_fsm_bake(&one, 1, always_resolves, ring_seconds, &ring, def2);
    CHECK(ring.reads == 1);
    CHECK(def2.actions[wa::kFire].delay_start == 17); // 0.25s: 15.625 + 0.5 -> 16 + 1
    CHECK(def2.actions[wa::kFire].delay_end == 0);

    // Explicit delays: zero reads, and the anim still binds (existence is the
    // lookup, not a read).
    ring.reads = 0;
    WeaponFsmActionRow fixed;
    set_row(fixed, "idle", "anim_wpn_idle", 3, 4);
    WeaponFsmDef def3;
    weapon_fsm_bake(&fixed, 1, always_resolves, ring_seconds, &ring, def3);
    CHECK(ring.reads == 0);
    CHECK(def3.actions[wa::kIdle].has_anim);
    CHECK(def3.actions[wa::kIdle].delay_start == 3);
    CHECK(def3.actions[wa::kIdle].delay_end == 4);
}

void test_recoil_effect_leg() {
    // The recoil-row DIRECT effect leg (casing eject): emitted exactly once per recoil
    // arbitration when the row authors a particle — for the AK's zero-length recoil,
    // one casing per shot at full auto cadence (the leg is NEVER suppressed by a live
    // previous casing: the original records no handle for it, param7=0).
    // [orig: WeaponAction_Recoil gate @ 0x542efa -> ActionSlot_SpawnEffect @ 0x542f64]
    WeaponFsmDef def = make_ak_def();
    std::snprintf(def.actions[wa::kRecoil].particle,
                  sizeof(def.actions[wa::kRecoil].particle), "Effect_CAR15Cas");
    std::snprintf(def.actions[wa::kRecoil].particle_userpoint,
                  sizeof(def.actions[wa::kRecoil].particle_userpoint), "bcasing");
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_pressed = true;
    in.fire_held = true;
    int fired = 0, casings = 0;
    for (int t = 0; t < 120; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        if (ev.fired) ++fired;
        if (ev.action_effect == wa::kRecoil) ++casings;
    }
    CHECK(fired >= 3);
    CHECK(casings == fired); // one casing per shot, none suppressed

    // A recoil row WITHOUT a particle emits no effect leg [orig: the ActionDef+16
    // zero-handle arm of the gate @ 0x542efa].
    WeaponFsmDef quiet = make_ak_def();
    WeaponSlotState qs = make_ak_slot();
    in.fire_pressed = true;
    in.fire_held = true;
    int quiet_effects = 0;
    for (int t = 0; t < 60; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(quiet, qs, in, ev);
        in.fire_pressed = false;
        if (ev.action_effect >= 0) ++quiet_effects;
    }
    CHECK(quiet_effects == 0);

    // The delaystart-carried bolt shape (M24: recoil delaystart 60): the casing
    // ejects at the recoil ARBITER tick — delaystart ticks into the recoil, not at
    // the shot [orig: the counter gate @ 0x542eb7 in front of the spawn].
    WeaponFsmActionRow rows[6];
    set_row(rows[0], "idle", "anim_wpn_idle", 0, -1);
    set_row(rows[1], "emptyidle", "anim_wpn_idle", 0, -1);
    set_row(rows[2], "fire", "anim_wpn_fire", 0, -1);
    set_row(rows[3], "recoil", "anim_wpn_recoil", 60, 0);
    set_row(rows[4], "reload", "anim_wpn_reload", 0, -1);
    set_row(rows[5], "empty", "anim_wpn_empty", 0, -1);
    WeaponFsmDef bolt;
    weapon_fsm_bake(rows, 6, clip_resolves, clip_seconds, nullptr, bolt);
    bolt.auto_fire = false;
    bolt.clip_capacity = 10;
    std::snprintf(bolt.actions[wa::kRecoil].particle,
                  sizeof(bolt.actions[wa::kRecoil].particle), "Effect_M24Cas");
    WeaponSlotState bs;
    bs.clip = 10;
    bs.reserve = 40;
    WeaponFsmInputs bin;
    bin.fire_pressed = true;
    int fired_tick = -1, casing_tick = -1;
    for (int t = 0; t < 200 && casing_tick < 0; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(bolt, bs, bin, ev);
        bin.fire_pressed = false;
        if (ev.fired && fired_tick < 0) fired_tick = t;
        if (ev.action_effect == wa::kRecoil && casing_tick < 0) casing_tick = t;
    }
    CHECK(fired_tick >= 0);
    CHECK(casing_tick >= 0);
    CHECK(casing_tick - fired_tick >= 60); // the bolt-work delay carried the eject
}

void test_switch_completion_signal() {
    // SWITCHFROM and SWITCHRANK reach the pending-slot swap directly in their
    // handlers rather than through FinishActivePhase. The caller needs a distinct,
    // one-tick signal for that inventory handoff.
    WeaponFsmActionRow rows[2];
    set_row(rows[0], "switchfrom", "", 0, 1);
    set_row(rows[1], "switchrank", "", 0, 0);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 2, clip_resolves, clip_seconds, nullptr, def);
    WeaponFsmInputs in;

    WeaponSlotState holster;
    holster.phase = weapon_phase::kDone;
    weapon_fsm_queue_switch_from(holster);
    int holster_completions = 0;
    for (int t = 0; t < 80; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, holster, in, ev);
        if (ev.switch_completed) {
            ++holster_completions;
            CHECK(ev.action_finished == -1);
        }
    }
    CHECK(holster_completions == 1);

    WeaponSlotState rank;
    rank.phase = weapon_phase::kDone;
    weapon_fsm_queue_switch_rank(rank);
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, rank, in, ev);
    CHECK(ev.switch_completed);
    CHECK(ev.action_finished == -1);
}

void test_emplaced_switchfrom_and_try_switchto_contract() {
    WeaponFsmActionRow rows[1];
    set_row(rows[0], "switchfrom", "", 1, 1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 1, clip_resolves, clip_seconds, nullptr, def);

    WeaponSlotState holster;
    holster.phase = weapon_phase::kDone;
    weapon_fsm_queue_switch_from(holster);
    WeaponFsmInputs in;
    in.instant_emplaced_switch = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, holster, in, ev);
    CHECK(ev.switch_completed); // shipped emplaced rows use delaystart=1

    WeaponSlotState ready_target;
    ready_target.phase = weapon_phase::kNone;
    ready_target.next = wa::kFire;
    weapon_fsm_try_queue_switch_to(ready_target);
    CHECK(ready_target.next == wa::kSwitchTo);

    WeaponSlotState busy_target;
    busy_target.current = wa::kRecoil;
    busy_target.phase = weapon_phase::kEntered;
    busy_target.next = wa::kFire;
    weapon_fsm_try_queue_switch_to(busy_target);
    CHECK(busy_target.current == wa::kRecoil);
    CHECK(busy_target.phase == weapon_phase::kEntered);
    CHECK(busy_target.next == wa::kIdle);

    WeaponSlotState drawing_target;
    drawing_target.current = wa::kSwitchTo;
    drawing_target.phase = weapon_phase::kActive;
    drawing_target.next = wa::kFire;
    weapon_fsm_try_queue_switch_to(drawing_target);
    CHECK(drawing_target.next == wa::kFire);

    WeaponSlotState ready_phase_target;
    ready_phase_target.phase = weapon_phase::kHeld;
    ready_phase_target.next = wa::kFire;
    weapon_fsm_try_queue_switch_to(ready_phase_target);
    CHECK(ready_phase_target.next == wa::kIdle);
}

} // namespace

// --- the heat model [orig: WeaponSlot_CalcAccumulatedHeat @ 0x53f780, the stamp
// @ 0x542f8b..0x542fdc, the deny/expiry @ 0x540fed..0x54125f] -------------------
//
// The def numbers are the shipped JOX 'WPN_EMPLCD50' line 'heat_values 2,4' after
// the parser's two truncating divides: 2 -> 131072/100 = 1310 per shot, 4 ->
// 262144/6200 = 42 per tick. Everything below is derived from that pair alone.
WeaponFsmDef make_emplaced_50_def() {
    WeaponFsmDef def = make_ak_def();
    def.heat_per_shot = 1310;
    def.heat_decay_per_tick = 42;
    def.heat_glow_threshold = 32768; // 'heat_effect heat, .5, ...' -> .5 in 16.16
    return def;
}

void test_heat_is_derived_from_the_window_deadline() {
    WeaponFsmDef def = make_emplaced_50_def();
    WeaponSlotState s{};
    // Cold: no window, no heat.
    CHECK(weapon_slot_accumulated_heat(def, s, 1000) == 0);
    // A window 100 ticks out reads rate x ticks-remaining, and sheds itself as the
    // tick advances — the cooldown is implicit in the deadline, never stored.
    s.heat_window_end_tick = 1100;
    CHECK(weapon_slot_accumulated_heat(def, s, 1000) == 42 * 100);
    CHECK(weapon_slot_accumulated_heat(def, s, 1050) == 42 * 50);
    CHECK(weapon_slot_accumulated_heat(def, s, 1100) == 0); // expired, not negative
    CHECK(weapon_slot_accumulated_heat(def, s, 1200) == 0);
    // The gate is the DEF field: a weapon with no heat model reads 0 even with a
    // window left over [orig: the def+876 test @ 0x53f79b].
    WeaponFsmDef cold = make_ak_def();
    CHECK(weapon_slot_accumulated_heat(cold, s, 1000) == 0);

    // The retail IMUL is low-32-bit, not widened arithmetic. INT32_MAX * 2
    // becomes 0xFFFFFFFE (-2); the world publisher's signed cap leaves it
    // alone and its final MOVZX AX exposes 65534.
    def.heat_decay_per_tick = INT32_MAX;
    s.heat_window_end_tick = 1002;
    CHECK(weapon_slot_accumulated_heat(def, s, 1000) == -2);
    CHECK(weapon_slot_world_heat_glow(def, s, 1000) == 65534);
}

void test_world_model_heat_glow_has_its_own_retail_clamp() {
    WeaponFsmDef def = make_emplaced_50_def();
    WeaponSlotState s{};
    const int32_t now = 1000;

    // The world-model cache writes zero every time the inline slot is cold; it
    // does not retain the prior entity's global CTRL value.
    CHECK(weapon_slot_world_heat_glow(def, s, now) == 0);
    s.heat_window_end_tick = now;
    CHECK(weapon_slot_world_heat_glow(def, s, now) == 0);

    // 42 * 1560 fits below the unsigned-word ceiling; one more tick of window
    // crosses it and must publish 0xFFFF, never FP's 0x10000 endpoint.
    s.heat_window_end_tick = now + 1560;
    CHECK(weapon_slot_accumulated_heat(def, s, now) == 65520);
    CHECK(weapon_slot_world_heat_glow(def, s, now) == 65520);
    s.heat_window_end_tick = now + 1561;
    CHECK(weapon_slot_accumulated_heat(def, s, now) == 65562);
    CHECK(weapon_slot_world_heat_glow(def, s, now) == 0xFFFF);

    // A stale active deadline on a definition with no heat model still
    // publishes literal zero.
    WeaponFsmDef cold = make_ak_def();
    CHECK(weapon_slot_world_heat_glow(cold, s, now) == 0);
}

// Hold the trigger on an emplaced .50 and walk the whole heat arc. The shot numbers
// below belong to THIS def's cadence (the AK rows fire every 8 ticks) crossed with
// the .50's heat rate: each shot buys 32 ticks of window but only 8 tick's worth is
// spent before the next one, so the level climbs 1008 a shot and the gun quits after
// the 65th. Change either the rows or heat_values and these move together.
void test_emplaced_gun_overheats_and_locks_out() {
    WeaponFsmDef def = make_emplaced_50_def();
    WeaponSlotState s{};
    def.clip_capacity = -1; // 'clipsize -1' — the emplaced guns feed from a belt
    WeaponFsmInputs in;
    in.fire_held = true;
    in.fire_pressed = true;
    WeaponFsmEvents ev;

    int shots = 0, first_glow_shot = 0, dry_at_shot = 0;
    int32_t peak_heat = 0, heat_when_denied = 0;
    int32_t tick = 0;
    for (; tick < 3000; ++tick) {
        in.current_tick = tick;
        // What the pump itself sees when it decides whether to deny.
        const int32_t heat_at_pump = weapon_slot_accumulated_heat(def, s, tick);
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false; // the press edge starts it; the refire chain sustains
        const int32_t h = weapon_slot_accumulated_heat(def, s, tick);
        if (h > peak_heat) peak_heat = h;
        if (ev.fired) ++shots;
        if (!first_glow_shot && h > def.heat_glow_threshold) first_glow_shot = shots;
        if (!dry_at_shot && ev.dry_fired) {
            dry_at_shot = shots;
            heat_when_denied = heat_at_pump;
            break;
        }
    }
    // 32 ticks of window bought per shot [orig: @ 0x542fb4].
    CHECK(def.heat_per_shot / def.heat_decay_per_tick + 1 == 32);
    // The glow lights around half the arc, long before the gun quits.
    CHECK(first_glow_shot == 33);
    // The deny converts the queued FIRE into the dry click, and it fires exactly
    // when the level the pump reads has passed kFull [orig: @ 0x541046].
    CHECK(dry_at_shot == 65);
    CHECK(heat_when_denied > weapon_heat::kFull);
    CHECK(s.current == wa::kEmpty);
    // Nothing ever banks past the ceiling.
    CHECK(peak_heat <= weapon_heat::kCeiling);

    // The volley does NOT resume on its own: the refire chain only re-arms from the
    // recoil window, so an overheat costs the player a fresh trigger press — the
    // same consequence the original's deferred-event design produces.
    const int shots_at_deny = shots;
    for (int i = 0; i < 400; ++i) {
        in.current_tick = ++tick;
        weapon_fsm_tick(def, s, in, ev);
        if (ev.fired) ++shots;
    }
    CHECK(shots == shots_at_deny);
    // The window is still live and still shedding at the authored rate — the gun is
    // hot for a good while after it quits, it does not reset on the dry click.
    CHECK(s.heat_window_end_tick > tick);
    CHECK(weapon_slot_accumulated_heat(def, s, tick) ==
          def.heat_decay_per_tick * (s.heat_window_end_tick - tick));
    // Once the deadline passes, the pump zeroes the window and the gun reads cold.
    tick = s.heat_window_end_tick + 1;
    in.current_tick = tick;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.heat_window_end_tick == 0); // [orig: @ 0x54125f]
    CHECK(weapon_slot_accumulated_heat(def, s, tick) == 0);
}

// The ceiling clamp, driven straight at the stamp: a window already worth nearly the
// ceiling takes one more shot and gets re-stamped to a fixed overrun instead of
// banking the sum. [orig: @ 0x542fc4..0x542fdc]
void test_heat_ceiling_clamps_the_window() {
    WeaponFsmDef def = make_emplaced_50_def();
    const int32_t now = 10000;
    const int32_t per_shot_ticks = def.heat_per_shot / def.heat_decay_per_tick + 1;

    WeaponSlotState s{};
    s.heat_window_end_tick = now + weapon_heat::kCeiling / def.heat_decay_per_tick;
    CHECK(weapon_slot_accumulated_heat(def, s, now) == 73710); // just under the ceiling
    // Unclamped, this shot would bank 75054 — past the ceiling.
    s.heat_window_end_tick += per_shot_ticks;
    CHECK(weapon_slot_accumulated_heat(def, s, now) == 75054);
    // The clamp re-stamps to the fixed overrun the original computes.
    s.heat_window_end_tick = now + weapon_heat::kCeiling / def.heat_decay_per_tick + 1;
    CHECK(weapon_slot_accumulated_heat(def, s, now) == 73752);
    // The lockout that buys: cooling from the ceiling back under kFull.
    const int32_t lockout =
            (73752 - weapon_heat::kFull) / def.heat_decay_per_tick + 1;
    CHECK(lockout == 196); // ~3.1 s at the 62 Hz logic rate
    CHECK(weapon_slot_accumulated_heat(def, s, now + lockout) <= weapon_heat::kFull);
}

// An infantry weapon authors no heat_values, so nothing in the model may engage.
void test_no_heat_model_never_stamps_a_window() {
    WeaponFsmDef def = make_ak_def(); // heat_per_shot == 0
    WeaponSlotState s = make_ak_slot();
    WeaponFsmInputs in;
    in.fire_held = true;
    in.fire_pressed = true;
    WeaponFsmEvents ev;
    for (int32_t tick = 0; tick < 400; ++tick) {
        in.current_tick = tick;
        weapon_fsm_tick(def, s, in, ev);
        in.fire_pressed = false;
        CHECK(s.heat_window_end_tick == 0);
        CHECK(weapon_slot_accumulated_heat(def, s, tick) == 0);
    }
    CHECK(s.next != wa::kEmpty || s.clip == 0); // never denied by heat
}

// Submerging drops the window unless the def carries Underwater (Flags 0x4).
void test_submerging_clears_the_heat_window() {
    WeaponFsmDef def = make_emplaced_50_def();
    WeaponSlotState s{};
    s.heat_window_end_tick = 500;
    WeaponFsmInputs in;
    in.current_tick = 100;
    in.submerged = true;
    WeaponFsmEvents ev;
    weapon_fsm_tick(def, s, in, ev);
    CHECK(s.heat_window_end_tick == 0); // [orig: @ 0x54125f]

    WeaponFsmDef wet = make_emplaced_50_def();
    wet.flags |= weapon_flag::kUnderwater;
    WeaponSlotState s2{};
    s2.heat_window_end_tick = 500;
    weapon_fsm_tick(wet, s2, in, ev);
    CHECK(s2.heat_window_end_tick == 500); // the window survives
}

int main() {
    test_ticks_from_ms();
    test_sights_card_eligibility();
    test_bake();
    test_bake_ring_read_multiplicity();
    test_fire_chains_recoil();
    test_auto_refire_cadence();
    test_revx_m4_zero_recoil_auto_cadence();
    test_semi_no_auto_refire();
    test_burst3();
    test_empty_clip_paths();
    test_empty_clip_held_auto_reload();
    test_tap_during_fire_banks_one();
    test_auto_reload_from_recoil();
    test_reload_scope_stash();
    test_reload_request_gate();
    test_scope_queue();
    test_idle_plays_once_per_entry();
    test_action_sound_legs();
    test_held_replay_skips_the_begin_sound_leg();
    test_fire_abort_finishes_silently();
    test_reload_end_leg();
    test_keep_scope_reload_class();
    test_non_local_recoil_makes_no_decision();
    test_recoil_effect_leg();
    test_switch_completion_signal();
    test_emplaced_switchfrom_and_try_switchto_contract();
    test_heat_is_derived_from_the_window_deadline();
    test_world_model_heat_glow_has_its_own_retail_clamp();
    test_emplaced_gun_overheats_and_locks_out();
    test_heat_ceiling_clamps_the_window();
    test_no_heat_model_never_stamps_a_window();
    test_submerging_clears_the_heat_window();
    if (failures == 0) std::printf("weapon_fsm_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
