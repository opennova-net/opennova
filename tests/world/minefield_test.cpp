// [orig: Entity_InitHardpoints @ 0x4417D0; Entity_LandmineThink @ 0x441A40]
#include <base/io/rotating_prng.h>
#include <base/resource_index/resource_index.h>
#include <runtime/mission/item_traits.h>
#include <runtime/assets/asset_store.h>
#include "common/test_paths.h"
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace opennova;
using namespace opennova::world;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Rig {
    World world;
    Rig() {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 128);
        world.tables.ammo.entries.resize(3);
        for (int i = 0; i < 3; ++i) {
            auto &ammo = world.tables.ammo.entries[i];
            ammo.valid = true;
            ammo.flags = kAmmoFlagInstantKillZone;
            ammo.kztype = ammo_kz::kStandard;
            ammo.kz_damage = 100;
            ammo.kz_minradius = 1;
            ammo.kz_maxradius = 2;
        }
    }
    EntityHandle field(int pool = 2, uint8_t type = 2) {
        Entity seed;
        seed.item_id = 1896;
        seed.bound_radius = 128;
        seed.yaw = 90; // identity placement
        const EntityHandle h = world.registry.spawn(pool, seed);
        std::vector<MinefieldPoint> points;
        for (int i = 0; i < 14; ++i)
            points.push_back({type, {i * 4 * 65536, 0, 0}});
        world.minefields.initialize(world, *world.registry.get(h), true, true, true,
                0x101, 2, "small_marker", "large_marker", points);
        return h;
    }
    EntityHandle actor(float x, float z, uint8_t stance = 0) {
        Entity seed;
        seed.item_id = 1;
        seed.position = {x, 0, z};
        seed.net_stance_bits = stance;
        return world.registry.spawn(0, seed);
    }
};

void contact_boundary_and_stances() {
    for (uint8_t stance = 0; stance < 4; ++stance) {
        auto rig = std::make_unique<Rig>();
        auto &w = rig->world;
        const auto h = rig->field();
        const float z = stance == 0 ? 1.0f : stance == 1 ? 0.0f : 0.5f;
        const auto a = rig->actor(0.75f, z, stance);
        w.minefields.think(w, *w.registry.get(h));
        CHECK(w.registry.get(h)->section_mask == 0);
        w.registry.get(a)->position.x -= 1.0f / 65536.0f;
        // Neither health, team, nor owner filters the original proximity walk.
        w.registry.get(a)->health = 0;
        w.registry.get(a)->alive = false;
        w.minefields.think(w, *w.registry.get(h));
        CHECK(w.registry.get(h)->section_mask == 1);
        CHECK(w.round_sim.fired.size() == 1);
        CHECK(w.round_sim.fired[0].ammo_index == 1); // DWORD lookup, byte fire
        CHECK(w.round_sim.fired[0].shooter == h);
        w.minefields.think(w, *w.registry.get(h));
        CHECK(w.round_sim.fired.size() == 1);
    }
}

void carried_and_vehicle() {
    auto rig = std::make_unique<Rig>();
    auto &w = rig->world;
    const auto h = rig->field();
    const auto a = rig->actor(0, 1);
    w.registry.get(a)->engine_flags |= 1;
    w.minefields.think(w, *w.registry.get(h));
    CHECK(w.registry.get(h)->section_mask == 0);
    Entity vehicle;
    vehicle.item_id = 42;
    vehicle.has_item_def = true;
    vehicle.item_type = 1;
    vehicle.bound_radius = 1;
    vehicle.position = {0, 0, 1};
    const auto v = w.registry.spawn(1, vehicle);
    // A parked vehicle triggers; no speed, team or life-state condition.
    w.minefields.think(w, *w.registry.get(h));
    CHECK(w.registry.get(h)->section_mask == 1);
    w.registry.get(v)->position.x = 4;
    w.registry.get(v)->item_type = 2;
    w.minefields.think(w, *w.registry.get(h));
    CHECK(w.registry.get(h)->section_mask == 1);
}

void peer_and_ceasefire() {
    for (int mode = 0; mode < 3; ++mode) {
        auto rig = std::make_unique<Rig>();
        auto &w = rig->world;
        const auto h = rig->field(2, 4);
        rig->actor(0, 1);
        rig->actor(0, 1);
        w.rules.logic_authority = mode != 1;
        w.rules.projectile_authority = mode != 1;
        w.rules.mp_session = mode == 1;
        w.rules.cease_fire = mode == 2;
        const auto before = w.out.rounds.last_stat();
        w.minefields.think(w, *w.registry.get(h));
        CHECK(w.registry.get(h)->section_mask == 1);
        CHECK(w.registry.get(h)->minefield.age == (mode == 1 ? 1 : 3));
        CHECK(w.round_sim.fired.size() == 1);
        CHECK(w.round_sim.fired[0].ammo_index == 2);
        CHECK(w.round_sim.active_count == 0); // instantkillzone
        CHECK(w.explosions.queue.size() == (mode == 0 ? 1u : 0u));
        CHECK(w.out.rounds.last_stat() == before + (mode == 0 ? 1 : 0));
    }
}

