#include "mission/event_runtime.h"

#include "world/world.h"

namespace opennova::mission {

using world::World;

namespace {
// Session-scoped load-parity flag, process-global like the original's BSS word.
// Static image value 1; the ONLY writer is the XOR at the end of each BMS load.
// [orig: dword_815174; EventTrigger_LoadAllData @0x454029] (D-EVT-3 cat 5)
int g_second_time_through = 1;
} // namespace

bool BmsEventSystem::second_time_through() {
    // Raw read, no normalization [orig: EventTrigger_EvaluateCondition @0x453b24].
    return g_second_time_through != 0;
}

void BmsEventSystem::load(const std::vector<bms::Event> &events,
                          const std::vector<bms::Trigger> &triggers,
                          const std::vector<bms::Action> &actions) {
    // One parity flip per BMS load: first load of a session -> reads 0,
    // restart -> 1, alternating. [orig: dword_815174 ^= 1 at the end of
    // EventTrigger_LoadAllData @0x454029, sole caller Mission_LoadBMSFile @0x40fc85]
    g_second_time_through ^= 1;
    zone_refs_resolved_ = false; // fresh file params carry zone IDS again
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

void BmsEventSystem::resolve_zone_refs(World &w) {
    auto area_degenerate = [&](int idx) {
        const opennova::world::Area *a = w.registry.area(idx);
        return a == nullptr || a->bounds.min.x == a->bounds.max.x ||
               a->bounds.min.y == a->bounds.max.y;
    };
    for (ScriptedEvent &se : events_) {
        for (bms::Trigger &t : se.triggers) {
            int32_t *zone_param = nullptr;
            if ((t.main_type == bms::TriggerMainType::Group ||
                 t.main_type == bms::TriggerMainType::Single) &&
                t.sub_type == 10) {
                zone_param = &t.param2; // Group/SingleIsWithinArea [orig: @0x453048]
            } else if (t.main_type == bms::TriggerMainType::Player && t.sub_type == 37) {
                zone_param = &t.param1; // PlayerSatchel zone [orig: @0x453058]
            }
            if (zone_param == nullptr) continue;
            const int idx = w.registry.area_index_by_zone_id(*zone_param);
            if (idx < 0 || area_degenerate(idx)) {
                // Neuter exactly like the original: main+sub zeroed, flags kept
                // [orig: @0x45309e/@0x4530b9 — a neutered trigger falls to the
                // evaluator default (false)].
                t.main_type = static_cast<bms::TriggerMainType>(0);
                t.sub_type = 0;
            } else {
                *zone_param = idx; // [orig: @0x453095]
            }
        }
        for (bms::Action &a : se.actions) {
            if (a.action_type != bms::ActionType::AreaAiRed &&
                a.action_type != bms::ActionType::AreaAiBlue) {
                continue;
            }
            const int idx = w.registry.area_index_by_zone_id(a.param1);
            if (idx < 0 || area_degenerate(idx)) {
                a.action_type = static_cast<bms::ActionType>(0); // [orig: @0x4531b1/@0x4531c6]
            } else {
                a.param1 = idx; // [orig: @0x453183 + the inline box copy @0x45318d..]
            }
        }
    }
}

void BmsEventSystem::on_load(World &w) {
    // File zone IDs -> zone-array indices, once per load() [orig: the mission-start
    // resolvers @0x453000/@0x453100 run right after EventTrigger_LoadAllData].
    if (!zone_refs_resolved_) {
        resolve_zone_refs(w);
        zone_refs_resolved_ = true;
    }
    // The sticky relation/visited/group state zeroes once per mission load
    // [orig: EventSystem_FreeAll @ 0x453210].
    w.relations.clear();
    for (ScriptedEvent &se : events_) {
        se.active = false;
        se.activate_countdown = 0;
        se.repeat_countdown = 0;
    }
    normal_gate_ = 0;
    quarter_cursor_ = 0;
    // The retail AWOL counter is a persistent global; zeroing here is a
    // load-time convenience with the same observable behavior (the counter is
    // re-derived within one quarter cycle either way, and it only grows while
    // the player is out of bounds). [orig: dword_A89160]
    awol_64tick_count_ = 0;
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
            // Matrix rows are keyed by the RAW authored SSN — no FindByNetId
            // resolution for the matrix sub-types [orig: the cat-2 mirror of
            // the @ 0x45364a sub-switch over the S-family matrices, record §3a].
            auto &rel = w.relations;
            using R = opennova::world::TriggerRelations;
            switch (static_cast<bms::SingleTriggerType>(t.sub_type)) {
                case bms::SingleTriggerType::SingleSeesGroup:
                    return rel.single_group(R::kSees, t.param1, t.param2);
                case bms::SingleTriggerType::SingleHasTargetedGroup:
                    return rel.single_group(R::kTargeted, t.param1, t.param2);
                case bms::SingleTriggerType::SingleHasShotGroup:
                    return rel.single_group(R::kShot, t.param1, t.param2);
                case bms::SingleTriggerType::SingleSeesSingle:
                    return rel.single_single(R::kSees, t.param1, t.param2);
                case bms::SingleTriggerType::SingleHasTargetedSingle:
                    return rel.single_single(R::kTargeted, t.param1, t.param2);
                case bms::SingleTriggerType::SingleHasShotSingle:
                    return rel.single_single(R::kShot, t.param1, t.param2);
                case bms::SingleTriggerType::SingleAtWaypoint:
                    // Visited matrix A: row = the single's SSN, cell = list p2,
                    // bit = number p3 [orig: 0xAC86F8; record §3a sub 7].
                    return rel.single_visited(t.param1, t.param2, t.param3);
                case bms::SingleTriggerType::SingleDestroyed:
                    return cmds.ssn_dead(static_cast<uint16_t>(t.param1));
                case bms::SingleTriggerType::SingleAlive:
                    return cmds.ssn_alive(static_cast<uint16_t>(t.param1));
                case bms::SingleTriggerType::SingleIsWithinArea:
                    return cmds.ssn_in_area(static_cast<uint16_t>(t.param1), t.param2);
                default:
                    // Cat-2 alert/count subs (3/6/9/11/12/14) and the distance/
                    // LOS family (42-45) stay unwitnessed for singles — false
                    // until a grill pins them (record §3a residuals).
                    return false;
            }
        }
        case bms::TriggerMainType::Group: {
            // The cat-1 sub-switch over the relation matrices + the 48-byte
            // group records [orig: @ 0x45364a; expressions record §3a / §7.4].
            auto &rel = w.relations;
            using R = opennova::world::TriggerRelations;
            const R::GroupState *grp = rel.group_or_null(t.param1);
            switch (static_cast<bms::GroupTriggerType>(t.sub_type)) {
                case bms::GroupTriggerType::GroupSeesGroup:
                    return rel.group_group(R::kSees, t.param1, t.param2);
                case bms::GroupTriggerType::GroupHasTargetedGroup:
                    return rel.group_group(R::kTargeted, t.param1, t.param2);
                case bms::GroupTriggerType::GroupHasShotGroup:
                    return rel.group_group(R::kShot, t.param1, t.param2);
                case bms::GroupTriggerType::GroupSeesSingle:
                    return rel.group_single(R::kSees, t.param1, t.param2);
                case bms::GroupTriggerType::GroupHasTargetedSingle:
                    return rel.group_single(R::kTargeted, t.param1, t.param2);
                case bms::GroupTriggerType::GroupHasShotSingle:
                    return rel.group_single(R::kShot, t.param1, t.param2);
                case bms::GroupTriggerType::GroupAtRedAlert:
                    return grp != nullptr && grp->alert == R::kAlertRed;
                case bms::GroupTriggerType::GroupAtYellowAlert:
                    return grp != nullptr && grp->alert == R::kAlertYellow;
                case bms::GroupTriggerType::GroupDestroyed:
                    // The count-based read, NOT a live member scan: the live
                    // count refreshes on the 62-tick cadence, so a kill reads
                    // stale for up to one cadence — retail behavior [orig:
                    // record §3a sub 4, group[p1].live <= 0].
                    return grp != nullptr && grp->live_count <= 0;
                case bms::GroupTriggerType::GroupAlive:
                    return grp != nullptr && grp->live_count > 0; // [orig: sub 5]
                case bms::GroupTriggerType::GroupHasLostMoreUnits:
                    return grp != nullptr && grp->initial_count - grp->live_count >= t.param2;
                case bms::GroupTriggerType::GroupIntact:
                    return grp != nullptr && grp->initial_count == grp->live_count;
                case bms::GroupTriggerType::GroupHasMoreUnits:
                    return grp != nullptr && grp->live_count >= t.param2;
                case bms::GroupTriggerType::GroupAtWaypoint:
                    // Visited matrix B: row = group, cell = list p2, bit = p3
                    // [orig: 0xAD86F8; record §3a sub 7].
                    return rel.group_visited(t.param1, t.param2, t.param3);
                case bms::GroupTriggerType::GroupIsWithinArea: {
                    // Any live member inside zone p2 [orig: @ 0x453763 ->
                    // Entity_IsTeamInTriggerBounds @ 0x43c730].
                    const opennova::world::Area *a = w.registry.area(t.param2);
                    if (a == nullptr) return false;
                    std::vector<opennova::world::EntityHandle> members;
                    w.registry.by_group(static_cast<uint8_t>(t.param1), members);
                    for (opennova::world::EntityHandle h : members) {
                        const opennova::world::Entity *e = w.registry.get(h);
                        if (e != nullptr && e->alive && a->bounds.contains(e->position))
                            return true;
                    }
                    return false;
                }
                case bms::GroupTriggerType::GroupHoldingGroup:
                    // Needs the held-object link (entity +616) our model does
                    // not carry yet [orig: sub_43C870 @ 0x43c870] — record §3a
                    // residual; false until that link lands.
                    return false;
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
        case bms::TriggerMainType::SecondTimeThrough:
            // Raw read of the session load-parity toggle. [orig: @0x453b24 ->
            // dword_815174] (D-EVT-3 cat 5)
            return second_time_through();
        case bms::TriggerMainType::Teammate: {
            switch (static_cast<bms::TeammateTriggerType>(t.sub_type)) {
                case bms::TeammateTriggerType::TeammateIsEnabled:
                    // In an MP session teammates are never "enabled" for this
                    // trigger; otherwise it is the game option's inverse.
                    // [orig: @0x453b53 is_in_session -> false; @0x453b67
                    //  !(dword_24D1E34 & 0x20)]
                    if (w.mp_session) return false;
                    return !w.teammates_disabled;
                case bms::TeammateTriggerType::TeammateMedicAssisting:
                case bms::TeammateTriggerType::TeammateEvacuating:
                    // Both subs read the same "any teammate heli-lift op in
                    // flight" state — indistinguishable in retail JO.
                    // [orig: @0x453b42 -> getter @0x451720 -> dword_AC4F40]
                    return w.heli_lift_active_count != 0;
            }
            return false; // [orig: sub-type range check @0x453b3c]
        }
        case bms::TriggerMainType::Player: {
            switch (static_cast<bms::PlayerTriggerType>(t.sub_type)) {
                case bms::PlayerTriggerType::PlayerAwol:
                    // AWOL quanta (once per 64 ticks, ~1.02 s each) vs the authored
                    // threshold. [orig: @0x453d40 — getter @0x439de0 >= param1] (D-EVT-2)
                    return awol_64tick_count_ >= t.param1;
                // The four mount subs resolve param1 as an SSN and test the LOCAL player's
                // mount/stand state, one carrier link deep. [orig: EventTrigger_
                // EvaluateCondition cat-7 subs 38-41 -> @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0]
                case bms::PlayerTriggerType::PlayerAttachedToSsn: // 38 PLYRATTACHED
                    return cmds.local_player_attached_to_ssn(static_cast<uint16_t>(t.param1));
                case bms::PlayerTriggerType::PlayerOnSsn:         // 39 PLYRONSSN
                    return cmds.local_player_standing_on_ssn(static_cast<uint16_t>(t.param1));
                case bms::PlayerTriggerType::PlayerDrivingSsn:    // 40 PLYRDRIVING
                    return cmds.local_player_driving_ssn(static_cast<uint16_t>(t.param1));
                case bms::PlayerTriggerType::PlayerOnGun:         // 41 PLYRONGUN
                    return cmds.local_player_on_gun_of_ssn(static_cast<uint16_t>(t.param1));
                default:
                    break;
            }
            // The remaining Player subs (view modes, dialog, satchel) ride their embedder
            // subsystems' ports; false until witnessed-wired.
            return false;
        }
        default:
            // Out-of-range main types read false, matching the dispatcher's
            // bounds behavior. [orig: EventTrigger_EvaluateCondition @0x453620]
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
        case bms::ActionType::RedirectSingleTo:
            cmds.set_ssn_waypoint(static_cast<uint16_t>(a.param1), a.param2, a.param3);
            break;
        case bms::ActionType::RedirectGroupTo:
            cmds.group_to_waypoint(a.param1, a.param2, a.param3);
            break;
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
        // Presentation effects: the engine hands these to the embedder's audio/HUD/overlay.
        case bms::ActionType::PlayWavList: // play dialog/wav param1 (param2 = always-play flag)
            w.effects.push({"dialog", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWaypoints:
            // The engine flag the waypoint HUD label + SP cycle key gate on
            // (init 1 at HUD bring-up); the effect stays as the presentation log.
            // [orig: action 40 -> Game_SetShowWaypoints @0x58fb50 ->
            //  g_showWaypoints @0x27238BC, init @0x5a4913]
            w.waypoints.show = (a.param1 != 0);
            w.effects.push({"show_waypoints", a.param1, 0, 0, 0, std::string()});
            break;
        case bms::ActionType::SetLightState:
            w.effects.push({"set_light", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWinSubgoal:
            // Set/clear the show-win bit — the RAW slot shift is the original's
            // (slot 1..8 -> bits 1..8). The "New Objective" toast rides the
            // effect. [orig: case 35 @0x4546af — bit @0x4546bf/@0x4546d0;
            //  HUD_ShowObjectiveNotification @0x4546e2]
            if (a.param2 != 0) w.subgoals.show_win |= (1u << a.param1);
            else w.subgoals.show_win &= ~(1u << a.param1);
            w.effects.push({"subgoal_show", a.param1, a.param2, /*lose=*/0, 0, std::string()});
            break;
        case bms::ActionType::ShowLoseSubgoal:
            // [orig: case 36 @0x454724 — the show-lose mirror @0x454734/@0x454745]
            if (a.param2 != 0) w.subgoals.show_lose |= (1u << a.param1);
            else w.subgoals.show_lose &= ~(1u << a.param1);
            w.effects.push({"subgoal_show", a.param1, a.param2, /*lose=*/1, 0, std::string()});
            break;
        // The three win actions end the round in-engine [orig: EventAction_Dispatch
        // @0x45447b/0x454495/0x4544af -> Server_ProcessRoundEnd(1/2/0); the call
        // sites gate on g_spawn_success_gate — process_round_end's own latch covers
        // that]. The "win" effect stays as the presentation signal.
        case bms::ActionType::BlueWin:
            w.effects.push({"win", 1, 0, 0, 0, std::string()});
            w.process_round_end(1);
            break;
        case bms::ActionType::RedWin:
            w.effects.push({"win", 2, 0, 0, 0, std::string()});
            w.process_round_end(2);
            break;
        case bms::ActionType::GreenWin:
            w.effects.push({"win", 0, 0, 0, 0, std::string()});
            w.process_round_end(0);
            break;
        case bms::ActionType::SubGoalWon: {
            // The already-won guard skips the WHOLE case — no re-announce, no
            // effect. Then the mask set + the STRWINMSG chat line, announce
            // gated on the round still running. Effect fields: a = slot,
            // b = the header WinConditions text id, c = announce. Deferred with
            // cites: the win_scores[slot]*100 score add [orig: @0x454526 —
            // byte_A763FB = header win_scores; the "Score_AccumulateBandwidth"
            // callee name is a kong misnomer] and the header unknown5[2]-masked
            // team-banner leg [orig: @0x45458d byte_A762D6].
            // [orig: case 14 @0x454500 — guard @0x45450a, set @0x45451d,
            //  round-running gate @0x45453a, STRWINMSG resolve @0x45456f]
            const uint32_t bit = 1u << a.param1;
            if ((w.subgoals.won & bit) != 0) break;
            w.subgoals.won |= bit;
            const int32_t text_id = (a.param1 >= 1 && a.param1 <= 8)
                    ? w.subgoals.win_text_ids[a.param1] : 0;
            const int32_t announce = w.round_end.ended ? 0 : 1;
            w.effects.push({"subgoal_won", a.param1, text_id, announce, 0, std::string()});
            break;
        }
        case bms::ActionType::SubGoalLost: {
            // No already-set guard (unlike won). The chat line AND the
            // persistent banner ride the same round-running gate; the
            // unknown5[3]-masked team-banner leg is deferred with its win
            // sibling. [orig: case 15 @0x4545e0 — set @0x4545ea, gate
            //  @0x4545f0, STRLOSEMSG chat @0x454632 + SetBannerText @0x454647;
            //  byte_A762D7 leg @0x45465c]
            w.subgoals.lost |= (1u << a.param1);
            const int32_t text_id = (a.param1 >= 1 && a.param1 <= 8)
                    ? w.subgoals.lose_text_ids[a.param1] : 0;
            const int32_t announce = w.round_end.ended ? 0 : 1;
            w.effects.push({"subgoal_lost", a.param1, text_id, announce, 0, std::string()});
            break;
        }
        case bms::ActionType::GroupResetHasVisited:
            // Clears ONE group row of the visited matrix — only lists 0..31,
            // the witnessed 0x80-byte memset quirk
            // [orig: EventTrigger_ClearSlotB @ 0x4535e0].
            w.relations.clear_group_visited_row(a.param1);
            break;
        case bms::ActionType::SingleResetHasVisited:
            // [orig: EventTrigger_ClearSlotA @ 0x453600] — the same 32-list quirk.
            w.relations.clear_single_visited_row(a.param1);
            break;
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
            // WAC program. Recorded as an effect here; the embedder wires the actual
            // WAC invocation (the WacSystem) at runtime.
            w.effects.push({"execute_wac", a.param1, 0, 0, 0, std::string()});
            break;
        default:
            // No faithful in-engine handler yet: record as an UNPORTED marker (coverage /
            // diagnostic only — never a presentation effect). Supported missions should
            // emit zero of these; a test asserts that. [tracked-TODO, not a command stream.]
            w.effects.push({"unported_action", static_cast<int32_t>(a.action_type), a.action_sub_type,
                            a.param1, a.param2, std::string()});
            break;
    }
}

void BmsEventSystem::fire(World &w, ScriptedEvent &se) {
    // [orig: the dispatch loops @0x454ca6/@0x454d0e — every action entry in order.
    //  The g_InputActionBits/g_EventInputBitsMirror commit around the dispatch
    //  (input-trigger bit consumption) is an embedder input subsystem not ported here;
    //  recorded in docs/mission/bms-event-runtime-re.md.]
    for (const bms::Action &a : se.actions) dispatch_action(w, a);
    // The waypoint completion hook: a fired event completes every route marker
    // linked to its index (the original computes the index from the record's
    // position in g_Events; events_ mirrors that array order).
    // [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0, called right after
    //  both dispatch loops @0x454cbd/@0x454d25]
    if (!events_.empty() && &se >= events_.data() && &se < events_.data() + events_.size())
        w.waypoints.on_event_fired(static_cast<int32_t>(&se - events_.data()));
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

void BmsEventSystem::run_post_mission_pass(World &w) {
    // ONE whole-list sweep over the PostMission-flag entries, called by the embedder
    // exactly once per transition — never periodically, so an authored post
    // delay of 1 fires at the transition and larger delays effectively never do
    // (D-EVT-4). [orig: EventTrigger_UpdateAllWithFlag4 @0x454e00; one-shot
    // callers Game_TeardownMission @0x52266c and the SP round-restart @0x5263a0]
    const uint32_t post_bit = static_cast<uint32_t>(bms::EventFlags::PostMission);
    for (ScriptedEvent &se : events_)
        if ((static_cast<uint32_t>(se.event.flags) & post_bit) != 0) update_entry(w, se);
}

void BmsEventSystem::tick(World &w, const opennova::world::TickContext &ctx) {
    if (!ctx.is_authority) return; // BMS events run only on the authoritative host

    const uint32_t pre_bit = static_cast<uint32_t>(bms::EventFlags::PreMission);
    const uint32_t post_bit = static_cast<uint32_t>(bms::EventFlags::PostMission);

    if (ctx.pre_mission) {
        // Pre-mission pass: every PreMission-flag entry, one whole-list sweep.
        // The embedder contract is ONE pre_mission tick per mission start, before
        // the clock runs (NovaSimulation delivers exactly one) — retail's pre
        // pass is a single call, never periodic (D-EVT-4).
        // [orig: EventTrigger_UpdateAllWithFlag2 @0x454dc0; sole caller
        //  Game_StartMission @0x525b86, before current_tick = 0 @0x525b9f]
        for (ScriptedEvent &se : events_)
            if ((static_cast<uint32_t>(se.event.flags) & pre_bit) != 0) update_entry(w, se);
        return;
    }
    // Normal events (neither flag): every 16th tick process ONE QUARTER of the
    // list, round-robin — each entry is evaluated once per 64 ticks, which is why
    // the timers decrement in 64-unit quanta. [orig: Server_TickUpdate @0x51d7e0
    // (++dword_C8D808 > 15) -> quarter pass @0x454d50 (cursor & 3, (flags & 6) == 0)]
    if (++normal_gate_ <= 15) return;
    normal_gate_ = 0;
    if (quarter_cursor_ == 0) {
        // Quarter-cycle piggyback, once per full cycle (64 ticks): the player-
        // AWOL counter the PlayerAwol trigger reads — increments while the local
        // player sits outside every active zone, resets otherwise (D-EVT-2).
        // [orig: quarter pass @0x454d50 cursor==0 -> Entity_UpdateStuckCounter
        //  @0x439dc0 -> Entity_IsLocalPlayerOutOfBounds @0x439d40; dword_A89160]
        if (w.commands.local_player_out_of_bounds()) ++awol_64tick_count_;
        else awol_64tick_count_ = 0;
    }
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
