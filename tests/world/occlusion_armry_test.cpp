// Model-file probe for the interior section-mask/portal engine: an armory
// (three rooms joined by two interior portals, one window, one whole-building
// open slot) loaded through threedi_3di3_read and run through the SAME
// model->runtime conversions the host performs, then the camera walked
// through the rooms and the per-room section masks asserted. Covers the
// room-transition chain the hand-built unit fixtures cannot: authored OFAC
// topology in a file, blink-box/section/render-part correspondence.
//
// Two legs share the camera walk: the synthetic fixtures/threedi/synth/armory.3di
// (tests/fixtures/minimal_3di_gen.cpp authors the retail floor plan below)
// always runs; the retail Armry01.3di (the 00TRa armory) runs when
// OPENNOVA_JO_ASSETS names the extracted JO corpus (docs/asset-gated-tests.md).
//
// The floor plan both models share (mission axes, entity-local; model =
// (-y, z, x)):
//   blink sec 1 = east room   x  1.1.. 5.7, y -3.2..2.8   (carries the window)
//   blink sec 2 = west room   x -6.8..-0.6, y -3.2..3.7
//   blink sec 3 = middle      x -0.5.. 1.1, y -3.2..2.8
//   occ records: type1 open (whole building), 4x type0 occluders,
//   type2 window A=1 B=0 on the east room's south wall, type3 portals
//   1<->3 @ (1.11, 1.24) and 3<->2 @ (-0.54, -2.25).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef OPENNOVA_ARMRY_FIXTURE
#define OPENNOVA_ARMRY_FIXTURE ""
#endif

#include <runtime/terrain_query/height_field.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>
#include "common/retail_paths.h"

using namespace opennova::world;
using namespace opennova::threedi;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

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

// The host's model->runtime collision conversion, replicated (the canonical
// copy is collision_model_from_3di in
// godot/src/simulation/simulation_internal.h — engine/runtime/world stays
// format-free by design, so the leaf test carries its own).
bool collision_from_3di(const ThreediCollisionModel *col, CollisionModel &out) {
    if (col == nullptr || col->volume_count == 0 || !threedi_3di3_collision_is_runtime_safe(col))
        return false;
    for (size_t i = 0; i < col->plane_count; ++i) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(std::lround(col->planes[i].normal[0] * 16384.0f));
        p.ny = static_cast<int16_t>(std::lround(col->planes[i].normal[1] * 16384.0f));
        p.nz = static_cast<int16_t>(std::lround(col->planes[i].normal[2] * 16384.0f));
        p.dist = fx(col->planes[i].radius);
        out.planes.push_back(p);
    }
    int32_t plane_cursor = 0;
    for (size_t i = 0; i < col->volume_count; ++i) {
        const ThreediBoundingVolume &sv = col->volumes[i];
        CollisionVolume v;
        v.type = sv.collidable_type;
        v.flags = static_cast<uint32_t>(sv.flags);
        v.min_x = sv.min_x_fp16;
        v.max_x = sv.max_x_fp16;
        v.min_y = sv.min_y_fp16;
        v.max_y = sv.max_y_fp16;
        v.min_z = sv.min_z_fp16;
        v.max_z = sv.max_z_fp16;
        v.plane_start = plane_cursor;
        v.plane_count = sv.plane_count;
        plane_cursor += sv.plane_count;
        out.volumes.push_back(v);
    }
    out.sections.assign(col->object_count, {});
    int32_t volume_cursor = 0;
    for (size_t s = 0; s < col->object_count; ++s) {
        CollisionSection &sec = out.sections[s];
        sec.volume_start = volume_cursor;
        sec.volume_count = col->objects[s].num_bounding_volumes;
        sec.parent_part_index = col->objects[s].parent_subobject_index;
        volume_cursor += sec.volume_count;
    }
    return true;
}

