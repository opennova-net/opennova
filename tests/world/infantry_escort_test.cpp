#include <runtime/world/world.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/entity_spawn.h>
#include <base/io/bam.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

using namespace opennova;
using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int32_t fx(float v) { return static_cast<int32_t>(v * 65536.0f); }

struct Motion : IRootMotionSource {
    std::set<int> missing;
    bool has_clip(int, int state) const override { return state > 0 && state < 252 && !missing.count(state); }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        ++phase; out = {};
        if (state == 138) out.dx = -2048; // authored backward root movement
        return has_clip(0, state);
    }
};
struct Anchors : IPoseProvider {
    bool enabled = true;
    int head_queries = 0, hand_queries = 0;
    bool resolve_skeletal_anchor(World &world, EntityHandle h, SkeletalAnchor anchor, int32_t out[3]) override {
        if (!enabled) return false;
        const AiEntity *body = world.ai.for_handle(h);
        if (!body) return false;
        std::copy_n(body->pos, 3, out);
        if (anchor == SkeletalAnchor::Head) {
            ++head_queries; out[0] += fx(.25f); out[1] += fx(.5f); out[2] += fx(1);
        } else {
            ++hand_queries; out[0] += fx(.75f); out[1] += fx(.25f); out[2] += fx(.5f);
        }
        return true;
    }
};
struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Motion motion;
    Anchors anchors;
    Fixture() {
        world.registry.configure_pool(0, 16);
        world.ai.is_authority = true;
        world.ai.root_motion = &motion;
        world.pose_provider = &anchors;
    }
    EntityHandle person(int ssn, float x, float y = 0, float z = 0) {
        Entity seed;
        seed.net_id = static_cast<uint16_t>(ssn);
        seed.kind = EntityKind::Organic;
        seed.item_id = 1; seed.item_type = 3; seed.has_item_def = true;
        seed.health = seed.health_max = 100; seed.leave_corpse = true;
        seed.position = {x,y,z}; seed.yaw = 90;
        const auto h = world.registry.spawn(0, seed);
        auto &body = *world.ai.at(world.ai.attach(h));
        body.net_id = seed.net_id; body.health = 100; body.inf.active = true;
        body.inf.adm_id = 1; body.inf.max_health = 100;
        body.inf.anim_state = 43; body.inf.anim_pending = 0;
        body.pos[0] = fx(x); body.pos[1] = fx(y); body.pos[2] = fx(z);
        return h;
    }
    AiEntity &body(EntityHandle h) { return *world.ai.for_handle(h); }
    Entity &entity(EntityHandle h) { return *world.registry.get(h); }
    void order(EntityHandle from, EntityHandle to) {
        auto &b = body(from);
        b.inf.move_mode = 0; b.inf.target_dist = 0;
        b.slot.f[37] = 125; b.slot.f[38] = entity(to).net_id;
        int32_t entry_heading = b.inf.target_heading;
        world.ai.infantry_board_think(b, world, 125, entry_heading);
    }
    void drag(EntityHandle corpse, EntityHandle medic) {
        entity(medic).dragger = medic;
        entity(medic).dragger_spawn_id = entity(medic).registry_spawn_id;
        entity(corpse).dragger = medic;
        entity(corpse).dragger_spawn_id = entity(medic).registry_spawn_id;
    }
};

void test_escort_approach_boundaries() {
    Fixture f;
    const auto medic = f.person(12000, -10), helper = f.person(12001, -10);
    const auto heli = f.person(11000, 0, 0, 5);
    f.order(medic, heli);
    CHECK(f.body(medic).inf.arrival_radius == 81920);
    CHECK(std::abs(f.body(medic).inf.move_target[0]) <= 1);
    CHECK(std::abs(f.body(medic).inf.move_target[1] + fx(4)) <= 1);
    CHECK(f.body(medic).inf.move_target[2] == fx(2.5f));
    // Offset distance is NOT fed back into the far arrival test.
    CHECK(f.body(medic).inf.target_dist == static_cast<int32_t>(std::sqrt(116.0) * 65536));
    f.body(medic).pos[0] = fx(-6); f.body(medic).pos[2] = fx(5);
    f.order(medic, heli);
    CHECK(std::abs(f.body(medic).inf.move_target[1] + fx(2)) <= 1);
    CHECK(f.body(medic).inf.move_target[2] == fx(5));
    f.body(medic).pos[0] = fx(-4);
    f.order(medic, heli);
    CHECK(std::abs(f.body(medic).inf.move_target[1] - fx(.75f)) <= 1);
    CHECK(f.body(medic).inf.move_target[2] == fx(3.25f));
    f.body(helper).pos[0] = fx(-3); f.body(helper).pos[2] = fx(5);
    f.order(helper, heli);
    CHECK(f.body(helper).inf.arrival_radius == fx(1));
    CHECK(f.body(helper).inf.move_target[2] == fx(3.5f));
    f.body(helper).pos[0] = fx(-1);
    f.order(helper, heli);
    CHECK(f.body(helper).inf.arrival_radius == 106496);
    CHECK(f.body(helper).inf.move_target[0] == 122880);
    CHECK(f.body(helper).inf.move_target[2] == fx(3.5f));
}

