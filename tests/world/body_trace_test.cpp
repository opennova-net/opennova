// The body readout (world/body_trace.h), an instrument for the body_trace probe: the per-tick
// record of each watched handle, the ground probe, force and blast records, nothing for an
// unwatched handle, and the ring's window. It changes no game word.
#include <runtime/world/ai.h>
#include <runtime/world/body_trace.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct Rig {
    std::unique_ptr<World> w = std::make_unique<World>();
    EntityHandle a;
    EntityHandle b;

    Rig() {
        w->registry.configure_pool(0, 4);
        w->registry.configure_pool(1, 4);
        Entity person;
        person.kind = EntityKind::Organic;
        person.health = 90;
        person.position = Vec3{1.0f, 2.0f, 3.0f};
        a = w->registry.spawn(0, person);
        b = w->registry.spawn(0, person);
        AiEntity &body = *w->ai.at(w->ai.attach(a));
        body.inf.active = true;
        body.inf.vel[2] = -5;
        w->logic_tick = 77;
    }
};

} // namespace

static void test_the_tick_records_each_watched_handle() {
    Rig r;
    World &w = *r.w;
    body_trace_tick(w);
    CHECK(w.body_trace.next_seq() == 0); // nothing watched, nothing recorded
    w.body_trace.watch[0] = r.a.packed;
    w.body_trace.watch[1] = 0x1FFE; // a handle the world does not hold
    CHECK(w.body_trace.watches(r.a));
    CHECK(!w.body_trace.watches(r.b));
    body_trace_tick(w);
    const std::vector<BodyTraceRecord> rows = w.body_trace.since(0);
    CHECK(rows.size() == 2);
    if (rows.size() != 2) return;
    CHECK(rows[0].seq == 0 && rows[0].event == BodyTraceEvent::kTick);
    CHECK(rows[0].handle == r.a.packed && rows[0].present);
    CHECK(rows[0].logic_tick == 77);
    CHECK(rows[0].pos[0] == (1 << 16) && rows[0].pos[1] == (2 << 16) && rows[0].pos[2] == (3 << 16));
    CHECK(rows[0].vel[2] == -5); // the infantry body's triple
    CHECK(rows[0].health == 90);
    CHECK(!rows[0].mounted && rows[0].mount_target == 0xFFFFu);
    CHECK(rows[0].ground_target == 0xFFFFu);
    CHECK(rows[0].collision_model == -1); // no collision world
    CHECK(rows[1].handle == 0x1FFE && !rows[1].present);
    w.body_trace.clear_watch();
    CHECK(!w.body_trace.any_watch());
}

static void test_the_event_records_follow_the_watch() {
    Rig r;
    World &w = *r.w;
    w.body_trace.watch[0] = r.a.packed;
    // The ground probe returns its hit unchanged, and records only for a watched body.
    const int32_t start[3] = {11, 22, 33};
    CHECK(body_trace_ground_probe(w, r.b, start, 7, r.a) == r.a);
    CHECK(w.body_trace.next_seq() == 0);
    CHECK(body_trace_ground_probe(w, r.a, start, 7, r.b) == r.b);
    // The force applicator's record: its bytes and the velocity triple after the kick.
    Entity &body = *w.registry.get(r.a);
    const FixedVec3 source{5, 6, 7};
    apply_collision_force(w, body, 0, 0, source, r.b);
    CHECK(w.ai.for_handle(r.a)->inf.vel[2] == -5); // force 0 kicks nothing
    // The blast record over the target's words.
    const int32_t centre[3] = {100, 200, 300};
    body_trace_blast(w, body, 4, r.b, centre, 25, 115);
    body_trace_blast(w, *w.registry.get(r.b), 4, r.a, centre, 25, 115); // unwatched
    const std::vector<BodyTraceRecord> rows = w.body_trace.since(0);
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].event == BodyTraceEvent::kGroundProbe && rows[0].handle == r.a.packed);
    CHECK(rows[0].pos[0] == 11 && rows[0].pos[1] == 22 && rows[0].pos[2] == 33);
    CHECK(rows[0].probe_z == 7 && rows[0].ground_target == r.b.packed);
    CHECK(rows[1].event == BodyTraceEvent::kForce && rows[1].other == r.b.packed);
    CHECK(rows[1].force_type == 0 && rows[1].vel[2] == -5);
    CHECK(rows[1].source[0] == 5 && rows[1].source[1] == 6 && rows[1].source[2] == 7);
    CHECK(rows[2].event == BodyTraceEvent::kBlast && rows[2].other == r.b.packed);
    CHECK(rows[2].hit_type == 4 && rows[2].damage == 25);
    CHECK(rows[2].health_before == 115 && rows[2].health == 90);
    CHECK(rows[2].source[2] == 300);
    CHECK(std::string(body_trace_event_name(rows[2].event)) == "blast");
}

// A watched row with a brain: its tick carries the brain's current and pending states and the
// think countdown (+0x2AC); a timed event dispatched to it gives an ai_event record with the
// event's type and the states either side of the dispatch's transition (here a pending 5 the
// dispatch commits, through state rows whose handlers do nothing).
static void test_a_brain_row_records_its_states_and_events() {
    Rig r;
    World &w = *r.w;
    Entity hull;
    hull.kind = EntityKind::Item;
    hull.health = 40;
    hull.spawn_phase = 7;
    const EntityHandle h = w.registry.spawn(1, hull);
    const int index = w.ai.attach(h);
    AiEntity &brain = *w.ai.at(index);
    brain.handle = h;
    brain.brain.f[AiBrain::kCurState] = 3;
    brain.brain.f[AiBrain::kPendState] = 5;
    w.body_trace.watch[0] = h.packed;
    w.body_trace.watch[1] = r.a.packed;
    body_trace_tick(w);
    AiEventEntry event{};
    event.f[0] = 1;
    event.f[1] = index << 16;
    event.set_timer(0.0f);
    w.ai.events.queue(event);
    w.ai.events.process_timed(w.ai, w);
    const std::vector<BodyTraceRecord> rows = w.body_trace.since(0);
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].handle == h.packed && rows[0].brain_state == 3 && rows[0].brain_pend == 5);
    CHECK(rows[0].think_clock == 7);
    CHECK(rows[1].handle == r.a.packed && rows[1].brain_state == -1 && rows[1].brain_pend == -1);
    CHECK(rows[2].event == BodyTraceEvent::kAiEvent && rows[2].handle == h.packed);
    CHECK(rows[2].ai_event == 1 && rows[2].brain_pend == 3 && rows[2].brain_state == 5);
    CHECK(std::string(body_trace_event_name(rows[2].event)) == "ai_event");
}

static void test_the_ring_keeps_the_last_records() {
    BodyTrace trace;
    for (std::size_t i = 0; i < BodyTrace::kCapacity + 3; ++i) {
        BodyTraceRecord record;
        record.logic_tick = static_cast<uint32_t>(i);
        trace.append(record);
    }
    CHECK(trace.next_seq() == BodyTrace::kCapacity + 3);
    CHECK(trace.oldest_seq() == 3);
    const std::vector<BodyTraceRecord> all = trace.since(0);
    CHECK(all.size() == BodyTrace::kCapacity);
    if (!all.empty()) CHECK(all.front().seq == 3 && all.front().logic_tick == 3);
    CHECK(trace.since(BodyTrace::kCapacity + 1).size() == 2);
}

int main() {
    test_the_tick_records_each_watched_handle();
    test_the_event_records_follow_the_watch();
    test_a_brain_row_records_its_states_and_events();
    test_the_ring_keeps_the_last_records();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("body_trace: all passed\n");
    return 0;
}
