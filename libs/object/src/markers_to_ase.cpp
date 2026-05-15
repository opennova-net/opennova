// libs/object/src/markers_to_ase.cpp
//
// Port of pyopennova/ase_from_3di3.py marker emission helpers:
//   _center_marker_objects  (lines 348-380)
//   _attach_marker_objects  (lines 382-406)
//   _userpoint_objects      (lines 408-455)
//
// Supporting helpers ported from the same file:
//   mtrx_to_center_rotation  (pyopennova/mesh_utils.py)
//   _direction_rot            (lines 917-927)
//   _userpoint_tm             (lines 929-966)
//   _userpoint_emit_order     (lines 968-1011)
//
// IR field mapping vs Python attribute names:
//   ir->lods[i]                     = self.ir.lods[i]
//   lod->render_objects[i].abs[]    = lod.parts[i].abs_position
//   lod->render_objects[i].parent_index = lod.parts[i].parent_index
//   lod->render_object_count        = lod.part_count
//   lod->part_animations[i]         = lod.part_animations[i]
//   lod->part_animation_count       = lod.part_animation_count
//   ir->mtrx.count                  = ir.matrix_count
//   ir->mtrx.matrices[i].m[]        = ir.matrices[i].m
//   ir->user_points[i]              = ir.userpoints[i]
//   ir->user_point_count            = ir.userpoint_count
//   up->x, up->y, up->z             = up.position[0..2]  (int32_t fixed-point /1000)
//   up->rot_x, rot_y, rot_z         = up.direction[0..2] (int32_t fixed-point /1000)
//   up->subobject_index             = up.part_index
//   up->userpoint_type              = up.type_code
//   up->name[]                      = up.name

#include "object/markers_to_ase.h"

#include "object/coords.h"    // object_render_space
#include "object/ase_vec.h"   // object_cube_mesh, object_tm_identity, object_tm_from_rot

#include "ase/ase.h"          // ase_alloc_object
#include "ase/types.h"        // ase_Object, ase_Face

#include "threedi/threedi_3di3.h"  // Threedi3di3, ThreediLod, ThreediUserPoint, ThreediPartAnimation

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Fill cube faces (12 tris) from raw int[36] into ase_Face array, with
// edge_visibility all 1 and material fields zeroed.
// material_ref on the object is set by caller (-1 for markers).
static void fill_cube_faces(ase_Face* faces, const int* raw_faces)
{
    for (int f = 0; f < 12; ++f) {
        ase_Face* af = &faces[f];
        af->vert[0] = raw_faces[f * 3 + 0];
        af->vert[1] = raw_faces[f * 3 + 1];
        af->vert[2] = raw_faces[f * 3 + 2];
        af->vert[3] = 0;
        af->edge_visibility[0] = 1;
        af->edge_visibility[1] = 1;
        af->edge_visibility[2] = 1;
        af->edge_visibility[3] = 0;
        af->smoothing_mask  = 0;
        af->material_id     = 0;
        af->material_index  = 0;
        af->uv[0] = af->uv[1] = af->uv[2] = af->uv[3] = 0;
        af->color[0] = af->color[1] = af->color[2] = 0;
        af->reserved1 = 0;
        af->reserved2 = 0;
    }
}

// Fill the 8 world-space vertices of a cube centered at origin (no rotation).
// out_verts[] must be size 8*3.
static void fill_cube_verts(float* out_verts, const float* local_verts, const float origin[3])
{
    for (int v = 0; v < 8; ++v) {
        out_verts[v * 3 + 0] = origin[0] + local_verts[v * 3 + 0];
        out_verts[v * 3 + 1] = origin[1] + local_verts[v * 3 + 1];
        out_verts[v * 3 + 2] = origin[2] + local_verts[v * 3 + 2];
    }
}

