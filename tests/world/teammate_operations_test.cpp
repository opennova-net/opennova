#include <runtime/mission/event_runtime.h>
#include <runtime/world/world.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_part_anim.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;
using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using State = TeammateOperations::State;
struct Factory : ITeammateSpawner {
    World &world;
    std::vector<TeammateSpawn> requests;
    int fail_request = -1;
    explicit Factory(World &w) : world(w) {}
    EntityHandle spawn_teammate(const TeammateSpawn &r) override {
        requests.push_back(r);
        if (int(requests.size()) == fail_request) return {};
        Entity seed; seed.kind = r.helicopter ? EntityKind::Item : EntityKind::Organic;
        seed.item_id = r.item_type; seed.net_id = r.ssn; seed.team = 1; seed.has_item_def = true;
        seed.position = {r.pos[0] / 65536.0f, r.pos[1] / 65536.0f, r.pos[2] / 65536.0f};
        seed.yaw = float(mission_yaw_deg_from_bam_heading(r.heading));
        seed.spawn_origin = kSpawnOriginNone;
        const EntityHandle h = world.registry.spawn(r.helicopter ? 1 : 0, seed);
        if (!h.valid()) return h;
        AiEntity &ai = *world.ai.at(world.ai.attach(h));
        std::copy_n(r.pos, 3, ai.pos); ai.heading = r.heading; ai.net_id = r.ssn;
        ai.inf.active = !r.helicopter;
        return h;
    }
};
struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Factory factory{world};
    mission::BmsEventSystem events;
    EntityHandle patient, marker;
    Fixture() {
        for (int p = 0; p < 4; ++p) world.registry.configure_pool(p, 64);
        world.teammate_spawner = &factory;
        world.add_system(&events);
        world.load_systems();
        Entity seed; seed.kind = EntityKind::Organic; seed.item_id = 111; seed.net_id = 73;
        seed.position = {10, 20, 3};
        patient = world.registry.spawn(0, seed);
        AiEntity &ai = *world.ai.at(world.ai.attach(patient));
        ai.pos[0] = 10 * 65536; ai.pos[1] = 20 * 65536; ai.pos[2] = 3 * 65536;
        seed.kind = EntityKind::Marker; seed.item_id = 6088; seed.net_id = 500; seed.wp_number = 9;
        seed.has_item_def = true; seed.yaw = 90;
        marker = world.registry.spawn(3, seed);
    }
    void action(int sub = 2, int patient_id = 73, int point = 9) {
        bms::Action a{}; a.action_type = bms::ActionType::Teammates;
        a.action_sub_type = sub; a.param1 = patient_id; a.param2 = point;
        events.dispatch_action_for_test(world, a);
    }
    void tick() { world.teammates.tick(world); ++world.logic_tick; }
    TeammateOperations::Slot slot() const { return *world.teammates.at(0); }
    AiEntity &air() { return *world.ai.for_handle(slot().helicopter.handle); }
    Entity &first() { return *world.registry.get(slot().first); }
    Entity &second() { return *world.registry.get(slot().second); }
    void move(EntityHandle h, int32_t x, int32_t y, int32_t z) {
        AiEntity &ai = *world.ai.for_handle(h); ai.pos[0] = x; ai.pos[1] = y; ai.pos[2] = z;
        world.registry.get(h)->position = {x / 65536.0f, y / 65536.0f, z / 65536.0f};
    }
    bool active_trigger(int sub) {
        bms::Trigger t{}; t.main_type = bms::TriggerMainType::Teammate; t.sub_type = sub;
        return events.evaluate_trigger_for_test(world, t);
    }
};
void test_dispatch_and_spawn() {
    Fixture f;
    f.action(3); f.action(2, 74); f.action(2, 73, 10);
    CHECK(f.factory.requests.empty()); CHECK(f.world.diagnostics.empty());
    f.action();
    CHECK(f.world.teammates.count() == 1);
    CHECK(f.factory.requests.size() == 3);
    CHECK(f.slot().state == State::FlyToHover);
    const auto &heli = f.factory.requests[0], &first = f.factory.requests[1], &second = f.factory.requests[2];
    CHECK(heli.item_type == 1281 && heli.ssn == 11000 && heli.heading == 2147483520);
    CHECK(heli.pos[0] == 610 * 65536 && heli.pos[1] == 20 * 65536 && heli.pos[2] == 83 * 65536);
    CHECK(first.item_type == 4529 && first.ssn == 12000 && first.pos[2] == heli.pos[2] - 98304);
    CHECK(first.heading == 1073741760);
    CHECK(second.item_type == 4520 && second.ssn == 12001);
    CHECK(second.pos[0] == first.pos[0] + 16384 && second.pos[1] == first.pos[1] + 16384);
    CHECK(f.first().ground_target == f.slot().helicopter.handle);
    CHECK(f.second().ground_target == f.slot().helicopter.handle);
    CHECK(f.world.registry.get(f.patient)->corpse_timer == 30000);
    CHECK(f.air().brain.f[13] == 3 && f.air().brain.f[19] == 38 * 65536 && f.air().brain.f[20] == 20 * 65536);
    CHECK(f.world.registry.get(f.slot().helicopter)->veh.part_spin.speed == kRotorSpeedMax);
    CHECK(f.world.ai.events.count() == 2);
    CHECK(f.world.ai.events.at(0).type() == 7 && f.world.ai.events.at(0).f[3] == 5);
    CHECK(f.world.ai.events.at(1).type() == 11 && f.world.ai.events.at(1).f[3] == 120);
    CHECK(f.active_trigger(2) && f.active_trigger(3));
}
void test_complete_sequence_and_boundaries() {
    Fixture f; f.action();
    f.world.logic_tick = 16;
    f.air().brain.f[22] = 0; f.tick();
    CHECK(f.slot().state == State::FlyToHover);
    // The patient animation at tick 16, retail values from jo-c's
    // scripted_lift_oracle (state 4): the angles scale by dbl_7C3608, not the
    // exact BAM scale. [orig: HeliLift_UpdateSlotState @0x45185c..0x451923]
    CHECK(f.world.ai.for_handle(f.patient)->inf.aim_pitch == -1565);
    CHECK(f.world.ai.for_handle(f.patient)->inf.torso_roll == -6434966);
    f.air().brain.f[22] = 40 * 65536; f.tick(); CHECK(f.slot().state == State::FlyToHover);
    f.air().brain.f[22] -= 1; f.tick(); CHECK(f.slot().state == State::Descend);
    CHECK(f.world.ai.events.at(2).f[3] == 20);
    f.air().brain.f[22] = 5 * 65536; f.tick(); CHECK(f.slot().state == State::Descend);
    f.air().brain.f[22] -= 1; f.tick(); CHECK(f.slot().state == State::Land);
    CHECK(f.air().brain.f[19] == 3 * 65536 && f.world.ai.events.at(3).f[3] == 0);
    auto slot = f.slot();
    Entity *heli = f.world.registry.get(slot.helicopter);
    heli->saved_live_pos[2] = f.air().pos[2] - 1;
    f.tick(); CHECK(f.slot().state == State::Land);
    heli->saved_live_pos[2] = f.air().pos[2];
    f.tick(); CHECK(f.slot().state == State::ApproachPatient);
    AiEntity *medic = f.world.ai.for_handle(slot.first.handle);
    CHECK(medic->slot.f[37] == 125 && medic->slot.f[38] == 73);
    CHECK(f.world.ai.for_handle(slot.second.handle)->slot.f[38] == 12000);
    f.move(slot.first.handle, 10 * 65536, 20 * 65536, 3 * 65536);
    f.tick(); CHECK(f.slot().state == State::Treat && f.slot().medic == 1);
    f.tick();
    CHECK((f.first().flags & 0x40) != 0);
    f.world.logic_tick = uint32_t(f.slot().deadline);
    f.tick(); CHECK(f.slot().state == State::Treat);
    f.tick(); CHECK(f.slot().state == State::Return);
    CHECK(f.first().dragger == slot.first.handle);
    CHECK(f.world.registry.get(f.patient)->dragger == slot.first.handle);
    CHECK(f.world.registry.get(f.patient)->dragger_spawn_id == f.first().registry_spawn_id);
    CHECK((f.first().flags & 0x40) == 0 && medic->slot.f[38] == 11000);
    const auto hx = f.air().pos[0], hy = f.air().pos[1], hz = f.air().pos[2];
    f.move(slot.first.handle, hx + 3 * 65536, hy, hz);
    f.move(slot.second.handle, hx + 65536, hy, hz);
    f.tick(); CHECK(f.slot().state == State::Return); // second alone cannot lead departure
    f.first().flags |= 2;
    f.tick(); CHECK(f.slot().state == State::Boarded);
    f.tick(); CHECK(f.slot().state == State::Ascend);
    f.move(slot.helicopter.handle, hx, hy, 37 * 65536);
    f.tick(); CHECK(f.slot().state == State::Ascend && f.air().brain.f[19] == 58 * 65536);
    f.move(slot.helicopter.handle, hx, hy, 38 * 65536);
    f.tick(); CHECK(f.slot().state == State::Depart && f.air().brain.f[22] == 0);
    f.tick(); CHECK(f.world.registry.get(f.patient) != nullptr);
    f.air().brain.f[22] = 30 * 65536;
    f.tick(); CHECK(f.slot().state == State::Depart);
    f.air().brain.f[22] -= 1;
    f.tick(); CHECK(f.slot().state == State::Finished && f.world.teammates.count() == 1);
    CHECK(f.world.registry.get(f.patient) == nullptr);
    CHECK(f.world.registry.get(slot.first) == nullptr && f.world.registry.get(slot.second) == nullptr);
    CHECK(f.world.registry.get(slot.helicopter) == nullptr);
    f.tick(); CHECK(f.world.teammates.count() == 0);
    CHECK(!f.active_trigger(2) && !f.active_trigger(3));
    CHECK(f.world.diagnostics.empty());
}
void test_same_tick_hover_fallthrough() {
    Fixture f; f.action();
    f.air().brain.f[22] = 4 * 65536;
    f.tick();
    CHECK(f.slot().state == State::Land);
    CHECK(f.world.ai.events.count() == 4);
    CHECK(f.world.ai.events.at(2).f[3] == 20 && f.world.ai.events.at(3).f[3] == 0);
}
void test_pickup_and_invalid_helicopter_boundary() {
    // Both medics spawn on the marker's own transform, whose heading is its
    // placement's spawn form: yaw 1 -> ((89 << 16) / 360) << 16. [orig:
    //  HeliLift_SpawnPickup @0x45262E (lea ebp,[eax+4]; the Entity_SpawnFromItemDef
    //  calls @0x452650/@0x4526A5); Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
    Fixture f; f.world.registry.get(f.marker)->yaw = 1; f.action(1);
    CHECK(f.factory.requests.size() == 2);
    CHECK(f.factory.requests[0].heading == 1061748736);
    CHECK(f.factory.requests[1].heading == 1061748736);
    CHECK(!f.slot().helicopter.valid());
    CHECK(f.world.registry.get(f.patient)->corpse_timer == 10000);
    CHECK(f.world.ai.for_handle(f.slot().first.handle)->slot.f[38] == 73);
    f.tick(); f.tick();
    f.world.logic_tick = uint32_t(f.slot().deadline);
    f.tick(); CHECK(f.slot().state == State::Treat);
    f.tick(); CHECK(f.slot().state == State::Finished);
    CHECK(f.world.diagnostics.total_calls() == 1);
    CHECK(f.world.registry.get(f.patient) != nullptr);
    CHECK((f.first().flags & 0x40) == 0);
}
void test_capacity_retry_and_failed_allocation() {
    Fixture f;
    f.action(); f.world.ai.capture_spawn_baseline();
    const auto baseline = f.world.snapshot();
    for (int i = 0; i < 8; ++i) f.action();
    CHECK(f.world.teammates.count() == 8);
    CHECK(f.factory.requests.size() == 24);
    f.world.restore(baseline);
    CHECK(f.world.teammates.count() == 1 && f.active_trigger(2));
    CHECK(f.world.registry.live_count() == baseline.registry.live_count());
    CHECK(f.world.teammate_spawner == &f.factory);
    CHECK(f.world.registry.get(f.slot().first) != nullptr);
    f.action(); CHECK(f.world.teammates.count() == 2);

    Fixture failed; failed.factory.fail_request = 3;
    failed.action();
    CHECK(failed.world.teammates.count() == 0 && failed.world.registry.live_count() == 2);
    CHECK(failed.world.ai.events.count() == 0);
    CHECK(failed.world.registry.get(failed.patient)->corpse_timer == 0);
    CHECK(failed.world.diagnostics.total_calls() == 1);
}
// The pickup announce goes to the nearest team-1 body inside the 40 u X/Y box
// whose 3D length is under the 0x40000000 seed; Z takes part in the ranking and
// in that ceiling, not in the box. [orig: Entity_FindNearestTeammate @0x4520B0:
//  seed @0x4520c3, box @0x452120/@0x452136, strict-less @0x452175]
void test_pickup_announce_seeds_the_nearest_teammate_at_0x40000000() {
    // The local player stands 1000 u from the marker, so the pickup's own
    // medics (spawned by the marker) never enter its box.
    constexpr int32_t kBase = 1000 * 65536;
    const auto person = [](Fixture &f, uint16_t ssn, int32_t x, int32_t y, int32_t z) {
        Entity seed; seed.kind = EntityKind::Organic; seed.item_id = 5; seed.net_id = ssn;
        seed.team = 1; seed.has_item_def = true;
        const EntityHandle h = f.world.registry.spawn(0, seed);
        f.world.ai.attach(h);
        f.move(h, x, y, z);
        return h;
    };
    Fixture f;
    f.world.cached.local_player = person(f, 1, kBase, kBase, 0);
    // Inside the box but 16384 u below: its 3D length reaches the seed, so it
    // never qualifies; the farther-in-X body inside the box is announced.
    person(f, 40, kBase + 30 * 65536, kBase, -0x40000000);
    const auto near = person(f, 41, kBase + 39 * 65536, kBase, 0);
    person(f, 42, kBase + 41 * 65536, kBase, 0); // outside the X box, close in 3D
    f.action(1);
    CHECK(f.world.script.voice.speaker() == near);
    CHECK(f.world.script.voice.snapshot().filename == "DltB086C.wav");

    Fixture lone;
    lone.world.cached.local_player = person(lone, 1, kBase, kBase, 0);
    person(lone, 40, kBase + 30 * 65536, kBase, -0x40000000);
    lone.action(1);
    CHECK(!lone.world.script.voice.speaker().valid());
}

