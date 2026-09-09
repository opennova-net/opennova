// Airborne collision response through real authored CB geometry.
// [orig: Entity_MovementCollisionResolver @0x4B304C..0x4B308C,
// second pass @0x4B365F..0x4B36A5]
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
constexpr int32_t fp(int units) { return units * 65536; }

// A broad CB with one controlling ceiling plane whose underside crosses the
// actor's column at `slab_z`. Other planes close the box well outside the
// capsule, so its response is determined by the authored ceiling normal,
// including ties between its vertical and horizontal axes.
CollisionModel ceiling_model(int16_t nx, int16_t ny, int16_t nz, int32_t slab_z = 432537) {
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
    // The resolver-only cases put the actor's head sample at Z=6.8, just
    // inside the default ceiling at Z=6.6.
    plane(nx, ny, nz, static_cast<int32_t>(-int64_t(nz) * slab_z / 16384));
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

// A flat 512x512 field at ground 0: the motors settle on it and the resolver's
// ground probe reads it through CollisionWorld::terrain.
struct FlatField {
    std::vector<uint16_t> heights = std::vector<uint16_t>(512 * 512, 0);
    std::vector<int> sectors = std::vector<int>(256, 1);
    opennova::terrain::TerrainHeightField field;
    FlatField() {
        field.heightmap = heights.data();
        field.dim = 512;
        field.layout.sector_grid = sectors.data();
    }
};

// Every clip plays without root motion and carries the 1.8 u capsule the
// resolver-only cases above pass by hand.
struct CapsuleSource final : IRootMotionSource {
    bool has_clip(int, int) const override { return true; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int, int32_t &phase, RootMotionFrame &out) override {
        ++phase;
        out = RootMotionFrame{};
        out.capsule_top = 117964;
        return true;
    }
};

// A motored body under the slab. Nothing here stamps Flags 0x2000: the org2
// player jumps into the slab and the org1 NPC rises under it, and the motors'
// own edges write the bit the resolver reads.
struct MotorRig {
    FlatField flat;
    CapsuleSource root;
    std::unique_ptr<World> w = std::make_unique<World>();
    CollisionWorld collision;
    EntityHandle body_h;
    AiEntity *body = nullptr;

    MotorRig(int16_t nx, int16_t ny, int16_t nz, int32_t slab_z, bool player, int32_t start_z) {
        w->registry.configure_pool(0, 4);
        w->registry.configure_pool(2, 4);
        Entity building;
        building.kind = EntityKind::Building;
        building.position = {10, 10, 0};
        building.yaw = 90;
        building.alive = true;
        const auto obstacle = w->registry.spawn(2, building);
        collision.assign_entity(obstacle, collision.add_model(ceiling_model(nx, ny, nz, slab_z)));
        collision.terrain = &flat.field;
        Entity e;
        e.kind = EntityKind::Organic;
        e.position = {10.0f, 10.0f, static_cast<float>(start_z) / 65536.0f};
        e.bound_radius = 1;
        e.alive = true;
        e.health = 100;
        e.health_max = 100;
        e.has_item_def = true;
        e.item_type = 3;
        if (player) e.flags |= kEntityFlagPlayer; // the retail player classifier
        body_h = w->registry.spawn(0, e);
        for (int i = 0; i < 17; ++i) collision.build_tick_tables(*w);
        w->tables.terrain = &flat.field;
        w->ai.terrain = &flat.field;
        w->ai.collision = &collision;
        w->ai.root_motion = &root;
        w->ai.is_authority = true;
        body = w->ai.at(w->ai.attach(body_h));
        body->inf.active = true;
        body->inf.adm_id = 1;
        body->inf.is_local_player = player;
        body->inf.anim_state = anim_state::kIdle;
        body->inf.anim_prev = anim_state::kIdle;
        body->pos[0] = fp(10);
        body->pos[1] = fp(10);
        body->pos[2] = start_z;
        body->health = 100;
    }
    Entity &entity() { return *w->registry.get(body_h); }
    uint32_t in_air() { return (entity().flags | entity().engine_flags) & kEntityFlagInAir; }
    void tick(uint32_t t) {
        w->logic_tick = t;
        w->ai.tick_infantry(*body, *w, t);
    }
};

// Retail keeps one Flags word: the org2 jump writes 0x2000 into it
// [orig: Entity_UpdateInfantryPlayerBody @0x4B7EDB / @0x4B7EEF] and the
// resolver tests that word [orig: @0x4B305E]. Jump a real player body into
// the slab and let the motor produce the bit.
void test_player_jump_into_ceiling_through_motor() {
    // Slab underside at 2.3 u. The player's eye sample is capped at 0xD000
    // by the motor's capsule fallback, so the head sample (0.9625 u lift +
    // its 0.9 u radius) is the one that reaches the slab: 0.4375 u = 28672
    // up, which the jump's 0x1600 - 208/tick arc crosses on its 7th tick.
    MotorRig rig(0, 0, -16384, 150733, /*player=*/true, 0);
    rig.tick(0); // settle on the ground
    CHECK(rig.in_air() == 0);
    CHECK(!rig.body->inf.airborne);
    rig.body->inf.jump_requested = true;
    rig.tick(1);
    CHECK(rig.body->inf.jump_cooldown == 32);
    CHECK(rig.body->inf.vel[2] == 0x1600);
    CHECK((rig.entity().flags & kEntityFlagInAir) != 0);
    CHECK((rig.entity().engine_flags & kEntityFlagInAir) != 0);
    for (uint32_t t = 2; t <= 6; ++t) {
        rig.tick(t); // free flight: gravity only, no contact
        CHECK(rig.body->inf.vel[2] == 0x1600 - 208 * static_cast<int32_t>(t - 1));
        CHECK(!rig.collision.dbg_last_contact.valid());
    }
    rig.tick(7);
    CHECK(rig.collision.dbg_last_contact.valid());
    CHECK(rig.body->inf.vel[2] == -167); // the head bonk on the contact tick
    CHECK(rig.body->pos[2] < 29632);     // pushed back out of the slab
    rig.tick(8);
    CHECK(rig.body->inf.vel[2] == -167 - 208); // plain gravity from there
    CHECK(rig.in_air() == kEntityFlagInAir);
    // It drops back to the ground; the landing clears the word and the mirror.
    uint32_t t = 9;
    while (rig.body->inf.airborne && t < 60) rig.tick(t++);
    CHECK(!rig.body->inf.airborne);
    CHECK(rig.in_air() == 0);
    CHECK(rig.body->pos[2] == 0);
}

// The org1 fall edge writes the same word [orig: Entity_UpdateInfantryAI
// @0x4BF8C8 / @0x4BF8CF]. A body launched upward under the slab arms the bit
// on its first even tick (the resolve precedes the edge) and its eye sample
// (1.8 u + the 0.3125 u radius) meets the slab on the next even tick: the
// dominant ceiling pins -167, the 45-degree overhang ties horizontal and
// vertical force and takes the -83 path on both passes.
void test_npc_fall_edge_under_overhang_through_motor() {
    for (bool dominant : {true, false}) {
        MotorRig rig(dominant ? 0 : -11585, 0, dominant ? -16384 : -11585, 432537,
                     /*player=*/false, 281805); // 4.3 u up under the 6.6 u slab
        rig.body->inf.vel[2] = 0x1600;
        rig.tick(0);
        CHECK(rig.body->inf.vel[2] == 0x1600 - 416);
        CHECK((rig.entity().flags & kEntityFlagInAir) != 0);
        CHECK((rig.entity().engine_flags & kEntityFlagInAir) != 0);
        CHECK(rig.body->inf.airborne);
        rig.tick(1); // odd tick: org1 skips the whole gravity/resolve block
        CHECK(rig.body->inf.vel[2] == 0x1600 - 416);
        rig.tick(2);
        CHECK(rig.body->inf.vel[2] == (dominant ? -167 : 0x1600 - 2 * 416 - 166));
    }
}
} // namespace

int main() {
    test_dominant_ceiling_response();
    test_horizontal_ceiling_response_and_threshold();
    test_airborne_and_carrier_gates();
    test_player_jump_into_ceiling_through_motor();
    test_npc_fall_edge_under_overhang_through_motor();
    if (failures == 0) std::puts("collision_vertical_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
