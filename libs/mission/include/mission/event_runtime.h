// BMS event runtime: the second scripting evaluator (alongside the WAC VM).
//
// Faithful port of the EventTrigger_*/EventAction_* system (Jointops.exe):
// EventTrigger_UpdateEntry @0x454c30 (per-event timers + fire), the trigger
// chain combiner sub_454050 @0x454050 / EventTrigger_EvaluateCondition @0x453620,
// and EventAction_Dispatch @0x4542e0. It is an ISystem that ticks on the shared
// World and drives the SAME entities + variable store the WAC VM uses (the
// dword_C6B240 mission-variable array is literally shared: BMS MisvarChange and
// WAC set(V#) mutate one store).
#ifndef OPENNOVA_MISSION_EVENT_RUNTIME_H
#define OPENNOVA_MISSION_EVENT_RUNTIME_H

#include <vector>

#include "mission/bms.h"
#include "world/world.h"

namespace opennova::mission {

// One runtime event = a BMS Event plus its resolved trigger/action slices and the
// per-event fired/active bookkeeping (the runtime fields the original keeps in the
// 24-byte event record).
struct ScriptedEvent {
    bms::Event event{};
    std::vector<bms::Trigger> triggers;
    std::vector<bms::Action> actions;
    bool fired = false;  // fire-once gate (cleared by ResetEvent / re-arm)
    bool active = false; // edge flag [orig: event +20 active bool]
};

class BmsEventSystem : public opennova::world::ISystem {
public:
    const char *name() const override { return "bms_events"; }

    // Build runtime events from the parsed BMS arrays (resolves trigger_index/
    // count and action_index/count into per-event slices, as EventTrigger_LoadAllData
    // resolves the relative pointers).
    void load(const std::vector<bms::Event> &events,
              const std::vector<bms::Trigger> &triggers,
              const std::vector<bms::Action> &actions);

    void on_load(opennova::world::World &) override;
    void tick(opennova::world::World &world, const opennova::world::TickContext &ctx) override;

    const std::vector<ScriptedEvent> &events() const { return events_; }

private:
    std::vector<ScriptedEvent> events_;

    bool evaluate_chain(opennova::world::World &w, const std::vector<bms::Trigger> &triggers);
    bool evaluate_trigger(opennova::world::World &w, const bms::Trigger &t);
    void dispatch_action(opennova::world::World &w, const bms::Action &a);
};

} // namespace opennova::mission

#endif // OPENNOVA_MISSION_EVENT_RUNTIME_H
