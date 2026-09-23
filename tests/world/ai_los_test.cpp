// The AI line of sight against the retail exclusion set [orig:
// Entity_CheckMutualLineOfSight @0x539BE0 -> Physics_RaycastTerrainAndSectors
// @0x539910]: the walk skips the two endpoint entities, each one's parent
// slot (the +0x16C seat mount over the +0x268 carried object,
// @0x5399BD..0x539A12) and any candidate standing on an endpoint (+0x28,
// @0x53882F..0x538877); the terrain leg is skipped only when both ENDPOINTS
// are indoors (@0x53993F..0x53994C). Hand-built box volumes on a flat or
// ridged field.
#include <cstdio>
#include <memory>
#include <vector>

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// A 512x512 field at ground 0, optionally with a 3 u ridge across columns 18..22.
struct Field {
    enum { kDim = 512 };
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    opennova::terrain::TerrainHeightField field;
    explicit Field(bool ridge) : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        if (ridge)
            for (int z = 0; z < kDim; ++z)
                for (int x = 18; x <= 22; ++x)
                    heightmap[z * kDim + x] = static_cast<uint16_t>(3.0 * 256.0);
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// A solid (type-1) box [(-hx,-hy,0)..(hx,hy,height)] in section-local space:
// six outward Q14 planes, dist = -face offset.
CollisionModel box_model(double hx, double hy, double height) {
    CollisionModel m;
    const auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = fx(d);
        m.planes.push_back(p);
    };
    plane(16384, 0, 0, -hx);
    plane(-16384, 0, 0, -hx);
    plane(0, 16384, 0, -hy);
    plane(0, -16384, 0, -hy);
    plane(0, 0, 16384, -height);
    plane(0, 0, -16384, 0.0);
    CollisionVolume v;
    v.type = 1;
    v.min_x = fx(-hx);
    v.max_x = fx(hx);
    v.min_y = fx(-hy);
    v.max_y = fx(hy);
    v.min_z = 0;
    v.max_z = fx(height);
    v.plane_start = 0;
    v.plane_count = 6;
    m.volumes.push_back(v);
    CollisionSection s;
    s.volume_start = 0;
    s.volume_count = 1;
    m.sections.push_back(s);
    return m;
}

struct LosRig {
    World world;
    CollisionWorld cw;
    Field field;

    explicit LosRig(bool ridge) : field(ridge) {
        world.registry.configure_pool(0, 8);
        world.registry.configure_pool(1, 8);
        world.registry.configure_pool(2, 8);
        cw.terrain = &field.field;
        world.collision = &cw;
        world.ai.terrain = &field.field;
        world.ai.collision = &cw;
    }

    Entity &at(EntityHandle h) { return *world.registry.get(h); }

    // A pool-1 item (a hull or an emplaced gun) facing +X, with a box volume.
    EntityHandle spawn_item(double x, double y, double hx, double hy, double height) {
        Entity seed{};
        seed.kind = EntityKind::Item;
        seed.alive = true;
        seed.health = 100;
        seed.yaw = 90; // mission yaw 90 = engine heading 0 (identity rotation)
        seed.position = Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f};
        const EntityHandle h = world.registry.spawn(1, seed);
        cw.assign_entity(h, cw.add_model(box_model(hx, hy, height)));
        return h;
    }

    // A pool-0 person: no collision instance, never walked.
    EntityHandle spawn_person(double x, double y, double z) {
        Entity seed{};
        seed.kind = EntityKind::Organic;
        seed.alive = true;
        seed.health = 100;
        seed.position = Vec3{static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(z)};
        return world.registry.spawn(0, seed);
    }

    // Candidate tables rebuild on the 17th tick [orig: dword_B57C84 vs 0x10 @0x4C240F].
    void rebuild() {
        for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
    }

    bool clear(double ax, double bx, double z, EntityHandle from, EntityHandle to) {
        const int32_t a[3] = {fx(ax), 0, fx(z)};
        const int32_t b[3] = {fx(bx), 0, fx(z)};
        return world.ai.line_of_sight_clear(world, a, b, from, to);
    }
};

} // namespace

