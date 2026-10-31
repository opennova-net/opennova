// D-COL-17: the clamped ground query's pools-2/1 face walk
// (CollisionWorld::minefield_ground). Its ray state carries no direction (the
// memset's zeros) and the query's Z as its length, and its back-face argument
// is 0. So:
// - a slot or entity gate measures the start point against the candidate's
//   sphere;
// - a face's in-triangle test reads the local start point;
// - a face without flag 1 is taken entering its front alone.
// [orig: Entity_ComputeClampedDisplacement @0x4AD6A0 - the memset @0x4AD6B6,
//  `xor ebp, ebp` @0x4AD6E5; Projectile_RaycastProximitySlots @0x4E5340 - the
//  slot gate @0x4E548F..0x4E554A; Physics_RaycastAgainstBoneCollision @0x4E4CB0
//  - the entity gate @0x4E4DE8..0x4E4EB4, the direction rule
//  @0x4E511D..0x4E514A, the test point @0x4E51E4..0x4E522A]

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

int32_t fx(double v) { return static_cast<int32_t>(std::lround(v * 65536.0)); }

// One floor triangle in the local Z=0 plane, its Q14 normal +Z (front up) or
// -Z (front down).
CollisionModel floor_model(bool front_up, uint32_t material_flags = 0) {
    CollisionModel floor;
    for (const auto &xy : {std::pair<int, int>{-2, -2}, {2, -2}, {0, 2}}) {
        CollisionVertex vertex;
        vertex.p[0] = xy.first * 65536;
        vertex.p[1] = xy.second * 65536;
        floor.vertices.push_back(vertex);
    }
    CollisionNormal normal;
    normal.n[2] = front_up ? 16384 : -16384;
    normal.dominant_axis = 1;
    floor.normals.push_back(normal);
    CollisionFace face;
    face.normal_index = 0;
    face.vertex_index[0] = 0;
    face.vertex_index[1] = 1;
    face.vertex_index[2] = 2;
    face.min[0] = face.min[1] = -2 * 65536;
    face.max[0] = face.max[1] = 2 * 65536;
    face.material_flags = material_flags;
    floor.faces.push_back(face);
    CollisionSection section;
    section.vertex_count = 3;
    section.normal_count = 1;
    section.face_count = 1;
    floor.sections.push_back(section);
    return floor;
}

// A steep face (normal (0.8, 0, 0.6), dominant axis X, so its in-triangle test
// projects onto Y / Z) through (0, 0, 2), the triangle spanning Z 1..3 about the
// local origin's vertical.
CollisionModel ramp_model() {
    CollisionModel ramp;
    const double corners[3][3] = {{0.75, -1.0, 1.0}, {0.75, 1.0, 1.0}, {-0.75, 0.0, 3.0}};
    for (const auto &c : corners) {
        CollisionVertex vertex;
        for (int k = 0; k < 3; ++k) vertex.p[k] = fx(c[k]);
        ramp.vertices.push_back(vertex);
    }
    CollisionNormal normal;
    normal.n[0] = 13107;
    normal.n[2] = 9830;
    normal.dominant_axis = 4;
    ramp.normals.push_back(normal);
    CollisionFace face;
    face.normal_index = 0;
    face.vertex_index[0] = 0;
    face.vertex_index[1] = 1;
    face.vertex_index[2] = 2;
    face.min[0] = fx(-0.75);
    face.max[0] = fx(0.75);
    face.min[1] = fx(-1.0);
    face.max[1] = fx(1.0);
    face.min[2] = fx(1.0);
    face.max[2] = fx(3.0);
    face.plane_dist = fx(-1.2);
    ramp.faces.push_back(face);
    CollisionSection section;
    section.vertex_count = 3;
    section.normal_count = 1;
    section.face_count = 1;
    ramp.sections.push_back(section);
    return ramp;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    bool wp_jo = test_titles::jo_world(*wp);
    World &w = *wp;
    std::unique_ptr<CollisionWorld> cwp = std::make_unique<CollisionWorld>();
    CollisionWorld &cw = *cwp;
    Rig() {
        w.registry.configure_pool(1, 8);
        w.collision = &cw;
    }
    EntityHandle add(CollisionModel model, Vec3 at, float bound_radius) {
        Entity e;
        e.kind = EntityKind::Item;
        e.item_id = 42;
        e.heading = placed_heading_bam(90);
        e.position = at;
        e.bound_radius = bound_radius;
        const EntityHandle h = w.registry.spawn(1, e);
        cw.assign_entity(h, cw.add_model(std::move(model)));
        cw.build_tick_tables(w);
        return h;
    }
};

// The back-face argument 0: a floor whose front faces down is reached from
// behind by the downward query and gives no height; flag 1 takes it either way.
void test_a_floor_reached_from_behind_gives_no_height() {
    {
        Rig r;
        r.add(floor_model(true), {0.0f, 0.0f, 2.0f}, 4.0f);
        CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(4.0)}, true) == fx(2.0));
    }
    {
        Rig r;
        r.add(floor_model(false), {0.0f, 0.0f, 2.0f}, 4.0f);
        CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(4.0)}, true) == fx(4.0));
    }
    {
        Rig r;
        r.add(floor_model(false, 0x1u), {0.0f, 0.0f, 2.0f}, 4.0f);
        CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(4.0)}, true) == fx(2.0));
    }
}

// The gates measure the start point: a deck whose sphere the query point lies
// outside gives no height, though the downward segment crosses the sphere and
// the face; a query at or below Z 0 finds no face.
void test_the_gates_measure_the_start_point() {
    Rig r;
    r.add(floor_model(true), {0.0f, 0.0f, 2.0f}, 4.0f);
    CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(5.5)}, true) == fx(2.0));
    CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(10.0)}, true) == fx(10.0));
    CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(-1.0)}, true) == fx(-1.0));
}

// The in-triangle test reads the local start point: the steep face's crossing
// at Z 2 lies inside its triangle, the start point's Y / Z projection (Z 10)
// does not, so the query gives no height.
void test_the_triangle_test_reads_the_start_point() {
    Rig r;
    r.add(ramp_model(), {0.0f, 0.0f, 0.0f}, 20.0f);
    CHECK(r.cw.minefield_ground(r.w, {}, {0, 0, fx(10.0)}, true) == fx(10.0));
}

} // namespace

int main() {
    test_a_floor_reached_from_behind_gives_no_height();
    test_the_gates_measure_the_start_point();
    test_the_triangle_test_reads_the_start_point();
    if (failures == 0) std::puts("clamped_ground_query: OK");
    return failures == 0 ? 0 : 1;
}