void quantization_rng_and_reset() {
    auto rig = std::make_unique<Rig>();
    auto &w = rig->world;
    uint32_t expected = io::kPrng16BSeed;
    for (int i = 0; i < 3072 * 3; ++i) io::rotating_prng_next16(expected);
    CHECK(w.prng16_b_state == expected);
    const auto h = rig->field();
    for (int i = 0; i < 14; ++i) {
        const auto value = io::rotating_prng_next16(expected);
        CHECK(w.registry.get(h)->minefield.rotations[i] == static_cast<uint8_t>(value));
    }
    CHECK(w.prng16_b_state == expected);
    const auto snapshot = w.snapshot();
    auto *field = w.registry.get(h);
    field->section_mask = 0x3FFF;
    w.minefields.think(w, *field);
    CHECK(w.registry.get(h) == nullptr);
    w.restore(snapshot);
    field = w.registry.get(h);
    CHECK(field != nullptr && field->section_mask == 0);
    CHECK(w.prng16_b_state == expected);
    w.minefields.initialize(w, *field, true, true, true, 0, 0, "", "", {});
    CHECK(w.prng16_b_state == expected); // resweeps do not reinitialize

    Entity seed;
    seed.item_id = 1896;
    seed.yaw = 90;
    const auto q = w.registry.spawn(2, seed);
    const std::vector<MinefieldPoint> points = {
        {1, {-1, 0x800000, -257}}, {3, {65536, 0, 0}}, {0, {}}
    };
    w.minefields.initialize(w, *w.registry.get(q), true, true, true, 1, 2, "s", "l", points);
    CHECK(w.minefields.point(*w.registry.get(q), 0).x == -256);
    CHECK(w.minefields.point(*w.registry.get(q), 0).y == -0x800000);
    CHECK(w.minefields.point(*w.registry.get(q), 0).z == -512);
    CHECK(w.registry.get(q)->minefield.types[2] == 0);
    w.registry.get(q)->position = {100, 100, 100};
    CHECK(w.minefields.point(*w.registry.get(q), 1).x == 65536); // cached placement
}

// One entity-update visit of a minefield row: its own pool-1 visit, or its
// pool's cohort walk. [orig: Entity_UpdatePool1Slot @0x4B8DD0; Entity_UpdateAllEntities
// pool 2 @0x4C2244, pool 3 @0x4C230C]
void visit_field(World &w, EntityHandle h, int pool) {
    if (pool != 1) {
        tick_item_event_pool(w, pool);
        return;
    }
    TickContext ctx;
    ctx.world = &w;
    ctx.is_authority = true;
    ctx.logic_tick = w.logic_tick;
    if (Entity *row = w.registry.get(h)) w.update_pool1_slot(*row, ctx);
}

// The pool-1 visit thinks on the PRE-decrement clock and steps it once, so the
// authority's re-arm to 3 thinks every third visit; pools 2/3 step 8/64 on
// their cohort. [orig: Entity_UpdatePool1Slot `cmp [esi+2ACh],0; jg`
// @0x4B8E1B, the think @0x4B8E3C, `add [esi+2ACh],-1` @0x4B8EA0;
// Entity_LandmineThink `mov [ebp+2ACh],eax` @0x441B1A (3 on the authority)]
void cadence_and_exhaustion() {
    for (int pool = 1; pool < 4; ++pool) {
        auto rig = std::make_unique<Rig>();
        auto &w = rig->world;
        const auto h = rig->field(pool);
        rig->actor(0, 1);
        auto *field = w.registry.get(h);
        field->minefield.age = 1;
        w.logic_tick = h.slot();
        visit_field(w, h, pool);
        CHECK(field->section_mask == 0);
        CHECK(field->minefield.age == (pool == 1 ? 0 : pool == 2 ? -7 : -63));
        visit_field(w, h, pool);
        CHECK(field->section_mask == 1);
        CHECK(field->minefield.age == (pool == 1 ? 2 : 3));
        if (pool == 1) {
            // Re-armed to 3 and stepped once: two quiet visits, then the think.
            visit_field(w, h, pool);
            visit_field(w, h, pool);
            CHECK(field->minefield.age == 0);
        }
        field->section_mask = 0x3FFE;
        field->minefield.age = 0;
        visit_field(w, h, pool);
        CHECK(w.registry.get(h) != nullptr);
        CHECK(field->section_mask == 0x3FFF);
        field->minefield.age = 0;
        visit_field(w, h, pool);
        CHECK(w.registry.get(h) == nullptr);
    }
}

