// Rendering-occlusion unit tests [orig: the Jointops.exe consumer set —
// Terrain_InitBuildingPortals @0x5c7480 (register/weld/flags),
// Terrain_BuildSectorVisibilityMasks @0x5c8610, Render_VisibilityPortalTraversal
// @0x5c4ae0, Terrain_TestSectorEntityOcclusion @0x5c4610, the entity-collector gates
// @0x5c6f20/0x5c8c60, Terrain_OcclusionCheckThreeRays @0x610ed0] over
// hand-built occlusion + collision models (docs/render/render-occlusion-re.md).
//
// Authoring convention used by the fixtures (derived from the witnessed side
// tests, D-OCC-3 tracks the exporter-side semantics): a record's plane normal
// points from section_a toward section_b; windows are (A = interior section,
// B = 0/exterior) with exterior-ward normals.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <map>
#include <memory>
#include <string>

#include <formats/mission/bms.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/occlusion_camera.h>
#include <runtime/world/occlusion_feed.h>
#include <runtime/world/world.h>

#include "common/boot_file_source.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// Flat height field at a constant raw16 column (raw = units * 256).
struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    explicit Field(uint16_t raw16) : heightmap(kDim * kDim, raw16), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// Mission -> 3DI float model space: model = (-dy, dz, dx) of the mission-local
// offset (the render_float_from_fixed swizzle without the /65536).
void model_from_mission(double dx, double dy, double dz, float out[3]) {
    out[0] = static_cast<float>(-dy);
    out[1] = static_cast<float>(dz);
    out[2] = static_cast<float>(dx);
}

struct QuadSpec {
    uint8_t type = kOccRecWindow;
    uint8_t section_a = 1;
    uint8_t section_b = 0;
    // Quad corners in mission-local offsets, wound consistently; the plane
    // normal (mission) points a->b.
    double corners[4][3];
    double normal[3];
    float priority = 0.0f;
};

// Build an OcclusionModel from quad records (two tris per quad, shared-diagonal
// edge cancellation, winding bit on the second tri's diagonal word).
OcclusionModel occ_model(const std::vector<QuadSpec> &quads) {
    OcclusionModel m;
    for (const QuadSpec &q : quads) {
        OcclusionPortalFace rec;
        rec.type = q.type;
        rec.section_a = q.section_a;
        rec.section_b = q.section_b;
        rec.vert_start = static_cast<int32_t>(m.vertices.size());
        rec.vert_count = 4;
        rec.plane_start = static_cast<int32_t>(m.planes.size());
        rec.plane_count = 1;
        rec.face_start = static_cast<int32_t>(m.faces.size());
        rec.face_count = 2;
        rec.slot_priority_scale = q.priority;

        float center[3] = {0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            OcclusionVertex v;
            model_from_mission(q.corners[i][0], q.corners[i][1], q.corners[i][2], v.p);
            for (int c = 0; c < 3; ++c) center[c] += v.p[c] * 0.25f;
            m.vertices.push_back(v);
        }
        rec.pos[0] = center[0];
        rec.pos[1] = center[1];
        rec.pos[2] = center[2];
        float ext = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const OcclusionVertex &v = m.vertices[rec.vert_start + i];
            const float dx = v.p[0] - center[0], dy = v.p[1] - center[1],
                        dz = v.p[2] - center[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > ext) ext = d;
        }
        rec.radius = ext;

        OcclusionPlane pl;
        model_from_mission(q.normal[0], q.normal[1], q.normal[2], pl.normal);
        pl.d = -(pl.normal[0] * m.vertices[rec.vert_start].p[0] +
                 pl.normal[1] * m.vertices[rec.vert_start].p[1] +
                 pl.normal[2] * m.vertices[rec.vert_start].p[2]);
        m.planes.push_back(pl);

        // Tri 0 = {0,1,2}, tri 1 = {0,2,3}; edge word = canonical (lo | hi<<8)
        // with bit 15 = "this face traverses the edge against canonical order"
        // — shared edges carry the same low-15 word and cancel; the winding bit
        // keeps the wedge cross products consistently oriented.
        auto edge = [](int a, int b) {
            const int lo = a < b ? a : b;
            const int hi = a < b ? b : a;
            return static_cast<uint16_t>((lo & 0xFF) | ((hi & 0x7F) << 8) | (a > b ? 0x8000 : 0));
        };
        OcclusionFaceRec f0;
        f0.v[0] = 0;
        f0.v[1] = 1;
        f0.v[2] = 2;
        f0.plane = 0;
        f0.edge[0] = edge(0, 1);
        f0.edge[1] = edge(1, 2);
        f0.edge[2] = edge(2, 0);
        m.faces.push_back(f0);
        OcclusionFaceRec f1;
        f1.v[0] = 0;
        f1.v[1] = 2;
        f1.v[2] = 3;
        f1.plane = 0;
        f1.edge[0] = edge(0, 2);
        f1.edge[1] = edge(2, 3);
        f1.edge[2] = edge(3, 0);
        m.faces.push_back(f1);
        m.records.push_back(rec);
    }
    return m;
}

