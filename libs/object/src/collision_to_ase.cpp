// libs/object/src/collision_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py::_collision_helper_objects (lines 457-530).
//
// Supporting helpers ported from the same file / pyopennova/scene_naming.py:
//   build_volume_name    (scene_naming.py::build_volume_name)
//   blinkbox_suffix      (scene_naming.py::blinkbox_enabled_suffix)
//   format_dup_suffix    (scene_naming.py::format_duplicate_suffix)
//
// IR field mapping:
//   Python                              C (Threedi3di3)
//   ir.collision[0].contents           ir->collision   (ThreediCollisionModel*)
//   coll.contents.volume_count         ir->collision->volume_count
//   coll.contents.volumes[i]           ir->collision->volumes[i]  (ThreediBoundingVolume)
//   vol.type                           vol.collidable_type
//   vol.flags                          vol.flags
//   vol.min = (x_fp16/65536 ...)       vol.min_x_fp16 / 65536.0f  (3 components)
//   vol.max                            vol.max_x_fp16 / 65536.0f
//   vol.plane_count                    vol.plane_count
//   coll.contents.planes[i]            ir->collision->planes[i]  (ThreediBoundingPlane)
//   plane.normal                       plane.normal[3]
//   plane.distance                     plane.radius  (Python property alias)
//   coll.contents.object_count         ir->collision->object_count
//   coll.objects[i].num_bvols         ir->collision->objects[i].num_bounding_volumes
//   ir.lods[0].part_count              ir->lods[0].render_object_count
//
// collision_space: (x,y,z) -> (y, -x, z)   [pyopennova/coords.py line 47]

#include "object/collision_to_ase.h"

#include "object/ase_vec.h"     // object_tm_identity
#include "object/polyhedron.h"  // polyhedron_compute

#include "ase/ase.h"            // ase_alloc_object
#include "ase/types.h"          // ase_Object, ase_Face

#include "threedi/threedi_3di3.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>
#include <string>

// ---------------------------------------------------------------------------
// Coordinate conversion
// collision_space: (x, y, z) -> (y, -x, z)
// ---------------------------------------------------------------------------

static inline void collision_space(float x, float y, float z, float out[3]) {
    out[0] = y;
    out[1] = -x;
    out[2] = z;
}

// fp16.16 fixed-point to float.
static inline float fp16(int32_t v) {
    return (float)v / 65536.0f;
}

// ---------------------------------------------------------------------------
// Name-building helpers
// Port of pyopennova/scene_naming.py
// ---------------------------------------------------------------------------

// blinkbox_enabled_suffix(flags) — bits ~flags & 0x3E
// Appends letters for each enabled bit: V(1), S(2), W(3), L(4), O(5).
static std::string blinkbox_suffix(int flags) {
    int enabled = (~flags) & 0x3E;
    if (enabled == 0) return "";
    std::string out;
    if (enabled & (1 << 1)) out += 'V';
    if (enabled & (1 << 2)) out += 'S';
    if (enabled & (1 << 3)) out += 'W';
    if (enabled & (1 << 4)) out += 'L';
    if (enabled & (1 << 5)) out += 'O';
    return out;
}

// format_duplicate_suffix(occurrence) — "" for <=1, "a","b",...,"z","aa",... for 2+.
static std::string format_dup_suffix(int occurrence) {
    if (occurrence <= 1) return "";
    int index = occurrence - 1;
    std::string out;
    while (index > 0) {
        index -= 1;
        out = char('a' + (index % 26)) + out;
        index /= 26;
    }
    return out;
}

// CollisionType enum values (scene_naming.py lines 42-45).
// CB=1 CS=2 CC=3 CL=4 CV=5 CA=6 VC=7 BB=8 CD=9 CT=10 CM=11 VK=12 CF=13 LP=14 CP=19
static const char* collision_type_name(int type_code) {
    switch (type_code) {
        case  1: return "CB";
        case  2: return "CS";
        case  3: return "CC";
        case  4: return "CL";
        case  5: return "CV";
        case  6: return "CA";
        case  7: return "VC";
        case  8: return "BB";
        case  9: return "CD";
        case 10: return "CT";
        case 11: return "CM";
        case 12: return "VK";
        case 13: return "CF";
        case 14: return "LP";
        case 19: return "CP";
        default: return "CX";
    }
}

