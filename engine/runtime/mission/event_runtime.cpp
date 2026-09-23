#include <runtime/mission/event_runtime.h>

#include <algorithm>
#include <cstdio>

#include <base/io/strutil.h>
#include <runtime/world/world.h>

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
        // The 24-byte records load as-is, so the reload words are the high
        // halves of the file dwords at +12/+16, which carry the authored
        // value << 22: the value << 6 [orig: event +14 / +18; the raw record
        // read EventTrigger_LoadAllData @0x453f87].
        se.repeat_reload = static_cast<uint16_t>(static_cast<uint32_t>(e.reset_after) << 6);
        se.activate_reload = static_cast<uint16_t>(static_cast<uint32_t>(e.delay) << 6);
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
    // Both walks read the event's count byte signed: 128..255 resolve nothing
    // [orig: the trigger walk @0x453022/@0x4530C8; the action walk
    //  @0x453127/@0x4531D2].
    const auto signed_count = [](uint8_t count, size_t slice) {
        const int n = count < 128 ? count : static_cast<int>(count) - 256;
        return n <= 0 ? size_t{0} : std::min(static_cast<size_t>(n), slice);
    };
    for (ScriptedEvent &se : events_) {
        const size_t trigger_count = signed_count(se.event.trigger_count, se.triggers.size());
        for (size_t ti = 0; ti < trigger_count; ++ti) {
            bms::Trigger &t = se.triggers[ti];
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
        const size_t action_count = signed_count(se.event.action_count, se.actions.size());
        for (size_t ai = 0; ai < action_count; ++ai) {
            bms::Action &a = se.actions[ai];
            if (a.action_type != bms::ActionType::AreaAiRed &&
                a.action_type != bms::ActionType::AreaAiBlue) {
                continue;
            }
            const int idx = w.registry.area_index_by_zone_id(a.param1);
            if (idx < 0 || area_degenerate(idx)) {
                a.action_type = static_cast<bms::ActionType>(0); // [orig: @0x4531b1/@0x4531c6]
            } else {
                // The index store is overwritten at once by the zone box, in
                // the record's own order: p1 = x_min, p3 = y_min, p4 = x_max,
                // reserved1 = y_max (p2 keeps the authored command argument).
                // [orig: EventTrigger_ResolveZoneActionRefs @0x453100 — the
                //  index @0x453183, x_min @0x45318d, y_min @0x453197, x_max
                //  @0x4531a1, y_max @0x4531ab]
                const world::Aabb &box = w.registry.area(idx)->bounds;
                a.param1 = world::to_fixed(box.min.x);
                a.param3 = world::to_fixed(box.min.y);
                a.param4 = world::to_fixed(box.max.x);
                a.reserved1 = world::to_fixed(box.max.y);
            }
        }
    }
}

void BmsEventSystem::on_load(World &w) {
    w.script.bms_events = this;
    w.teammates.reset(w);
    // File zone IDs -> zone-array indices, once per load() [orig: the mission-start
    // resolvers @0x453000/@0x453100 run right after EventTrigger_LoadAllData].
    if (!zone_refs_resolved_) {
        resolve_zone_refs(w);
        zone_refs_resolved_ = true;
    }
    // The sticky relation/visited/group state zeroes once per mission load:
    // the matrices and visited words in EventSystem_FreeAll, the group
    // records (alert, counts, speed) in the mission reset's memset.
    // [orig: EventSystem_FreeAll @ 0x453210; CAIGroup_HasGuardTaskFromIndex2
    //  @0x40DB80 (the load reset; the 0xA33F90 x 0xC00 memset @0x40DBAE)]
    w.script.relations.clear();
    // Round init clears both dialog tables the PLYRDIALOG subs read [orig:
    // Game_InitNewRound @0x422741/@0x4227ac -> Dialog_ResetAll @0x44dc90].
    // The input-action word and its mirror are BSS words with no load-time
    // writer; they keep whatever the producers left.
    w.script.dialog.reset();
    for (ScriptedEvent &se : events_) {
        se.active = 0;
        se.activate_countdown = 0;
        se.repeat_countdown = 0;
    }
    normal_gate_ = 0;
    quarter_cursor_ = 0;
    // The AWOL counter zeroes at load too: FreeAll's tail jumps into the
    // counter reset. [orig: dword_A89160; EventSystem_FreeAll @0x453210 — the
    //  tail jump @0x453375 to `mov g_PlayerAwolCounter, 0` @0x439DB0]
    awol_64tick_count_ = 0;
}

