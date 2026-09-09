// Airborne collision response through real authored CB geometry.
// [orig: Entity_MovementCollisionResolver @0x4B304C..0x4B308C,
// second pass @0x4B365F..0x4B36A5]
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <utility>

using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
constexpr int32_t fp(int units) { return units * 65536; }

// A broad CB with one controlling ceiling plane. Other planes close the box
// well outside the capsule, so its response is determined by the authored
// ceiling normal, including ties between its vertical and horizontal axes.
CollisionModel ceiling_model(int16_t nx, int16_t ny, int16_t nz) {
    CollisionModel model;
    auto plane = [&](int16_t x, int16_t y, int16_t z, int32_t dist) {
        CollisionPlane p;
        p.nx = x; p.ny = y; p.nz = z; p.dist = dist;
        model.planes.push_back(p);
    };
    plane(16384, 0, 0, -fp(20));
    plane(-16384, 0, 0, -fp(20));
    plane(0, 16384, 0, -fp(20));
    plane(0, -16384, 0, -fp(20));
    plane(0, 0, 16384, -fp(20));
    // The actor's head sample at Z=6.8 is just inside a ceiling at Z=6.6.
    plane(nx, ny, nz, static_cast<int32_t>(-int64_t(nz) * 432537 / 16384));
    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = volume.min_y = volume.min_z = -fp(20);
    volume.max_x = volume.max_y = volume.max_z = fp(20);
    volume.plane_count = 6;
    model.volumes.push_back(volume);
    CollisionSection section;
    section.volume_count = 1;
    model.sections.push_back(section);
    return model;
}

struct Result { int32_t velocity; int32_t height; bool contact; };

Result resolve(int16_t nx, int16_t ny, int16_t nz, int32_t velocity,
               uint32_t flags, bool player = false, bool authority = true,
               bool replica = false, bool engine_flags_only = false) {
    auto world = std::make_unique<World>();
    CollisionWorld collision;
    world->registry.configure_pool(0, 1);
    world->registry.configure_pool(2, 1);
    Entity building;
    building.kind = EntityKind::Building;
    building.position = {10, 10, 0};
    building.yaw = 90;
    building.alive = true;
    const auto obstacle = world->registry.spawn(2, building);
    collision.assign_entity(obstacle, collision.add_model(ceiling_model(nx, ny, nz)));
    EntityHandle actor;
    if (!replica) {
        Entity body;
        body.kind = EntityKind::Organic;
        body.position = {10, 10, 5};
        body.bound_radius = 1;
        body.alive = true;
        if (engine_flags_only) body.engine_flags = flags;
        else body.flags = flags;
        actor = world->registry.spawn(0, body);
    }
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(*world);
    CollisionWorld::ResolveState state;
    state.prev_valid = true;
    state.prev_pos[0] = state.prev_pos[1] = fp(10);
    state.prev_pos[2] = fp(4);
    int32_t pos[3] = {fp(10), fp(10), fp(5)};
    int32_t horizontal[2] = {};
    if (replica) {
        collision.resolve_replica(*world, state, pos, horizontal, velocity,
            0, 117964, fp(1), player, 0, 43, 1, nullptr, 0, 0xffff, &flags, nullptr);
    } else {
        int16_t health = 100;
        collision.resolve_entity(*world, actor, state, pos, horizontal, velocity,
            0, 117964, 0, 0, player, authority, 0, 43, 1, health);
    }
    return {velocity, pos[2], collision.dbg_last_contact.valid()};
}

void test_dominant_ceiling_response() {
    for (bool player : {false, true}) {
        for (bool authority : {false, true}) {
            auto hit = resolve(0, 0, -16384, 500, kEntityFlagInAir, player, authority);
            CHECK(hit.contact);
            CHECK(hit.height < fp(5));
            CHECK(hit.velocity == -167);
        }
    }
    CHECK(resolve(0, 0, -16384, 500, kEntityFlagInAir, false, true, false, true).velocity == -167);
    CHECK(resolve(0, 0, -16384, 500, kEntityFlagInAir, true, false, true).velocity == -167);
}

void test_horizontal_ceiling_response_and_threshold() {
    // The two passes both damp while the velocity remains above -167.
    for (auto normal : {std::pair<int16_t, int16_t>{-11585, 0},
                        std::pair<int16_t, int16_t>{11585, 0},
                        std::pair<int16_t, int16_t>{0, -11585}}) {
        auto hit = resolve(normal.first, normal.second, -11585, 0, kEntityFlagInAir);
        CHECK(hit.contact);
        CHECK(hit.velocity == -166);
    }
    CHECK(resolve(-15360, 0, -5120, 0, kEntityFlagInAir).velocity == -166);
    CHECK(resolve(-11585, 0, -11585, -100, kEntityFlagInAir).velocity == -183);
    CHECK(resolve(-11585, 0, -11585, -166, kEntityFlagInAir).velocity == -249);
    CHECK(resolve(0, 0, -16384, -167, kEntityFlagInAir).velocity == -167);
    CHECK(resolve(0, 0, -16384, -900, kEntityFlagInAir).velocity == -900);
}

void test_airborne_and_carrier_gates() {
    for (uint32_t flags : {0u, kEntityFlagDrowning}) {
        auto hit = resolve(0, 0, -16384, 500, flags);
        CHECK(hit.contact);
        CHECK(hit.velocity == 500);
    }
    auto carried = resolve(0, 0, -16384, 500, kEntityFlagInAir | kEntityFlagMounted);
    CHECK(!carried.contact);
    CHECK(carried.velocity == 500);
}
} // namespace

int main() {
    test_dominant_ceiling_response();
    test_horizontal_ceiling_response_and_threshold();
    test_airborne_and_carrier_gates();
    if (failures == 0) std::puts("collision_vertical_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