void test_reused_patient_handle_is_not_dereferenced() {
    Fixture f; f.action(1);
    const auto old = f.slot().patient;
    f.world.commands.remove_ssn(old.handle);
    Entity seed; seed.item_id = 99; seed.net_id = 88;
    CHECK(f.world.registry.spawn_at(old.handle, seed) == old.handle);
    f.tick();
    CHECK(f.slot().state == State::Finished);
    CHECK(f.world.registry.get(old.handle)->dragger.valid() == false);
    CHECK(f.world.registry.get(old.handle)->corpse_timer == 0);
}
// The land state aims the helicopter from the original's heading formula:
// 0x42000000 minus the truncated atan2 bearing scaled by -2^31/pi, then the
// hover offsets 30 u along that heading from the goal. Retail vectors from
// jo-c's scripted_lift_oracle (state 6, the helicopter at the origin, the
// goal (123456, 234567, 0)): heading slot[7] / brain+0x210, brain+0x44/+0x48.
// [orig: HeliLift_UpdateSlotState @0x451730 — @0x451954..0x451973 heading,
//  @0x4519a1..0x4519d7 offsets]
void test_land_heading_matches_the_original() {
    struct Case { int32_t px, py; uint32_t heading; int32_t x, y; };
    const Case cases[] = {
        {655360, 0, 0x42000000u, 26888, 2198273},        // east
        {0, 655360, 0x82000000u, -1840265, 138281},      // north
        {-655360, 0, 0xC2000000u, 219835, -1729150},     // west
        {0, -655360, 0x02000000u, 2087167, 331040},      // south
        {655360, 655360, 0x62000000u, -1333411, 1554798}, // north-east
        {-300000, 900000, 0x8F1BFAE2u, -1709038, -477779},
    };
    for (const Case &c : cases) {
        Fixture f;
        f.world.registry.get(f.marker)->position = {123456 / 65536.0f, 234567 / 65536.0f, 0.0f};
        f.action();
        f.air().brain.f[22] = 4 * 65536;
        f.tick(); // FlyToHover falls through Descend into Land
        CHECK(f.slot().state == State::Land);
        f.move(f.slot().helicopter.handle, 0, 0, 0);
        f.move(f.patient, c.px, c.py, 0);
        f.tick();
        CHECK(uint32_t(f.slot().heading) == c.heading);
        CHECK(uint32_t(f.air().brain.f[AiBrain::kWorkHeading]) == c.heading);
        CHECK(f.air().brain.f[17] == c.x);
        CHECK(f.air().brain.f[18] == c.y);
    }
}
// A flyover whose helicopter is destroyed before landing stalls in its arm:
// retail reads the brain through the stale slot pointer to the zeroed row,
// gets 0, and neither the hover arms nor the land arm advance (jo-c's
// scripted_lift_oracle no_brain cases for states 4..6). The operation stays
// live, so the teammate triggers keep reading it.
// [orig: HeliLift_UpdateSlotState @0x451761/@0x4517c6/@0x45192e]
void test_flyover_with_a_destroyed_helicopter_stalls() {
    for (bool landed : {false, true}) {
        Fixture f; f.action();
        if (landed) {
            f.air().brain.f[22] = 4 * 65536;
            f.tick();
        }
        const State held = landed ? State::Land : State::FlyToHover;
        CHECK(f.slot().state == held);
        f.world.commands.remove_ssn(f.slot().helicopter.handle);
        for (int i = 0; i < 8; ++i) f.tick();
        CHECK(f.slot().state == held);
        CHECK(f.world.teammates.count() == 1 && f.active_trigger(2));
        CHECK(f.world.registry.get(f.patient) != nullptr);
    }
}
} // namespace
int main() {
    test_dispatch_and_spawn();
    test_complete_sequence_and_boundaries();
    test_same_tick_hover_fallthrough();
    test_pickup_and_invalid_helicopter_boundary();
    test_capacity_retry_and_failed_allocation();
    test_pickup_announce_seeds_the_nearest_teammate_at_0x40000000();
    test_reused_patient_handle_is_not_dereferenced();
    test_land_heading_matches_the_original();
    test_flyover_with_a_destroyed_helicopter_stalls();
    std::printf("teammate operations: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