// mtrx_to_center_rotation: port of pyopennova/mesh_utils.py.
// mat_data: 16-float row-major matrix.
// out_rot: 3x3 in [row][col] order.
// Returns false if any NaN (zero-axis sentinel).
static bool mtrx_to_center_rotation(const float mat_data[16], float out_rot[3][3])
{
    for (int i = 0; i < 16; ++i) {
        if (std::isnan(mat_data[i]))
            return false;
    }
    const double g00 = mat_data[0];
    const double g01 = mat_data[1];
    const double g02 = mat_data[2];
    const double g10 = mat_data[4];
    const double g11 = mat_data[5];
    const double g12 = mat_data[6];
    const double g20 = mat_data[8];
    const double g21 = mat_data[9];
    const double g22 = mat_data[10];

    const double c00 = g11 * g22 - g12 * g21;
    const double c01 = -(g10 * g22 - g12 * g20);
    const double c02 = g10 * g21 - g11 * g20;
    const double c10 = -(g01 * g22 - g02 * g21);
    const double c11 = g00 * g22 - g02 * g20;
    const double c12 = -(g00 * g21 - g01 * g20);
    const double c20 = g01 * g12 - g02 * g11;
    const double c21 = -(g00 * g12 - g02 * g10);
    const double c22 = g00 * g11 - g01 * g10;
    const double det = g00 * c00 + g01 * c01 + g02 * c02;

    const double m00 = c00 / det;
    const double m01 = c10 / det;
    const double m02 = c20 / det;
    const double m10 = c01 / det;
    const double m11 = c11 / det;
    const double m12 = c21 / det;
    const double m20 = c02 / det;
    const double m21 = c12 / det;
    const double m22 = c22 / det;

    float ax0[3] = { (float)m22, (float)-m20, (float)m21 };
    float ax1[3] = { (float)-m02, (float)m00, (float)-m01 };
    float ax2[3] = { (float)m12, (float)-m10, (float)m11 };

    // Python returns:
    //   row0 = (ax1[1], -ax0[1],  ax2[1])
    //   row1 = (-ax1[0], ax0[0], -ax2[0])
    //   row2 = (ax1[2], -ax0[2],  ax2[2])
    out_rot[0][0] =  ax1[1]; out_rot[0][1] = -ax0[1]; out_rot[0][2] =  ax2[1];
    out_rot[1][0] = -ax1[0]; out_rot[1][1] =  ax0[0]; out_rot[1][2] = -ax2[0];
    out_rot[2][0] =  ax1[2]; out_rot[2][1] = -ax0[2]; out_rot[2][2] =  ax2[2];
    return true;
}

// Vector helpers (local to this TU).
static float vec3_len_sq(float x, float y, float z) { return x*x + y*y + z*z; }
static void  vec3_normalize(float v[3]) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-12f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}
static void vec3_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}
static float vec3_dot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

// _userpoint_tm — port of ase_from_3di3.py lines 929-965.
// pos: render-space translation (already swizzled).
// direction: raw int32_t rot_x/rot_y/rot_z from ThreediUserPoint (fixed-point / 65536).
// out_tm: 4x3 TM.
static void userpoint_tm(const float pos[3], const int32_t dir_int[3], float out_tm[4][3])
{
    // direction swizzle: Python's ThreediUserPoint.direction property returns
    //   (rot_y / 65536, rot_z / 65536, rot_x / 65536)  = (dir[0], dir[1], dir[2])
    // Python _userpoint_tm line 947-950:
    //   z_axis = (direction[0], -direction[2], direction[1]) then normalize
    //          = (rot_y/65536, -rot_x/65536, rot_z/65536)
    // Here dir_int = {rot_x, rot_y, rot_z} (raw fixed-point, scale = 65536).
    // Use double precision to match Python's float arithmetic (Python float = C double).
    // This ensures identical IEEE 754 negative-zero propagation through cross products,
    // matching pyopennova/ase_from_3di3.py::_userpoint_tm (lines 929-965).
    double d0 = dir_int[1] / 65536.0; // rot_y → direction[0] → z_axis[0]
    double d1 = dir_int[0] / 65536.0; // rot_x → direction[2] → z_axis[1] = -dir[2]
    double d2 = dir_int[2] / 65536.0; // rot_z → direction[1] → z_axis[2]

    // Python _userpoint_tm: z_axis = (direction[0], -direction[2], direction[1])
    double zx = d0, zy = -d1, zz = d2;

    // normalize
    double zlen = std::sqrt(zx*zx + zy*zy + zz*zz);
    if (zlen > 1e-12) { zx /= zlen; zy /= zlen; zz /= zlen; }
    else { zx = 0.0; zy = 0.0; zz = 1.0; }

    // world_up = (0,0,1); if aligned switch to (0,1,0)
    double upx = 0.0, upy = 0.0, upz = 1.0;
    if (std::fabs(zx*upx + zy*upy + zz*upz) > 0.999) {
        upx = 0.0; upy = 1.0; upz = 0.0;
    }

    // x_axis = cross(world_up, z_axis), normalized
    double xx = upy*zz - upz*zy;
    double xy = upz*zx - upx*zz;
    double xz = upx*zy - upy*zx;
    double xlen = std::sqrt(xx*xx + xy*xy + xz*xz);
    if (xlen > 1e-12) { xx /= xlen; xy /= xlen; xz /= xlen; }

    // y_axis = cross(z_axis, x_axis), normalized
    double yx = zy*xz - zz*xy;
    double yy = zz*xx - zx*xz;
    double yz = zx*xy - zy*xx;
    double ylen = std::sqrt(yx*yx + yy*yy + yz*yz);
    if (ylen > 1e-12) { yx /= ylen; yy /= ylen; yz /= ylen; }

    // Python _userpoint_tm (line 960-964) returns (x_axis, y_axis, z_axis, translation)
    // directly as TM rows — NOT transposed. This differs from _tm(rot=...) which
    // transposes. So we set TM rows directly rather than using object_tm_from_rot
    // (which does the transpose that _tm applies).
    out_tm[0][0] = (float)xx; out_tm[0][1] = (float)xy; out_tm[0][2] = (float)xz;  // ROW0 = x_axis
    out_tm[1][0] = (float)yx; out_tm[1][1] = (float)yy; out_tm[1][2] = (float)yz;  // ROW1 = y_axis
    out_tm[2][0] = (float)zx; out_tm[2][1] = (float)zy; out_tm[2][2] = (float)zz;  // ROW2 = z_axis
    out_tm[3][0] = pos[0];    out_tm[3][1] = pos[1];    out_tm[3][2] = pos[2];      // ROW3 = translation
}

