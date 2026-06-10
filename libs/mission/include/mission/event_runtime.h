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
#include "world/world.h"

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

    // Post-mission phase: when set, the tick runs the PostMission-flag pass
    // [orig: UpdateAllWithFlag4 @0x454e00, called from the debrief/video contexts]
    // instead of the normal quarter pass. The host flips this at mission end.
    void set_post_mission(bool v) { post_mission_ = v; }
    bool post_mission() const { return post_mission_; }

    // "Event N has fired": the active latch is set and the activation delay has
    // elapsed. This is exactly what the Event trigger category reads.
    // [orig: EventTrigger_EvaluateCondition @0x453620 case 3 @0x453a75 —
    //  entry[+20] && entry[+16 word] == 0]
    bool event_fired(size_t index) const {
        return index < events_.size() && events_[index].active &&
               events_[index].activate_countdown == 0;
    }

    const std::vector<ScriptedEvent> &events() const { return events_; }

private:
    std::vector<ScriptedEvent> events_;
    int normal_gate_ = 0;    // every-16th-tick gate [orig: dword_C8D808 in Server_TickUpdate]
    int quarter_cursor_ = 0; // round-robin quarter index 0..3 [orig: dword_AE06FC]
    bool post_mission_ = false;

    void update_entry(opennova::world::World &w, ScriptedEvent &se);
    void fire(opennova::world::World &w, ScriptedEvent &se);
    bool evaluate_chain(opennova::world::World &w, const std::vector<bms::Trigger> &triggers);
    bool evaluate_trigger(opennova::world::World &w, const bms::Trigger &t);
    void dispatch_action(opennova::world::World &w, const bms::Action &a);
};

} // namespace opennova::mission

#endif // OPENNOVA_MISSION_EVENT_RUNTIME_H