// A closed axis box occluder record (type 0/1): 8 verts, 6 planes, 12 tris.
// Mission-local box [min .. max].
OcclusionModel box_occluder(uint8_t type, const double mn[3], const double mx[3]) {
    OcclusionModel m;
    OcclusionPortalFace rec;
    rec.type = type;
    rec.section_a = 0;
    rec.section_b = 0;
    rec.vert_start = 0;
    rec.vert_count = 8;
    rec.plane_start = 0;
    rec.plane_count = 6;
    rec.face_start = 0;
    rec.face_count = 12;

    double c[3] = {(mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5, (mn[2] + mx[2]) * 0.5};
    model_from_mission(c[0], c[1], c[2], rec.pos);
    const double hx = (mx[0] - mn[0]) * 0.5, hy = (mx[1] - mn[1]) * 0.5,
                 hz = (mx[2] - mn[2]) * 0.5;
    rec.radius = static_cast<float>(std::sqrt(hx * hx + hy * hy + hz * hz));

    // Vertex i: bit0 -> max x, bit1 -> max y, bit2 -> max z (mission).
    for (int i = 0; i < 8; ++i) {
        OcclusionVertex v;
        model_from_mission((i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1],
                           (i & 4) ? mx[2] : mn[2], v.p);
        m.vertices.push_back(v);
    }
    // Outward planes: -x +x -y +y -z +z (mission).
    const double n[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    const double off[6] = {mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]};
    for (int p = 0; p < 6; ++p) {
        OcclusionPlane pl;
        model_from_mission(n[p][0], n[p][1], n[p][2], pl.normal);
        // d so that dot(point_on_face, n) + d = 0
        pl.d = static_cast<float>(-(n[p][0] * ((p == 1) ? mx[0] : (p == 0 ? mn[0] : c[0])) +
                                    n[p][1] * ((p == 3) ? mx[1] : (p == 2 ? mn[1] : c[1])) +
                                    n[p][2] * ((p == 5) ? mx[2] : (p == 4 ? mn[2] : c[2]))));
        (void)off;
        m.planes.push_back(pl);
    }
    // Faces per box side: the quad traversed CCW as seen from OUTSIDE (mission
    // axes), split into 2 tris; edge words canonical (lo | hi<<8) with bit 15 =
    // traversal-against-canonical, so shared edges cancel across the hull and
    // the silhouette wedge planes orient consistently.
    const int sides[6][4] = {
        {0, 4, 6, 2}, // -x
        {1, 3, 7, 5}, // +x
        {0, 1, 5, 4}, // -y
        {2, 6, 7, 3}, // +y
        {0, 2, 3, 1}, // -z
        {4, 5, 7, 6}, // +z
    };
    auto edge = [](int a, int b) {
        const int lo = a < b ? a : b;
        const int hi = a < b ? b : a;
        return static_cast<uint16_t>((lo & 0xFF) | ((hi & 0x7F) << 8) | (a > b ? 0x8000 : 0));
    };
    for (int s = 0; s < 6; ++s) {
        const int *q = sides[s];
        OcclusionFaceRec f0;
        f0.v[0] = static_cast<uint8_t>(q[0]);
        f0.v[1] = static_cast<uint8_t>(q[1]);
        f0.v[2] = static_cast<uint8_t>(q[2]);
        f0.plane = static_cast<uint8_t>(s);
        f0.edge[0] = edge(q[0], q[1]);
        f0.edge[1] = edge(q[1], q[2]);
        f0.edge[2] = edge(q[2], q[0]);
        m.faces.push_back(f0);
        OcclusionFaceRec f1;
        f1.v[0] = static_cast<uint8_t>(q[0]);
        f1.v[1] = static_cast<uint8_t>(q[2]);
        f1.v[2] = static_cast<uint8_t>(q[3]);
        f1.plane = static_cast<uint8_t>(s);
        f1.edge[0] = edge(q[0], q[2]);
        f1.edge[1] = edge(q[2], q[3]);
        f1.edge[2] = edge(q[3], q[0]);
        m.faces.push_back(f1);
    }
    m.records.push_back(rec);
    return m;
}

// Collision model: section 0 = exterior shell (type-1 solid), section 1 = the
// interior blink box (type 8, V-letter flags = 4 so accum ^6 sets bit 2).
CollisionModel building_collision(double hx, double hy, double height) {
    CollisionModel m;
    auto box = [&](int32_t type, uint32_t flags, double bx, double by, double bz0, double bz1) {
        const int32_t plane_start = static_cast<int32_t>(m.planes.size());
        auto plane = [&](int nx, int ny, int nz, double d) {
            CollisionPlane p;
            p.nx = static_cast<int16_t>(nx);
            p.ny = static_cast<int16_t>(ny);
            p.nz = static_cast<int16_t>(nz);
            p.dist = fx(d);
            m.planes.push_back(p);
        };
        plane(16384, 0, 0, -bx);
        plane(-16384, 0, 0, -bx);
        plane(0, 16384, 0, -by);
        plane(0, -16384, 0, -by);
        plane(0, 0, 16384, -bz1);
        plane(0, 0, -16384, bz0);
        CollisionVolume v;
        v.type = type;
        v.flags = flags;
        v.min_x = fx(-bx);
        v.max_x = fx(bx);
        v.min_y = fx(-by);
        v.max_y = fx(by);
        v.min_z = fx(bz0);
        v.max_z = fx(bz1);
        v.plane_start = plane_start;
        v.plane_count = 6;
        m.volumes.push_back(v);
    };
    // Section 0: exterior solid shell.
    box(1, 0, hx, hy, 0.0, height);
    CollisionSection s0;
    s0.volume_start = 0;
    s0.volume_count = 1;
    m.sections.push_back(s0);
    // Section 1: interior blink volume (slightly inset).
    box(8, 4, hx - 0.25, hy - 0.25, 0.0, height - 0.25);
    CollisionSection s1;
    s1.volume_start = 1;
    s1.volume_count = 1;
    m.sections.push_back(s1);
    m.finalize_sections();
    return m;
}

struct Rig {
    World world;
    CollisionWorld cw;
    OcclusionWorld ow;
    Field field{0};

    Rig() {
        world.registry.configure_pool(0, 8);
        world.registry.configure_pool(2, 16);
        cw.terrain = &field.field;
        // Slot 0 packs blink hits to a zero low word; keep it occupied like
        // the collision tests (the original shares the ambiguity).
        Entity filler;
        filler.kind = EntityKind::Marker;
        filler.net_id = 99;
        world.registry.spawn(2, filler);
    }

    uint16_t next_net_id = 100;

    EntityHandle add_building(double x, double y, CollisionModel cm, OcclusionModel om,
                              OcclusionWorld::EntityDefBits bits = {}) {
        Entity b;
        b.kind = EntityKind::Building;
        b.net_id = next_net_id++;
        b.position = {static_cast<float>(x), static_cast<float>(y), 0.0f};
        b.yaw = 90; // mission yaw 90 -> engine heading 0 (identity rotation)
        b.alive = true;
        const EntityHandle h = world.registry.spawn(2, b);
        const int32_t cid = cw.add_model(std::move(cm));
        cw.assign_entity(h, cid);
        if (!om.records.empty()) {
            const int32_t oid = ow.add_model(std::move(om));
            ow.assign_entity(h, oid, bits);
        }
        return h;
    }

    void rebuild() {
        for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
    }

    // Frame camera at a mission position looking along +-X (mission), fog 500 u.
    OcclusionFrameCamera camera(double x, double y, double z, uint32_t blink_flags = 0,
                                int look_x = +1) const {
        OcclusionFrameCamera cam;
        cam.pos_fixed[0] = fx(x);
        cam.pos_fixed[1] = fx(y);
        cam.pos_fixed[2] = fx(z);
        render_float_from_fixed(cam.pos_fixed, cam.pos_float);
        // Wide-open frustum: a single near plane 0.05 u behind the camera,
        // facing mission +-X (mission (1,0,0) -> render (0,0,1) swizzle).
        const float dir = look_x >= 0 ? 1.0f : -1.0f;
        cam.frustum_count = 1;
        cam.frustum[0][0] = 0.0f;
        cam.frustum[0][1] = 0.0f;
        cam.frustum[0][2] = dir;
        cam.frustum[0][3] = -dir * cam.pos_float[2] + 0.05f;
        // View rows (mission axes, Q22): forward = +-X, lateral rows +-Y / +Z.
        cam.view_rows_q22[0][0] = look_x >= 0 ? (1 << 22) : -(1 << 22);
        cam.view_rows_q22[0][1] = 0;
        cam.view_rows_q22[0][2] = 0;
        cam.view_rows_q22[1][0] = 0;
        cam.view_rows_q22[1][1] = look_x >= 0 ? (1 << 22) : -(1 << 22);
        cam.view_rows_q22[1][2] = 0;
        cam.view_rows_q22[2][0] = 0;
        cam.view_rows_q22[2][1] = 0;
        cam.view_rows_q22[2][2] = 1 << 22;
        // A 640-wide viewport at a 77.3 deg horizontal fov: the person leg's
        // sub-pixel floor projects radius * 400 / depth.
        cam.focal_pixels = 400;
        cam.fog_dist = fx(500.0);
        cam.water_z = fx(-100.0);
        cam.local_blink_flags = blink_flags;
        return cam;
    }
};

// A one-room building at (bx, by): 4x4 footprint, window on the +X wall
// (mission plane x = bx+2), record sections (A=1 interior, B=0 exterior),
// normal exterior-ward (+X).
OcclusionModel one_room_window(uint8_t type = kOccRecWindow) {
    QuadSpec q;
    q.type = type;
    q.section_a = 1;
    q.section_b = 0;
    const double c[4][3] = {{2.0, -1.0, 1.0}, {2.0, 1.0, 1.0}, {2.0, 1.0, 2.5}, {2.0, -1.0, 2.5}};
    std::memcpy(q.corners, c, sizeof(c));
    q.normal[0] = 1.0;
    q.normal[1] = 0.0;
    q.normal[2] = 0.0;
    return occ_model({q});
}

// ---------------------------------------------------------------------------
// The native visibility bound sphere. [orig: Entity_ComputeBoundingSphere
// @ 0x5c69a0 — the odd-width midpoint keeps the positive-side half (radius 7,
// not 5, for these bounds: halves {4,5,3}), a nonzero scale multiplies center
// and radius with the +0x8000 rule (@ 0x5c6ac8..0x5c6b52), and the unset-bound
// sentinels clamp to a zero center with 0x40000000 halves (@ 0x5c6a02..0x5c6a39)]
void test_bound_sphere() {
    CollisionModel cm;
    cm.min[0] = -3; cm.min[1] = -2; cm.min[2] = 10;
    cm.max[0] = 4;  cm.max[1] = 7;  cm.max[2] = 15;
    int32_t c[3] = {0, 0, 0};
    int32_t r = 0;
    OcclusionWorld::bound_sphere_fixed(cm, c, r, 0);
    CHECK(c[0] == 0 && c[1] == 2 && c[2] == 12);
    CHECK(r == 7);
    OcclusionWorld::bound_sphere_fixed(cm, c, r, 0x18000); // 1.5x
    CHECK(c[0] == 0 && c[1] == 3 && c[2] == 18);
    CHECK(r == 11);

    CollisionModel unset;
    for (int axis = 0; axis < 3; ++axis) {
        unset.min[axis] = 0x40000000;
        unset.max[axis] = -0x40000000;
    }
    OcclusionWorld::bound_sphere_fixed(unset, c, r, 0);
    CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0);
    CHECK(r == 1859775393); // sqrt(3) * 2^30, truncated
}

void test_render_math() {
    // The fixed->float swizzle. [orig: Math_FixedPointToFloat3_YNegated @ 0x611210]
    const int32_t p[3] = {fx(3.0), fx(5.0), fx(7.0)};
    float f[3];
    render_float_from_fixed(p, f);
    CHECK(std::fabs(f[0] - (-5.0f)) < 1e-4f);
    CHECK(std::fabs(f[1] - 7.0f) < 1e-4f);
    CHECK(std::fabs(f[2] - 3.0f) < 1e-4f);

    // Identity pose = pure swizzled translation. [orig: @ 0x612200]
    const RenderMatrix m = render_matrix_from_pose(p, 0, 0, 0);
    const float local[3] = {1.0f, 2.0f, 3.0f};
    float w[3];
    m.transform_point(local, w);
    CHECK(std::fabs(w[0] - (1.0f - 5.0f)) < 1e-4f);
    CHECK(std::fabs(w[1] - (2.0f + 7.0f)) < 1e-4f);
    CHECK(std::fabs(w[2] - (3.0f + 3.0f)) < 1e-4f);

    // Quarter-turn yaw: quantized trig lands within a Q22 step of exact.
    const int32_t zero[3] = {0, 0, 0};
    const RenderMatrix q = render_matrix_from_pose(zero, 0x40000000, 0, 0);
    const float x_axis[3] = {1.0f, 0.0f, 0.0f};
    float r[3];
    q.rotate_vector(x_axis, r);
    // Rotation about float Y: X -> -sin quantized on Z... assert the magnitudes.
    CHECK(std::fabs(std::fabs(r[2]) - 1.0f) < 1e-3f);
    CHECK(std::fabs(r[0]) < 1e-3f);
    CHECK(std::fabs(r[1]) < 1e-6f);

    // The entity wrapper must retain all three authored angles. These are the
    // live fields consumed by every float-space portal and TOC transform.
    Entity e;
    e.position = {3.0f, 5.0f, 7.0f};
    e.yaw = 90;
    e.pitch = 45;
    e.roll = -30;
    const RenderMatrix from_entity = render_matrix_from_entity_pose(e);
    const RenderMatrix expected =
        render_matrix_from_pose(p, 0, static_cast<int32_t>(0x20000000u),
                                static_cast<int32_t>(0xEAAAAAAAu));
    for (int i = 0; i < 16; ++i)
        CHECK(std::fabs(from_entity.m[i] - expected.m[i]) < 1e-6f);
}

