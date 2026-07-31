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
#ifndef OPENNOVA_MISSION_EVENT_RUNTIME_H
#define OPENNOVA_MISSION_EVENT_RUNTIME_H

#include <cstdint>
#include <vector>

#include "mission/bms.h"
#include "world/system.h"

namespace opennova::mission {

// One runtime event = a BMS Event plus its resolved trigger/action slices and the
// runtime fields the original keeps in the 24-byte event record.
struct ScriptedEvent {
    bms::Event event{};
    std::vector<bms::Trigger> triggers;
    std::vector<bms::Action> actions;
    // Live timer words. Units: the on-disk reload values are authored_value << 6,
    // decremented 64 per processing pass — for normal events (processed once per
    // 64 ticks) one authored unit amortizes to 64 ticks (~1.02 s at 62 Hz).
    uint16_t activate_countdown = 0; // delay before actions run [orig: event +16]
    uint16_t repeat_countdown = 0;   // re-arm cooldown for repeat events [orig: event +12]
    // Active latch: set the moment the chain passes, cleared when the repeat
    // cooldown expires (or by a ResetEvent action). For fire-once events this IS
    // the has-fired state. [orig: event byte +20]
    bool active = false;
};

class BmsEventSystem : public opennova::world::ISystem {
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
    // entries, called by the embedder exactly once per transition — retail invokes
    // it from mission teardown and the SP round restart, never periodically
    // (D-EVT-4). [orig: EventTrigger_UpdateAllWithFlag4 @0x454e00; callers
    // Game_TeardownMission @0x52266c and the SP round-restart @0x5263a0]
    // (The pre pass is the tick()'s ctx.pre_mission path under the same
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
    // [orig: counter dword_A89160, updated by Entity_UpdateStuckCounter
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

    const std::vector<ScriptedEvent> &events() const { return events_; }

    // Test seams over the private evaluator/dispatcher (public API for the
    // ctest suite; no behavior of their own).
    bool evaluate_trigger_for_test(opennova::world::World &w, const bms::Trigger &t) {
        return evaluate_trigger(w, t);
    }
    void dispatch_action_for_test(opennova::world::World &w, const bms::Action &a) {
        dispatch_action(w, a);
    }

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
    // AreaAiRed/Blue (types 12/13, param1 — retail also inlines the zone box into
    // the action params; our runtime resolves boxes at dispatch, so the index
    // rewrite alone preserves behavior).
    void resolve_zone_refs(opennova::world::World &w);

    void update_entry(opennova::world::World &w, ScriptedEvent &se);
    void fire(opennova::world::World &w, ScriptedEvent &se);
    bool evaluate_chain(opennova::world::World &w, const std::vector<bms::Trigger> &triggers);
    bool evaluate_trigger(opennova::world::World &w, const bms::Trigger &t);
    void dispatch_action(opennova::world::World &w, const bms::Action &a);
};

} // namespace opennova::mission

#endif // OPENNOVA_MISSION_EVENT_RUNTIME_H
