// The BMS event runtime against the original's own instructions. Every row of
// fixtures/mission_event_vectors.inc is a synthetic record set that
// scripts/oracles/mission_event_parity.py executed in the retail PE under
// Unicorn: the condition evaluator, the chain fold, the action dispatch, the
// entry scheduler and the quarter/pre/post passes (jo-c's mission-event
// probes), plus the signed trigger count, the raw Berserk fold, the
// SingleDestroyed/SingleAlive row walk and the authority-gated linked-spawn
// marks. The test rebuilds each record set in a World, loads the raw 24-byte
// runtime words through BmsEventSystem::event_for_test, runs the same root
// and compares every snapshot word.
// [orig: EventTrigger_EvaluateCondition @0x453620; EventTrigger_EvaluateChain
//  @0x454050; EventAction_Dispatch @0x4542E0; EventTrigger_UpdateEntry
//  @0x454C30; the passes @0x454D50/@0x454DC0/@0x454E00;
//  EventTrigger_MarkLinkedSpawnPoints @0x452CE0]
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <runtime/mission/event_runtime.h>
#include <runtime/world/world.h>

using namespace opennova;
using world::World;

namespace {

const int32_t kVectors[] = {
#include "fixtures/mission_event_vectors.inc"
};

enum Root { kCondition, kChain, kAction, kEntry, kQuarter, kPre, kPost };

struct Reader {
    const int32_t *p;
    int32_t next() { return *p++; }
};

bms::Trigger trigger_from(Reader &r) {
    bms::Trigger t{};
    t.condition_flags = r.next();
    t.main_type = static_cast<bms::TriggerMainType>(r.next());
    t.sub_type = r.next();
    t.param1 = r.next();
    t.param2 = r.next();
    t.param3 = r.next();
    t.param4 = r.next();
    t.unknown7 = r.next();
    return t;
}

bms::Action action_from(Reader &r) {
    bms::Action a{};
    a.reserved0 = r.next();
    a.action_type = static_cast<bms::ActionType>(r.next());
    a.action_sub_type = r.next();
    a.param1 = r.next();
    a.param2 = r.next();
    a.param3 = r.next();
    a.param4 = r.next();
    a.reserved1 = r.next();
    return a;
}

struct RawEvent {
    int32_t flags, trigger_start, action_start;
    uint16_t cooldown, reload, delay, delay_reload;
    uint8_t active, triggers, actions;
};

// Runs one fixture row; returns the number of mismatched words.
int run_case(const int32_t *row, int32_t length, int index) {
    Reader r{row};
    const int root = r.next();
    const int steps = r.next();
    const int entry_index = r.next();
    const bool authority = r.next() != 0;
    const uint32_t input = static_cast<uint32_t>(r.next());
    const uint32_t mirror = static_cast<uint32_t>(r.next());
    const int32_t local_behavior = r.next();
    std::vector<int32_t> vars(static_cast<size_t>(r.next()));
    for (int32_t &v : vars) v = r.next();
    std::vector<std::pair<int32_t, int32_t>> rows(static_cast<size_t>(r.next()));
    for (auto &row_words : rows) {
        row_words.first = r.next();
        row_words.second = r.next();
    }
    std::vector<RawEvent> raw(static_cast<size_t>(r.next()));
    for (RawEvent &e : raw) {
        e.flags = r.next();
        e.trigger_start = r.next();
        e.action_start = r.next();
        e.cooldown = static_cast<uint16_t>(r.next());
        e.reload = static_cast<uint16_t>(r.next());
        e.delay = static_cast<uint16_t>(r.next());
        e.delay_reload = static_cast<uint16_t>(r.next());
        e.active = static_cast<uint8_t>(r.next());
        e.triggers = static_cast<uint8_t>(r.next());
        e.actions = static_cast<uint8_t>(r.next());
    }
    std::vector<bms::Trigger> triggers(static_cast<size_t>(r.next()));
    for (bms::Trigger &t : triggers) t = trigger_from(r);
    std::vector<bms::Action> actions(static_cast<size_t>(r.next()));
    for (bms::Action &a : actions) a = action_from(r);
    std::vector<std::pair<int32_t, int32_t>> waypoints(static_cast<size_t>(r.next()));
    for (auto &wp : waypoints) {
        wp.first = r.next();
        wp.second = r.next();
    }

    auto world = std::make_unique<World>();
    World &w = *world;
    w.cached.humans = 1;
    for (int pool = 0; pool <= 3; ++pool) w.registry.configure_pool(pool, 64);
    for (const auto &row_words : rows) {
        world::Entity e{};
        e.net_id = static_cast<uint16_t>(row_words.first);
        e.engine_flags = static_cast<uint32_t>(row_words.second);
        w.registry.spawn(0, e);
    }
    if (local_behavior >= 0) {
        world::Entity player{};
        const world::EntityHandle h = w.registry.spawn(0, player);
        w.cached.local_player = h;
        w.ai.attach(h);
        w.ai.for_handle(h)->slot.f[world::AiSlot::kBehaviorFlags] = local_behavior;
    }

    std::vector<bms::Event> events;
    for (const RawEvent &e : raw) {
        bms::Event ev{};
        ev.flags = static_cast<bms::EventFlags>(e.flags);
        ev.trigger_index = e.trigger_start;
        ev.action_index = e.action_start;
        ev.trigger_count = e.triggers;
        ev.action_count = e.actions;
        events.push_back(ev);
    }
    mission::BmsEventSystem sys;
    sys.load(events, triggers, actions);
    w.add_system(&sys);
    w.load_systems();
    w.rules.logic_authority = authority;
    for (size_t i = 0; i < raw.size(); ++i) {
        mission::ScriptedEvent &se = sys.event_for_test(i);
        se.repeat_countdown = raw[i].cooldown;
        se.repeat_reload = raw[i].reload;
        se.activate_countdown = raw[i].delay;
        se.activate_reload = raw[i].delay_reload;
        se.active = raw[i].active;
    }
    for (size_t i = 0; i < vars.size(); ++i) w.script.vars.set_mission(static_cast<int32_t>(i), vars[i]);
    w.script.input_action_bits = input;
    w.script.input_action_mirror = mirror;
    for (const auto &wp : waypoints) {
        world::WaypointEntry entry;
        entry.linked_event = static_cast<int16_t>(wp.first);
        entry.chain_back = wp.second != 0;
        w.script.waypoints.entries.push_back(entry);
    }
    w.script.waypoints.current = -1;

    int mismatches = 0;
    const auto expect = [&](int32_t actual, const char *what, int step) {
        const int32_t expected = r.next();
        if (actual == expected) return;
        if (mismatches < 4)
            std::printf("FAIL case %d root %d step %d %s: got %d expected %d\n", index, root,
                        step, what, actual, expected);
        ++mismatches;
    };
    world::TickContext ctx{};
    ctx.world = &w;
    ctx.is_authority = true;
    ctx.script_admitted = true;
    for (int step = 0; step < steps; ++step) {
        int32_t result = 0;
        switch (root) {
            case kCondition: result = sys.evaluate_trigger_for_test(w, triggers[0]); break;
            case kChain: result = sys.evaluate_chain_for_test(w, static_cast<size_t>(entry_index)); break;
            case kAction: sys.dispatch_action_for_test(w, actions[0]); break;
            case kEntry: sys.update_entry_for_test(w, static_cast<size_t>(entry_index)); break;
            case kQuarter:
                // The quarter pass runs on the 16th tick of the gate.
                for (int tick = 0; tick < 16; ++tick) sys.tick(w, ctx);
                break;
            case kPre:
                ctx.phase = world::TickPhase::PreMission;
                sys.tick(w, ctx);
                ctx.phase = world::TickPhase::Gameplay;
                break;
            case kPost: sys.run_post_mission_pass(w); break;
            default: ++mismatches; break;
        }
        expect(result, "result", step);
        for (size_t i = 0; i < raw.size(); ++i) {
            const mission::ScriptedEvent &se = sys.events()[i];
            expect(se.repeat_countdown, "cooldown", step);
            expect(se.repeat_reload, "reload", step);
            expect(se.activate_countdown, "delay", step);
            expect(se.activate_reload, "delay_reload", step);
            expect(se.active, "latch", step);
        }
        for (int32_t v = 0; v < 8; ++v) expect(w.script.vars.get_mission(v), "var", step);
        expect(static_cast<int32_t>(w.script.input_action_bits), "input", step);
        expect(static_cast<int32_t>(w.script.input_action_mirror), "mirror", step);
        for (const world::WaypointEntry &entry : w.script.waypoints.entries)
            expect(entry.done ? 1 : 0, "mark", step);
    }
    if (r.p != row + length) {
        std::printf("FAIL case %d: row length mismatch\n", index);
        ++mismatches;
    }
    return mismatches;
}

} // namespace

int main() {
    int failed = 0;
    int cases = 0;
    const size_t total = sizeof(kVectors) / sizeof(kVectors[0]);
    for (size_t i = 0; i < total;) {
        const int32_t length = kVectors[i];
        if (run_case(&kVectors[i + 1], length - 1, cases) != 0) ++failed;
        i += static_cast<size_t>(length);
        ++cases;
    }
    if (failed != 0) {
        std::printf("%d of %d retail cases failed\n", failed, cases);
        return 1;
    }
    std::printf("mission_event_vectors: %d retail cases passed\n", cases);
    return 0;
}