int32_t BmsEventSystem::evaluate_trigger(World &w, const bms::Trigger &t) {
    auto &cmds = w.commands;
    switch (t.main_type) {
        case bms::TriggerMainType::MissionVariable: {
            // [orig: EventTrigger_EvaluateCondition cat 4 — dword_C6B240[param1] <op> param2.]
            // THE shared variable store (same array WAC V# uses).
            int32_t v = w.script.vars.get_mission(t.param1);
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
            auto &rel = w.script.relations;
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
                    // The negation of the alive read: an SSN no row carries
                    // (never placed, rejected, SSN 0, or removed) reads
                    // DESTROYED. [orig: EventTrigger_EvaluateCondition cat 2
                    // sub 4 @0x453985 -> Entity_IsAliveByBmsRef, neg/sbb/add
                    // @0x453991..0x453995]
                    return !cmds.bms_ref_alive(t.param1);
                case bms::SingleTriggerType::SingleAlive:
                    // [orig: EventTrigger_EvaluateCondition cat 2 sub 5
                    //  @0x45399d -> Entity_IsAliveByBmsRef @0x43e640]
                    return cmds.bms_ref_alive(t.param1);
                case bms::SingleTriggerType::SingleIsWithinArea:
                    return cmds.ssn_in_area(t.param1, t.param2);
                // The 2026-08-13 grill closed the rest of the cat-2 switch
                // (record §3b): every helper's RAW sense is positive — the
                // negated flavor of these enum names is authoring-display
                // convention, and the chain-negation bit does the flipping.
                case bms::SingleTriggerType::SingleAtRedAlert:
                    // [orig: @0x453965 -> Entity_IsSsnAtAlertLevel(p1, 2)]
                    return cmds.ssn_at_alert(static_cast<uint16_t>(t.param1), 2);
                case bms::SingleTriggerType::SingleAtYellowAlert:
                    // [orig: @0x453978 -> Entity_IsSsnAtAlertLevel(p1, 1)]
                    return cmds.ssn_at_alert(static_cast<uint16_t>(t.param1), 1);
                case bms::SingleTriggerType::SingleHasLostMoreUnits:
                    // Health, not unit counts [orig: @0x4539b6 ->
                    // Entity_HasDamageCapacity(p1, p2)].
                    return cmds.ssn_damage_taken_at_least(
                            static_cast<uint16_t>(t.param1), t.param2);
                case bms::SingleTriggerType::SingleIntact:
                    // [orig: @0x4539e0 -> Entity_HasFullHealth(p1)]
                    return cmds.ssn_full_health(static_cast<uint16_t>(t.param1));
                case bms::SingleTriggerType::SingleHasMoreUnits:
                    // [orig: @0x453a18 -> Entity_HasHealthAboveThreshold(p1, p2)]
                    return cmds.ssn_health_at_least(
                            static_cast<uint16_t>(t.param1), t.param2);
                case bms::SingleTriggerType::SingleHoldingGroup:
                    // [orig: @0x453a03 -> Entity_IsSsnHoldingItemGroup(p1, p2)]
                    return cmds.ssn_holding_group(
                            static_cast<uint16_t>(t.param1), t.param2);
                case bms::SingleTriggerType::SingleOnTopOf:
                    // [orig: @0x453835 -> Entity_IsOnTopOfChain over the
                    // FindByNetId handles]
                    return cmds.ssn_on_chain_of(static_cast<uint16_t>(t.param1),
                            static_cast<uint16_t>(t.param2));
                case bms::SingleTriggerType::SingleFartherThan:
                    // RAW = "within" [orig: @0x45387f ->
                    // Entity_CheckProximity(hA, hB, p3<<16)].
                    return cmds.ssn_within_distance(static_cast<uint16_t>(t.param1),
                            static_cast<uint16_t>(t.param2),
                            static_cast<int32_t>(static_cast<uint32_t>(t.param3) << 16));
                case bms::SingleTriggerType::SingleHasNoLOS:
                    // RAW = "in range AND ray clear" [orig: @0x4538c9 ->
                    // Entity_CheckLineOfSightInRange(hA, hB, p3<<16)].
                    return cmds.ssn_los_clear_within(static_cast<uint16_t>(t.param1),
                            static_cast<uint16_t>(t.param2),
                            static_cast<int32_t>(static_cast<uint32_t>(t.param3) << 16));
                case bms::SingleTriggerType::SingleDoesNotSeeOrFarther:
                    // RAW = "sees": range + ray + the ±30° facing cone
                    // [orig: @0x453913 -> Entity_CheckLineOfSight(hA, hB, p3<<16)].
                    return cmds.ssn_sees_within(static_cast<uint16_t>(t.param1),
                            static_cast<uint16_t>(t.param2),
                            static_cast<int32_t>(static_cast<uint32_t>(t.param3) << 16));
                default:
                    // Sub 8 is absent from the retail jump table too (§3b).
                    return false;
            }
        }
        case bms::TriggerMainType::Group: {
            // The cat-1 sub-switch over the relation matrices + the 48-byte
            // group records [orig: @ 0x45364a; expressions record §3a / §7.4].
            auto &rel = w.script.relations;
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
                case bms::GroupTriggerType::GroupIsWithinArea:
                    // [orig: group-area trigger dispatch @0x453763]
                    return cmds.group_in_area(t.param1, t.param2);
                case bms::GroupTriggerType::GroupHoldingGroup:
                    // Any pool-0 member of group p1 carrying an object of
                    // group p2 via the mounted_child link. Match owns the
                    // live flag writers; generic carryables retain their
                    // vehicle-attachment owner (§3b item 4).
                    // [orig: @0x453778 -> TriggerGroup_AnyMemberHoldingItemGroup]
                    return cmds.group_holding_group(t.param1, t.param2);
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
            // Raw read of the session load-parity word, no normalization.
            // [orig: EventTrigger_EvaluateCondition @0x453b24 -> dword_815174]
            // (D-EVT-3 cat 5)
            return g_second_time_through;
        case bms::TriggerMainType::Teammate: {
            switch (static_cast<bms::TeammateTriggerType>(t.sub_type)) {
                case bms::TeammateTriggerType::TeammateIsEnabled:
                    // In an MP session teammates are never "enabled" for this
                    // trigger; otherwise it is the game option's inverse.
                    // [orig: @0x453b53 is_in_session -> false; @0x453b67
                    //  !(dword_24D1E34 & 0x20)]
                    if (w.rules.mp_session) return false;
                    return !w.rules.teammates_disabled;
                case bms::TeammateTriggerType::TeammateMedicAssisting:
                case bms::TeammateTriggerType::TeammateEvacuating:
                    // Both subs read the same "any teammate heli-lift op in
                    // flight" state — indistinguishable in retail JO.
                    // [orig: @0x453b42 -> getter @0x451720 -> dword_AC4F40]
                    return w.script.heli_lift_active_count != 0;
            }
            return false; // [orig: sub-type range check @0x453b3c]
        }
        case bms::TriggerMainType::Player: {
            // [orig: EventTrigger_EvaluateCondition cat 7 @0x453b7e]
            // The shared leg every input sub takes: test the LIVE word, toggle
            // the matched bit in the chain's mirror, true. The mirror is
            // committed back to the live word only when the event fires
            // (fire()), so a matched bit is consumed exactly then and stays
            // set otherwise. [orig: @0x453ba5 test -> 0; @0x453bab
            //  g_EventInputBitsMirror ^= mask; @0x453bb1 result 1]
            auto input_bit = [&](uint32_t mask) {
                if ((mask & w.script.input_action_bits) == 0) return false;
                w.script.input_action_mirror ^= mask;
                return true;
            };
            switch (static_cast<bms::PlayerTriggerType>(t.sub_type)) {
                case bms::PlayerTriggerType::PlayerBerserk: {
                    // The local player's AiSlot behavior word masked to 0x200
                    // (Berserk), returned RAW: the chain folds 0x200, so
                    // NOT-Berserk (xor 1) reads 0x201. No brain reads 0
                    // (retail dereferences the runtime pointer unchecked).
                    // [orig: EventTrigger_EvaluateCondition @0x453b85..0x453b99
                    //  g_local_player_entity->aiRuntime[1] & 0x200]
                    const world::AiEntity *ai = w.ai.for_handle(w.cached.local_player);
                    return ai != nullptr ? (ai->slot.f[world::AiSlot::kBehaviorFlags] & 0x200) : 0;
                }
                // The fixed-mask input subs; the 22-25/28-30 masks have no
                // setter in the image, so they read false in retail as well.
                case bms::PlayerTriggerType::PlayerFirstPerson:  return input_bit(0x4000000u);  // @0x453b9a
                case bms::PlayerTriggerType::PlayerThirdPerson:  return input_bit(0x8000000u);  // @0x453c73
                case bms::PlayerTriggerType::PlayerCockpitView:  return input_bit(0x10000000u); // @0x453c69
                case bms::PlayerTriggerType::PlayerInputBit10:   return input_bit(0x400u);      // @0x453c7d
                case bms::PlayerTriggerType::PlayerInputBit11:   return input_bit(0x800u);      // @0x453c87
                case bms::PlayerTriggerType::PlayerInputBit12:   return input_bit(0x1000u);     // @0x453c91
                case bms::PlayerTriggerType::PlayerInputBit13:   return input_bit(0x2000u);     // @0x453c9b
                case bms::PlayerTriggerType::PlayerInputBit29:   return input_bit(0x20000000u); // @0x453cc5
                case bms::PlayerTriggerType::PlayerInputBit14:   return input_bit(0x4000u);     // @0x453ccf
                case bms::PlayerTriggerType::PlayerInputBit15:   return input_bit(0x8000u);     // @0x453cd9
                case bms::PlayerTriggerType::PlayerInputBitIndex:
                    // `1 << p1` (the x86 shift count is masked to 5 bits) [orig: @0x453ceb]
                    return input_bit(1u << (static_cast<uint32_t>(t.param1) & 31u));
                case bms::PlayerTriggerType::PlayerInputBitIndexPlus15:
                    // `1 << (byte @ trigger+12 + 15)`: the low byte of param1 [orig: @0x453cfd]
                    return input_bit(1u << ((static_cast<uint32_t>(static_cast<uint8_t>(t.param1)) + 15u) & 31u));
                case bms::PlayerTriggerType::PlayerLookByteBit0Clear:
                    // `(byte_27234FC & 1) == 0`: the byte is hudInfo
                    // (dword_2723388) +0x174, whose only store in the image
                    // writes 0, beside the struct memsets and the per-frame
                    // save/restore copy, so bit 0 never sets: sub 26 is always
                    // true and sub 27 always false. The byte's other readers
                    // test bit 0x20 (HUD_DrawLookModeLabel @0x594100) and bits
                    // 0/0x40 (the lock reticle @0x594580). [orig:
                    //  EventTrigger_EvaluateCondition @0x453ca5 (sub 26) /
                    //  @0x453cb6 (sub 27); the store HUD_BuildEntityInfo
                    //  @0x4b84d9; memsets HUD_RenderAllOverlays @0x5a80b1 and
                    //  HUD_InitOverlaySystem @0x5a493c; the copy
                    //  HUD_RenderOverlays @0x5a7c25]
                    return 1;
                case bms::PlayerTriggerType::PlayerLookByteBit0Set:
                    return 0;
                case bms::PlayerTriggerType::PlayerDialogDone:
                    // [orig: @0x453d1b Dialog_ExistsByIndex(p1) == 0 -- absent
                    //  from the 16-slot active table]
                    return !w.script.dialog.active_exists(t.param1);
                case bms::PlayerTriggerType::PlayerDialogFinished:
                    // [orig: @0x453d20 sub_44E220(p1): in the registered-history
                    //  list (@0x44e253..0x44e28e, miss -> 0 @0x44e291) AND absent
                    //  from the active table (found -> 0 @0x44e313, else 1
                    //  @0x44e2f5)]
                    return w.script.dialog.registered(t.param1) &&
                           !w.script.dialog.active_exists(t.param1);
                case bms::PlayerTriggerType::PlayerAwol:
                    // AWOL quanta (once per 64 ticks, ~1.02 s each) vs the authored
                    // threshold. [orig: @0x453d40 — getter @0x439de0 >= param1] (D-EVT-2)
                    return awol_64tick_count_ >= t.param1;
                case bms::PlayerTriggerType::PlayerSatchel: {
                    // Any populated pool-1 row whose ammo def is the satchel and
                    // whose position lies inside area record param1 (the zone
                    // INDEX after resolve_zone_refs): the x/y box always, the z
                    // range only when the record's flag byte has bit 2, else
                    // -0x40000000..0x40000000 (16.16) -- exactly the unbounded z
                    // the registered area carries. Inclusive compares.
                    // [orig: EventTrigger_AnySatchelInArea @0x547160: record
                    //  @0x54716d, z gate @0x547196, populated-row gate @0x5471cc,
                    //  == g_ammo_satchel @0x5471ea, x/y/z @0x5471f9/@0x547208/@0x547215]
                    const world::Area *area = w.registry.area(t.param1);
                    if (area == nullptr) return false;
                    for (const world::PlacedDevice &d : w.throwables.devices) {
                        if (!d.active) continue;
                        const world::Entity *e = w.registry.get(d.entity);
                        if (e == nullptr || e->registry_spawn_id != d.entity_spawn_id) continue;
                        const world::AmmoTableEntry *ammo = w.tables.ammo.by_index(d.ammo_index);
                        if (ammo == nullptr || !strutil::iequals(ammo->name, "satchel")) continue;
                        if (area->bounds.contains(e->position)) return true;
                    }
                    return false;
                }
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
            // Subs outside 18..41 (and the unassigned 31) fall to the
            // dispatcher default [orig: @0x453d41].
            return false;
        }
        default:
            // Out-of-range main types read false, matching the dispatcher's
            // bounds behavior. [orig: EventTrigger_EvaluateCondition @0x453620]
            return false;
    }
}

int32_t BmsEventSystem::evaluate_chain(World &w, const ScriptedEvent &se) {
    // [orig: EventTrigger_EvaluateChain @0x454050.] Each trigger negated by its own
    // bit0 (`xor 1`); the join operator (or/xor/else and, bitwise over the raw
    // ints) comes from the PREVIOUS trigger. Every predicate runs (no
    // short-circuit), so every matched input bit of the chain toggles in the
    // mirror even when an earlier term already decided the result. The mirror
    // is seeded from the live word at entry, before the count test
    // [orig: @0x45405a]. A zero count byte passes (@0x45405f); otherwise
    // trigger 0 always runs and the count is read SIGNED for the rest, so
    // 128..255 evaluate trigger 0 alone (@0x45408c `jle`, @0x4540c7 movsx).
    // The loop stays within the loaded slice.
    w.script.input_action_mirror = w.script.input_action_bits;
    if (se.event.trigger_count == 0 || se.triggers.empty()) return 1;
    const std::vector<bms::Trigger> &triggers = se.triggers;
    int32_t acc = evaluate_trigger(w, triggers[0]);
    if (triggers[0].is_negated()) acc ^= 1; // [orig: @0x454084]
    const int count = se.event.trigger_count < 128
            ? se.event.trigger_count : static_cast<int>(se.event.trigger_count) - 256;
    for (int i = 1; i < count && static_cast<size_t>(i) < triggers.size(); ++i) {
        int32_t v = evaluate_trigger(w, triggers[static_cast<size_t>(i)]);
        if (triggers[static_cast<size_t>(i)].is_negated()) v ^= 1; // [orig: @0x4540ac]
        const bms::Trigger &prev = triggers[static_cast<size_t>(i) - 1];
        if (prev.is_or()) acc |= v;       // [orig: @0x4540b8]
        else if (prev.is_xor()) acc ^= v; // [orig: @0x4540c1]
        else acc &= v;                    // [orig: @0x4540c5]
    }
    return acc;
}

void BmsEventSystem::dispatch_action(World &w, const bms::Action &a, int32_t event, int32_t action) {
    // [orig: EventAction_Dispatch @0x4542e0 switch(action_type).]
    auto &cmds = w.commands;
    switch (a.action_type) {
        case bms::ActionType::Null:
            break;
        case bms::ActionType::VaporizeGroup:
            // [orig: EventAction_Dispatch case 4 @0x4542E0 ->
            // Entity_TeleportAllByNetId @0x43D5D0] Misnamed retail callee:
            // this removes the group.
            cmds.remove_group(a.param1);
            break;
        case bms::ActionType::GroupOpenDoorAction:
        case bms::ActionType::GroupCloseDoorAction:
            w.doors.command_group(w, a.param1,
                    a.action_type == bms::ActionType::GroupOpenDoorAction, true);
            break;
        case bms::ActionType::GroupVelocity:
            // [orig: Entity_SetMoveSpeedKPH @0x43A960]
            cmds.set_group_move_speed_kph(a.param1, a.param2);
            break;
        case bms::ActionType::ChangeGTeamAction:
            // [orig: Entity_SetTeamByNetId @0x43C680]
            cmds.set_group_team(a.param1, a.param2);
            break;
        case bms::ActionType::ChangeGroupAction:
            // [orig: Entity_UpdateNetIdReferences @0x43C5B0]
            cmds.change_group(a.param1, a.param2);
            break;
        case bms::ActionType::GroupTeleportAction:
            // [orig: Entity_TeleportTeamToSpawn @0x43D390]
            cmds.teleport_group_to_marker(a.param1, a.param2);
            break;
        case bms::ActionType::SingleVelocity:
            // [orig: EventAction_Dispatch case 23 @0x4542E0 -> sub_43DEA0]
            // Retail scans pool 1 and performs no mutation.
            break;
        case bms::ActionType::ChangeSteamAction:
            // [orig: Entity_FindByDCBAndSetFlag @0x43DB30]
            cmds.set_ssn_team(static_cast<uint16_t>(a.param1), a.param2);
            break;
        case bms::ActionType::SingleChangeGroup:
            // [orig: Entity_SetNetIdByParentRef @0x43D6C0]
            cmds.set_ssn_group(static_cast<uint16_t>(a.param1), a.param2);
            break;
        case bms::ActionType::SingleTeleportAction:
            // [orig: EventAction_TeleportEntityToSpawn @0x43DFC0]
            cmds.teleport_ssn_to_marker(static_cast<uint16_t>(a.param1),
                                        a.param2);
            break;
        case bms::ActionType::MisvarChange: {
            // [orig: case 5 — writes dword_C6B240[param1]; the shared var store.]
            // The arithmetic is the dword add/sub, so it wraps at the 32-bit
            // edge [orig: EventAction_Dispatch case 5 @0x45437e, subs 1..5
            //  @0x454394..0x454406].
            const uint32_t cur = static_cast<uint32_t>(w.script.vars.get_mission(a.param1));
            const uint32_t arg = static_cast<uint32_t>(a.param2);
            switch (static_cast<bms::MissionVariableActionSubType>(a.action_sub_type)) {
                case bms::MissionVariableActionSubType::Set: w.script.vars.set_mission(a.param1, a.param2); break;
                case bms::MissionVariableActionSubType::Add: w.script.vars.set_mission(a.param1, static_cast<int32_t>(cur + arg)); break;
                case bms::MissionVariableActionSubType::Subtract: w.script.vars.set_mission(a.param1, static_cast<int32_t>(cur - arg)); break;
                case bms::MissionVariableActionSubType::Increment: w.script.vars.set_mission(a.param1, static_cast<int32_t>(cur + 1u)); break;
                case bms::MissionVariableActionSubType::Decrement: w.script.vars.set_mission(a.param1, static_cast<int32_t>(cur - 1u)); break;
                default: break;
            }
            break;
        }
        case bms::ActionType::KillSingle: cmds.kill_ssn(static_cast<uint16_t>(a.param1)); break;
        case bms::ActionType::VaporizeSingle:
            // [orig: EventAction_Dispatch case 22 @0x454828 ->
            //  find_entity_by_parent_and_dispatch @0x43E210]
            cmds.remove_bms_ref(a.param1);
            break;
        case bms::ActionType::KillGroup: cmds.kill_group(a.param1); break;
        case bms::ActionType::RedirectSingleTo:
            // [orig: EventAction_Dispatch case 19 @0x4547CF..0x4547DB ->
            //  Entity_SetWaypointForTeam @0x43DD00]
            cmds.redirect_ssn_to_waypoint(a.param1, a.param2, a.param3);
            break;
        case bms::ActionType::RedirectGroupTo:
            cmds.group_to_waypoint(a.param1, a.param2, a.param3);
            break;
        // AI-change family: param1 = target, action_sub_type selects the command, param2/3/4 are
        // its slots. Mutates the AI component in-engine (no effect emitted). [orig: cases 3/0x15
        // -> Entity_HandleAlertCommand / Entity_HandleAlertStateEvent @0x43dee0 -> Entity_ApplyCommand
        // @0x43ab60; AREA_AI_RED/BLUE apply to a zone's red/blue units. team: blue=1, red=2.]
        case bms::ActionType::ChangeSingleAI:
            // [orig: EventAction_Dispatch case 21 @0x45480f]
            cmds.apply_bms_single_ai_command(a.param1, a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::ParticleEffectAction:
            // [orig: case 0x1B @0x4542e0 -> EventAction_SpawnParticleEffect (ex sub_4540E0)] param1 selects the authored
            // 6088 markers by WP_NUMBER (not team -- see the command's comment).
            cmds.spawn_marker_particle_effects(a.param1);
            break;
        case bms::ActionType::ChangeGroupAI:
            cmds.apply_group_ai_command(a.param1, a.action_sub_type, a.param2, a.param3, a.param4);
            break;
        case bms::ActionType::AreaAiRed:
        case bms::ActionType::AreaAiBlue:
            // The record's words as the zone resolver left them: p1/p3 and
            // p4/reserved1 hold the zone corners. Red = team 2, blue = 1.
            // [orig: EventAction_Dispatch cases 12,13 @0x4544e7 ->
            //  Entity_KillTeamInBounds @0x43D030 — the team pick @0x43d03b..0x43d053]
            cmds.apply_area_ai_command(a.action_type == bms::ActionType::AreaAiRed ? 2 : 1,
                    a.action_sub_type, a.param1, a.param2, a.param3, a.param4, a.reserved1);
            break;
        case bms::ActionType::OutputText:
            w.out.effects.push({"text", a.param1, 0, 0, 0, std::string()});
            break;
        // Presentation effects: the engine hands these to the embedder's audio/HUD/overlay.
        case bms::ActionType::PlayWavList:
            // Dialog param1 plays only on a client, and once the round-over
            // latch holds only when param2 == 1 forces it; a skipped play never
            // reaches the dialog registry the PLYRDIALOG subs read.
            // [orig: EventAction_Dispatch case 7 — is_mp_session_peer @0x45443d,
            //  param2 == 1 @0x45444a, g_spawn_success_gate @0x454450,
            //  Dialog_PlayByIndex @0x454461]
            if (w.rules.mp_session_peer && (a.param2 == 1 || !w.match.outcome().ended))
                w.out.effects.push({"dialog", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWaypoints:
            // The engine flag the waypoint HUD label + SP cycle key gate on
            // (init 1 at HUD bring-up); the effect stays as the presentation log.
            // [orig: action 40 -> Game_SetShowWaypoints @0x58fb50 ->
            //  g_showWaypoints @0x27238BC, init @0x5a4913]
            w.script.waypoints.show = (a.param1 != 0);
            w.out.effects.push({"show_waypoints", a.param1, 0, 0, 0, std::string()});
            break;
        case bms::ActionType::SetLightState:
            w.out.effects.push({"set_light", a.param1, a.param2, 0, 0, std::string()});
            break;
        case bms::ActionType::ShowWinSubgoal:
            // Set/clear the show-win bit — the RAW slot shift is the original's
            // (slot 1..8 -> bits 1..8); the shift count is masked to 5 bits
            // like the x86 shl. Then the "New Objective" notification (win,
            // relay flag 1) and, for a shown objective, the NEW_GOAL sound at
            // the local player through the misnamed full-volume play wrapper.
            // [orig: EventAction_Dispatch case 35 @0x4546af — shl
            //  @0x4546bd/@0x4546cc, bit @0x4546bf/@0x4546d0; the
            //  HUD_ShowObjectiveNotification call @0x4546e2; the local-player
            //  and param2 gates @0x4546e7..0x4546fb, the HUD_DrawDefaultProgressBar
            //  call @0x45470c with dword_24E0990 (the NEW_GOAL set) and the
            //  player position; HUD_DrawDefaultProgressBar @0x527e60 wraps
            //  Sound_Play3DPositional(set, pos, 0, 255)]
            if (a.param2 != 0) w.script.subgoals.show_win |= (1u << (a.param1 & 31));
            else w.script.subgoals.show_win &= ~(1u << (a.param1 & 31));
            w.show_objective_notification(a.param1, 1, a.param2, 1);
            if (a.param2 != 0) {
                if (const world::Entity *player = w.registry.get(w.cached.local_player))
                    w.out.fire_sounds.play_immediate("NEW_GOAL", player->position, 0);
            }
            break;
        case bms::ActionType::ShowLoseSubgoal:
            // The lose mirror: no sound, relay flag 0.
            // [orig: EventAction_Dispatch case 36 @0x454724 — shl
            //  @0x454732/@0x454741, the show-lose mirror @0x454734/@0x454745;
            //  the HUD_ShowObjectiveNotification call @0x454757]
            if (a.param2 != 0) w.script.subgoals.show_lose |= (1u << (a.param1 & 31));
            else w.script.subgoals.show_lose &= ~(1u << (a.param1 & 31));
            w.show_objective_notification(a.param1, 0, a.param2, 0);
            break;
        // The three win actions end the round in-engine [orig: EventAction_Dispatch
        // @0x4542E0 (the Server_ProcessRoundEnd(1/2/0) calls @0x45447b/@0x454495/
        // @0x4544af)]; the round-over gate sits inside the callee [orig:
        // Server_ProcessRoundEnd @0x5164F6, `cmp g_spawn_success_gate, ebx`],
        // which process_round_end's own latch ports. The "win" effect stays as
        // the presentation signal.
        case bms::ActionType::BlueWin:
            w.out.effects.push({"win", 1, 0, 0, 0, std::string()});
            w.process_round_end(1);
            break;
        case bms::ActionType::RedWin:
            w.out.effects.push({"win", 2, 0, 0, 0, std::string()});
            w.process_round_end(2);
            break;
        case bms::ActionType::GreenWin:
            w.out.effects.push({"win", 0, 0, 0, 0, std::string()});
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
            // The announcement relays to the joiners as the key with team 1.
            // [orig: EventAction_Dispatch case 14 @0x454500 — shl @0x454508,
            //  guard @0x45450a, set @0x45451d, round-running gate @0x45453a,
            //  the STRWINMSG%03i key @0x454552, its resolve @0x45456f, the
            //  GameMsg_AddChatLineAndRelay(line, 1, key) call @0x454578]
            const uint32_t bit = 1u << (a.param1 & 31);
            if ((w.script.subgoals.won & bit) != 0) break;
            w.script.subgoals.won |= bit;
            const int32_t text_id = (a.param1 >= 1 && a.param1 <= 8)
                    ? w.script.subgoals.win_text_ids[a.param1] : 0;
            const int32_t announce = w.match.outcome().ended ? 0 : 1;
            w.out.effects.push({"subgoal_won", a.param1, text_id, announce, 0, std::string()});
            if (announce != 0) {
                char key[32];
                std::snprintf(key, sizeof(key), "STRWINMSG%03d", text_id);
                w.relay_mission_text_chat(1, key);
            }
            break;
        }
        case bms::ActionType::SubGoalLost: {
            // No already-set guard (unlike won). The chat line AND the
            // persistent banner ride the same round-running gate; the
            // unknown5[3]-masked team-banner leg is deferred with its win
            // sibling. The announcement relays to the joiners as the key with
            // team 0. [orig: EventAction_Dispatch case 15 @0x4545e0 — set
            //  @0x4545ea, gate @0x4545f0, the STRLOSEMSG%03i key @0x45460c,
            //  the GameMsg_AddChatLineAndRelay(line, 0, key) call @0x454632,
            //  the GameMsg_SetBannerText call @0x454647; byte_A762D7 leg
            //  @0x45465c; the masked shl @0x4545e8]
            w.script.subgoals.lost |= (1u << (a.param1 & 31));
            const int32_t text_id = (a.param1 >= 1 && a.param1 <= 8)
                    ? w.script.subgoals.lose_text_ids[a.param1] : 0;
            const int32_t announce = w.match.outcome().ended ? 0 : 1;
            w.out.effects.push({"subgoal_lost", a.param1, text_id, announce, 0, std::string()});
            if (announce != 0) {
                char key[32];
                std::snprintf(key, sizeof(key), "STRLOSEMSG%03d", text_id);
                w.relay_mission_text_chat(0, key);
            }
            break;
        }
        case bms::ActionType::GroupResetHasVisited:
            // Clears ONE group row of the visited matrix — only lists 0..31,
            // the witnessed 0x80-byte memset quirk
            // [orig: EventTrigger_ClearSlotB @ 0x4535e0].
            w.script.relations.clear_group_visited_row(a.param1);
            break;
        case bms::ActionType::SingleResetHasVisited:
            // [orig: EventTrigger_ClearSlotA @ 0x453600] — the same 32-list quirk.
            w.script.relations.clear_single_visited_row(a.param1);
            break;
        case bms::ActionType::ResetEvent:
            // Clears ONLY the active latch; live countdowns keep ticking.
            // [orig: EventAction_Dispatch case 0x22 @0x454974 — event[+20] = 0]
            if (a.param1 >= 0 && static_cast<size_t>(a.param1) < events_.size()) {
                events_[a.param1].active = false;
            }
            break;
        case bms::ActionType::AttachToEmplaced:
            // The action carries ONLY the occupant SSN (param1); the vehicle is
            // the one the occupant's AI slot +0x90 already names (a board
            // order armed it), the same entry as WAC ssnuse.
            // [orig: EventAction_Dispatch case 37 @0x454989 (the
            //  EntityPool_FindByNetId call @0x454992) ->
            //  WacScript_TryMountEntityToVehicle @0x4F70F0]
            cmds.use_boarding_target(static_cast<uint16_t>(a.param1));
            break;
        case bms::ActionType::Teammates:
            // [orig: EventAction_Dispatch @0x4542E0, case 39]
            w.teammates.start(w, a.action_sub_type, a.param1, a.param2);
            break;
        case bms::ActionType::ExecuteWac:
            // Retail has no case 41 arm; the default returns without a call.
            // [orig: EventAction_Dispatch @0x4542E0]
            break;
        case bms::ActionType::SsnTargetSsnPri:
            cmds.set_ssn_target_selector(a.param1, world::AiTargetSelector::PreferredSsn, a.param2);
            break;
        case bms::ActionType::SsnTargetSsnExc:
            cmds.set_ssn_target_selector(a.param1, world::AiTargetSelector::ExclusiveSsn, a.param2);
            break;
        case bms::ActionType::SsnTargetGroupPri:
            cmds.set_ssn_target_selector(a.param1, world::AiTargetSelector::PreferredGroup, a.param2);
            break;
        case bms::ActionType::SsnTargetGroupExc:
            cmds.set_ssn_target_selector(a.param1, world::AiTargetSelector::ExclusiveGroup, a.param2);
            break;
        case bms::ActionType::GroupTargetSsnPri:
            cmds.set_group_target_selector(a.param1, world::AiTargetSelector::PreferredSsn, a.param2);
            break;
        case bms::ActionType::GroupTargetSsnExc:
            cmds.set_group_target_selector(a.param1, world::AiTargetSelector::ExclusiveSsn, a.param2);
            break;
        case bms::ActionType::GroupTargetGroupPri:
            cmds.set_group_target_selector(a.param1, world::AiTargetSelector::PreferredGroup, a.param2);
            break;
        case bms::ActionType::GroupTargetGroupExc:
            cmds.set_group_target_selector(a.param1, world::AiTargetSelector::ExclusiveGroup, a.param2);
            break;
        case bms::ActionType::SpecialSubType:
            // Three subs; every other one returns without a write.
            // [orig: EventAction_Dispatch case 28 @0x4548e0 (the
            //  EventAction_HandleSpecialTypes call @0x4548e1) ->
            //  EventAction_HandleSpecialTypes @0x4535a0, the fall-out @0x4535b4]
            switch (a.action_sub_type) {
                case 37:
                    // The HUD item flash: timer param1 takes param2 and the HUD
                    // layer table rebuilds at declutter level 0. The HUD owns the
                    // 16 timers, their per-frame countdown and the blink, so the
                    // write rides to it as the "hud_item_flash" effect
                    // (a = timer, b = value).
                    // [orig: EventAction_HandleSpecialTypes @0x4535cd — the
                    //  RenderState_SetLayerVisibilityByIndex call @0x4535d5;
                    //  RenderState_SetLayerVisibilityByIndex @0x5a3020 — the
                    //  store @0x5a302a, the CRenderState_SetLayerVisibility(0)
                    //  call @0x5a3031]
                    w.out.effects.push({"hud_item_flash", a.param1, a.param2, 0, 0, std::string()});
                    break;
                case 38:
                    // Every pending input bit drops [orig: @0x4535c2].
                    w.script.input_action_bits = 0;
                    break;
                case 39:
                    // dword_AE0718 = (param1 == 0): a word no code reads (its only
                    // other references zero it at load), so nothing is kept.
                    // [orig: @0x4535b6..0x4535bc; EventSystem_FreeAll @0x45336e;
                    //  EventTrigger_LoadAllData @0x454031]
                    break;
                default:
                    break;
            }
            break;
        default:
            // No faithful in-engine handler yet: record as an UNPORTED marker (coverage /
            // diagnostic only — never a presentation effect). Supported missions should
            // emit zero of these; a test asserts that. The arm is diagnostic-only
            // for unauthored action types; the last explicit boundary closed as
            // D-EVT-6 (bms-event-runtime-re §10.1).
            w.diagnostics.record({world::RuntimeGapKind::BmsAction, int32_t(a.action_type), a.action_sub_type, event, action},
                    w.logic_tick, {a.param1, a.param2, a.param3, a.param4});
            w.out.effects.push({"unported_action", static_cast<int32_t>(a.action_type), a.action_sub_type,
                            a.param1, a.param2, std::string()});
            break;
    }
}

void BmsEventSystem::fire(World &w, ScriptedEvent &se) {
    // [orig: the dispatch loops @0x454ca6/@0x454d0e: signed byte count, in order.]
    // The input-bit consume: the chain mirror (the live word with every
    // matched bit toggled off) is committed back to the live word right
    // before the actions run, at BOTH dispatch sites -- the immediate fire
    // and the delayed fire, which commits whatever the mirror holds at
    // expiry [orig: @0x454c8b immediate, @0x454cfa delayed].
    w.script.input_action_bits = w.script.input_action_mirror;
    const int32_t event = static_cast<int32_t>(&se - events_.data());
    // The raw file byte is signed only at dispatch: 128..255 execute no
    // actions, while the input consume, linked-spawn hook and timers still run.
    // Spell the sign extension without implementation-defined uint8 -> int8.
    // [orig: immediate @0x454C92/@0x454CAB; delayed @0x454D01/@0x454D13]
    const int action_count = se.event.action_count < 128
            ? se.event.action_count : static_cast<int>(se.event.action_count) - 256;
    for (int index = 0;
            index < action_count && static_cast<size_t>(index) < se.actions.size(); ++index)
        dispatch_action(w, se.actions[index], event, se.event.action_index + index);
    // The waypoint completion hook: a fired event completes every route marker
    // linked to its index (the original computes the index from the record's
    // position in g_Events; events_ mirrors that array order).
    // [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0, called right after
    //  both dispatch loops @0x454cbd/@0x454d25]
    // The hook returns at once off the authority [orig: the is_authority test
    //  @0x452ce0].
    if (w.rules.logic_authority && !events_.empty() && &se >= events_.data() &&
            &se < events_.data() + events_.size())
        w.script.waypoints.on_event_fired(static_cast<int32_t>(&se - events_.data()));
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
        if (se.active == 0) {
            // Any nonzero raw chain result passes [orig: @0x454c6b].
            if (evaluate_chain(w, se) != 0) {
                se.active = 1; // latched the moment the chain passes (@0x454c7a)
                if (se.activate_reload != 0) {
                    se.activate_countdown = se.activate_reload; // arm [orig: +16 = +18 @0x454c80]
                } else {
                    fire(w, se); // no delay: fire now
                }
                if ((static_cast<uint32_t>(se.event.flags) &
                     static_cast<uint32_t>(bms::EventFlags::ResetAfter)) != 0) {
                    if (se.repeat_reload != 0) {
                        // arm cooldown; no decrement this call [orig: +12 = +14 @0x454cd9]
                        se.repeat_countdown = se.repeat_reload;
                        return;
                    }
                    // No reload: re-evaluable next pass [orig:
                    //  EventTrigger_UpdateEntry @0x454d46].
                    se.active = 0;
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
            se.active = 0; // cooldown over -> the chain is evaluated again
        }
    }
}

void BmsEventSystem::run_post_mission_pass(World &w) {
    // ONE whole-list sweep over the PostMission-flag entries, run once by the
    // mission teardown (MissionKernel::run_post_mission_pass) — never
    // periodically, so an authored post delay of 1 fires at the transition and
    // larger delays effectively never do (D-EVT-4). The SP restart's call
    // finds the list already freed and sweeps nothing.
    // [orig: EventTrigger_UpdateAllWithFlag4 @0x454e00; live caller
    //  Game_TeardownMission @0x52266c; Game_RestartRoundSP @0x5263ae runs
    //  after EventSystem_FreeAll zeroed the count @0x453266]
    const uint32_t post_bit = static_cast<uint32_t>(bms::EventFlags::PostMission);
    for (ScriptedEvent &se : events_)
        if ((static_cast<uint32_t>(se.event.flags) & post_bit) != 0) update_entry(w, se);
}

void BmsEventSystem::tick(World &w, const opennova::world::TickContext &ctx) {
    if (!ctx.is_authority) return; // BMS events run only on the authoritative host

    const uint32_t pre_bit = static_cast<uint32_t>(bms::EventFlags::PreMission);
    const uint32_t post_bit = static_cast<uint32_t>(bms::EventFlags::PostMission);

    if (ctx.phase == opennova::world::TickPhase::PreMission) {
        // Pre-mission pass: every PreMission-flag entry, one whole-list sweep.
        // The embedder contract is ONE PreMission tick per mission start, before
        // the clock runs (MissionKernel delivers exactly one) — retail's pre
        // pass is a single call, never periodic (D-EVT-4).
        // [orig: EventTrigger_UpdateAllWithFlag2 @0x454dc0; sole caller
        //  Game_StartMission @0x525b86, before tick = 0 @0x525b9f]
        for (ScriptedEvent &se : events_)
            if ((static_cast<uint32_t>(se.event.flags) & pre_bit) != 0) update_entry(w, se);
        return;
    }
    // An empty host does not advance the mission. Retail wraps the WAC tick, the
    // idle sweep and this pump in one condition whose live half is
    // `wac_var_humans || !wac_var_ticks` — see World::script_may_advance. The
    // pre-mission pass above is deliberately OUTSIDE it, matching retail, where
    // that pass runs from Game_StartMission rather than the server tick.
    const bool admitted = ctx.script_admitted.has_value()
            ? *ctx.script_admitted : w.script_may_advance();
    if (!admitted) return;

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
        // [orig: quarter pass @0x454d50 cursor==0 -> Entity_UpdatePlayerAwolCounter
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