// ---------------------------------------------------------------------------
void test_weld_and_flags() {
    Rig rig;
    // Two identical buildings sharing a wall plane: A at (10, 10), B at
    // (14.5, 10) — A's +X window at x = 12, B's -X window at x = 12.5
    // (0.5 u apart, opposite normals, coplanar within 0.2? plane gap 0.5 --
    // the coplanarity term measures along A's normal: |dot(nA, pB - pA)| =
    // 0.5 > 0.2 fails!). Put B at 14.1 so the gap is 0.1 u.
    OcclusionModel occ_a = one_room_window();
    // An authored window in section 2 lies on the welded face's plane. A's
    // def carries the recurse-windows bit, so the welded type-5 face ALSO
    // recurses into its far section (the exterior), and that walk reaches the
    // section-2 window: bit 2 enters A's mask.
    QuadSpec hidden;
    hidden.type = kOccRecWindow;
    hidden.section_a = 2;
    hidden.section_b = 0;
    const double ch[4][3] = {
        {2.0, -1.0, 1.0}, {2.0, 1.0, 1.0}, {2.0, 1.0, 2.5}, {2.0, -1.0, 2.5}};
    std::memcpy(hidden.corners, ch, sizeof(ch));
    hidden.normal[0] = -1.0;
    OcclusionModel hidden_model = occ_model({hidden});
    {
        const int32_t vbase = static_cast<int32_t>(occ_a.vertices.size());
        const int32_t pbase = static_cast<int32_t>(occ_a.planes.size());
        const int32_t fbase = static_cast<int32_t>(occ_a.faces.size());
        occ_a.vertices.insert(occ_a.vertices.end(), hidden_model.vertices.begin(),
                              hidden_model.vertices.end());
        occ_a.planes.insert(occ_a.planes.end(), hidden_model.planes.begin(),
                            hidden_model.planes.end());
        occ_a.faces.insert(occ_a.faces.end(), hidden_model.faces.begin(),
                           hidden_model.faces.end());
        OcclusionPortalFace rec = hidden_model.records[0];
        rec.vert_start += vbase;
        rec.plane_start += pbase;
        rec.face_start += fbase;
        occ_a.records.push_back(rec);
    }
    QuadSpec qb;
    qb.type = kOccRecWindow;
    qb.section_a = 33; // x86 shifts mask the authored ordinal to bit 1
    qb.section_b = 0;
    const double cb[4][3] = {
        {-2.0, -1.0, 1.0}, {-2.0, 1.0, 1.0}, {-2.0, 1.0, 2.5}, {-2.0, -1.0, 2.5}};
    std::memcpy(qb.corners, cb, sizeof(cb));
    qb.normal[0] = -1.0;
    qb.normal[1] = 0.0;
    qb.normal[2] = 0.0;
    OcclusionModel occ_b = occ_model({qb});

    OcclusionWorld::EntityDefBits recurse_weldable;
    recurse_weldable.weldable = true;
    recurse_weldable.recurse_windows = true;
    OcclusionWorld::EntityDefBits weldable;
    weldable.weldable = true;
    const EntityHandle a =
        rig.add_building(10.0, 10.0, building_collision(2, 2, 3), std::move(occ_a),
                         recurse_weldable);
    const EntityHandle b =
        rig.add_building(14.1, 10.0, building_collision(2, 2, 3), std::move(occ_b), weldable);
    rig.rebuild();

    rig.ow.init_mission(rig.world, rig.cw);
    CHECK(rig.ow.weld_records().size() == 2);
    if (rig.ow.weld_records().size() == 2) {
        const OcclusionWorld::WeldRecord &w0 = rig.ow.weld_records()[0];
        const OcclusionWorld::WeldRecord &w1 = rig.ow.weld_records()[1];
        CHECK(w0.own_entity == a || w0.own_entity == b);
        CHECK(w0.other_entity == w1.own_entity);
        CHECK(w1.other_entity == w0.own_entity);
        const OcclusionWorld::WeldRecord &wa = w0.own_entity == a ? w0 : w1;
        const OcclusionWorld::WeldRecord &wb = w0.own_entity == b ? w0 : w1;
        CHECK(wa.own_section == 1 && wa.other_section == 33);
        CHECK(wb.own_section == 33 && wb.other_section == 1);
    }
    // The welded records rewrote to type 5. A retains its separate section-2
    // window; B has links only.
    const OcclusionWorld::BuildingFlags fa = rig.ow.building_flags(a);
    const OcclusionWorld::BuildingFlags fb = rig.ow.building_flags(b);
    CHECK(fa.has_links);
    CHECK(fa.has_windows);
    CHECK(!fa.has_open);
    CHECK(fb.has_links);
    CHECK(!fb.has_windows);

    const OcclusionFrameCamera cam = rig.camera(10.0, 10.0, 1.0, 0x2);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    // [orig: Render_VisibilityPortalTraversal — the type-5 track leg's
    // `jmp loc_5C560C` @ 0x5c5519 joins the recurse-windows gate @ 0x5c560c
    // and the recursion @ 0x5c5619]
    CHECK((rig.ow.section_mask(a) & (1u << 2)) != 0);
    CHECK((rig.ow.section_mask(b) & (1u << 1)) != 0);

    // Distance gate: a third building far away does not weld.
    Rig rig2;
    OcclusionWorld::EntityDefBits wb2;
    wb2.weldable = true;
    rig2.add_building(10.0, 10.0, building_collision(2, 2, 3), one_room_window(), wb2);
    rig2.add_building(30.0, 10.0, building_collision(2, 2, 3), one_room_window(), wb2);
    rig2.rebuild();
    rig2.ow.init_mission(rig2.world, rig2.cw);
    CHECK(rig2.ow.weld_records().empty());
    CHECK(rig2.ow.building_flags(EntityHandle{}).has_links == false);
}

// ---------------------------------------------------------------------------
void test_tilted_pose_weld() {
    Rig rig;
    OcclusionWorld::EntityDefBits weldable;
    weldable.weldable = true;
    const EntityHandle a =
        rig.add_building(10.0, 10.0, building_collision(2, 2, 3), one_room_window(), weldable);
    const EntityHandle b =
        rig.add_building(14.0, 10.0, building_collision(2, 2, 3), one_room_window(), weldable);
    // A 180-degree authored pitch reverses B's +X face. Raising its origin by
    // 3.5u mirrors the 1..2.5u vertex range back onto A's face, so the two
    // records are coincident and opposite only when the full pose is honored.
    Entity *tilted = rig.world.registry.get(b);
    CHECK(tilted != nullptr);
    if (tilted != nullptr) {
        tilted->position.z = 3.5f;
        tilted->pitch = 180;
    }
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    CHECK(rig.ow.weld_records().size() == 2);
    if (rig.ow.weld_records().size() == 2) {
        CHECK(rig.ow.weld_records()[0].own_entity == a ||
              rig.ow.weld_records()[1].own_entity == a);
        CHECK(rig.ow.weld_records()[0].own_entity == b ||
              rig.ow.weld_records()[1].own_entity == b);
    }
}

// ---------------------------------------------------------------------------
void test_outdoor_masks() {
    Rig rig;
    // Windowed building + a windowless building.
    const EntityHandle windowed =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    const EntityHandle bare =
        rig.add_building(20.0, 30.0, building_collision(2, 2, 3), OcclusionModel{});
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // Camera outdoors far from both, outdoors blink flags (bit 2 clear).
    const OcclusionFrameCamera cam = rig.camera(5.0, 20.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    CHECK(!rig.ow.camera_indoors());
    CHECK(rig.ow.exterior_visible());
    CHECK(rig.ow.water_visible()); // outdoors -> water override on
    CHECK(rig.ow.building_visible(windowed));
    CHECK(rig.ow.building_visible(bare));
    // Windowed building renders everything; the bare one exterior-only.
    // [orig: the outdoor rule @ 0x5c87d9-0x5c8825]
    CHECK(rig.ow.section_mask(windowed) == 0xFFFFFFFu);
    CHECK(rig.ow.section_mask(bare) == 1u);
}

// ---------------------------------------------------------------------------
void test_negative_static_slot_fog_collection() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(-20.0, -10.0, building_collision(2, 2, 3), OcclusionModel{});
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // Static proximity coordinates are stored as u16 words. The building is
    // five units ahead at a negative mission coordinate; signed decoding must
    // keep it inside both this tight fog gate and the +X camera frustum.
    Entity *entity = rig.world.registry.get(building);
    CHECK(entity != nullptr);
    if (entity != nullptr) entity->occlusion_latch = 1;
    OcclusionFrameCamera cam = rig.camera(-25.0, -10.0, 1.5);
    cam.fog_dist = fx(10.0);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    CHECK(rig.ow.building_batched(building));
    CHECK(rig.ow.building_visible(building));
}

// ---------------------------------------------------------------------------
// The static pose memo behind collect_buildings caches only pure derivations
// (the placed bound sphere, the record world positions) behind value keys:
// a steady frame reproduces the identical admission, and a changed pose (the
// guard for a husk swap or any future mover) recomputes rather than reading
// a stale center. The latch is held high so the PRNG stream stays untouched.
void test_static_pose_memo_recomputes_on_pose_change() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), OcclusionModel{});
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    Entity *entity = rig.world.registry.get(building);
    CHECK(entity != nullptr);
    if (entity == nullptr) return;
    entity->occlusion_latch = 200;
    const OcclusionFrameCamera cam = rig.camera(15.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.building_batched(building));
    // Steady frame: the memo hit reproduces the same admission.
    entity->occlusion_latch = 200;
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.building_batched(building));
    // The static proximity slot keeps its load-time coords, so only the
    // memoized placed sphere sees the move: a stale memo (center still ahead
    // at x=20) would keep the building batched, the recompute must drop it
    // behind the camera's near plane (camera x=15 looking +X).
    entity->position = {5.0f, 10.0f, 0.0f};
    entity->occlusion_latch = 200;
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(!rig.ow.building_batched(building));
    // And back: the guard re-admits at the original pose.
    entity->position = {20.0f, 10.0f, 0.0f};
    entity->occlusion_latch = 200;
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.building_batched(building));
}