// build_volume_name(type_code, flags, index, occurrence) -> string.
// Port of scene_naming.py::build_volume_name (lines 110-122).
// index<0 => display_index=0; occurrence drives the dup suffix.
static std::string build_volume_name(int type_code, int flags, int index, int occurrence) {
    std::string base = collision_type_name(type_code);
    if (type_code == 8) {  // BB
        std::string sfx = blinkbox_suffix(flags);
        if (!sfx.empty()) base += sfx;
    }
    int display = (index >= 0) ? index : 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d", display + 1);
    return base + buf + format_dup_suffix(occurrence);
}

// ---------------------------------------------------------------------------
// collision_volume_metadata
// Port of pyopennova/model_access.py::collision_volume_metadata (lines 38-63).
// Returns per-volume owner object index and plane start.
// ---------------------------------------------------------------------------

static void collision_volume_metadata(
    const ThreediCollisionModel* coll,
    std::vector<int>& owners,
    std::vector<int>& plane_starts)
{
    int vol_count = (int)coll->volume_count;
    owners.assign(vol_count, -1);
    plane_starts.assign(vol_count, -1);

    int volume_cursor = 0;
    int plane_cursor = 0;

    for (int object_idx = 0; object_idx < (int)coll->object_count; ++object_idx) {
        const ThreediCollisionObject* obj = &coll->objects[object_idx];
        int volume_count = obj->num_bounding_volumes;
        if (volume_count < 0) volume_count = 0;

        for (int v = 0; v < volume_count; ++v) {
            if (volume_cursor >= vol_count) break;
            owners[volume_cursor] = object_idx;
            plane_starts[volume_cursor] = plane_cursor;
            int pc = (int)coll->volumes[volume_cursor].plane_count;
            if (pc < 0) pc = 0;
            plane_cursor += pc;
            volume_cursor++;
        }
    }
    // remaining volumes with no owner
    while (volume_cursor < vol_count) {
        plane_starts[volume_cursor] = plane_cursor;
        int pc = (int)coll->volumes[volume_cursor].plane_count;
        if (pc < 0) pc = 0;
        plane_cursor += pc;
        volume_cursor++;
    }
}

// ---------------------------------------------------------------------------
// Fan-triangulate a polygon face.
// Port of _triangulate (ase_from_3di3.py lines 1014-1021).
// ---------------------------------------------------------------------------
static void triangulate_poly(const int* face_verts, int face_len,
                             std::vector<std::array<int,3>>& out_tris) {
    if (face_len < 3) return;
    for (int i = 1; i < face_len - 1; ++i)
        out_tris.push_back({ face_verts[0], face_verts[i], face_verts[i + 1] });
}

// ---------------------------------------------------------------------------
// Fill an ase_Object from vertices + triangles.
// Mirrors Python dict -> ase_Object path used in flat_mesh_to_ase.
// smoothing_group is set to `sg` for all faces.
// ---------------------------------------------------------------------------
static void fill_ase_object(
    ase_Object* obj,
    const char* name,
    const char* parent,
    const float center[3],
    const std::vector<std::array<float, 3>>& world_verts,
    const std::vector<std::array<int, 3>>& tris,
    int smoothing_group)
{
    int vert_count = (int)world_verts.size();
    int face_count = (int)tris.size();
    ase_alloc_object(obj, vert_count, 0, face_count, 0, 0);

    std::strncpy(obj->name, name, 63);
    obj->name[63] = '\0';
    std::strncpy(obj->parent_name, parent, 63);
    obj->parent_name[63] = '\0';

    float tm[4][3];
    object_tm_identity(center, tm);
    std::memcpy(obj->tm_row, tm, sizeof(tm));

    obj->node_id = -1;       // Python line 727: spec.get("node_id", -1) default is -1
    obj->material_ref = -1;
    obj->skinned = 0;
    obj->weight_count = 0;
    obj->uv_count = 0;

    for (int i = 0; i < vert_count; ++i) {
        obj->verts[i * 3 + 0] = world_verts[i][0];
        obj->verts[i * 3 + 1] = world_verts[i][1];
        obj->verts[i * 3 + 2] = world_verts[i][2];
    }

    for (int f = 0; f < face_count; ++f) {
        ase_Face* af = &obj->faces[f];
        std::memset(af, 0, sizeof(ase_Face));
        af->vert[0] = tris[f][0];
        af->vert[1] = tris[f][1];
        af->vert[2] = tris[f][2];
        af->vert[3] = 0;
        af->edge_visibility[0] = 1;
        af->edge_visibility[1] = 1;
        af->edge_visibility[2] = 1;
        af->smoothing_mask = (uint32_t)smoothing_group;
        af->material_id = 0;
        af->material_index = 0;
    }
}