// _userpoint_emit_order — port of ase_from_3di3.py lines 968-1011.
// Bubble-sort that pre-inverts OED ConvertToInternal's sort so the baked
// USRP records land in stock 3DI3 order.
struct UpDescriptor {
    int original_index;
    int subobj;
    int type_code;
    std::string name;
};

static std::vector<int> userpoint_emit_order(const Threedi3di3* ir)
{
    const int n = (int)ir->user_point_count;
    std::vector<UpDescriptor> arr(n);
    for (int i = 0; i < n; ++i) {
        const ThreediUserPoint* up = &ir->user_points[i];
        arr[i].original_index = i;
        arr[i].subobj     = up->subobject_index;
        arr[i].type_code  = up->userpoint_type;
        // Null-terminated; strip trailing NULs.
        arr[i].name = std::string(up->name, strnlen(up->name, sizeof(up->name)));
    }

    // O(n^2) bubble — mirrors Python exactly.
    for (int i = 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const UpDescriptor& a = arr[i];
            const UpDescriptor& b = arr[j];

            // casefold comparison (ASCII only; mirror of str.casefold).
            std::string na = a.name, nb = b.name;
            for (char& c : na) c = (char)std::tolower((unsigned char)c);
            for (char& c : nb) c = (char)std::tolower((unsigned char)c);

            bool cond = (a.subobj < b.subobj)
                     || (a.type_code > b.type_code)
                     || (na > nb);
            if (cond)
                std::swap(arr[i], arr[j]);
        }
    }

    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = arr[i].original_index;
    return idx;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" int object_emit_center_markers(
    const Threedi3di3* ir,
    int lod_index,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects || lod_index < 0 || (size_t)lod_index >= ir->lod_count)
        return 0;

    const ThreediLod* lod = &ir->lods[lod_index];
    const int n = (int)lod->render_object_count;
    if (n > out_capacity)
        return 0;

    // cube_mesh(0.015) in Python -> half = 0.0075 in C++.
    float local_verts[24];
    int   local_faces[36];
    object_cube_mesh(0.0075f, local_verts, local_faces);

    for (int i = 0; i < n; ++i) {
        ase_Object* obj = &out_objects[i];
        ase_alloc_object(obj, 8, 0, 12, 0, 0);

        // name: "_01 center", "_02 center", ...
        std::snprintf(obj->name, sizeof(obj->name), "_%02d center", i + 1);

        // parent: _export_name("PN{i+1:02d}") -> "{i+1:02d}"
        std::snprintf(obj->parent_name, sizeof(obj->parent_name), "%02d", i + 1);

        obj->node_id     = -1;
        obj->material_ref = -1;
        obj->skinned     = 0;

        // origin: render_space(lod->render_objects[i].abs)
        float origin[3];
        object_render_space(
            lod->render_objects[i].abs[0],
            lod->render_objects[i].abs[1],
            lod->render_objects[i].abs[2],
            origin);

        // TM: identity or matrix-derived rotation.
        float tm[4][3];
        bool has_rot = false;
        bool zero_axis = false;

        if (i < (int)lod->part_animation_count && ir->mtrx.count > 0) {
            const ThreediPartAnimation* pa = &lod->part_animations[i];
            uint8_t mi = pa->matrix_index;
            if (mi != 0xFF && (uint32_t)mi < ir->mtrx.count) {
                float rot[3][3];
                if (mtrx_to_center_rotation(ir->mtrx.matrices[mi].m, rot)) {
                    object_tm_from_rot(rot, origin, tm);
                    has_rot = true;
                } else {
                    // NaN matrix: zero-axis sentinel
                    zero_axis = true;
                }
            } else {
                zero_axis = true;
            }
        }

        if (!has_rot) {
            if (zero_axis) {
                // Python lines 363-369: all rotation rows zero.
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        tm[r][c] = 0.0f;
                tm[3][0] = origin[0];
                tm[3][1] = origin[1];
                tm[3][2] = origin[2];
            } else {
                object_tm_identity(origin, tm);
            }
        }

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                obj->tm_row[r][c] = tm[r][c];

        fill_cube_verts(obj->verts, local_verts, origin);
        fill_cube_faces(obj->faces, local_faces);
    }

    return n;
}