// ---------------------------------------------------------------------------
// The results-identical pin for the pose memo: two identical scenes, one
// with the memo off (every derivation recomputed), driven through the same
// frames — moves, a return, a steady hold — must batch the same buildings.
// The latch is held high so the PRNG stream is not the oracle.
void test_static_pose_memo_matches_the_unmemoized_walk() {
    Rig memo_rig;
    Rig plain_rig;
    plain_rig.ow.set_static_pose_memo_enabled(false);
    std::vector<EntityHandle> memo_buildings, plain_buildings;
    const double xs[] = {20.0, 26.0, 33.0, 40.0};
    const double ys[] = {10.0, 14.0, 6.0, 10.0};
    for (int k = 0; k < 4; ++k) {
        memo_buildings.push_back(memo_rig.add_building(
            xs[k], ys[k], building_collision(2, 2, 3), OcclusionModel{}));
        plain_buildings.push_back(plain_rig.add_building(
            xs[k], ys[k], building_collision(2, 2, 3), OcclusionModel{}));
    }
    memo_rig.rebuild();
    plain_rig.rebuild();
    memo_rig.ow.init_mission(memo_rig.world, memo_rig.cw);
    plain_rig.ow.init_mission(plain_rig.world, plain_rig.cw);
    const OcclusionFrameCamera cam = memo_rig.camera(15.0, 10.0, 1.5);
    auto hold_latches = [&](Rig &rig, const std::vector<EntityHandle> &hs) {
        for (EntityHandle h : hs) {
            Entity *entity = rig.world.registry.get(h);
            if (entity != nullptr) entity->occlusion_latch = 200;
        }
    };
    auto move = [&](Rig &rig, const std::vector<EntityHandle> &hs, int k, float x) {
        Entity *entity = rig.world.registry.get(hs[static_cast<size_t>(k)]);
        if (entity != nullptr) entity->position = {x, entity->position.y, 0.0f};
    };
    int agreements = 0;
    for (int frame = 0; frame < 8; ++frame) {
        if (frame == 2) { move(memo_rig, memo_buildings, 1, 5.0f); move(plain_rig, plain_buildings, 1, 5.0f); }
        if (frame == 4) { move(memo_rig, memo_buildings, 1, 26.0f); move(plain_rig, plain_buildings, 1, 26.0f); }
        if (frame == 5) { move(memo_rig, memo_buildings, 3, 4.0f); move(plain_rig, plain_buildings, 3, 4.0f); }
        hold_latches(memo_rig, memo_buildings);
        hold_latches(plain_rig, plain_buildings);
        memo_rig.ow.build_frame(memo_rig.world, memo_rig.cw, cam);
        plain_rig.ow.build_frame(plain_rig.world, plain_rig.cw, cam);
        for (int k = 0; k < 4; ++k) {
            const bool a = memo_rig.ow.building_batched(memo_buildings[static_cast<size_t>(k)]);
            const bool b = plain_rig.ow.building_batched(plain_buildings[static_cast<size_t>(k)]);
            CHECK(a == b);
            agreements += (a == b) ? 1 : 0;
        }
    }
    CHECK(agreements == 32);
    // The scripted moves did change the answer, so the agreement is not vacuous.
    CHECK(!memo_rig.ow.building_batched(memo_buildings[3]));
    CHECK(memo_rig.ow.building_batched(memo_buildings[0]));
}

// ---------------------------------------------------------------------------
void test_indoor_masks_and_gate() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // Camera inside the room (mission (20, 10, 1)), indoors blink flags.
    const OcclusionFrameCamera cam = rig.camera(20.0, 10.0, 1.0, 0x2);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    CHECK(rig.ow.camera_indoors());
    // Camera in section 1, positionally on the interior side of the window
    // plane (x < 22): the record processes A->B (the flip on section_a), so
    // bit 0 (exterior) + bit 1 (the seed) set, a window frustum banks, and
    // the type-2 camera-inside dispatch latches the exterior visible.
    // [orig: @ 0x5c5528]
    const uint32_t mask = rig.ow.section_mask(building);
    CHECK((mask & 0x2u) != 0);
    CHECK((mask & 0x1u) != 0);
    CHECK(rig.ow.window_frustum_group_count() == 1);
    CHECK(rig.ow.exterior_visible());
    CHECK(rig.ow.water_visible()); // the exterior-visible tail override

    // The blink-hits entity render gate. [orig: @ 0x5c7022-0x5c708a]
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.net_id = 5;
    npc.position = {20.0f, 10.0f, 0.0f};
    npc.bound_radius = 1.5625f;
    npc.alive = true;
    const EntityHandle nh = rig.world.registry.spawn(0, npc);
    rig.rebuild();
    Entity *n = rig.world.registry.get(nh);
    rig.cw.refresh_blink(rig.world, *n);
    CHECK(n->blink_hits[0] != 0); // inside the blink box

    // Section 1 is render-active -> visible.
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *n, cam));

    // Hide the section: rebuild the frame with the camera far outside and the
    // building windowless... simpler: fake a mask wipe by moving the camera
    // outdoors far away so the building leaves the batch (mask = 0).
    const OcclusionFrameCamera far_cam = rig.camera(20.0, 480.0, 1.0);
    // fog 500: |dy| = 470 < 500 + radius keeps it batched; push farther out.
    OcclusionFrameCamera cam2 = far_cam;
    cam2.fog_dist = fx(50.0);
    rig.ow.build_frame(rig.world, rig.cw, cam2);
    CHECK(!rig.ow.building_visible(building));
    CHECK(!rig.ow.building_batched(building)); // fog-culled = never entered the batch
    CHECK(rig.ow.section_mask(building) == 0u ||
          (rig.ow.section_mask(building) & 0x2u) == 0);
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *n, cam2));
}

// ---------------------------------------------------------------------------
void test_outside_in_viewthru() {
    Rig rig;
    // A building with an OPEN (type 1) box occluder slab on its east wall plus
    // TWO windows (east + west): the open slot marks bits 30/31 and drives the
    // outside-in traversal; the east window (facing the camera) recurses into
    // the room; the west window, met at depth 1, banks a see-through viewthru
    // wedge. [orig: the bank @ 0x5c565a]
    const double mn[3] = {1.6, -1.6, 0.0};
    const double mx[3] = {2.0, 1.6, 2.6};
    OcclusionModel om = box_occluder(kOccRecOpen, mn, mx);
    auto append = [&om](const OcclusionModel &win) {
        const int32_t vbase = static_cast<int32_t>(om.vertices.size());
        const int32_t pbase = static_cast<int32_t>(om.planes.size());
        const int32_t fbase = static_cast<int32_t>(om.faces.size());
        for (const auto &v : win.vertices) om.vertices.push_back(v);
        for (const auto &p : win.planes) om.planes.push_back(p);
        for (const auto &f : win.faces) om.faces.push_back(f);
        OcclusionPortalFace rec = win.records[0];
        rec.vert_start += vbase;
        rec.plane_start += pbase;
        rec.face_start += fbase;
        om.records.push_back(rec);
    };
    append(one_room_window()); // east wall (x = +2), normal +X
    {
        QuadSpec west;
        west.type = kOccRecWindow;
        west.section_a = 1;
        west.section_b = 0;
        const double c[4][3] = {
            {-2.0, 1.0, 1.0}, {-2.0, -1.0, 1.0}, {-2.0, -1.0, 2.5}, {-2.0, 1.0, 2.5}};
        std::memcpy(west.corners, c, sizeof(c));
        west.normal[0] = -1.0;
        west.normal[1] = 0.0;
        west.normal[2] = 0.0;
        append(occ_model({west}));
    }
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), std::move(om));
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // Camera outdoors EAST of the building, looking -X at the east wall,
    // within slot range (250 u).
    const OcclusionFrameCamera cam = rig.camera(32.0, 10.0, 1.5, 0, -1);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    CHECK(rig.ow.slot_count() >= 1); // the open record collected as a slot
    CHECK(rig.ow.building_open_flagged(building));
    const uint32_t mask = rig.ow.section_mask(building);
    // Outside-in traversal ran (bit 30 marker path): interior bit 1 reachable
    // through the east window, exterior bit 0 seeded, bit 31 marker set.
    CHECK((mask & 0x1u) != 0);
    CHECK((mask & 0x80000000u) != 0);
    CHECK((mask & 0x2u) != 0);
    CHECK(rig.ow.viewthru_group_count() >= 1);
}

// ---------------------------------------------------------------------------
void test_toc_occlusion() {
    Rig rig;
    // Occluder building: a big solid box slab (type 0 occluder record) between
    // the camera and a small building behind it.
    const double mn[3] = {2.0, -8.0, 0.0};
    const double mx[3] = {3.0, 8.0, 6.0};
    const EntityHandle occluder = rig.add_building(
        20.0, 10.0, building_collision(2, 8, 6), box_occluder(kOccRecOccluder, mn, mx));
    // The candidate sits well behind the slab (mission +X), small bounds.
    const EntityHandle candidate =
        rig.add_building(40.0, 10.0, building_collision(1.0, 1.0, 2.0), OcclusionModel{});
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // Camera in front of the slab at slab height.
    const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    CHECK(rig.ow.slot_count() >= 1);
    CHECK(rig.ow.building_visible(occluder));
    // The candidate is fully inside the slab's shadow wedge -> TOC culls it.
    // [orig: Terrain_TestSectorEntityOcclusion @ 0x5c4610]
    CHECK(!rig.ow.building_visible(candidate));
    CHECK(rig.ow.section_mask(candidate) == 0u);
    // The debug introspection split: TOC-culled = batched but not visible; the
    // model id resolves only for occlusion-carrying buildings.
    CHECK(rig.ow.building_batched(candidate));
    CHECK(rig.ow.instance_model_id(occluder) >= 0);
    CHECK(rig.ow.instance_model_id(candidate) == -1);
    CHECK(rig.ow.instance_count() == 1);

    // A candidate beside the slab (outside the wedge) stays visible.
    Rig rig2;
    rig2.add_building(20.0, 10.0, building_collision(2, 8, 6),
                      box_occluder(kOccRecOccluder, mn, mx));
    const EntityHandle beside =
        rig2.add_building(40.0, 60.0, building_collision(1.0, 1.0, 2.0), OcclusionModel{});
    rig2.rebuild();
    rig2.ow.init_mission(rig2.world, rig2.cw);
    const OcclusionFrameCamera cam2 = rig2.camera(5.0, 10.0, 1.5);
    rig2.ow.build_frame(rig2.world, rig2.cw, cam2);
    CHECK(rig2.ow.building_visible(beside));
}