// The host's model->runtime occlusion conversion, replicated (canonical copy:
// occlusion_model_from_3di in simulation_internal.h).
bool occlusion_from_3di(const Threedi3di3 &model, OcclusionModel &out) {
    if (model.occlusion_object_count == 0) return false;
    for (size_t i = 0; i < model.occlusion_vertex_count; ++i) {
        OcclusionVertex v;
        v.p[0] = model.occlusion_vertices[i].position[0];
        v.p[1] = model.occlusion_vertices[i].position[1];
        v.p[2] = model.occlusion_vertices[i].position[2];
        out.vertices.push_back(v);
    }
    for (size_t i = 0; i < model.occlusion_plane_count; ++i) {
        OcclusionPlane p;
        p.normal[0] = model.occlusion_planes[i].normal[0];
        p.normal[1] = model.occlusion_planes[i].normal[1];
        p.normal[2] = model.occlusion_planes[i].normal[2];
        p.d = model.occlusion_planes[i].radius;
        out.planes.push_back(p);
    }
    for (size_t i = 0; i < model.occlusion_face_count; ++i) {
        const ThreediOcclusionFace &sf = model.occlusion_faces[i];
        OcclusionFaceRec f;
        f.v[0] = static_cast<uint8_t>(sf.raw_indices & 0xFF);
        f.v[1] = static_cast<uint8_t>((sf.raw_indices >> 8) & 0xFF);
        f.v[2] = static_cast<uint8_t>((sf.raw_indices >> 16) & 0xFF);
        f.plane = static_cast<uint8_t>((sf.raw_indices >> 24) & 0xFF);
        f.edge[0] = static_cast<uint16_t>(sf.edge_data & 0xFFFF);
        f.edge[1] = static_cast<uint16_t>(sf.edge_data >> 16);
        f.edge[2] = static_cast<uint16_t>(sf.other_edge_data & 0xFFFF);
        out.faces.push_back(f);
    }
    int32_t vert_cursor = 0, plane_cursor = 0, face_cursor = 0;
    for (size_t i = 0; i < model.occlusion_object_count; ++i) {
        const ThreediOcclusionObject &so = model.occlusion_objects[i];
        OcclusionPortalFace rec;
        rec.type = so.type;
        rec.section_a = so.parent_subobject_index;
        rec.section_b = so.connecting_subobject;
        rec.pos[0] = so.position[0];
        rec.pos[1] = so.position[1];
        rec.pos[2] = so.position[2];
        rec.radius = so.radius;
        rec.vert_start = vert_cursor;
        rec.vert_count = so.num_vertices;
        rec.plane_start = plane_cursor;
        rec.plane_count = so.num_planes;
        rec.face_start = face_cursor;
        rec.face_count = so.face_count;
        rec.glow_scale = so.glow_scale;
        if (so.num_vertices > 0) vert_cursor += so.num_vertices;
        if (so.num_planes > 0) plane_cursor += so.num_planes;
        if (so.face_count > 0) face_cursor += so.face_count;
        out.records.push_back(rec);
    }
    return true;
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
        // the unit rig (the original shares the ambiguity).
        Entity filler;
        filler.kind = EntityKind::Marker;
        filler.net_id = 99;
        world.registry.spawn(2, filler);
    }

    EntityHandle add_armory(double x, double y, CollisionModel cm, OcclusionModel om) {
        Entity b;
        b.kind = EntityKind::Building;
        b.net_id = 100;
        b.position = {static_cast<float>(x), static_cast<float>(y), 0.0f};
        b.yaw = 90; // mission yaw 90 -> engine heading 0 (identity rotation)
        b.alive = true;
        const EntityHandle h = world.registry.spawn(2, b);
        cw.assign_entity(h, cw.add_model(std::move(cm)));
        ow.assign_entity(h, ow.add_model(std::move(om)), {});
        return h;
    }

    void rebuild() {
        for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
    }

    // Wide-open single-plane frustum facing mission +-X, like the unit rig.
    OcclusionFrameCamera camera(double x, double y, double z, uint32_t blink_flags,
                                int look_x) const {
        OcclusionFrameCamera cam;
        cam.pos_fixed[0] = fx(x);
        cam.pos_fixed[1] = fx(y);
        cam.pos_fixed[2] = fx(z);
        render_float_from_fixed(cam.pos_fixed, cam.pos_float);
        const float dir = look_x >= 0 ? 1.0f : -1.0f;
        cam.frustum_count = 1;
        cam.frustum[0][0] = 0.0f;
        cam.frustum[0][1] = 0.0f;
        cam.frustum[0][2] = dir; // mission (1,0,0) -> render (0,0,1)
        cam.frustum[0][3] = -dir * cam.pos_float[2] + 0.05f;
        cam.view_rows_q22[0][0] = look_x >= 0 ? (1 << 22) : -(1 << 22);
        cam.view_rows_q22[1][1] = look_x >= 0 ? (1 << 22) : -(1 << 22);
        cam.view_rows_q22[2][2] = 1 << 22;
        cam.fog_dist = fx(500.0);
        cam.water_z = fx(-100.0);
        cam.local_blink_flags = blink_flags;
        return cam;
    }
};

} // namespace

