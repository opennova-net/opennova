// BMS event runtime: the second scripting evaluator (alongside the WAC VM).
//
// Faithful port of the EventTrigger_*/EventAction_* system (Jointops.exe):
// EventTrigger_UpdateEntry @0x454c30 (per-event delay/cooldown timers + fire),
// the trigger chain combiner @0x454050, EventTrigger_EvaluateCondition @0x453620,
// EventAction_Dispatch @0x4542e0, and the three update passes — pre-mission
// (UpdateAllWithFlag2 @0x454dc0), post-mission (UpdateAllWithFlag4 @0x454e00) and
// the normal quarter-list round-robin (@0x454d50, gated to every 16th tick in
// Server_TickUpdate @0x51d7e0). It is an ISystem that ticks on the shared World
// and drives the SAME entities + variable store the WAC VM uses (the dword_C6B240
// mission-variable array is literally shared: BMS MisvarChange and WAC set(V#)
// mutate one store).
#pragma once

#include <cstdint>
#include <vector>

#include <formats/mission/bms.h>
#include <runtime/world/system.h>
#include <runtime/world/script_events.h>

namespace opennova::mission {

// One runtime event = a BMS Event plus its resolved trigger/action slices and the
// runtime words the original keeps in the 24-byte event record.
struct ScriptedEvent {
    bms::Event event{};
    // Bounded raw-byte slices. The chain, the dispatch and the zone resolvers
    // read both counts SIGNED: 128..255 evaluate trigger 0 only, run no
    // actions and resolve no zone refs. [orig: EventTrigger_EvaluateChain
    //  @0x45405F/@0x45408C/@0x4540C7; the dispatch @0x454C92/@0x454D01; the
    //  resolvers @0x453022/@0x4530C8 and @0x453127/@0x4531D2]
    std::vector<bms::Trigger> triggers;
    std::vector<bms::Action> actions;
    // Live timer words and their reloads. Units: a reload is the authored value
    // << 6, and a live word drops 64 per processing pass, so for normal events
    // (processed once per 64 ticks) one authored unit amortizes to 64 ticks
    // (~1.02 s at 62 Hz). [orig: event +12 repeat countdown, +14 its reload,
    //  +16 activation countdown, +18 its reload]
    uint16_t repeat_countdown = 0;
    uint16_t repeat_reload = 0;
    uint16_t activate_countdown = 0;
    uint16_t activate_reload = 0;
    // Active latch byte: set the moment the chain passes, cleared when the
    // repeat cooldown expires (or by a ResetEvent action). For fire-once events
    // this IS the has-fired state. [orig: event byte +20]
    uint8_t active = 0;
};

class BmsEventSystem : public opennova::world::ISystem, public opennova::world::IScriptEventQuery {
public:
    const char *name() const override { return "bms_events"; }

    // Build runtime events from the parsed BMS arrays (resolves trigger_index/
    // count and action_index/count into per-event slices, as EventTrigger_LoadAllData
    // @0x453eb0 resolves the relative pointers).
    void load(const std::vector<bms::Event> &events,
              const std::vector<bms::Trigger> &triggers,
              const std::vector<bms::Action> &actions);

    void on_load(opennova::world::World &) override;
    void tick(opennova::world::World &world, const opennova::world::TickContext &ctx) override;

    // The post-mission pass: ONE whole-list sweep over the PostMission-flag
    // entries, run once by the mission teardown after pools 0..2 are destroyed
    // (MissionKernel::run_post_mission_pass), never periodically (D-EVT-4).
    // [orig: EventTrigger_UpdateAllWithFlag4 @0x454e00; live caller
    // Game_TeardownMission @0x52266c (the SP restart's call @0x5263ae finds
    // the list freed)]
    // (The pre pass is the tick()'s TickPhase::PreMission path under the same
    // one-call-per-transition contract [orig: Game_StartMission @0x525b86].)
    void run_post_mission_pass(opennova::world::World &w);

    // The session-scoped load-parity toggle the SecondTimeThrough trigger
    // category reads raw: the static image value is 1 and the ONLY writer is
    // one XOR per BMS load, so the first load of a session reads 0 (false) and
    // a restart reads 1 — alternating. Save-game persistence is unported (no
    // save system yet). [orig: dword_815174; toggle at the end of
    // EventTrigger_LoadAllData @0x454029; read raw @0x453b24] (D-EVT-3 cat 5)
    static bool second_time_through();