// ---------------------------------------------------------------------------
void test_three_ray_latch() {
    Rig rig;
    // A ridge between the camera and the target: raise a terrain wall column.
    // The retail LOS marcher advances in roughly four-unit steps, so keep the
    // ridge wider than one stride (raw16 = units * 256).
    Rig *r = &rig;
    for (int y = 0; y < Field::kDim; ++y)
        for (int x = 29; x <= 33; ++x)
            r->field.heightmap[y * Field::kDim + x] = 20 * 256; // 20 u wall

    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.net_id = 7;
    npc.position = {60.0f, 10.0f, 0.0f};
    npc.bound_radius = 1.5625f;
    npc.alive = true;
    const EntityHandle nh = rig.world.registry.spawn(0, npc);
    rig.rebuild();
    Entity *n = rig.world.registry.get(nh);
    CHECK(n->blink_hits[0] == 0);

    // Outdoors camera on the near side of the wall.
    const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    // All three rays blocked -> culled, latch stays 0 (re-probes next frame).
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *n, cam));
    CHECK(n->occlusion_latch == 0);

    // Indoors flag skips the probe entirely: everything passes.
    // [orig: the (~flags & 2) gate @ 0x5c6f56]
    const OcclusionFrameCamera cam_in = rig.camera(5.0, 10.0, 1.5, 0x2);
    rig.ow.build_frame(rig.world, rig.cw, cam_in);
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *n, cam_in));
    CHECK(n->occlusion_latch >= 16 && n->occlusion_latch <= 23);
    const uint8_t armed = n->occlusion_latch;

    // While latched the counter just decrements (even outdoors + blocked).
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *n, cam));
    CHECK(n->occlusion_latch == armed - 1);

    // Flat terrain: the probe passes outdoors and re-arms.
    Rig rig3;
    Entity npc3;
    npc3.kind = EntityKind::Organic;
    npc3.net_id = 8;
    npc3.position = {60.0f, 10.0f, 0.0f};
    npc3.bound_radius = 1.5625f;
    npc3.alive = true;
    const EntityHandle nh3 = rig3.world.registry.spawn(0, npc3);
    rig3.rebuild();
    Entity *n3 = rig3.world.registry.get(nh3);
    const OcclusionFrameCamera cam3 = rig3.camera(5.0, 10.0, 1.5);
    rig3.ow.build_frame(rig3.world, rig3.cw, cam3);
    CHECK(rig3.ow.entity_render_visible(rig3.world, rig3.cw, *n3, cam3));
    CHECK(n3->occlusion_latch >= 16 && n3->occlusion_latch <= 23);
}

// ---------------------------------------------------------------------------
void test_camera_blink_query() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), OcclusionModel{});
    rig.rebuild();

    BlinkAccum accum;
    const int32_t inside[3] = {fx(20.0), fx(10.0), fx(1.0)};
    rig.cw.query_blink_boxes_at_point(rig.world, inside, accum);
    CHECK(accum.hit_count == 1);
    CHECK((accum.hits[0] & 0xFFFF) != 0);
    CHECK(static_cast<int32_t>((accum.hits[0] >> 12) & 0x1F) == 1); // section 1
    CHECK(EntityHandle::make(2, static_cast<int32_t>(accum.hits[0] >> 20)) == building);
    CHECK((accum.flags & kBlinkIndoorsBit) != 0); // flags 4 ^ 6 = 2

    const int32_t outside[3] = {fx(40.0), fx(10.0), fx(1.0)};
    rig.cw.query_blink_boxes_at_point(rig.world, outside, accum);
    CHECK(accum.hit_count == 0);
}

// ---------------------------------------------------------------------------
// The def's forced sections (itemDef +0x891 first_door - 1, +0x892
// first_subobject - 1) OR into the DRAW mask only; every other reader keeps
// the raw word. [orig: Terrain_RenderSectorModels @ 0x5c5d7c..0x5c5da8]
void test_forced_visible_bits() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    // A windowless building without OOBJ (the Iblock01 shape): outdoors its
    // raw mask is the exterior bit alone, and first_door 2 forces its door.
    const EntityHandle windowless =
        rig.add_building(20.0, 30.0, building_collision(2, 2, 3), OcclusionModel{});
    rig.ow.assign_forced_sections(building, 5, 0);
    rig.ow.assign_forced_sections(windowless, 1, 0);
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    OcclusionFrameCamera cam = rig.camera(20.0, 480.0, 1.5);
    cam.fog_dist = fx(50.0); // out of range -> raw mask 0
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.section_mask(building) == 0u);
    CHECK(rig.ow.forced_section_mask(building) == (0xFFFFFFFFu << 5));
    CHECK((rig.ow.section_mask(building) | rig.ow.forced_section_mask(building)) ==
          (0xFFFFFFFFu << 5));

    const OcclusionFrameCamera near_cam = rig.camera(5.0, 30.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, near_cam);
    CHECK(rig.ow.building_visible(windowless));
    CHECK(rig.ow.section_mask(windowless) == 1u);
    CHECK((rig.ow.section_mask(windowless) | rig.ow.forced_section_mask(windowless)) ==
          0xFFFFFFFFu);
    // Both bytes merge; a zero byte forces nothing; the pair clears.
    rig.ow.assign_forced_sections(windowless, 0, 4);
    CHECK(rig.ow.forced_section_mask(windowless) == (0xFFFFFFFFu << 4));
    rig.ow.assign_forced_sections(windowless, 3, 4);
    CHECK(rig.ow.forced_section_mask(windowless) == (0xFFFFFFFFu << 3));
    rig.ow.assign_forced_sections(windowless, 0, 0);
    CHECK(rig.ow.forced_section_mask(windowless) == 0u);
}

// ---------------------------------------------------------------------------
// Raise a terrain ridge across x = 29..33 (wider than one LOS march stride).
// The LOS sampler reads half-unit steps (raw16 >> 7), so heights are 0.5 u
// multiples.
void raise_ridge(Rig &rig, double height_units) {
    const uint16_t raw = static_cast<uint16_t>(height_units * 256.0 + 0.5);
    for (int y = 0; y < Field::kDim; ++y)
        for (int x = 29; x <= 33; ++x) rig.field.heightmap[y * Field::kDim + x] = raw;
}

// The three rays start ONE unit above the eye. [orig:
// Terrain_OcclusionCheckThreeRays `add edx, 10000h` @ 0x610eef] A 0.9 u
// sphere 55 u out behind a 1.5 u crest: from eye + 0.25 every ray dips below
// the crest (the top ray at most 1.39 u over it), from eye + 1.0 the top ray
// clears it (at least 1.67 u).
void test_three_rays_start_one_unit_up() {
    Rig rig;
    raise_ridge(rig, 1.5);
    rig.rebuild();
    const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    const int32_t center[3] = {fx(60.0), fx(10.0), 0};
    uint8_t latch = 0;
    CHECK(rig.ow.sphere_render_visible(rig.cw, cam, center, fx(0.9), latch, 0));
    CHECK(latch >= 16 && latch <= 23);
}

// The person leg: the entity position with the entity+0 bound radius, the
// item-185 radius under the parachute flag, and the sub-pixel floor ahead of
// the latch. [orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8dd7..0x5c8e10,
// @ 0x5c8e5e, the latch @ 0x5c8e7b..0x5c8eab]
void test_person_collector_leg() {
    Rig rig;
    // A 2 u crest: the top ray to a 1 u sphere dips below it (at most 1.86 u
    // over the crest); to a 2 u person radius it clears (at least 2.24 u).
    raise_ridge(rig, 2.0);
    auto spawn_person = [&](double x, float bound, uint32_t flags) {
        Entity p;
        p.kind = EntityKind::Organic;
        p.net_id = rig.next_net_id++;
        p.position = {static_cast<float>(x), 10.0f, 0.0f};
        p.bound_radius = bound;
        p.flags = flags;
        p.alive = true;
        return rig.world.registry.spawn(0, p);
    };
    const EntityHandle person = spawn_person(60.0, 2.0f, 0);
    const EntityHandle small = spawn_person(60.0, 1.0f, 0);
    const EntityHandle chute = spawn_person(60.0, 1.0f, kEntityFlagParachute);
    const EntityHandle far = spawn_person(1005.0, 1.5625f, 0);
    rig.rebuild();
    rig.ow.set_parachute_radius_q16(fx(3.0));
    OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    cam.fog_dist = fx(2000.0);
    rig.ow.build_frame(rig.world, rig.cw, cam);

    Entity *p = rig.world.registry.get(person);
    Entity *s = rig.world.registry.get(small);
    Entity *c = rig.world.registry.get(chute);
    Entity *f = rig.world.registry.get(far);
    CHECK(p != nullptr && s != nullptr && c != nullptr && f != nullptr);
    if (p == nullptr || s == nullptr || c == nullptr || f == nullptr) return;
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *p, cam));
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *s, cam));
    CHECK(rig.ow.person_collector_radius(fx(1.0), true) == fx(3.0));
    CHECK(rig.ow.person_collector_radius(fx(1.0), false) == fx(1.0));
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *c, cam));

    // 1000 u out: 1.5625 * 400 / 1000 = 0.625 px <= 0.75 px -> dropped, and
    // the held latch is not counted down.
    f->occlusion_latch = 5;
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *f, cam));
    CHECK(f->occlusion_latch == 5);
    // The same person 100 u out projects 6.25 px and takes the latch.
    const int32_t near_pos[3] = {fx(105.0), fx(10.0), 0};
    uint8_t latch = 5;
    CHECK(rig.ow.person_render_visible(rig.cw, cam, near_pos, fx(1.5625), latch, 0));
    CHECK(latch == 4);
}

// A slab occluder building (type 0 record) at mission (x, y).
EntityHandle add_slab(Rig &rig, double x, double y, float priority = 0.0f) {
    const double mn[3] = {2.0, -8.0, 0.0};
    const double mx[3] = {3.0, 8.0, 6.0};
    OcclusionModel om = box_occluder(kOccRecOccluder, mn, mx);
    om.records[0].slot_priority_scale = priority;
    return rig.add_building(x, y, building_collision(2, 8, 6), std::move(om));
}

// The slot sort keeps the 14 highest-priority slots in stable order: a 15th
// collected occluder occludes nothing. [orig: Terrain_SortPortalSlotsByPriority
// @ 0x5c4410 — the compare @ 0x5c4440, the clamp @ 0x5c449f..0x5c44a4]
void test_portal_slot_sort_and_clamp() {
    for (int weighted = 0; weighted < 2; ++weighted) {
        Rig rig;
        rig.world.registry.configure_pool(2, 32);
        Entity filler; // slot 0 stays occupied, like the Rig's own filler
        filler.kind = EntityKind::Marker;
        filler.net_id = 99;
        rig.world.registry.spawn(2, filler);
        // Fourteen slabs off to the side (y = 40), each a collected slot.
        for (int k = 0; k < 14; ++k) add_slab(rig, 8.0 + 5.0 * k, 40.0);
        // The fifteenth, collected last, is the only one between the camera
        // and the candidate.
        const EntityHandle blocker = add_slab(rig, 20.0, 10.0, weighted ? 1.0f : 0.0f);
        const EntityHandle candidate =
            rig.add_building(40.0, 10.0, building_collision(1.0, 1.0, 2.0), OcclusionModel{});
        rig.rebuild();
        rig.ow.init_mission(rig.world, rig.cw);
        const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.slot_count() == 14);
        CHECK(rig.ow.building_visible(blocker));
        if (weighted) {
            // A nonzero weight sorts the blocker ahead of the zero-weight
            // slots, so it survives the clamp and occludes the candidate.
            CHECK(!rig.ow.building_visible(candidate));
        } else {
            // All weights zero: collection order holds and the blocker is cut.
            CHECK(rig.ow.building_visible(candidate));
        }
    }
}

