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

void set_row(WeaponFsmActionRow &row, const char *name, const char *anim, int32_t ds,
             int32_t de) {
    std::snprintf(row.name, sizeof(row.name), "%s", name);
    std::snprintf(row.anim, sizeof(row.anim), "%s", anim);
    row.delaystart = ds;
    row.delayend = de;
}

// The JOX AK-47-shaped def: idle (delayend auto), fire (0/0), recoil (delaystart auto,
// anim_wpn_fire), reload (delayend auto), empty (0/auto), auto-fire flags.
WeaponFsmDef make_ak_def() {
    WeaponFsmActionRow rows[5];
    set_row(rows[0], "idle", "anim_wpn_idle", 0, -1);
    set_row(rows[1], "fire", "anim_wpn_idle", 0, 0);
    set_row(rows[2], "recoil", "anim_wpn_fire", -1, 0);
    set_row(rows[3], "reload", "anim_wpn_reload", 0, -1);
    set_row(rows[4], "empty", "anim_wpn_empty", 0, -1);
    WeaponFsmDef def;
    weapon_fsm_bake(rows, 5, clip_seconds, nullptr, def);
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

void test_bake() {
    WeaponFsmDef def = make_ak_def();
    // idle: ds explicit 0; de auto -> full clip ticks (64; ticks > ds -> ticks - 0).
    CHECK(def.actions[wa::kIdle].delay_start == 0);
    CHECK(def.actions[wa::kIdle].delay_end == 64);
    CHECK(def.actions[wa::kIdle].has_anim);
    // recoil: ds auto -> 7 ticks (anim_wpn_fire); de explicit 0.
    CHECK(def.actions[wa::kRecoil].delay_start == 7);
    CHECK(def.actions[wa::kRecoil].delay_end == 0);
    CHECK(std::strcmp(def.actions[wa::kRecoil].anim_key, "anim_wpn_fire") == 0);
    // absent rows (scopeup/scopedown/overheated/emptyidle...) bake to zero-length.
    CHECK(def.actions[wa::kScopeUp].delay_start == 0);
    CHECK(def.actions[wa::kScopeUp].delay_end == 0);
    CHECK(!def.actions[wa::kScopeUp].has_anim);
    // 'auto' with an unresolvable clip collapses to 0 [orig: @ 0x542202..0x542210].
    WeaponFsmActionRow bad;
    set_row(bad, "reload", "missing", -1, -1);
    WeaponFsmDef def2;
    weapon_fsm_bake(&bad, 1, clip_seconds, nullptr, def2);
    CHECK(def2.actions[wa::kReload].delay_start == 0);
    CHECK(def2.actions[wa::kReload].delay_end == 0);
    CHECK(!def2.actions[wa::kReload].has_anim);
    // delayend auto with ticks > delaystart -> ticks - delaystart
    // [orig: @ 0x5421e8..0x5421ec].
    WeaponFsmActionRow part;
    set_row(part, "reload", "anim_wpn_reload", 10, -1);
    WeaponFsmDef def3;
    weapon_fsm_bake(&part, 1, clip_seconds, nullptr, def3);
    CHECK(def3.actions[wa::kReload].delay_start == 10);
    CHECK(def3.actions[wa::kReload].delay_end == 22); // 32 - 10
    // suffix binding is case-insensitive [orig: stricmp @ 0x402360].
    WeaponFsmActionRow upper;
    set_row(upper, "RELOAD", "anim_wpn_reload", 3, 4);
    WeaponFsmDef def4;
    weapon_fsm_bake(&upper, 1, clip_seconds, nullptr, def4);
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
    CHECK(s.clip == 29);             // ammo consumed [orig: consume_weapon_ammo]
    CHECK(s.next == wa::kRecoil);    // [orig: @ 0x542c9e unconditional]
    CHECK(s.kick > 0);
    in.fire_pressed = false;
    weapon_fsm_tick(def, s, in, ev); // FIRE(done) -> RECOIL
    CHECK(s.current == wa::kRecoil);
    // The recoil anim (anim_wpn_fire) starts on its entry tick.
    CHECK(ev.play_anim);
    CHECK(std::strcmp(ev.anim_key, "anim_wpn_fire") == 0);
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
    // Recoil ds = 7 ticks; the full cycle fire->recoil->fire is 9 ticks
    // (entry tick + 7 counter ticks + the re-queued transition tick), so a 100-tick
    // hold lands 11-12 shots. The pin is the CADENCE (clip-length-driven), not an
    // invented rate.
    CHECK(fired >= 10 && fired <= 13);
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
    weapon_fsm_bake(rows, 5, clip_seconds, nullptr, def);
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
    // fire entry + 3 counter ticks + the zero-length recoil pass = a 5-6 tick
    // cycle -> 16-20 shots in 100 ticks. A wedge (0-2 shots) or a runaway
    // (>25) are the failure modes this pins.
    CHECK(fired >= 14 && fired <= 22);
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
    // [orig: @ 0x541cb9]; the recoil arbiter lands the same [orig: @ 0x543036]). A held
    // trigger then oscillates EMPTYIDLE <-> EMPTY (repeated dry clicks
    // [orig: WeaponSlot_RequestFire {1} -> 5]).
    CHECK(s.clip == 0);
    CHECK(saw_emptyidle);
    CHECK(s.current == wa::kEmptyIdle || s.current == wa::kEmpty);
    bool saw_empty_click = false;
    for (int t = 0; t < 20; ++t) {
        WeaponFsmEvents ev;
        weapon_fsm_tick(def, s, in, ev);
        saw_empty_click |= ev.dry_fired;
    }
    CHECK(saw_empty_click);
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
    weapon_fsm_bake(rows, 3, clip_seconds, nullptr, def);
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
}

} // namespace

int main() {
    test_ticks_from_ms();
    test_bake();
    test_fire_chains_recoil();
    test_auto_refire_cadence();
    test_revx_m4_zero_recoil_auto_cadence();
    test_semi_no_auto_refire();
    test_burst3();
    test_empty_clip_paths();
    test_auto_reload_from_recoil();
    test_reload_scope_stash();
    test_reload_request_gate();
    test_scope_queue();
    test_idle_plays_once_per_entry();
    test_action_sound_legs();
    test_fire_abort_finishes_silently();
    test_reload_end_leg();
    test_keep_scope_reload_class();
    test_non_local_recoil_makes_no_decision();
    if (failures == 0) std::printf("weapon_fsm_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