// The volume families a leg's model authors (Super OED Manual v1.1 §1.1.3.4
// names them): generic CBs, the CA armory trigger, the VC vehicle hull, the
// CL ladder, and the three BB rooms with their blink letters.
struct VolumeShape {
    int cb, cl, ca, vc, bb;
};

// Load one armory, pin its authored shape, then walk the camera through the
// rooms. Returns the number of failed checks for this leg.
int run_leg(const char *label, const std::string &path, const VolumeShape &expected) {
    const int failures_before = failures;
    std::printf("occlusion_armry: %s leg (%s)\n", label, path.c_str());
    Threedi3di3 model = {};
    if (threedi_3di3_read(path.c_str(), &model) != 0) {
        std::fprintf(stderr, "FAIL: %s not readable\n", path.c_str());
        return 1;
    }

    CollisionModel cm;
    OcclusionModel om;
    CHECK(collision_from_3di(model.collision, cm));
    CHECK(occlusion_from_3di(model, om));
    // The authored shape this test's assertions are keyed to.
    CHECK(cm.sections.size() == 4);
    CHECK(om.records.size() == 8);
    int cb_count = 0, cl_count = 0, ca_count = 0, vc_count = 0, bb_count = 0;
    int bb_2e_count = 0, bb_28_count = 0;
    for (const CollisionVolume &volume : cm.volumes) {
        if (volume.type == 1) ++cb_count;
        if (volume.type == 4) ++cl_count;
        if (volume.type == 6) ++ca_count;
        if (volume.type == 7) ++vc_count;
        if (volume.type == 8) {
            ++bb_count;
            if (volume.flags == 0x2Eu) ++bb_2e_count;
            if (volume.flags == 0x28u) ++bb_28_count;
        }
    }
    CHECK(cb_count == expected.cb);
    CHECK(cl_count == expected.cl);
    CHECK(ca_count == expected.ca);
    CHECK(vc_count == expected.vc);
    CHECK(bb_count == expected.bb);
    CHECK(bb_2e_count == 2 && bb_28_count == 1);
    threedi_3di3_free(&model);
    if (failures != failures_before) return failures - failures_before; // shape mismatch: don't chase derived checks

    Rig rig;
    const EntityHandle armory = rig.add_armory(100.0, 100.0, std::move(cm), std::move(om));
    rig.rebuild();
    rig.ow.init_mission(rig.world, rig.cw);

    // A single free-standing building: no welds; the flag stamp sees the open
    // record (type 1) and the window (type 2), no links.
    CHECK(rig.ow.weld_records().empty());
    const OcclusionWorld::BuildingFlags flags = rig.ow.building_flags(armory);
    CHECK(flags.has_open);
    CHECK(flags.has_windows);
    CHECK(!flags.has_links);
    CHECK(rig.ow.instance_model_id(armory) >= 0);

    // Blink letters as authored: east/middle boxes carry 0x2E (accum 0x28, no
    // indoors letter — the windowed side keeps the world rendering), the west
    // box 0x28 (accum 0x2E, indoors set).
    const uint32_t kEastLetters = 0x28, kMiddleLetters = 0x28, kWestLetters = 0x2E;

    { // East room (section 1), looking west through the 1<->3 doorway.
        const OcclusionFrameCamera cam = rig.camera(103.4, 99.8, 0.9, kEastLetters, -1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.camera_indoors());
        CHECK(rig.ow.building_visible(armory));
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & (1u << 1)) != 0); // own room
        CHECK((mask & (1u << 3)) != 0); // middle room through the doorway
        // The window lives in this room: the exterior stays reachable.
        CHECK((mask & 1u) != 0);
    }

    { // Middle room (section 3), looking east: room 1 through its doorway.
        const OcclusionFrameCamera cam = rig.camera(100.3, 99.8, 0.9, kMiddleLetters, +1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.camera_indoors());
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & (1u << 3)) != 0);
        CHECK((mask & (1u << 1)) != 0);
        // The window (south wall of room 1) sits outside the doorway wedge
        // from here: the exterior must NOT leak in (the black-void contract).
        CHECK((mask & 1u) == 0);
    }

    { // Middle room, looking west: room 2 through the 3<->2 doorway.
        const OcclusionFrameCamera cam = rig.camera(100.3, 99.8, 0.9, kMiddleLetters, -1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & (1u << 3)) != 0);
        CHECK((mask & (1u << 2)) != 0);
    }

    { // West room (section 2), looking east.
        const OcclusionFrameCamera cam = rig.camera(96.3, 100.2, 0.9, kWestLetters, +1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.camera_indoors());
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & (1u << 2)) != 0);
        CHECK((mask & (1u << 3)) != 0); // middle through the doorway
        CHECK((mask & 1u) == 0);        // no exterior leak from the deep room
    }

    { // Standing in the 1<->3 doorway: both rooms' boxes hit, both seeded.
        const OcclusionFrameCamera cam = rig.camera(101.1, 101.2, 0.9, kEastLetters, -1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(rig.ow.camera_indoors());
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & (1u << 1)) != 0);
        CHECK((mask & (1u << 3)) != 0);
    }

    { // Outside, east of the building looking at its windowless faces: the
      // south-wall window is backfacing from here, so the interior stays
      // hidden — exterior only.
        const OcclusionFrameCamera cam = rig.camera(110.0, 100.0, 1.0, 0, -1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(!rig.ow.camera_indoors());
        CHECK(rig.ow.exterior_visible());
        CHECK(rig.ow.building_visible(armory));
        CHECK(rig.ow.building_open_flagged(armory));
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & 1u) != 0);
        CHECK((mask & (1u << 1)) == 0); // the window faces away: no interior
    }

    { // Outside, SOUTH of the window (it faces -y): the outside-in traversal
      // sees through it into room 1; the open slot marks bit 31.
        const OcclusionFrameCamera cam = rig.camera(102.4, 90.0, 1.0, 0, +1);
        rig.ow.build_frame(rig.world, rig.cw, cam);
        CHECK(!rig.ow.camera_indoors());
        CHECK(rig.ow.exterior_visible());
        CHECK(rig.ow.building_visible(armory));
        const uint32_t mask = rig.ow.section_mask(armory);
        CHECK((mask & 1u) != 0);
        CHECK((mask & (1u << 1)) != 0); // through the window from outside
    }

    return failures - failures_before;
}

int main() {
    // The synthetic armory: five CB walls/roof, one CA trigger, one VC hull,
    // one CL ladder, three BB rooms (tests/fixtures/minimal_3di_gen.cpp).
    const std::string synthetic = OPENNOVA_ARMRY_FIXTURE;
    if (synthetic.empty()) {
        std::fprintf(stderr, "FAIL: no synthetic armory fixture path configured\n");
        return 1;
    }
    run_leg("synthetic", synthetic, VolumeShape{5, 1, 1, 1, 3});

    // The retail 00TRa armory: fourteen CBs, one CA, one VC, no ladder,
    // three BB rooms.
    const std::string assets = retail::assets();
    if (assets.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS for the retail Armry01.3di");
    } else {
        run_leg("retail", retail::asset_file("Armry01.3di"), VolumeShape{14, 0, 1, 1, 3});
    }

    if (failures == 0) std::printf("occlusion_armry: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