// render_TOC measures the candidate by its entity+0 bound radius. [orig:
// Terrain_TestSectorEntityOcclusion `mov eax,[esi]; fild dword ptr [eax]`
// @ 0x5c463e..0x5c4640] A 1 u bound sphere 1.5 u inside the slab's shadow
// wedge is fully inside it; the wide collision box's corner length would
// reach past the wedge and let the 8-corner refinement keep it visible.
void test_toc_candidate_uses_entity_bound_radius() {
    Rig rig;
    add_slab(rig, 20.0, 10.0);
    const EntityHandle candidate =
        rig.add_building(40.0, 24.8, building_collision(1.0, 3.0, 2.0), OcclusionModel{});
    Entity *c = rig.world.registry.get(candidate);
    CHECK(c != nullptr);
    if (c == nullptr) return;
    c->bound_radius = 1.0f;
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.building_batched(candidate));
    CHECK(!rig.ow.building_visible(candidate));
}

// Both render waves run render_TOC on every collected entity in no blink box
// and skip it for a contained one (entity+0x1D0, the first blink hit,
// nonzero), which draws on the collector's section gate alone. [orig:
// Terrain_RenderSectorEntities @ 0x5c7b92..0x5c7ba6,
// Terrain_RenderSectorEntitiesBySide @ 0x5c7d8b..0x5c7da0]
void test_render_wave_toc_skips_contained_entities() {
    auto spawn_person = [](Rig &rig, double x, uint16_t net_id) {
        Entity p;
        p.kind = EntityKind::Organic;
        p.net_id = net_id;
        p.position = {static_cast<float>(x), 10.0f, 0.0f};
        p.bound_radius = 1.0f;
        p.alive = true;
        return rig.world.registry.spawn(0, p);
    };

    // Indoors: a blink room with no occlusion records. The camera inside it
    // latches camera-inside mode with no exterior plane, where render_TOC
    // culls every candidate outright. [orig: Terrain_TestSectorEntityOcclusion
    // @ 0x5c4693..0x5c46b0]
    {
        Rig rig;
        const EntityHandle room =
            rig.add_building(20.0, 10.0, building_collision(2, 2, 3), OcclusionModel{});
        const EntityHandle inside = spawn_person(rig, 21.0, 5);
        const EntityHandle outside = spawn_person(rig, 60.0, 6);
        rig.rebuild();
        rig.ow.init_mission(rig.world, rig.cw);
        Entity *in = rig.world.registry.get(inside);
        Entity *out = rig.world.registry.get(outside);
        CHECK(in != nullptr && out != nullptr);
        if (in == nullptr || out == nullptr) return;
        rig.cw.refresh_blink(rig.world, *in);
        rig.cw.refresh_blink(rig.world, *out);
        CHECK(in->blink_hits[0] != 0);
        CHECK(out->blink_hits[0] == 0);

        const OcclusionFrameCamera cam = rig.camera(20.0, 10.0, 1.0, 0x2);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.camera_indoors());
        CHECK((rig.ow.section_mask(room) & 0x2u) != 0);
        // Contained: the section gate passes and render_TOC never runs.
        CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *in, cam));
        // Outside every blink box: collected (in view, indoors = no rays),
        // then culled by render_TOC.
        CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *out, cam));

        // The same rule over a caller-built candidate (a decoded wire row).
        OcclusionWorld::TocCandidate row;
        row.pos_fixed[0] = fx(60.0);
        row.pos_fixed[1] = fx(10.0);
        row.radius_q16 = fx(1.0);
        const uint32_t no_hits[4] = {0, 0, 0, 0};
        CHECK(rig.ow.render_wave_toc_occluded(no_hits, row));
        CHECK(!rig.ow.render_wave_toc_occluded(in->blink_hits, row));
    }

    // Outdoors: a person inside the slab's shadow wedge passes the collector
    // (flat terrain clears the rays) and render_TOC culls it; one beside the
    // wedge draws.
    {
        Rig rig;
        add_slab(rig, 20.0, 10.0);
        const EntityHandle behind = spawn_person(rig, 40.0, 7);
        Entity beside_def;
        beside_def.kind = EntityKind::Organic;
        beside_def.net_id = 8;
        beside_def.position = {40.0f, 60.0f, 0.0f};
        beside_def.bound_radius = 1.0f;
        beside_def.alive = true;
        const EntityHandle beside = rig.world.registry.spawn(0, beside_def);
        rig.rebuild();
        rig.ow.init_mission(rig.world, rig.cw);
        const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        Entity *b = rig.world.registry.get(behind);
        Entity *s = rig.world.registry.get(beside);
        CHECK(b != nullptr && s != nullptr);
        if (b == nullptr || s == nullptr) return;
        CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *b, cam));
        CHECK(b->occlusion_latch != 0); // the collector took it before the TOC
        CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *s, cam));
    }
}

// A weld-linked building straddles the water plane by its bound SPHERE (z +
// center z +- radius), not its collision header. [orig:
// Terrain_BuildSectorVisibilityMasks @ 0x5c8985..0x5c89a9] The linked box spans z
// 0..3 (header: below 3.5 u water) while its sphere (center 1.5, radius 3.2)
// reaches above it.
void test_link_water_straddle_uses_bound_sphere() {
    Rig rig;
    QuadSpec qb;
    qb.type = kOccRecWindow;
    qb.section_a = 1;
    qb.section_b = 0;
    const double cb[4][3] = {
        {-2.0, -1.0, 1.0}, {-2.0, 1.0, 1.0}, {-2.0, 1.0, 2.5}, {-2.0, -1.0, 2.5}};
    std::memcpy(qb.corners, cb, sizeof(cb));
    qb.normal[0] = -1.0;
    OcclusionWorld::EntityDefBits weldable;
    weldable.weldable = true;
    rig.add_building(10.0, 10.0, building_collision(2, 2, 3), one_room_window(), weldable);
    const EntityHandle b =
        rig.add_building(14.1, 10.0, building_collision(2, 2, 3), occ_model({qb}), weldable);
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    CHECK(rig.ow.weld_records().size() == 2);
    // Inside A (the only record is the welded link: no exterior latch).
    OcclusionFrameCamera cam = rig.camera(10.0, 10.0, 1.0, 0x2);
    cam.water_z = fx(3.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.camera_indoors());
    CHECK(!rig.ow.exterior_visible());
    CHECK((rig.ow.section_mask(b) & 0xFFFFFFEu) != 0);
    CHECK(rig.ow.water_visible());
    // Water above the sphere top: neither form straddles.
    cam.water_z = fx(5.0);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(!rig.ow.water_visible());
}

// The collectors place the sphere center through the full Euler pose.
// [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40 from
// Terrain_CollectVisibleSectorUserpoints @ 0x5c6c6a] A building whose bounds sit
// 10..12 u ahead of its origin is admitted yaw-only; pitched 180 deg the
// center swings 11 u behind the camera and the batch drops it.
void test_collector_center_uses_full_euler_pose() {
    for (int pitched = 0; pitched < 2; ++pitched) {
        Rig rig;
        // The volumes' extents feed the model bounds (finalize_sections);
        // shifting them moves the bound sphere 11 u ahead of the origin.
        CollisionModel cm = building_collision(1, 1, 2);
        for (CollisionVolume &v : cm.volumes) {
            v.min_x += fx(11.0);
            v.max_x += fx(11.0);
        }
        const EntityHandle h = rig.add_building(20.0, 10.0, std::move(cm), OcclusionModel{});
        Entity *e = rig.world.registry.get(h);
        CHECK(e != nullptr);
        if (e == nullptr) return;
        e->pitch = pitched ? 180.0f : 0.0f;
        e->occlusion_latch = 200;
        rig.rebuild();
        rig.ow.init_mission(rig.world, rig.cw);
        const OcclusionFrameCamera cam = rig.camera(21.0, 10.0, 1.0);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.building_batched(h) == (pitched == 0));
    }
}

// The mission-start blink stamp: every pool-1 row and every non-building
// pool-2 row carries its blink hits (and the indoors flag) before the first
// entity tick. [orig: Entity_BuildProximityListsForPools12 @ 0x5240a0, from
// Game_StartMission @ 0x525898]
void test_mission_start_blink_stamp() {
    Rig rig;
    rig.world.registry.configure_pool(1, 4);
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), OcclusionModel{});
    auto spawn_inside = [&](int pool) {
        Entity e;
        e.kind = EntityKind::Item;
        e.net_id = rig.next_net_id++;
        e.position = {20.0f, 10.0f, 1.0f};
        e.alive = true;
        return rig.world.registry.spawn(pool, e);
    };
    const EntityHandle item = spawn_inside(2);
    const EntityHandle vehicle = spawn_inside(1);
    rig.rebuild();
    Entity *i = rig.world.registry.get(item);
    Entity *v = rig.world.registry.get(vehicle);
    CHECK(i != nullptr && v != nullptr);
    if (i == nullptr || v == nullptr) return;
    CHECK(i->blink_hits[0] == 0 && v->blink_hits[0] == 0);
    rig.cw.refresh_mission_start_blink(rig.world);
    CHECK(EntityHandle::make(2, BlinkAccum::hit_pool_entity_index(i->blink_hits[0])) == building);
    CHECK(BlinkAccum::hit_section(i->blink_hits[0]) == 1);
    CHECK((i->flags & kEntityFlagIndoors) != 0);
    CHECK(v->blink_hits[0] == i->blink_hits[0]);
    CHECK((v->flags & kEntityFlagIndoors) != 0);
}