void empty_slots_and_markers() {
    auto rig = std::make_unique<Rig>();
    auto &w = rig->world;
    Entity seed;
    seed.item_id = 1;
    seed.bound_radius = 10;
    seed.yaw = 90;
    const auto h = w.registry.spawn(2, seed);
    auto *field = w.registry.get(h);
    w.minefields.initialize(w, *field, true, true, true, 1, 2, "s", "l",
            {{1, {}}, {2, {}}, {3, {}}, {4, {}}});
    field->minefield.rotations[0] = 0;
    std::vector<MinefieldDraw> draws;
    w.minefields.compile_draws(w, draws);
    CHECK(draws.size() == 2);
    CHECK(draws[0].model == "s" && draws[1].model == "l");
    CHECK(draws[0].transform.m[0] == 0x400000);
    CHECK(draws[0].transform.m[5] == 0x400000);
    CHECK(draws[0].transform.m[10] == 0x400000);
    field->section_mask = 1;
    w.minefields.compile_draws(w, draws);
    CHECK(draws.size() == 1 && draws[0].slot == 2);
    rig->actor(0, 1);
    w.minefields.think(w, *field);
    CHECK(field->section_mask == 0x3FFF); // all 14 slots participate, even zero-filled
    CHECK(w.round_sim.fired.size() == 3); // zero types spend without firing
    w.minefields.compile_draws(w, draws);
    CHECK(draws.empty());
}


void vehicle_ground_query() {
    auto rig = std::make_unique<Rig>();
    auto &w = rig->world;
    auto collision_storage = std::make_unique<CollisionWorld>();
    auto &collision = *collision_storage;
    w.collision = &collision;
    CollisionModel floor;
    for (const auto &xy : {std::pair<int, int>{-2, -2}, {2, -2}, {0, 2}}) {
        CollisionVertex vertex;
        vertex.p[0] = xy.first * 65536;
        vertex.p[1] = xy.second * 65536;
        floor.vertices.push_back(vertex);
    }
    CollisionNormal normal;
    normal.n[2] = 16384;
    normal.dominant_axis = 1;
    floor.normals.push_back(normal);
    CollisionFace face;
    face.normal_index = 0;
    face.vertex_index[0] = 0; face.vertex_index[1] = 1; face.vertex_index[2] = 2;
    face.min[0] = face.min[1] = -2 * 65536;
    face.max[0] = face.max[1] = 2 * 65536;
    floor.faces.push_back(face);
    CollisionSection section;
    section.vertex_count = 3; section.normal_count = 1; section.face_count = 1;
    floor.sections.push_back(section);
    Entity deck;
    deck.kind = EntityKind::Item;
    deck.item_id = 42;
    deck.yaw = 90;
    deck.position = {0, 0, 2};
    deck.bound_radius = 4;
    const auto h = w.registry.spawn(1, deck);
    collision.assign_entity(h, collision.add_model(std::move(floor)));
    collision.build_tick_tables(w);
    CHECK(collision.minefield_ground(w, {}, {0, 0, 4 * 65536}, true) == 2 * 65536);
    CHECK(collision.minefield_ground(w, h, {0, 0, 4 * 65536}, true) == 4 * 65536);
    // No CFAC hit leaves the input Z indoors, but uses terrain outside.
    CHECK(collision.minefield_ground(w, {}, {10 * 65536, 0, 4 * 65536}, true) == 4 * 65536);
    CHECK(collision.minefield_ground(w, {}, {10 * 65536, 0, 4 * 65536}, false) == 0);
    // An unresolved model has a broad-phase sphere in ordinary projectiles;
    // the mine ground query must never substitute that sphere for a CFAC hit.
    Entity unresolved = deck;
    unresolved.position = {10, 0, 2};
    w.registry.spawn(1, unresolved);
    collision.build_tick_tables(w);
    CHECK(collision.minefield_ground(w, {}, {10 * 65536, 0, 4 * 65536}, true) == 4 * 65536);
}