// R2-7: an emplaced child's parent slots are empty (it stands on the hull
// through +0x28, which only excludes candidates standing on an ENDPOINT), so
// the hull between a shooter and the child still blocks the ray.
static void test_hull_blocks_the_ray_to_its_emplaced_child() {
    auto rig = std::make_unique<LosRig>(false);
    const EntityHandle shooter = rig->spawn_person(0, 0, 0);
    const EntityHandle hull = rig->spawn_item(10, 0, 2, 2, 3); // x 8..12, z 0..3
    const EntityHandle child = rig->spawn_item(14, 0, 0.5, 0.5, 1);
    rig->at(child).ground_target = hull;
    rig->at(child).emplacement_parent = hull;
    rig->rebuild();
    CHECK(!rig->clear(0, 14, 1.5, shooter, child));
    CHECK(rig->clear(0, 14, 4.5, shooter, child)); // over the hull roof
}

// R2-7: a seated rider's parent slot is its hull, so the hull never occludes
// the rider's sight; the hull's other children stand on the HULL, not on an
// endpoint, and still block.
static void test_rider_sees_out_of_its_hull_but_not_through_its_children() {
    auto rig = std::make_unique<LosRig>(false);
    const EntityHandle hull = rig->spawn_item(10, 0, 2, 2, 3);
    const EntityHandle child = rig->spawn_item(16, 0, 1, 1, 3); // x 15..17
    rig->at(child).ground_target = hull;
    rig->at(child).emplacement_parent = hull;
    const EntityHandle rider = rig->spawn_person(10, 0, 1);
    rig->at(rider).mounted = true;
    rig->at(rider).mount_target = hull;
    const EntityHandle target = rig->spawn_person(30, 0, 0);
    rig->rebuild();
    CHECK(!rig->clear(10, 30, 1.5, rider, target));
    rig->at(child).position.y = 10.0f; // off the line of sight
    rig->rebuild();
    CHECK(rig->clear(10, 30, 1.5, rider, target));
}

// R2-7: the indoors skip reads the endpoints' own Flags, not their carriers':
// an indoors rider in an outdoors hull and an indoors target skip the terrain
// leg; an outdoors target brings the ridge back into the ray.
static void test_indoors_skip_reads_the_endpoints() {
    auto rig = std::make_unique<LosRig>(true);
    const EntityHandle hull = rig->spawn_item(10, 0, 2, 2, 3);
    const EntityHandle rider = rig->spawn_person(10, 0, 1);
    rig->at(rider).mounted = true;
    rig->at(rider).mount_target = hull;
    const EntityHandle target = rig->spawn_person(30, 0, 0);
    rig->rebuild();
    rig->at(rider).flags |= kEntityFlagIndoors;
    rig->at(target).flags |= kEntityFlagIndoors;
    CHECK(rig->clear(10, 30, 1.5, rider, target));
    rig->at(target).flags &= ~kEntityFlagIndoors;
    CHECK(!rig->clear(10, 30, 1.5, rider, target));
}

// R2-9: the script LOS pair takes the entity-aware walker (the first entity's
// proximity slice, every type admitted) for an authored range up to 20 u and
// the AI LOS above it, so a crate carrying Flags 0x8000000, which the AI LOS
// walk skips, blocks only the short-range queries.
// [orig: Entity_CheckLineOfSightInRange `cmp ecx,140000h; jg`
//  @0x4F1769..0x4F1773; Entity_CheckLineOfSight @0x4F1919..0x4F1953]
static void test_script_los_splits_at_twenty_units() {
    auto rig = std::make_unique<LosRig>(false);
    const EntityHandle watcher = rig->spawn_person(0, 0, 1);
    rig->at(watcher).item_id = 1001;
    rig->at(watcher).yaw = 90; // mission yaw 90 = engine heading 0: faces +X
    const EntityHandle crate = rig->spawn_item(3, 0, 1, 1, 3); // inside the watcher's slice
    rig->at(crate).engine_flags |= 0x8000000u;
    const EntityHandle target = rig->spawn_person(15, 0, 1);
    rig->at(target).item_id = 1001;
    rig->rebuild();
    EntityCommands &cmds = rig->world.commands;
    CHECK(!cmds.ssn_los_clear_within(watcher, target, 20 << 16));
    CHECK(cmds.ssn_los_clear_within(watcher, target, (20 << 16) + 1));
    CHECK(!cmds.ssn_sees_within(watcher, target, 20 << 16));
    CHECK(cmds.ssn_sees_within(watcher, target, (20 << 16) + 1));
}

int main() {
    test_hull_blocks_the_ray_to_its_emplaced_child();
    test_rider_sees_out_of_its_hull_but_not_through_its_children();
    test_indoors_skip_reads_the_endpoints();
    test_script_los_splits_at_twenty_units();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai_los: all passed\n");
    return 0;
}