// The feed's building verdict word round-trips the full 32-bit mask (its top
// bit included) beside the visible flag, and an all-ones mask never reads as
// visible on its own.
void test_building_visibility_feed_word() {
    const int64_t hidden_full = pack_building_visibility(0xFFFFFFFFu, false);
    CHECK(building_visibility_mask(hidden_full) == 0xFFFFFFFFu);
    CHECK(!building_visibility_visible(hidden_full));
    const int64_t visible_full = pack_building_visibility(0xFFFFFFFFu, true);
    CHECK(building_visibility_mask(visible_full) == 0xFFFFFFFFu);
    CHECK(building_visibility_visible(visible_full));
    const int64_t visible_none = pack_building_visibility(0u, true);
    CHECK(building_visibility_mask(visible_none) == 0u);
    CHECK(building_visibility_visible(visible_none));
    CHECK(visible_none == (int64_t(1) << kBuildingVisibleBit));
    CHECK(pack_building_visibility(0x80000001u, false) == int64_t(0x80000001u));
}

// The model leg's render model: an entity without one is never collected (its
// latch untouched); without a usable collision instance the render model's
// CMDL sphere stands, placed through the entity pose and scaled by the entity
// scale; a model without the collision block keeps the zero spawn words, a
// radius-0 sphere at the position. The camera at x = 5 looks down +X with its
// near plane at x = 4.95. [orig: the entity+0x30 gates
// Terrain_CollectVisibleEntities_0 @ 0x5c6fd8..0x5c6fe1 /
// Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8cf6..0x5c8cff;
// Entity_ComputeBoundingSphere @ 0x5c69a0, the null-block early out @ 0x5c69be]
void test_model_leg_render_model_sphere() {
    Rig rig;
    // Floating 1 u up, so every three-ray probe over the flat field clears.
    auto spawn_item = [&](double x) {
        Entity e;
        e.kind = EntityKind::Item;
        e.net_id = rig.next_net_id++;
        e.position = {static_cast<float>(x), 10.0f, 1.0f};
        e.yaw = 90; // identity heading
        e.alive = true;
        return rig.world.registry.spawn(2, e);
    };
    const EntityHandle modelless = spawn_item(60.0);
    const EntityHandle behind = spawn_item(3.0);
    const EntityHandle blockless = spawn_item(4.5);
    const EntityHandle blockless_far = spawn_item(60.0);
    rig.rebuild();
    // A CMDL sphere 11 u ahead of the origin: the entity at x = 3 is behind the
    // near plane (a position-centred unit sphere there is culled), its sphere
    // centre at x = 14 is not.
    opennova::renderer::ObjectProjectionSphere ahead;
    ahead.valid = true;
    ahead.center_q16 = {fx(11.0), 0, 0};
    ahead.radius_q16 = fx(1.0);
    rig.ow.assign_render_model(behind, ahead);
    // No collision block: the zero spawn words. A unit sphere at x = 4.5 would
    // still touch the near plane; radius 0 does not.
    opennova::renderer::ObjectProjectionSphere unstamped;
    unstamped.valid = true;
    rig.ow.assign_render_model(blockless, unstamped);
    rig.ow.assign_render_model(blockless_far, unstamped);

    const OcclusionFrameCamera cam = rig.camera(5.0, 10.0, 1.5);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    Entity *m = rig.world.registry.get(modelless);
    Entity *b = rig.world.registry.get(behind);
    Entity *z = rig.world.registry.get(blockless);
    Entity *f = rig.world.registry.get(blockless_far);
    CHECK(m != nullptr && b != nullptr && z != nullptr && f != nullptr);
    if (m == nullptr || b == nullptr || z == nullptr || f == nullptr) return;
    CHECK(!rig.ow.has_render_model(modelless));
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *m, cam));
    CHECK(m->occlusion_latch == 0); // never collected: the latch never ran
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *b, cam));
    CHECK(b->occlusion_latch >= 16 && b->occlusion_latch <= 23);
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *z, cam));
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *f, cam));
    // The entity scale applies to the stamped (unscaled) sphere: at scale
    // 0.25 the centre sits 2.75 u ahead (x = 5.75) with radius 0.25, in view.
    b->uniform_scale_q16 = 0x4000;
    b->occlusion_latch = 0;
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *b, cam));
    // Retiring the instance drops the render model with it.
    rig.ow.remove_entity_instance(behind);
    CHECK(!rig.ow.has_render_model(behind));
    b->occlusion_latch = 0;
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *b, cam));
}

// The collector per view: the weapon Inset pass re-runs the whole collect
// over its own camera (Render_WeaponInsetScene @ 0x5c9740 ->
// Terrain_RenderWorldScene @ 0x5c9de9 -> Terrain_CollectVisibleEntities
// @ 0x5c94f0), so each view carries its own batch and section masks while the
// entity latches tick once per collect, whichever view collects.
void test_collector_runs_per_view() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    Entity npc;
    npc.kind = EntityKind::Organic;
    npc.net_id = rig.next_net_id++;
    npc.position = {60.0f, 10.0f, 0.0f};
    npc.bound_radius = 1.5625f;
    npc.alive = true;
    const EntityHandle nh = rig.world.registry.spawn(0, npc);
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    Entity *n = rig.world.registry.get(nh);
    CHECK(n != nullptr);
    if (n == nullptr) return;

    const OcclusionFrameCamera main_cam = rig.camera(5.0, 10.0, 1.5);
    CHECK(rig.ow.selected_view() == OcclusionView::kMain);
    rig.ow.build_frame(rig.world, rig.cw, main_cam);
    CHECK(rig.ow.building_visible(building));
    const uint32_t main_mask = rig.ow.section_mask(building);
    CHECK(main_mask == 0xFFFFFFFu); // the windowed building, outdoors
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *n, main_cam));
    const uint8_t armed = n->occlusion_latch;
    CHECK(armed >= 16 && armed <= 23);

    // The Inset pass looks from far out with the fog pulled in: the building
    // leaves its batch, while the person still draws there.
    OcclusionFrameCamera inset_cam = rig.camera(20.0, 480.0, 1.5);
    inset_cam.fog_dist = fx(50.0);
    rig.ow.select_view(OcclusionView::kInset);
    rig.ow.build_frame(rig.world, rig.cw, inset_cam);
    CHECK(!rig.ow.building_batched(building));
    CHECK(!rig.ow.building_visible(building));
    CHECK(rig.ow.section_mask(building) == 0u);
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *n, main_cam));
    CHECK(n->occlusion_latch == armed - 1); // the second collect ticks it again

    // Back on the main view: its own words, untouched by the Inset pass.
    rig.ow.select_view(OcclusionView::kMain);
    CHECK(rig.ow.building_batched(building));
    CHECK(rig.ow.building_visible(building));
    CHECK(rig.ow.section_mask(building) == main_mask);
    CHECK(rig.ow.exterior_visible());
    // And the parked Inset words are the Inset's.
    rig.ow.select_view(OcclusionView::kInset);
    CHECK(rig.ow.section_mask(building) == 0u);
    rig.ow.select_view(OcclusionView::kMain);
}

// A round restart never re-runs the portal register + weld: the kernel's
// OcclusionWorld outlives MissionKernel::restore_baseline, so the weld
// registry, the retyped shared records and the building flags stay the first
// start's (Game_RestartRoundSP passes 1 @ 0x5263d9, so Game_StartMission's
// portal arg is 0 and Terrain_InitBuildingPortals skips register + weld;
// render-occlusion-re.md §2, D-OCC-4).
void test_round_restart_keeps_the_first_weld() {
    opennova::bms::File m{};
    for (int i = 0; i < 2; ++i) {
        opennova::bms::Entity bldg{};
        bldg.type = opennova::bms::ItemType::Building;
        bldg.id = 50 + i;
        m.buildings.push_back(bldg);
    }
    m.events.push_back(opennova::bms::Event{});
    std::map<std::string, std::string> files;
    auto kernel = std::make_unique<opennova::mission::MissionKernel>();
    kernel->open_document(std::move(m), "synth", test_boot::source_over(&files));
    opennova::mission::KernelBootOptions options;
    options.playable = false;
    std::string error;
    CHECK(kernel->boot(options, error));
    Entity *a = kernel->world.registry.by_net_id(50);
    Entity *b = kernel->world.registry.by_net_id(51);
    CHECK(a != nullptr && b != nullptr);
    if (a == nullptr || b == nullptr) return;
    // The test_weld_and_flags pair: A's +X window 0.1 u from B's -X window.
    a->position = {10.0f, 10.0f, 0.0f};
    b->position = {14.1f, 10.0f, 0.0f};
    a->yaw = b->yaw = 90.0f; // identity heading
    a->pitch = b->pitch = a->roll = b->roll = 0.0f;
    QuadSpec qb;
    qb.type = kOccRecWindow;
    qb.section_a = 1;
    qb.section_b = 0;
    const double cb[4][3] = {
        {-2.0, -1.0, 1.0}, {-2.0, 1.0, 1.0}, {-2.0, 1.0, 2.5}, {-2.0, -1.0, 2.5}};
    std::memcpy(qb.corners, cb, sizeof(cb));
    qb.normal[0] = -1.0;
    OcclusionWorld::EntityDefBits weldable;
    weldable.weldable = true;
    const EntityHandle ha = a->handle;
    const EntityHandle hb = b->handle;
    kernel->collision.assign_entity(ha, kernel->collision.add_model(building_collision(2, 2, 3)));
    kernel->collision.assign_entity(hb, kernel->collision.add_model(building_collision(2, 2, 3)));
    kernel->occlusion.assign_entity(ha, kernel->occlusion.add_model(one_room_window()), weldable);
    kernel->occlusion.assign_entity(hb, kernel->occlusion.add_model(occ_model({qb})), weldable);
    // The first start's portal init over the attached models, then the seal.
    kernel->occlusion_init_mission();
    const std::vector<OcclusionWorld::WeldRecord> first_welds = kernel->occlusion.weld_records();
    CHECK(first_welds.size() == 2);
    const OcclusionWorld::BuildingFlags first_a = kernel->occlusion.building_flags(ha);
    const OcclusionWorld::BuildingFlags first_b = kernel->occlusion.building_flags(hb);
    CHECK(first_a.has_links && first_b.has_links);
    kernel->capture_baseline();

    auto same_as_first = [&]() {
        const std::vector<OcclusionWorld::WeldRecord> &welds = kernel->occlusion.weld_records();
        CHECK(welds.size() == first_welds.size());
        for (size_t i = 0; i < welds.size() && i < first_welds.size(); ++i) {
            CHECK(welds[i].own_entity == first_welds[i].own_entity);
            CHECK(welds[i].other_entity == first_welds[i].other_entity);
            CHECK(welds[i].own_section == first_welds[i].own_section);
            CHECK(welds[i].other_section == first_welds[i].other_section);
        }
        const OcclusionWorld::BuildingFlags fa = kernel->occlusion.building_flags(ha);
        const OcclusionWorld::BuildingFlags fb = kernel->occlusion.building_flags(hb);
        CHECK(fa.has_open == first_a.has_open && fa.has_windows == first_a.has_windows &&
              fa.has_links == first_a.has_links);
        CHECK(fb.has_open == first_b.has_open && fb.has_windows == first_b.has_windows &&
              fb.has_links == first_b.has_links);
    };
    CHECK(kernel->restore_baseline());
    same_as_first();
    // A second restart keeps them too (no register + weld on any restart).
    CHECK(kernel->restore_baseline());
    same_as_first();
}

} // namespace

