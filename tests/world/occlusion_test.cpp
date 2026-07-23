// Rendering-occlusion unit tests [orig: the Jointops.exe consumer set —
// Terrain_InitBuildingPortals @0x5c7480 (register/weld/flags),
// build_sector_visibility_masks @0x5c8610, render_visibility_portal_traversal
// @0x5c4ae0, test_sector_entity_occlusion @0x5c4610, the entity-collector gates
// @0x5c6f20/0x5c8c60, terrain_occlusion_check_three_rays @0x610ed0] over
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

#include "terrain/height_field.h"
#include "world/collision.h"
#include "world/occlusion.h"
#include "world/world.h"

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
    float glow = 0.0f;
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
        rec.glow_scale = q.glow;

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
    // An inward-facing authored window in section 2 is deliberately invisible
    // from the camera's section 1. If a welded type-5 face incorrectly takes
    // the bit-27 same-building recursion, the exterior seed walk reaches it
    // and leaks bit 2 into A's mask.
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
    CHECK((rig.ow.section_mask(a) & (1u << 2)) == 0);
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
    // [orig: test_sector_entity_occlusion @ 0x5c4610]
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
void test_forced_visible_bits() {
    Rig rig;
    OcclusionWorld::EntityDefBits bits;
    bits.forced_lo = 5; // destruction bone map base
    const EntityHandle building =
        rig.add_building(20.0, 10.0, building_collision(2, 2, 3), one_room_window(), bits);
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);
    OcclusionFrameCamera cam = rig.camera(20.0, 480.0, 1.5);
    cam.fog_dist = fx(50.0); // out of range -> raw mask 0
    rig.ow.build_frame(rig.world, rig.cw, cam);
    // [orig: mask |= -1 << def+2193 @ 0x5c5d7c-0x5c5da8]
    CHECK(rig.ow.section_mask(building) == (0xFFFFFFFFu << 5));
}

} // namespace

int main() {
    test_render_math();
    test_weld_and_flags();
    test_tilted_pose_weld();
    test_outdoor_masks();
    test_negative_static_slot_fog_collection();
    test_indoor_masks_and_gate();
    test_outside_in_viewthru();
    test_toc_occlusion();
    test_three_ray_latch();
    test_camera_blink_query();
    test_forced_visible_bits();
    if (failures == 0) std::printf("occlusion_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