extern "C" int object_emit_attach_markers(
    const Threedi3di3* ir,
    int lod_index,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects || lod_index < 0 || (size_t)lod_index >= ir->lod_count)
        return 0;

    const ThreediLod* lod = &ir->lods[lod_index];

    // cube_mesh(0.012) in Python -> half = 0.006.
    float local_verts[24];
    int   local_faces[36];
    object_cube_mesh(0.006f, local_verts, local_faces);

    // Mirror _attach_marker_objects: iterate parts, skip i==0 or parent_index < 0.
    // Count per parent-index to build the a/b/c suffix.
    int counters[512] = {};  // indexed by parent_index (pi); safe for typical models.
    int emitted = 0;

    for (int i = 0; i < (int)lod->render_object_count; ++i) {
        const ThreediRenderObject* ro = &lod->render_objects[i];
        int pi = ro->parent_index;
        if (i == 0 || pi < 0)
            continue;
        if (emitted >= out_capacity)
            break;

        ase_Object* obj = &out_objects[emitted];
        ase_alloc_object(obj, 8, 0, 12, 0, 0);

        // Count suffix letter per parent-index pi.
        int cnt = (pi < 512) ? counters[pi] : 0;
        if (pi < 512) counters[pi]++;
        char suffix = (char)('a' + (cnt % 26));

        // name: "~{pi+1:02d}{suffix} attach"
        std::snprintf(obj->name, sizeof(obj->name), "~%02d%c attach", pi + 1, suffix);

        // parent: _export_name("PN{i+1:02d}") -> "{i+1:02d}"
        std::snprintf(obj->parent_name, sizeof(obj->parent_name), "%02d", i + 1);

        obj->node_id      = -1;
        obj->material_ref = -1;
        obj->skinned      = 0;

        // origin: render_space(ro->abs)
        float origin[3];
        object_render_space(ro->abs[0], ro->abs[1], ro->abs[2], origin);

        float tm[4][3];
        object_tm_identity(origin, tm);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                obj->tm_row[r][c] = tm[r][c];

        fill_cube_verts(obj->verts, local_verts, origin);
        fill_cube_faces(obj->faces, local_faces);

        ++emitted;
    }

    return emitted;
}

extern "C" int object_emit_part_dummies(
    const Threedi3di3* ir,
    int lod_index,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects || lod_index < 0 || (size_t)lod_index >= ir->lod_count)
        return 0;

    const ThreediLod* lod = &ir->lods[lod_index];
    const int n = (int)lod->render_object_count;
    if (n <= 0 || n > out_capacity)
        return 0;

    // Tiny cube (1mm - effectively invisible in viewport). Just enough geometry
    // for ase_write to emit a valid *GEOMOBJECT block.
    float local_verts[24];
    int   local_faces[36];
    object_cube_mesh(0.0005f, local_verts, local_faces);

    for (int i = 0; i < n; ++i) {
        ase_Object* obj = &out_objects[i];
        ase_alloc_object(obj, 8, 0, 12, 0, 0);

        // name: "PN01", "PN02", ...  OED's parser strips "PN" prefix to
        // recover the subobject index. Pre-deletion Python's _export_name
        // (ase_from_3di3.py:1078) implemented the same strip.
        std::snprintf(obj->name, sizeof(obj->name), "PN%02d", i + 1);

        // Top-level: subobject hierarchy roots have no parent.
        obj->parent_name[0] = '\0';

        obj->node_id      = -1;
        obj->material_ref = -1;
        obj->skinned      = 0;

        // origin: render_space(lod->render_objects[i].abs) - same swizzle as
        // object_emit_center_markers.
        float origin[3];
        object_render_space(
            lod->render_objects[i].abs[0],
            lod->render_objects[i].abs[1],
            lod->render_objects[i].abs[2],
            origin);

        float tm[4][3];
        object_tm_identity(origin, tm);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                obj->tm_row[r][c] = tm[r][c];

        fill_cube_verts(obj->verts, local_verts, origin);
        fill_cube_faces(obj->faces, local_faces);
    }

    return n;
}