// The occlusion frustum's five planes pass through the eye (retail's
// g_CameraFrustumPlanes5): an eye 0.1 u inside a window's plane still clips the
// window in, banks its wedge, and an outdoor person beyond it draws. A near
// plane ahead of the eye (the shell camera's 0.2 u near, the old anchor) left
// every window vertex behind it: the window was skipped as fully clipped, no
// wedge and no exterior plane came out, and the camera-inside rule culled the
// whole exterior — the Ghost Harvest spawn / doorway report.
// [orig: Viewport_BuildProjectionMatrix — the forward plane through the eye
//  @ 0x411577..0x4115c6; render_VPT's all-below skip @ 0x5c4d63..0x5c4df2;
//  Terrain_TestSectorEntityOcclusion's camera-inside rule @ 0x5c4662..0x5c4721]
void test_eye_in_front_of_a_window_keeps_the_exterior() {
    Rig rig;
    rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    Entity p;
    p.kind = EntityKind::Organic;
    p.net_id = 7;
    p.position = {40.0f, 10.0f, 0.0f};
    p.bound_radius = 1.0f;
    p.alive = true;
    const EntityHandle person = rig.world.registry.spawn(0, p);
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    Entity *out = rig.world.registry.get(person);
    CHECK(out != nullptr);
    if (out == nullptr) return;
    rig.cw.refresh_blink(rig.world, *out);
    CHECK(out->blink_hits[0] == 0);

    // The shell's hand-over: the eye 0.1 u inside the window plane (x = 22),
    // at the window's height, looking out along mission +X.
    OcclusionViewSpec view;
    const float eye_mission[3] = {21.9f, 10.0f, 1.75f};
    view.eye[0] = eye_mission[0];  // presentation = mission (x, z, -y)
    view.eye[1] = eye_mission[2];
    view.eye[2] = -eye_mission[1];
    view.forward[0] = 1.0f; view.forward[1] = 0.0f; view.forward[2] = 0.0f;  // mission +X
    view.right[0] = 0.0f; view.right[1] = 0.0f; view.right[2] = 1.0f;        // mission -Y
    view.up[0] = 0.0f; view.up[1] = 1.0f; view.up[2] = 0.0f;                 // mission +Z
    view.fov_y_deg = 53.4468f;
    view.aspect = 1.6667f;
    view.viewport_width = 640.0f;
    view.fog_dist_units = 500.0f;
    view.water_z_units = -100.0f;
    OcclusionFrameCamera cam;
    occlusion_camera_from_view(view, cam);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.camera_indoors());
    CHECK(rig.ow.window_frustum_group_count() == 1);
    CHECK(rig.ow.exterior_visible());
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *out, cam));

    // The divergence it replaced: the same camera with its forward plane moved
    // 0.2 u ahead of the eye drops the window, and the whole exterior with it.
    OcclusionFrameCamera near_cam = cam;
    near_cam.frustum[0][3] -= 0.2f;
    rig.ow.build_frame(rig.world, rig.cw, near_cam);
    CHECK(rig.ow.window_frustum_group_count() == 0);
    CHECK(!rig.ow.exterior_visible());
    CHECK(!rig.ow.entity_render_visible(rig.world, rig.cw, *out, near_cam));
}

// The statics table splits pool 2 on the items.def TYPE: a decoration (def type
// 2) inside a building stays out of the building prefix and batch, takes its
// own blink quad at the mission-start refresh, and is entity-collected by that
// quad — so from inside its room it draws while the camera faces away from
// every window, where the building batch's render_TOC (camera-inside rule, no
// window wedge) would cull it. [orig: Entity_BuildProximityLists_Pool2 pass 1
//  `def->type == ItemType_Building` @ 0x4b946e, pass 2 @ 0x4b9502; the
//  mission-start refresh's pool-2 filter @ 0x5240f3; Terrain_CollectVisibleEntities_0
//  @ 0x5c6f48..0x5c708a; the render wave's contained skip @ 0x5c7b92..0x5c7b99]
void test_decoration_rows_are_entity_collected() {
    Rig rig;
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window());
    if (Entity *b = rig.world.registry.get(building)) {
        b->has_item_def = true;
        b->item_type = kItemDefTypeBuilding;
    }
    // A crate stack inside the room: a pool-2 row (the BMS building record
    // family), def type 2, a solid half-unit box of its own.
    CollisionModel crate_cm;
    {
        const double h = 0.5;
        const int nx[6] = {16384, -16384, 0, 0, 0, 0};
        const int ny[6] = {0, 0, 16384, -16384, 0, 0};
        const int nz[6] = {0, 0, 0, 0, 16384, -16384};
        const double d[6] = {-h, -h, -h, -h, -2.0 * h, 0.0};
        for (int i = 0; i < 6; ++i) {
            CollisionPlane pl;
            pl.nx = static_cast<int16_t>(nx[i]);
            pl.ny = static_cast<int16_t>(ny[i]);
            pl.nz = static_cast<int16_t>(nz[i]);
            pl.dist = fx(d[i]);
            crate_cm.planes.push_back(pl);
        }
        CollisionVolume v;
        v.type = 1;
        v.min_x = fx(-h);
        v.max_x = fx(h);
        v.min_y = fx(-h);
        v.max_y = fx(h);
        v.min_z = 0;
        v.max_z = fx(2.0 * h);
        v.plane_start = 0;
        v.plane_count = 6;
        crate_cm.volumes.push_back(v);
        CollisionSection s0;
        s0.volume_start = 0;
        s0.volume_count = 1;
        crate_cm.sections.push_back(s0);
        crate_cm.finalize_sections();
    }
    Entity c;
    c.kind = EntityKind::Building;
    c.net_id = rig.next_net_id++;
    c.position = {19.0f, 10.0f, 0.5f};
    c.yaw = 90;
    c.alive = true;
    c.has_item_def = true;
    c.item_type = 2;
    c.bound_radius = 0.9f;
    const EntityHandle crate = rig.world.registry.spawn(2, c);
    // Its twin authored with a Building-type def: the row the port made of
    // EVERY pool-2 record before the split keyed on the def type.
    Entity cb = c;
    cb.net_id = rig.next_net_id++;
    cb.position = {19.0f, 11.0f, 0.5f};
    cb.item_type = kItemDefTypeBuilding;
    const EntityHandle crate_as_building = rig.world.registry.spawn(2, cb);
    const int32_t crate_model = rig.cw.add_model(std::move(crate_cm));
    rig.cw.assign_entity(crate, crate_model);
    rig.cw.assign_entity(crate_as_building, crate_model);
    rig.rebuild();
    CHECK(rig.cw.static_building_count() == 2);
    CHECK(rig.cw.static_count() == 3);
    rig.cw.refresh_mission_start_blink(rig.world);
    rig.ow.init_mission(rig.world, rig.cw);
    Entity *ce = rig.world.registry.get(crate);
    CHECK(ce != nullptr);
    if (ce == nullptr) return;
    CHECK(EntityHandle::make(2, BlinkAccum::hit_pool_entity_index(ce->blink_hits[0])) == building);
    CHECK(BlinkAccum::hit_section(ce->blink_hits[0]) == 1);

    // Camera in the room facing mission -X, the window behind it.
    const OcclusionFrameCamera cam = rig.camera(21.0, 10.0, 1.5, 0x2, -1);
    rig.ow.build_frame(rig.world, rig.cw, cam);
    CHECK(rig.ow.camera_indoors());
    CHECK((rig.ow.section_mask(building) & 0x2u) != 0);
    CHECK(rig.ow.window_frustum_group_count() == 0);
    CHECK(!rig.ow.building_batched(crate)); // not a building-batch member
    CHECK(rig.ow.entity_render_visible(rig.world, rig.cw, *ce, cam));
    // The Building-typed twin: a batch member, culled from inside its own
    // room by render_TOC's camera-inside rule (no window wedge) — how the
    // decoration drew before the split.
    CHECK(rig.ow.building_batched(crate_as_building));
    CHECK(!rig.ow.building_visible(crate_as_building));
}

int main() {
    test_bound_sphere();
    test_render_math();
    test_weld_and_flags();
    test_tilted_pose_weld();
    test_outdoor_masks();
    test_negative_static_slot_fog_collection();
    test_static_pose_memo_recomputes_on_pose_change();
    test_static_pose_memo_matches_the_unmemoized_walk();
    test_indoor_masks_and_gate();
    test_outside_in_viewthru();
    test_toc_occlusion();
    test_three_ray_latch();
    test_camera_blink_query();
    test_forced_visible_bits();
    test_three_rays_start_one_unit_up();
    test_person_collector_leg();
    test_portal_slot_sort_and_clamp();
    test_toc_candidate_uses_entity_bound_radius();
    test_render_wave_toc_skips_contained_entities();
    test_link_water_straddle_uses_bound_sphere();
    test_collector_center_uses_full_euler_pose();
    test_mission_start_blink_stamp();
    test_building_visibility_feed_word();
    test_model_leg_render_model_sphere();
    test_collector_runs_per_view();
    test_round_restart_keeps_the_first_weld();
    test_eye_in_front_of_a_window_keeps_the_exterior();
    test_decoration_rows_are_entity_collected();
    if (failures == 0) std::printf("occlusion_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