void test_patient_head_approach_and_fallback() {
    Fixture f;
    const auto medic = f.person(12000, -3), patient = f.person(9, 0, 0, 4);
    f.body(medic).inf.aim_pitch = 123;
    f.order(medic, patient);
    CHECK(f.body(medic).inf.move_target[0] == fx(.25f));
    CHECK(f.body(medic).inf.move_target[1] == fx(.5f));
    CHECK(f.body(medic).inf.move_target[2] == fx(4)); // no head-Z substitution
    CHECK(f.body(medic).inf.arrival_radius == 24576);
    CHECK(f.body(medic).inf.aim_pitch == 0);
    CHECK(f.anchors.head_queries == 1);
    f.anchors.enabled = false;
    f.order(medic, patient);
    CHECK(f.body(medic).inf.move_target[0] == 0);
    CHECK(f.body(medic).inf.move_target[1] == 0);
}

void test_backward_drag_gait_and_root_motion() {
    Fixture f;
    const auto medic = f.person(12000, -5), patient = f.person(9, 0);
    f.drag(patient, medic);
    auto &b = f.body(medic);
    b.inf.body_heading = INT32_MIN;
    b.heading = INT32_MIN;
    b.inf.aim_pitch = 123;
    f.order(medic, patient);
    f.world.ai.infantry_select(b, f.world);
    CHECK(b.inf.body_heading == INT32_MIN);
    CHECK(io::bam_abs(io::bam_sub(b.inf.target_heading, INT32_MIN)) < 100000000);
    CHECK(b.inf.anim_state == 138);
    CHECK(b.inf.aim_override && b.inf.aim_pitch == 0);
    const int32_t before = b.pos[0];
    // This entity's staggered think is tick 0 mod 16. Tick 1 runs its motor.
    f.world.ai.tick_infantry(b, f.world, 1);
    CHECK(b.pos[0] > before);
    b.inf.move_mode = 0; b.inf.target_dist = 0; b.inf.anim_state = 43;
    f.world.ai.infantry_select(b, f.world);
    CHECK(b.inf.anim_state == 137);
    f.motion.missing.insert(137); b.inf.anim_state = 43;
    f.world.ai.infantry_select(b, f.world);
    CHECK(b.inf.anim_state == 43);
}

void test_corpse_hand_follow_and_lifetime() {
    Fixture f;
    const auto medic = f.person(12000, 2, 3), patient = f.person(9, 0, 0);
    f.drag(patient, medic);
    auto &b = f.body(patient);
    b.health = 0;
    f.entity(patient).health = 0; f.entity(patient).alive = false;
    f.entity(patient).flags = kEntityFlagDead | kEntityFlagMounted;
    b.inf.anim_state = 198; b.inf.anim_pending = 43;
    b.inf.aim_valid = true; // died holding an aim solution (aimFlag set)
    f.body(medic).inf.body_heading = 9876543;
    f.world.ai.tick_infantry(b, f.world, 1);
    CHECK(b.pos[0] == fx(2.5f) && b.pos[1] == fx(2.75f));
    CHECK(b.pos[2] == 0); // ground/gravity, never teleported to the hand's Z
    CHECK(b.inf.anim_state == 139 && b.inf.anim_pending == 0);
    CHECK(b.inf.aim_heading == 9876543);
    // The drag clears aimFlag [orig: @0x4B9E30], so the same tick's look chase
    // takes the eighth-step arm. The org1 body turn first quarter-steps the
    // body heading by (9876543 + 2) >> 2 = 2469136 and moves the live look yaw
    // by that same step [orig: @0x4be8fd..0x4be931]; the look chase then adds
    // (9876543 - 2469136 + 4) >> 3 = 925926 (the quarter-step arm would add
    // 1851852).
    CHECK(!b.inf.aim_valid);
    CHECK(b.inf.body_heading == 2469136);
    CHECK(b.heading == 2469136 + 925926);
    CHECK((f.entity(patient).flags & kEntityFlagMounted) == 0);
    CHECK(f.anchors.head_queries == 1 && f.anchors.hand_queries == 1);
    const auto baseline = f.world.snapshot();
    entity_reset_to_spawn_state(f.entity(patient));
    CHECK(!f.entity(patient).dragger.valid());
    f.world.restore(baseline);
    CHECK(f.entity(patient).dragger == medic);
    const int32_t before_x = f.body(patient).pos[0];
    f.world.registry.despawn(medic);
    const auto replacement = f.person(12000, 100, 100);
    CHECK(replacement == medic);
    f.world.ai.tick_infantry(f.body(patient), f.world, 2);
    CHECK(f.body(patient).pos[0] == before_x);
}

void test_draggee_clip_and_dead_medic_gates() {
    Fixture f;
    const auto medic = f.person(12000, 2), patient = f.person(9, 0);
    f.drag(patient, medic);
    auto &b = f.body(patient);
    b.health = 0; f.entity(patient).health = 0; f.entity(patient).alive = false;
    f.entity(patient).flags = kEntityFlagDead; b.inf.anim_state = 198;
    f.motion.missing.insert(139);
    f.world.ai.tick_infantry(b, f.world, 1);
    CHECK(b.pos[0] == 0 && b.inf.anim_state == 198);
    f.motion.missing.clear();
    f.entity(medic).flags |= kEntityFlagDead;
    f.world.ai.tick_infantry(b, f.world, 2);
    CHECK(b.pos[0] == 0 && f.anchors.hand_queries == 0);
}
} // namespace
int main() {
    test_escort_approach_boundaries();
    test_patient_head_approach_and_fallback();
    test_backward_drag_gait_and_root_motion();
    test_corpse_hand_follow_and_lifetime();
    test_draggee_clip_and_dead_medic_gates();
    if (!failures) std::puts("infantry_escort: OK");
    return failures ? 1 : 0;
}