// ---------------------------------------------------------------------------
// object_emit_collision_helpers
// Port of _collision_helper_objects (ase_from_3di3.py lines 457-530).
// ---------------------------------------------------------------------------

extern "C" int object_emit_collision_helpers(
    const Threedi3di3* ir,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects || out_capacity <= 0)
        return 0;
    if (!ir->collision || ir->lod_count == 0)
        return 0;

    const ThreediCollisionModel* coll = ir->collision;
    const ThreediLod* lod0 = &ir->lods[0];
    int part_count = (int)lod0->render_object_count;

    std::vector<int> volume_owners, volume_plane_starts;
    collision_volume_metadata(coll, volume_owners, volume_plane_starts);

    // counters keyed by (type, key_index) — mirrors Python dict[(int,int)]
    std::map<std::pair<int,int>, int> counters;

    int emitted = 0;

    for (int vol_idx = 0; vol_idx < (int)coll->volume_count; ++vol_idx) {
        if (emitted >= out_capacity) break;

        const ThreediBoundingVolume* vol = &coll->volumes[vol_idx];
        int owner_idx = (vol_idx < (int)volume_owners.size()) ? volume_owners[vol_idx] : -1;
        int plane_start = (vol_idx < (int)volume_plane_starts.size()) ? volume_plane_starts[vol_idx] : -1;

        // parent name: _export_name("PN{owner_idx+1:02d}") strips "PN" -> "01"
        char parent[64] = "";
        if (owner_idx >= 0 && owner_idx < part_count)
            std::snprintf(parent, sizeof(parent), "%02d", owner_idx + 1);

        // volume min/max in collision_space
        // vol.min = (min_x_fp16/65536, min_y_fp16/65536, min_z_fp16/65536)  (x,y,z)
        // collision_space(x,y,z) = (y, -x, z)
        float raw_min[3] = { fp16(vol->min_x_fp16), fp16(vol->min_y_fp16), fp16(vol->min_z_fp16) };
        float raw_max[3] = { fp16(vol->max_x_fp16), fp16(vol->max_y_fp16), fp16(vol->max_z_fp16) };
        float world_min[3], world_max[3];
        collision_space(raw_min[0], raw_min[1], raw_min[2], world_min);
        collision_space(raw_max[0], raw_max[1], raw_max[2], world_max);

        float world_center[3] = {
            (world_min[0] + world_max[0]) * 0.5f,
            (world_min[1] + world_max[1]) * 0.5f,
            (world_min[2] + world_max[2]) * 0.5f,
        };

        // counters key
        int name_index = owner_idx;
        int key_index = (name_index >= 0) ? name_index : -1;
        auto key = std::make_pair((int)vol->collidable_type, key_index);
        counters[key]++;
        int occurrence = counters[key];

        // mesh_name = build_volume_name(...) + "-colonly"
        std::string mesh_name = build_volume_name(
            (int)vol->collidable_type,
            (int)vol->flags,
            name_index,
            occurrence) + "-colonly";

        // Build halfspaces in collision_space.
        std::vector<float> halfspaces;
        bool has_planes = (vol->plane_count > 0
                           && plane_start >= 0
                           && plane_start + vol->plane_count <= (int)coll->plane_count);
        if (has_planes) {
            for (int p = 0; p < vol->plane_count; ++p) {
                const ThreediBoundingPlane* plane = &coll->planes[plane_start + p];
                float n[3];
                collision_space(plane->normal[0], plane->normal[1], plane->normal[2], n);
                // halfspace: (nx, ny, nz, d) where d = plane->radius (Python: plane.distance)
                halfspaces.push_back(n[0]);
                halfspaces.push_back(n[1]);
                halfspaces.push_back(n[2]);
                halfspaces.push_back(plane->radius);
            }
        }

        // Try polyhedron; fall back to cube.
        std::vector<std::array<float, 3>> world_verts;
        std::vector<std::array<int, 3>> tri_faces;

        bool used_polyhedron = false;
        if (!halfspaces.empty()) {
            int hs_count = vol->plane_count;
            // Worst-case buffer sizes.
            int max_verts = hs_count * hs_count * hs_count + 8;
            int max_face_ints = hs_count * (hs_count + 2);
            std::vector<float> ph_verts(max_verts * 3);
            std::vector<int>   ph_faces(max_face_ints);
            int ph_vert_count = 0, ph_face_ints = 0, ph_face_count = 0;

            int ok = polyhedron_compute(
                halfspaces.data(), hs_count,
                ph_verts.data(), &ph_vert_count,
                ph_faces.data(), &ph_face_ints, &ph_face_count);

            if (ok && ph_vert_count >= 4) {
                // Build world_verts from polyhedron output.
                world_verts.resize(ph_vert_count);
                for (int i = 0; i < ph_vert_count; ++i) {
                    world_verts[i][0] = ph_verts[i * 3 + 0];
                    world_verts[i][1] = ph_verts[i * 3 + 1];
                    world_verts[i][2] = ph_verts[i * 3 + 2];
                }

                // Decode packed face buffer and triangulate.
                // local_vertices = [v - world_center for v in polyhedron_verts]
                // But polyhedron already returns world-space verts (no offset applied).
                // Python then: world_vertices = [center + local for local in local_verts]
                // where local_verts = [poly_v - center for poly_v in poly_verts].
                // Net effect: world_vertices == poly_verts. We keep them as-is.

                int cursor = 0;
                for (int f = 0; f < ph_face_count; ++f) {
                    int sz = ph_faces[cursor++];
                    std::vector<int> poly(sz);
                    for (int j = 0; j < sz; ++j)
                        poly[j] = ph_faces[cursor++];
                    triangulate_poly(poly.data(), sz, tri_faces);
                }

                used_polyhedron = !tri_faces.empty();
            }
        }

        if (!used_polyhedron) {
            // Fallback: axis-aligned box (Python lines 500-518).
            float half[3] = {
                std::fabs((world_max[0] - world_min[0]) * 0.5f),
                std::fabs((world_max[1] - world_min[1]) * 0.5f),
                std::fabs((world_max[2] - world_min[2]) * 0.5f),
            };
            if (half[0] <= 0.0f && half[1] <= 0.0f && half[2] <= 0.0f)
                continue;

            // 8 vertices: all sign combinations of ±half[0/1/2]
            // Python order: for s0 in (-1,1) for s1 in (-1,1) for s2 in (-1,1)
            world_verts.resize(8);
            {
                int vi = 0;
                static const int signs[2] = {-1, 1};
                for (int i0 = 0; i0 < 2; ++i0)
                for (int i1 = 0; i1 < 2; ++i1)
                for (int i2 = 0; i2 < 2; ++i2) {
                    world_verts[vi][0] = world_center[0] + signs[i0] * half[0];
                    world_verts[vi][1] = world_center[1] + signs[i1] * half[1];
                    world_verts[vi][2] = world_center[2] + signs[i2] * half[2];
                    ++vi;
                }
            }

            // 6 quads -> 12 tris (fan triangulation).
            // Python quads: (0,2,3,1),(4,5,7,6),(0,1,5,4),(2,6,7,3),(0,4,6,2),(1,3,7,5)
            static const int quads[6][4] = {
                {0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}
            };
            for (auto& q : quads) {
                int poly[4] = {q[0], q[1], q[2], q[3]};
                triangulate_poly(poly, 4, tri_faces);
            }
        }

        if (tri_faces.empty()) continue;

        fill_ase_object(
            &out_objects[emitted],
            mesh_name.c_str(),
            parent,
            world_center,
            world_verts,
            tri_faces,
            1);  // smoothing_group=1 (Python line 528)

        ++emitted;
    }

    return emitted;
}