extern "C" int object_emit_userpoint_markers(
    const Threedi3di3* ir,
    ase_Object* out_objects,
    int out_capacity)
{
    if (!ir || !out_objects)
        return 0;
    if (ir->user_point_count == 0)
        return 0;
    if (ir->lod_count == 0)
        return 0;

    // cube_mesh(0.015) in Python -> half = 0.0075.
    // (Note: Python uses 0.015 for userpoints, same as center markers.)
    float local_verts[24];
    int   local_faces[36];
    object_cube_mesh(0.0075f, local_verts, local_faces);

    // Compute emit order (pre-inverted OED bubble sort).
    std::vector<int> emit_order = userpoint_emit_order(ir);

    int emitted = 0;
    for (int idx : emit_order) {
        if (emitted >= out_capacity)
            break;

        const ThreediUserPoint* up = &ir->user_points[idx];
        ase_Object* obj = &out_objects[emitted];
        ase_alloc_object(obj, 8, 0, 12, 0, 0);

        // display_idx: part_index (clamp to 0 if < 0) for the NN part.
        int display_idx = up->subobject_index >= 0 ? up->subobject_index : 0;
        int type_code   = up->userpoint_type;

        // prefix: "UP{chr(type_code)}" if printable ASCII, else "USR".
        char prefix[8];
        if (type_code && type_code >= 32 && type_code <= 126) {
            prefix[0] = 'U'; prefix[1] = 'P';
            prefix[2] = (char)type_code; prefix[3] = '\0';
        } else {
            std::strncpy(prefix, "USR", sizeof(prefix));
        }

        // source_name: null-terminated name field.
        char source_name[18] = {};
        std::strncpy(source_name, up->name, 17);
        // strip trailing nulls (already handled by strncpy + zero init).

        // marker_name: "{prefix}{display_idx+1:02d}" optionally " {source_name}".
        if (source_name[0]) {
            std::snprintf(obj->name, sizeof(obj->name),
                          "%s%02d %s", prefix, display_idx + 1, source_name);
        } else {
            std::snprintf(obj->name, sizeof(obj->name),
                          "%s%02d", prefix, display_idx + 1);
        }

        // parent: _export_name("PN{part_index+1:02d}") if valid, else "".
        if (up->subobject_index >= 0) {
            std::snprintf(obj->parent_name, sizeof(obj->parent_name),
                          "%02d", up->subobject_index + 1);
        } else {
            obj->parent_name[0] = '\0';
        }

        obj->node_id      = -1;
        obj->material_ref = -1;
        obj->skinned      = 0;

        // pos: swizzle raw int32 fixed-point (/ 65536) to render space.
        // Python ThreediUserPoint.position property (threedi_ffi.py line 333):
        //   position = (y / 65536, z / 65536, x / 65536)  = (pos[0], pos[1], pos[2])
        // Python line 441: pos = (float(up.position[0]), -float(up.position[2]), float(up.position[1]))
        //   = (y/65536, -x/65536, z/65536)
        // IMPORTANT: divide FIRST then negate, so negation applies to float not int.
        // C++ -(int)0 = 0 (int), losing negative zero; (float)x / 65536 negated later preserves -0.0.
        float px = (float)up->y / 65536.0f;
        float py_raw = (float)up->x / 65536.0f; float py = -py_raw; // negate float, not int
        float pz = (float)up->z / 65536.0f;
        float pos[3] = { px, py, pz };

        // direction raw ints: rot_x, rot_y, rot_z.
        int32_t dir_int[3] = { up->rot_x, up->rot_y, up->rot_z };

        float tm[4][3];
        userpoint_tm(pos, dir_int, tm);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                obj->tm_row[r][c] = tm[r][c];

        fill_cube_verts(obj->verts, local_verts, pos);
        fill_cube_faces(obj->faces, local_faces);

        ++emitted;
    }

    return emitted;
}
