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
        se.active = false;
        se.activate_countdown = 0;
        se.repeat_countdown = 0;
    }
    normal_gate_ = 0;
    quarter_cursor_ = 0;
    post_mission_ = false;
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
            // True iff the referenced event's active latch is set AND its activation
            // delay has elapsed. For repeat events this is a WINDOW (true until the
            // repeat cooldown expires), not a sticky has-fired bit.
            // [orig: EventTrigger_EvaluateCondition @0x453620 case 3 @0x453a75 —
            //  entry[+20] && entry[+16 word] == 0]
            if (t.param1 >= 0) return event_fired(static_cast<size_t>(t.param1));
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
        // AI-change family: param1 = target, action_sub_type selects the command, param2/3/4 are
        // its slots. Mutates the AI component in-engine (no effect emitted). [orig: cases 3/0x15
        // -> Entity_HandleAlertCommand / Entity_HandleAlertStateEvent @0x43dee0 -> Entity_ApplyCommand
        // @0x43ab60; AREA_AI_RED/BLUE apply to a zone's red/blue units. team: blue=1, red=2.]
        case bms::ActionType::ChangeSingleAI:
            cmds.apply_ai_command(static_cast<uint16_t>(a.param1), a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::ChangeGroupAI:
            cmds.apply_group_ai_command(a.param1, a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::AreaAiRed:
            cmds.apply_area_ai_command(a.param1, /*team=*/2, a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::AreaAiBlue:
            cmds.apply_area_ai_command(a.param1, /*team=*/1, a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::OutputText:
            w.effects.push({"text", a.param1, 0, 0, 0, std::string()});
            break;
        // Host-presentation effects: the engine hands these to the host's audio/HUD/overlay.
        case bms::ActionType::PlayWavList: // play dialog/wav param1 (param2 = always-play flag)
            w.effects.push({"dialog", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWaypoints:
            w.effects.push({"show_waypoints", a.param1, 0, 0, 0, std::string()});
            break;
        case bms::ActionType::SetLightState:
            w.effects.push({"set_light", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWinSubgoal:
            w.effects.push({"subgoal_show", a.param1, a.param2, /*lose=*/0, 0, std::string()});
            break;
        case bms::ActionType::ShowLoseSubgoal:
            w.effects.push({"subgoal_show", a.param1, a.param2, /*lose=*/1, 0, std::string()});
            break;
        case bms::ActionType::BlueWin: w.effects.push({"win", 1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::RedWin: w.effects.push({"win", 2, 0, 0, 0, std::string()}); break;
        case bms::ActionType::GreenWin: w.effects.push({"win", 0, 0, 0, 0, std::string()}); break;
        case bms::ActionType::SubGoalWon: w.effects.push({"subgoal_won", a.param1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::SubGoalLost: w.effects.push({"subgoal_lost", a.param1, 0, 0, 0, std::string()}); break;
        case bms::ActionType::ResetEvent:
            // Clears ONLY the active latch; live countdowns keep ticking.
            // [orig: EventAction_Dispatch case 0x22 @0x454974 — event[+20] = 0]
            if (a.param1 >= 0 && static_cast<size_t>(a.param1) < events_.size()) {
                events_[a.param1].active = false;
            }
            break;
        case bms::ActionType::AttachToEmplaced:
            // [orig: case 0x25 @0x4542e0 -> WacScript_TryMountEntityToVehicle @0x4f70f0.] The action
            // carries ONLY the occupant SSN (param1); the gun is found implicitly (proximity proxy).
            cmds.mount_best(static_cast<uint16_t>(a.param1));
            break;
        case bms::ActionType::ExecuteWac:
            // One front-end invoking the other: the BMS action installs/runs a
            // WAC program. Recorded as an effect here; the host wires the actual
            // WAC invocation (the WacSystem) at runtime.
            w.effects.push({"execute_wac", a.param1, 0, 0, 0, std::string()});
            break;
        default:
            // No faithful in-engine handler yet: record as an UNPORTED marker (coverage /
            // diagnostic only — never a host presentation effect). Supported missions should
            // emit zero of these; a test asserts that. [tracked-TODO, not a command stream.]
            w.effects.push({"unported_action", static_cast<int32_t>(a.action_type), a.action_sub_type,
                            a.param1, a.param2, std::string()});
            break;
    }
}

void BmsEventSystem::fire(World &w, ScriptedEvent &se) {
    // [orig: the dispatch loops @0x454ca6/@0x454d0e — every action entry in order.
    //  The g_InputActionBits/g_EventInputBitsMirror commit around the dispatch
    //  (input-trigger bit consumption) and the linked-spawn-point activation hook
    //  (@0x452ce0 -> SpawnPoint_SkipBlocked) are host subsystems not ported here;
    //  both are recorded in docs/mission/bms-event-runtime-re.md.]
    for (const bms::Action &a : se.actions) dispatch_action(w, a);
}

void BmsEventSystem::update_entry(World &w, ScriptedEvent &se) {
    // Faithful port of EventTrigger_UpdateEntry @0x454c30. Timer words are read
    // unsigned and the decremented value is tested as SIGNED 16-bit
    // (@0x454cef/@0x454d40): reload values >= (513 << 6) wrap negative on the
    // first decrement and expire immediately — replicated, not "fixed".
    if (se.activate_countdown != 0) {
        // Activation delay counting down; on expiry the actions run.
        const int16_t nv = static_cast<int16_t>(se.activate_countdown - 64);
        se.activate_countdown = static_cast<uint16_t>(nv);
        if (nv <= 0) {
            se.activate_countdown = 0;
            fire(w, se);
        }
        // Falls through: the repeat cooldown ticks in the same call (@0x454d2d).
    } else if (se.repeat_countdown == 0) {
        if (!se.active) {
            if (evaluate_chain(w, se.triggers)) {
                se.active = true; // latched the moment the chain passes (@0x454c7a)
                const uint16_t delay =
                    static_cast<uint16_t>(static_cast<uint32_t>(se.event.delay) << 6);
                if (delay != 0) se.activate_countdown = delay; // arm [orig: +16 = +18]
                else fire(w, se);                              // no delay: fire now
                if ((static_cast<uint32_t>(se.event.flags) &
                     static_cast<uint32_t>(bms::EventFlags::ResetAfter)) != 0) {
                    const uint16_t cool =
                        static_cast<uint16_t>(static_cast<uint32_t>(se.event.reset_after) << 6);
                    if (cool != 0) {
                        se.repeat_countdown = cool; // arm cooldown; no decrement this call
                        return;
                    }
                    se.active = false; // reset_after == 0: re-evaluable next pass (LABEL_24)
                    return;
                }
            }
        }
        return; // fire-once stays latched forever (only ResetEvent clears it)
    }
    if (se.repeat_countdown != 0) {
        const int16_t nv = static_cast<int16_t>(se.repeat_countdown - 64);
        se.repeat_countdown = static_cast<uint16_t>(nv);
        if (nv <= 0) {
            se.repeat_countdown = 0;
            se.active = false; // cooldown over -> the chain is evaluated again
        }
    }
}

void BmsEventSystem::tick(World &w, const opennova::world::TickContext &ctx) {
    if (!ctx.is_authority) return; // BMS events run only on the authoritative host

    const uint32_t pre_bit = static_cast<uint32_t>(bms::EventFlags::PreMission);
    const uint32_t post_bit = static_cast<uint32_t>(bms::EventFlags::PostMission);

    if (ctx.pre_mission) {
        // Pre-mission pass: every PreMission-flag entry, whole list per call.
        // [orig: EventTrigger_UpdateAllWithFlag2 @0x454dc0, mission-start context]
        for (ScriptedEvent &se : events_)
            if ((static_cast<uint32_t>(se.event.flags) & pre_bit) != 0) update_entry(w, se);
        return;
    }
    if (post_mission_) {
        // Post-mission pass: every PostMission-flag entry, whole list per call.
        // [orig: EventTrigger_UpdateAllWithFlag4 @0x454e00, debrief/video contexts]
        for (ScriptedEvent &se : events_)
            if ((static_cast<uint32_t>(se.event.flags) & post_bit) != 0) update_entry(w, se);
        return;
    }
    // Normal events (neither flag): every 16th tick process ONE QUARTER of the
    // list, round-robin — each entry is evaluated once per 64 ticks, which is why
    // the timers decrement in 64-unit quanta. [orig: Server_TickUpdate @0x51d7e0
    // (++dword_C8D808 > 15) -> quarter pass @0x454d50 (cursor & 3, (flags & 6) == 0)]
    if (++normal_gate_ <= 15) return;
    normal_gate_ = 0;
    const size_t quarter = (events_.size() + 3) / 4;
    const size_t start = quarter * static_cast<size_t>(quarter_cursor_);
    const size_t end = (start + quarter < events_.size()) ? start + quarter : events_.size();
    for (size_t i = start; i < end; ++i) {
        ScriptedEvent &se = events_[i];
        if ((static_cast<uint32_t>(se.event.flags) & (pre_bit | post_bit)) == 0)
            update_entry(w, se);
    }
    quarter_cursor_ = (quarter_cursor_ + 1) & 3;
}

} // namespace opennova::mission