void buried_mine_blast() {
    auto rig = std::make_unique<Rig>();
    auto &w = rig->world;
    auto collision = std::make_unique<CollisionWorld>();
    std::vector<uint16_t> heights(512 * 512, 256); // one-unit ground
    std::vector<int> sectors(256, 1);
    terrain::TerrainHeightField ground;
    ground.heightmap = heights.data(); ground.dim = 512;
    ground.layout.sector_grid = sectors.data();
    collision->terrain = &ground;
    w.collision = collision.get();
    const auto target = rig->actor(0, 2);
    w.registry.get(target)->health = 150;
    w.registry.get(target)->bound_radius = 0.6f;
    w.tables.ammo.entries[1].kz_damage = 155;
    w.tables.ammo.entries[1].kztype = ammo_kz::kC4;
    const int32_t from[3] = {0, 0, 2 * 65536};
    const int32_t to[3] = {0, 0, 65535};
    CHECK(!collision->raycast_clear(w, from, to, target, {}));
    CHECK(collision->entity_los_clear(w, target, {}, from, to, -0x4000));
    ExplosionEntry blast;
    blast.pos = {0, 0, 65535.0f / 65536};
    blast.ammo_index = 1;
    blast.type = ammo_kz::kC4;
    w.explosions.queue_explosion(w, blast);
    w.explosions.process(w, collision.get(), &ground, 0, w.out.destruction);
    CHECK(w.registry.get(target)->health <= 0);
}

void authored_binding() {
    ResourceIndex index;
    CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth"));
    assets::AssetStore models{&index};

    def::DefItemDef definition{};
    definition.id = 101896;
    std::strcpy(definition.graphic, "pump_minefield");
    std::strcpy(definition.ai_function, "LnDm");
    std::strcpy(definition.render_function, "lndm");
    std::strcpy(definition.ammo_closeattack, "small");
    std::strcpy(definition.ammo_marker3, "LARGE");
    std::strcpy(definition.huskfinal, "gun");
    std::strcpy(definition.husk, "crate");
    def::DefItemsFile definitions{};
    definitions.entries = &definition;
    definitions.count = 1;
    for (int yaw : {0, 90}) {
        auto rig = std::make_unique<Rig>();
        auto &w = rig->world;
        w.tables.ammo.entries[1].name = "SMALL";
        w.tables.ammo.entries[2].name = "large";
        Entity field;
        field.item_id = 1896;
        field.position = {12, 34, 56};
        field.yaw = yaw;
        field.uniform_scale_q16 = 0x20000;
        const auto handle = w.registry.spawn(2, field);
        mission::resolve_minefields(w, definitions, models);
        const auto &bound = *w.registry.get(handle);
        CHECK(bound.minefield.think && bound.minefield.render);
        CHECK(bound.minefield.ammo_small == 1 && bound.minefield.ammo_large == 2);
        for (int i = 0; i < 14; ++i) {
            CHECK(bound.minefield.types[i] == i % 4 + 1);
            const auto p = w.minefields.point(bound, i);
            CHECK(p.x == (yaw == 90 ? 12 * 65536 + i * 32768 : 13 * 65536));
            CHECK(p.y == (yaw == 90 ? 33 * 65536 : 34 * 65536 + i * 32768));
            CHECK(p.z == 0); // terrain snap after the identity-bone model-global point
        }
        const auto rng = w.prng16_b_state;
        mission::resolve_minefields(w, definitions, models);
        CHECK(w.prng16_b_state == rng);
    }
}

// The think binds the ai_function's lndm event row by whole name, ignoring
// case; the render binds the render_function's lndm bone row by its first four
// characters, case kept.
// [orig: Entity_LookupRenderCallbacks stricmp @0x407dd8, row 'lndm' @0x813198;
//  EntityDef_InitAllCallbacks @0x4a5ab8..0x4a5af1 -> BoneCallback_LookupByTag
//  @0x4e32c6, row 'lndm' @0x82cfc0]
void binding_names() {
    struct Case {
        const char *ai_function;
        const char *render_function;
        bool think;
        bool render;
    };
    const Case cases[] = {
        {"lndmX", "lndmX", false, true},
        {"LNDM", "LNDM", true, false},
        {"lnd", "lnd", false, false},
    };
    ResourceIndex index;
    assets::AssetStore models{&index};
    for (const Case &c : cases) {
        def::DefItemDef definition{};
        definition.id = 101896;
        std::strcpy(definition.ai_function, c.ai_function);
        std::strcpy(definition.render_function, c.render_function);
        def::DefItemsFile definitions{};
        definitions.entries = &definition;
        definitions.count = 1;
        auto rig = std::make_unique<Rig>();
        auto &w = rig->world;
        Entity field;
        field.item_id = 1896;
        const auto handle = w.registry.spawn(2, field);
        mission::resolve_minefields(w, definitions, models);
        const auto &bound = *w.registry.get(handle);
        CHECK(bound.minefield.initialized == (c.think || c.render));
        CHECK(bound.minefield.think == c.think);
        CHECK(bound.minefield.render == c.render);
    }
}

} // namespace

int main() {
    vehicle_ground_query();
    buried_mine_blast();
    authored_binding();
    binding_names();
    contact_boundary_and_stances();
    carried_and_vehicle();
    peer_and_ceasefire();
    quantization_rng_and_reset();
    cadence_and_exhaustion();
    empty_slots_and_markers();
    return failures ? 1 : 0;
}