    // Player-AWOL 64-tick quanta: incremented once per full quarter cycle while
    // the local player sits outside every active zone, reset otherwise; the
    // PlayerAwol trigger compares it against the authored threshold.
    // [orig: counter dword_A89160, updated by Entity_UpdatePlayerAwolCounter
    // @0x439dc0 from the quarter pass @0x454d50 when the cursor is 0] (D-EVT-2)
    int32_t awol_count() const { return awol_64tick_count_; }

    // "Event N has fired": the active latch is set and the activation delay has
    // elapsed. This is exactly what the Event trigger category reads.
    // [orig: EventTrigger_EvaluateCondition @0x453620 case 3 @0x453a75 —
    //  entry[+20] && entry[+16 word] == 0]
    bool event_fired(size_t index) const {
        return index < events_.size() && events_[index].active &&
               events_[index].activate_countdown == 0;
    }

    // WAC sees the active latch even while an activation delay is pending.
    // [orig: WacCmd_Event @0x4ED1E0, unlike BMS event_fired above]
    bool is_active(int32_t index) const override {
        return index >= 0 && static_cast<size_t>(index) < events_.size() &&
                events_[static_cast<size_t>(index)].active;
    }

    const std::vector<ScriptedEvent> &events() const { return events_; }

    // Test seams over the private evaluator/dispatcher and the raw 24-byte
    // record words (public API for the ctest suite; no behavior of their own).
    int32_t evaluate_trigger_for_test(opennova::world::World &w, const bms::Trigger &t) {
        return evaluate_trigger(w, t);
    }
    int32_t evaluate_chain_for_test(opennova::world::World &w, size_t index) {
        return evaluate_chain(w, events_[index]);
    }
    void update_entry_for_test(opennova::world::World &w, size_t index) {
        update_entry(w, events_[index]);
    }
    void dispatch_action_for_test(opennova::world::World &w, const bms::Action &a) {
        dispatch_action(w, a);
    }
    ScriptedEvent &event_for_test(size_t index) { return events_[index]; }

private:
    std::vector<ScriptedEvent> events_;
    int normal_gate_ = 0;    // every-16th-tick gate [orig: dword_C8D808 in Server_TickUpdate]
    int quarter_cursor_ = 0; // round-robin quarter index 0..3 [orig: dword_AE06FC]
    int32_t awol_64tick_count_ = 0; // [orig: dword_A89160] (D-EVT-2)
    // One-shot: file zone IDs rewrite to zone-array INDICES on the first on_load
    // after load() (the params then HOLD indices — re-resolving would corrupt).
    bool zone_refs_resolved_ = false;

    // Load-time zone-ref resolution [orig: the two post-load resolvers called from
    // mission start — @0x453000 (trigger refs; the IDB carried a 'WeaponActionRefs'
    // misnomer) and @0x453100 (action refs)]. Trigger records carry the authored
    // zone ID; the resolver rewrites it to the zone ARRAY INDEX by matching the
    // record id, and NEUTERS the trigger (main_type=0, sub_type=0, flags kept)
    // when the id is missing or the zone box is degenerate (x_min==x_max ||
    // y_min==y_max). Covers Group/Single IsWithinArea (sub 10, param2) and
    // PlayerSatchel (main 7 sub 37, param1); the action sibling covers
    // AreaAiRed/Blue (types 12/13, param1) and overwrites the index at once with
    // the zone box: p1 = x_min, p3 = y_min, p4 = x_max, reserved1 = y_max, the
    // words the dispatch's bounds test and command arguments then read.
    void resolve_zone_refs(opennova::world::World &w);

    void update_entry(opennova::world::World &w, ScriptedEvent &se);
    void fire(opennova::world::World &w, ScriptedEvent &se);
    // Both return the original's raw ints: every evaluator answers 0/1 except
    // the Berserk read (the 0x200 bit) and the load-parity word, and the chain
    // folds them bitwise. [orig: EventTrigger_EvaluateCondition @0x453620;
    //  EventTrigger_EvaluateChain @0x454050]
    int32_t evaluate_chain(opennova::world::World &w, const ScriptedEvent &se);
    int32_t evaluate_trigger(opennova::world::World &w, const bms::Trigger &t);
    void dispatch_action(opennova::world::World &w, const bms::Action &a, int32_t event = -1, int32_t action = -1);
};

} // namespace opennova::mission
