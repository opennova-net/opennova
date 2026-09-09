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
        world.add_system(&world.ai);
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
    CHECK(f.world.ai.for_handle(f.patient)->inf.aim_pitch == 0);
    CHECK(f.world.ai.for_handle(f.patient)->inf.torso_roll != 0);
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
    Fixture f; f.action(1);
    CHECK(f.factory.requests.size() == 2);
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
} // namespace
int main() {
    test_dispatch_and_spawn();
    test_complete_sequence_and_boundaries();
    test_same_tick_hover_fallthrough();
    test_pickup_and_invalid_helicopter_boundary();
    test_capacity_retry_and_failed_allocation();
    test_reused_patient_handle_is_not_dereferenced();
    std::printf("teammate operations: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
