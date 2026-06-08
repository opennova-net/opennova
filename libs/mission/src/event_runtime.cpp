#include "mission/event_runtime.h"

namespace opennova::mission {

using world::World;

void BmsEventSystem::load(const std::vector<bms::Event> &events,
                          const std::vector<bms::Trigger> &triggers,
                          const std::vector<bms::Action> &actions) {
    events_.clear();
    events_.reserve(events.size());
    for (const bms::Event &e : events) {
        ScriptedEvent se;
        se.event = e;
        // Resolve trigger/action slices by index+count (EventTrigger_LoadAllData
        // fixes up the relative pointers; here we copy the slices).
        for (int i = 0; i < e.trigger_count; ++i) {
            size_t idx = static_cast<size_t>(e.trigger_index) + i;
            if (idx < triggers.size()) se.triggers.push_back(triggers[idx]);
        }
        for (int i = 0; i < e.action_count; ++i) {
            size_t idx = static_cast<size_t>(e.action_index) + i;
            if (idx < actions.size()) se.actions.push_back(actions[idx]);
        }
        events_.push_back(std::move(se));
    }
}

void BmsEventSystem::on_load(World &) {
    for (ScriptedEvent &se : events_) {
        se.fired = false;
        se.active = false;
    }
}

bool BmsEventSystem::evaluate_trigger(World &w, const bms::Trigger &t) {
    auto &cmds = w.commands;
    switch (t.main_type) {
        case bms::TriggerMainType::MissionVariable: {
            // [orig: EventTrigger_EvaluateCondition cat 4 — dword_C6B240[param1] <op> param2.]
            // THE shared variable store (same array WAC V# uses).
            int32_t v = w.vars.get_mission(t.param1);
            switch (static_cast<bms::MissionVariableTriggerType>(t.sub_type)) {
                case bms::MissionVariableTriggerType::MissionVariableIsEqual: return v == t.param2;
                case bms::MissionVariableTriggerType::MissionVariableIsLessThan: return v < t.param2;
                case bms::MissionVariableTriggerType::MissionVariableIsGreaterThan: return v > t.param2;
                case bms::MissionVariableTriggerType::MissionVariableIsLessThanOrEqual: return v <= t.param2;
                case bms::MissionVariableTriggerType::MissionVariableIsGreaterThanOrEqual: return v >= t.param2;
            }
            return false;
        }
        case bms::TriggerMainType::Single: {
            switch (static_cast<bms::SingleTriggerType>(t.sub_type)) {
                case bms::SingleTriggerType::SingleDestroyed:
                    return cmds.ssn_dead(static_cast<uint16_t>(t.param1));
                case bms::SingleTriggerType::SingleAlive:
                    return cmds.ssn_alive(static_cast<uint16_t>(t.param1));
                case bms::SingleTriggerType::SingleIsWithinArea:
                    return cmds.ssn_in_area(static_cast<uint16_t>(t.param1), t.param2);
                default:
                    return false;
            }
        }
        case bms::TriggerMainType::Group: {
            switch (static_cast<bms::GroupTriggerType>(t.sub_type)) {
                case bms::GroupTriggerType::GroupDestroyed:
                    return cmds.group_dead(t.param1);
                case bms::GroupTriggerType::GroupAlive:
                    return cmds.group_alive(t.param1);
                default:
                    return false;
            }
        }
        case bms::TriggerMainType::Event: {
            // True if the referenced event has fired. [orig: cat 3 / event matrix.]
            if (t.param1 >= 0 && static_cast<size_t>(t.param1) < events_.size()) {
                return events_[t.param1].fired;
            }
            return false;
        }
        default:
            // Team/zone, net/session, input/player categories not modeled in this
            // foundational core; treated as false.
            return false;
    }
}

bool BmsEventSystem::evaluate_chain(World &w, const std::vector<bms::Trigger> &triggers) {
    // [orig: sub_454050 @0x454050.] Each trigger negated by its own bit0; the
    // join operator (and/or/xor) comes from the PREVIOUS trigger. Empty => true.
    if (triggers.empty()) return true;
    bool acc = evaluate_trigger(w, triggers[0]);
    if (triggers[0].is_negated()) acc = !acc;
    for (size_t i = 1; i < triggers.size(); ++i) {
        bool v = evaluate_trigger(w, triggers[i]);
        if (triggers[i].is_negated()) v = !v;
        const bms::Trigger &prev = triggers[i - 1];
        if (prev.is_or()) acc = acc || v;
        else if (prev.is_xor()) acc = acc != v;
        else acc = acc && v;
    }
    return acc;
}

void BmsEventSystem::dispatch_action(World &w, const bms::Action &a) {
    // [orig: EventAction_Dispatch @0x4542e0 switch(action_type).]
    auto &cmds = w.commands;
    switch (a.action_type) {
        case bms::ActionType::MisvarChange: {
            // [orig: case 5 — writes dword_C6B240[param1]; the shared var store.]
            int32_t cur = w.vars.get_mission(a.param1);
            switch (static_cast<bms::MissionVariableActionSubType>(a.action_sub_type)) {
                case bms::MissionVariableActionSubType::Set: w.vars.set_mission(a.param1, a.param2); break;
                case bms::MissionVariableActionSubType::Add: w.vars.set_mission(a.param1, cur + a.param2); break;
                case bms::MissionVariableActionSubType::Subtract: w.vars.set_mission(a.param1, cur - a.param2); break;
                case bms::MissionVariableActionSubType::Increment: w.vars.set_mission(a.param1, cur + 1); break;
                case bms::MissionVariableActionSubType::Decrement: w.vars.set_mission(a.param1, cur - 1); break;
                default: break;
            }
            break;
        }
        case bms::ActionType::KillSingle: cmds.kill_ssn(static_cast<uint16_t>(a.param1)); break;
        case bms::ActionType::VaporizeSingle: cmds.remove_ssn(static_cast<uint16_t>(a.param1)); break;
        case bms::ActionType::KillGroup: cmds.kill_group(a.param1); break;
        case bms::ActionType::RedirectSingleTo: cmds.set_ssn_waypoint(static_cast<uint16_t>(a.param1), a.param2); break;
        case bms::ActionType::RedirectGroupTo: cmds.group_to_waypoint(a.param1, a.param2); break;
        case bms::ActionType::OutputText:
            w.effects.push({"text", a.param1, 0, 0, 0, std::string()});
            break;
        case bms::ActionType::BlueWin: w.effects.push({"win", 1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::RedWin: w.effects.push({"win", 2, 0, 0, 0, std::string()}); break;
        case bms::ActionType::GreenWin: w.effects.push({"win", 0, 0, 0, 0, std::string()}); break;
        case bms::ActionType::SubGoalWon: w.effects.push({"subgoal_won", a.param1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::SubGoalLost: w.effects.push({"subgoal_lost", a.param1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::ResetEvent:
            if (a.param1 >= 0 && static_cast<size_t>(a.param1) < events_.size()) {
                events_[a.param1].fired = false;
                events_[a.param1].active = false;
            }
            break;
        case bms::ActionType::ExecuteWac:
            // One front-end invoking the other: the BMS action installs/runs a
            // WAC program. Recorded as an effect here; the host wires the actual
            // WAC invocation (the WacSystem) at runtime.
            w.effects.push({"execute_wac", a.param1, 0, 0, 0, std::string()});
            break;
        default:
            w.effects.push({"bms_action", static_cast<int32_t>(a.action_type), a.action_sub_type,
                            a.param1, a.param2, std::string()});
            break;
    }
}

void BmsEventSystem::tick(World &w, const opennova::world::TickContext &ctx) {
    if (!ctx.is_authority) return; // BMS events run only on the authoritative host
    for (ScriptedEvent &se : events_) {
        const bool pre = (static_cast<uint32_t>(se.event.flags) &
                          static_cast<uint32_t>(bms::EventFlags::PreMission)) != 0;
        // The two passes: PreMission events on the pre pass, the rest on the normal
        // pass. [orig: UpdateAllWithFlag2 vs UpdateAllWithFlag4.]
        if (pre != ctx.pre_mission) continue;

        const bool repeat = (static_cast<uint32_t>(se.event.flags) &
                             static_cast<uint32_t>(bms::EventFlags::ResetAfter)) != 0;
        bool cond = evaluate_chain(w, se.triggers);
        // ResetAfter (repeat) events re-fire while the condition holds; fire-once
        // events fire only the first time the condition is true (cleared by
        // ResetEvent). The original re-arms a repeat via the reset_after/delay
        // timers (decrement 64/tick) -- modeling those exactly is a deferred
        // refinement; here repeat re-fires each tick (reset_after == 0).
        if (cond && (repeat || !se.fired)) {
            for (const bms::Action &a : se.actions) dispatch_action(w, a);
            se.fired = true;
            se.active = true;
        } else if (!cond) {
            se.active = false;
        }
    }
}

} // namespace opennova::mission
