#include "mesh_simp.h"
#include "trace.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace opennova {

// Global simplification state
MeshSimpGlobals g_simp = {};

// Float constants from IDA
static constexpr float CURVATURE_WEIGHT = 0.01f;      // flt_42A460 (set dynamically)
static constexpr float MAX_EDGE_THRESHOLD = 32.0f;    // flt_42A464
static constexpr float BOUNDARY_DOT_THRESHOLD = 0.999f; // dbl_424878 (approx)

// Global mutable curvature weight (flt_42A460 — set by callers)
float g_curvature_weight = 0.01f;

// Match sub_406F30 exactly: z² stored as float, (y²+z²) stored as float, x²+sum stays double, sqrt
// Original x87 uses fstp dword (float) for intermediate sums between sub_406F20 calls.
static inline double vec3_length_original(float x, float y, float z) {
    float sum_f = (float)((double)z * z);
    sum_f       = (float)((double)y * y + (double)sum_f);
    return std::sqrt((double)x * x + (double)sum_f);
}

// Match sub_407040 accumulation order: (bz*az + by*ay) + bx*ax — z-component first
// Original x87: fld bz; fmul az; fld by; fmul ay; faddp; fld bx; fmul ax; faddp
static inline double vec3_dot_original(float ax, float ay, float az,
                                        float bx, float by, float bz) {
    return ((double)bz * az + (double)by * ay) + (double)bx * ax;
}

// ===== Helper: dynamic array grow for pointer arrays =====
// Realloc helper (inlined pattern in original)

static void grow_ptr_array(void*** arr, int* count, int* capacity) {
    int new_cap = *capacity ? *capacity * 2 : 4;
    void** new_arr = (void**)std::malloc(sizeof(void*) * new_cap);
    std::memset(new_arr, 0, sizeof(void*) * new_cap);
    if (*arr) {
        int copy_count = std::min(*capacity, new_cap);
        std::memcpy(new_arr, *arr, sizeof(void*) * copy_count);
        std::free(*arr);
    }
    *arr = new_arr;
    *capacity = new_cap;
}

// ===== MeshVertex_Init (sub_4083E0) =====

MeshVertex* MeshVertex_Init(MeshVertex* v, int x, int y, int z, int id) {
    std::memset(v, 0, sizeof(MeshVertex));
    std::memcpy(&v->pos[0], &x, 4); // bitwise copy — these are float bits stored as int
    std::memcpy(&v->pos[1], &y, 4);
    std::memcpy(&v->pos[2], &z, 4);
    v->id = id;

    // Add to global vertex array
    if (g_simp.vertex_count == g_simp.vertex_capacity) {
        grow_ptr_array((void***)&g_simp.vertices, &g_simp.vertex_count, &g_simp.vertex_capacity);
    }
    g_simp.vertices[g_simp.vertex_count++] = v;

    return v;
}

// ===== MeshFace_ComputeNormal (sub_4075C0) =====

void MeshFace_ComputeNormal(MeshFace* f) {
    float* p0 = f->v[0]->pos;
    float* p1 = f->v[1]->pos;
    float* p2 = f->v[2]->pos;

    // edge1 = v2 - v1, edge2 = v1 - v0
    float e1[3] = {p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]};
    float e2[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};

    // cross product — x87: fld; fmul; fld; fmul; fsubp; fstp dword
    // Each a*b - c*d computed at 53-bit precision, stored to float
    float n[3];
    n[0] = (float)((double)e2[1] * e1[2] - (double)e2[2] * e1[1]);
    n[1] = (float)((double)e2[2] * e1[0] - (double)e2[0] * e1[2]);
    n[2] = (float)((double)e2[0] * e1[1] - (double)e2[1] * e1[0]);

    // IDA: calls sub_406F30 (with intermediate float truncation) for length check,
    // then sub_406F70 for normalization (which calls sub_406F30 again internally)
    double len = vec3_length_original(n[0], n[1], n[2]);
    if (len > 0.0) {
        if (len == 0.0) len = (double)0.1f; // flt_42486C: must match original's float constant
        f->normal[0] = (float)((double)n[0] / len);
        f->normal[1] = (float)((double)n[1] / len);
        f->normal[2] = (float)((double)n[2] / len);
    }
}

// ===== MeshFace_Init (sub_4071B0) =====

MeshFace* MeshFace_Init(MeshFace* f, MeshVertex* v0, MeshVertex* v1, MeshVertex* v2) {
    f->normal[0] = f->normal[1] = f->normal[2] = 0.0f;
    f->v[0] = v0;
    f->v[1] = v1;
    f->v[2] = v2;

    MeshFace_ComputeNormal(f);

    // Add to global face array
    if (g_simp.face_count == g_simp.face_capacity) {
        grow_ptr_array((void***)&g_simp.faces, &g_simp.face_count, &g_simp.face_capacity);
    }
    g_simp.faces[g_simp.face_count++] = f;

    // Add face to each vertex's face list, and build neighbor lists
    for (int i = 0; i < 3; i++) {
        MeshVertex* vi = f->v[i];

        // Add face to vertex's face list
        if (vi->face_count == vi->face_capacity) {
            grow_ptr_array((void***)&vi->faces_array, &vi->face_count, &vi->face_capacity);
        }
        // Note: faces_array is typed as MeshVertex** but actually stores MeshFace* pointers
        vi->faces_array[vi->face_count++] = reinterpret_cast<MeshVertex*>(f);

        // Add neighbors (other vertices in this face)
        for (int j = 0; j < 3; j++) {
            if (i == j) continue;
            MeshVertex* vj = f->v[j];

            // Check if already a neighbor
            bool found = false;
            for (int k = 0; k < vi->neighbor_count; k++) {
                if (vi->neighbors[k] == vj) { found = true; break; }
            }
            if (!found) {
                if (vi->neighbor_count == vi->neighbor_capacity) {
                    grow_ptr_array((void***)&vi->neighbors, &vi->neighbor_count, &vi->neighbor_capacity);
                }
                vi->neighbors[vi->neighbor_count++] = vj;
            }
        }
    }

    return f;
}

// ===== MeshSimp_RebuildVertices (sub_408E60) =====

void MeshSimp_RebuildVertices(const uint8_t* data_ptr, int count) {

    // Resize global vertex array
    if (count > 0) {
        if (g_simp.vertices) {
            void* new_arr = std::malloc(sizeof(void*) * count);
            std::memset(new_arr, 0, sizeof(void*) * count);
            int copy = std::min(g_simp.vertex_capacity, count);
            std::memcpy(new_arr, g_simp.vertices, sizeof(void*) * copy);
            std::free(g_simp.vertices);
            g_simp.vertices = static_cast<MeshVertex**>(new_arr);
        } else {
            g_simp.vertices = static_cast<MeshVertex**>(std::malloc(sizeof(void*) * count));
        }
        g_simp.vertex_capacity = count;
    } else {
        if (g_simp.vertices) std::free(g_simp.vertices);
        g_simp.vertices = nullptr;
        g_simp.vertex_capacity = 0;
    }
    g_simp.vertex_count = 0;

    // Create vertices from 12-byte records (3 ints/floats each)
    for (int i = 0; i < count; i++) {
        auto* v = static_cast<MeshVertex*>(std::malloc(sizeof(MeshVertex)));
        int x, y, z;
        std::memcpy(&x, data_ptr + 12 * i, 4);
        std::memcpy(&y, data_ptr + 12 * i + 4, 4);
        std::memcpy(&z, data_ptr + 12 * i + 8, 4);
        MeshVertex_Init(v, x, y, z, i);
    }
}

// ===== MeshSimp_RebuildFaces (sub_408F60) =====

void MeshSimp_RebuildFaces(const uint8_t* data_ptr, int count) {

    // Resize global face array
    if (count > 0) {
        if (g_simp.faces) {
            void* new_arr = std::malloc(sizeof(void*) * count);
            std::memset(new_arr, 0, sizeof(void*) * count);
            int copy = std::min(g_simp.face_capacity, count);
            std::memcpy(new_arr, g_simp.faces, sizeof(void*) * copy);
            std::free(g_simp.faces);
            g_simp.faces = static_cast<MeshFace**>(new_arr);
        } else {
            g_simp.faces = static_cast<MeshFace**>(std::malloc(sizeof(void*) * count));
        }
        g_simp.face_capacity = count;
    } else {
        if (g_simp.faces) std::free(g_simp.faces);
        g_simp.faces = nullptr;
        g_simp.face_capacity = 0;
    }
    g_simp.face_count = 0;

    // Create faces — indices reference the global vertex array
    for (int i = 0; i < count; i++) {
        int32_t idx[3];
        std::memcpy(idx, data_ptr + 12 * i, 12);

        auto* f = static_cast<MeshFace*>(std::malloc(sizeof(MeshFace)));
        MeshFace_Init(f, g_simp.vertices[idx[0]], g_simp.vertices[idx[1]], g_simp.vertices[idx[2]]);
    }
}

// ===== MeshSimp_ComputeMinCost (sub_408C10) =====

// Debug: track ComputeMinCost calls for specific vertex during diag
static int s_diag_rt_step = -1; // set from ReduceToTarget during diag

void MeshSimp_ComputeMinCost(MeshVertex* v) {
    if (v->neighbor_count == 0) {
        v->collapse_cost2 = 0.0f;
        v->collapse_cost = -0.01f;
        v->collapse_target = nullptr;
        return;
    }

    v->collapse_cost = 1000000.0f;
    v->collapse_cost2 = 1000000.0f;
    v->collapse_target = nullptr;

    for (int i = 0; i < v->neighbor_count; i++) {
        MeshVertex* n = v->neighbors[i];
        float cost2;
        double cost = MeshEdge_ComputeCostFull(v, n, &cost2);

        // Log per-neighbor cost for critical vertices near divergence point
        if (s_diag_rt_step >= 3100 && (v->id == 3803 || v->id == 1033)) {
            trace_log("{\"event\":\"diag_costfull\",\"step\":%d,\"vid\":%d,"
                      "\"neighbor\":%d,\"cost\":%.17g,\"cost2\":%.9g}",
                      s_diag_rt_step, v->id, n->id, cost, cost2);
        }

        if (cost < (double)v->collapse_cost) {
            v->collapse_cost = (float)cost;
            v->collapse_cost2 = cost2;
            v->collapse_target = n;
        }
    }

    // Log cost computation for critical vertices during diagnostic
    if (s_diag_rt_step >= 3100 && (v->id == 3803 || v->id == 1033)) {
        trace_log("{\"event\":\"diag_mincost\",\"step\":%d,\"vid\":%d,"
                  "\"cost\":%.9g,\"cost2\":%.9g,\"target_vid\":%d,\"neighbors\":%d}",
                  s_diag_rt_step, v->id, v->collapse_cost, v->collapse_cost2,
                  v->collapse_target ? v->collapse_target->id : -1, v->neighbor_count);
    }
}

// ===== MeshSimp_ComputeAllCosts (sub_408C90) =====

void MeshSimp_ComputeAllCosts() {
    for (int i = 0; i < g_simp.vertex_count; i++) {
        MeshSimp_ComputeMinCost(g_simp.vertices[i]);
    }
}

// ===== MeshSimp_FindMinCostVertex (sub_409030) =====

MeshVertex* MeshSimp_FindMinCostVertex() {
    MeshVertex* best = g_simp.vertices[0];
    bool found = false;

    for (int i = 0; i < g_simp.vertex_count; i++) {
        if (g_simp.vertices[i]->collapse_cost <= best->collapse_cost) {
            best = g_simp.vertices[i];
            found = true;
        }
    }

    return found ? best : nullptr;
}

// ===== MeshFace_MaxEdgeDelta (sub_4078D0) =====
float MeshFace_MaxEdgeDelta(MeshFace* f, MeshVertex* v_from, MeshVertex* v_to) {
    // Get the 3 vertex positions, replacing v_from with v_to
    float verts[3][3];
    for (int i = 0; i < 3; i++) {
        MeshVertex* src = (f->v[i] == v_from) ? v_to : f->v[i];
        verts[i][0] = src->pos[0];
        verts[i][1] = src->pos[1];
        verts[i][2] = src->pos[2];
    }

    float max_delta = 0.0f;
    // Check all 3 edges × 3 components = 9 comparisons
    for (int comp = 0; comp < 3; comp++) {
        float d;
        d = std::fabs(verts[0][comp] - verts[1][comp]);
        if (d > max_delta) max_delta = d;
        d = std::fabs(verts[1][comp] - verts[2][comp]);
        if (d > max_delta) max_delta = d;
        d = std::fabs(verts[2][comp] - verts[0][comp]);
        if (d > max_delta) max_delta = d;
    }
    return max_delta;
}

// ===== MeshEdge_ComputeNormalCost (sub_407710) =====
// Returns double — original returns via x87 st(0) at PC=2 precision.
double MeshEdge_ComputeNormalCost(MeshFace* f, MeshVertex* v_from, MeshVertex* v_to) {
    // Get modified vertex positions (float from struct)
    float p0[3], p1[3], p2[3];
    MeshVertex* mv0 = (f->v[0] == v_from) ? v_to : f->v[0];
    MeshVertex* mv1 = (f->v[1] == v_from) ? v_to : f->v[1];
    MeshVertex* mv2 = (f->v[2] == v_from) ? v_to : f->v[2];

    for (int i = 0; i < 3; i++) {
        p0[i] = mv0->pos[i];
        p1[i] = mv1->pos[i];
        p2[i] = mv2->pos[i];
    }

    // edge1 = v2 - v1, edge2 = v1 - v0 (float arrays, matching IDA stack vars)
    float e1[3] = {p2[0]-p1[0], p2[1]-p1[1], p2[2]-p1[2]};
    float e2[3] = {p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]};

    // cross product — x87: fld; fmul; fld; fmul; fsubp; fstp dword (sub_407060)
    float cross[3];
    cross[0] = (float)((double)e2[1]*e1[2] - (double)e2[2]*e1[1]);
    cross[1] = (float)((double)e2[2]*e1[0] - (double)e2[0]*e1[2]);
    cross[2] = (float)((double)e2[0]*e1[1] - (double)e2[1]*e1[0]);

    // Length via sub_406F30 (intermediate float truncation), normalize via sub_406F70
    double len = vec3_length_original(cross[0], cross[1], cross[2]);
    if (len == 0.0) return 0.0;

    float nx = (float)((double)cross[0] / len);
    float ny = (float)((double)cross[1] / len);
    float nz = (float)((double)cross[2] / len);

    // Final dot product — sub_407040 with z-first accumulation, returned via st(0)
    return vec3_dot_original(nx, ny, nz, f->normal[0], f->normal[1], f->normal[2]);
}

// ===== MeshFace_CurvatureChange (sub_407A90) =====
// Returns double — original returns via x87 st(0) at PC=2 precision.
double MeshFace_CurvatureChange(MeshFace* f, MeshVertex* v_from, MeshVertex* v_to) {
    // Get original 3 vertex positions
    float p0[3], p1[3], p2[3];
    for (int i = 0; i < 3; i++) {
        p0[i] = f->v[0]->pos[i];
        p1[i] = f->v[1]->pos[i];
        p2[i] = f->v[2]->pos[i];
    }

    // Scale ONLY Y (height) by 0.25 (flt_4244A8)
    // IDA: fmul + fstp dword — stored back to float
    p0[1] *= 0.25f;
    p1[1] *= 0.25f;
    p2[1] *= 0.25f;

    // Edge vectors stored to float arrays (IDA stack vars)
    float e01[3], e12[3], e20[3];
    for (int i = 0; i < 3; i++) {
        e01[i] = p1[i] - p0[i];
        e12[i] = p2[i] - p1[i];
        e20[i] = p0[i] - p2[i];
    }

    // Normalize via sub_406F70 which calls sub_406F30 for length.
    // sub_406F30 uses sub_406F20 with intermediate float truncation:
    //   z²→float, y²+z²→float, then sqrt(x² + float_sum) at double
    auto normalize3 = [](float v[3]) {
        double len = vec3_length_original(v[0], v[1], v[2]);
        if (len == 0.0) len = (double)0.1f; // flt_42486C: must match original's float constant
        v[0] = (float)((double)v[0] / len);
        v[1] = (float)((double)v[1] / len);
        v[2] = (float)((double)v[2] / len);
    };

    normalize3(e01);
    normalize3(e12);
    normalize3(e20);

    // IDA: sub_407020 negates first vector, then sub_407040 dots with z-first accumulation
    // Since (-a)*b = -(a*b) exactly in IEEE, result = -vec3_dot_original
    auto neg_dot = [](float a[3], float b[3]) -> double {
        return -vec3_dot_original(a[0], a[1], a[2], b[0], b[1], b[2]);
    };

    // orig_max/mod_max: stored as float on stack (fstp dword when new max found),
    // but comparisons happen at double (fcom compares x87 register vs float from stack)
    float orig_max = (float)neg_dot(e20, e01);
    double d = neg_dot(e01, e12);
    if (d > (double)orig_max) orig_max = (float)d;
    d = neg_dot(e12, e20);
    if (d > (double)orig_max) orig_max = (float)d;

    // Modified triangle (replace v_from with v_to)
    float mp0[3], mp1[3], mp2[3];
    for (int i = 0; i < 3; i++) {
        mp0[i] = (f->v[0] == v_from) ? v_to->pos[i] : f->v[0]->pos[i];
        mp1[i] = (f->v[1] == v_from) ? v_to->pos[i] : f->v[1]->pos[i];
        mp2[i] = (f->v[2] == v_from) ? v_to->pos[i] : f->v[2]->pos[i];
    }
    mp0[1] *= 0.25f;
    mp1[1] *= 0.25f;
    mp2[1] *= 0.25f;

    float me01[3], me12[3], me20[3];
    for (int i = 0; i < 3; i++) {
        me01[i] = mp1[i] - mp0[i];
        me12[i] = mp2[i] - mp1[i];
        me20[i] = mp0[i] - mp2[i];
    }

    normalize3(me01);
    normalize3(me12);
    normalize3(me20);

    float mod_max = (float)neg_dot(me20, me01);
    d = neg_dot(me01, me12);
    if (d > (double)mod_max) mod_max = (float)d;
    d = neg_dot(me12, me20);
    if (d > (double)mod_max) mod_max = (float)d;

    // mod_max^3 - orig_max^3: each fld loads from float, cubing at x87 precision,
    // subtract at x87, return via st(0) as double
    double mm = (double)mod_max;
    double om = (double)orig_max;
    return mm * mm * mm - om * om * om;
}

// ===== MeshEdge_ComputeCostFull (sub_408800) =====
// Returns double — original returns via x87 st(0) at PC=2 precision.
// All intermediates use double (matching x87 53-bit mantissa at PC=2).
// Variables stored as float match IDA's fstp dword truncation points.
double MeshEdge_ComputeCostFull(MeshVertex* v, MeshVertex* target, float* out_cost2) {
    // Compute edge length — IDA: sub_406F30 (with intermediate float truncation),
    // result stored as float via fstp [var_34]
    float dx = target->pos[0] - v->pos[0];
    float dy = target->pos[1] - v->pos[1];
    float dz = target->pos[2] - v->pos[2];
    float edge_len = (float)vec3_length_original(dx, dy, dz);

    *out_cost2 = 1e10f;

    // Collect shared faces (faces of v that also contain target)
    g_simp.temp_face_count = 0;

    for (int i = 0; i < v->face_count; i++) {
        auto* face = reinterpret_cast<MeshFace*>(v->faces_array[i]);
        if (face->v[0] == target || face->v[1] == target || face->v[2] == target) {
            if (g_simp.temp_face_count == g_simp.temp_face_capacity) {
                grow_ptr_array((void***)&g_simp.temp_faces,
                               &g_simp.temp_face_count, &g_simp.temp_face_capacity);
            }
            g_simp.temp_faces[g_simp.temp_face_count++] = face;
        }
    }

    // IDA: a2a = 0x3BA3D70A (0.005f), stored as float on stack
    float max_face_cost = 0.005f;
    // IDA: curvature_sum (var_3C) and min_quality (var_38) are float stack vars
    float curvature_sum = 0.0f;
    int curvature_count = 0;
    float min_quality = 1.0f;

    for (int fi = 0; fi < v->face_count; fi++) {
        auto* face = reinterpret_cast<MeshFace*>(v->faces_array[fi]);

        // Inner loop: compute min cost against shared faces
        // IDA: min_shared_cost (a4) is float on stack
        float min_shared_cost = 1.0f;
        for (int si = 0; si < g_simp.temp_face_count; si++) {
            float* fn = face->normal;
            float* sn = g_simp.temp_faces[si]->normal;
            // IDA: sub_407040 (dot) with z-first accumulation, returns via st(0)
            // Then: fsubr 1.0; fmul 0.5 — all at x87 precision
            double dot = vec3_dot_original(fn[0], fn[1], fn[2], sn[0], sn[1], sn[2]);
            double cost_d = (1.0 - dot) * 0.5;
            // IDA: fld [a4]; fcomp st(1) — compares float (promoted) against x87 intermediate
            // Then fstp [a4] stores as float
            if ((double)min_shared_cost >= cost_d) min_shared_cost = (float)cost_d;
        }

        // IDA: fld [a2]; fcomp [a4] — float vs float comparison
        // Then integer copy: mov [a2], [a4]
        if (max_face_cost <= min_shared_cost)
            max_face_cost = min_shared_cost;

        // Skip faces that contain target (they become degenerate)
        bool contains_target = (face->v[0] == target || face->v[1] == target || face->v[2] == target);
        if (contains_target) continue;

        // IDA: sub_407A90 returns via st(0), then fmul flt_42A460, fadd 1.0
        // scaled stays in x87 register for cubing, then fstp [var_3C] stores to float
        double curv = MeshFace_CurvatureChange(face, v, target);
        double scaled = curv * (double)g_curvature_weight + 1.0;
        float new_sum = (float)((double)curvature_sum + scaled * scaled * scaled);
        curvature_sum = new_sum;
        curvature_count++;

        // IDA: sub_407710 returns via st(0), fcom [var_38] compares x87 vs float
        double ncost = MeshEdge_ComputeNormalCost(face, v, target);
        if (ncost < (double)min_quality) min_quality = (float)ncost;

        // Max edge delta — precision-insensitive (float subtractions + fabs)
        float delta = MeshFace_MaxEdgeDelta(face, v, target);
        if (delta > 32.0f) {
            *out_cost2 = 1e9f;
            g_simp.temp_face_count = 0;
            return 1e10f;
        }
    }

    // IDA: if count > 0: fild [var_40]; fdivr [var_3C] → avg in st(0) at x87 precision
    //       else: fld 1.0
    // Then: fld [a2]; fmul [var_34]; fstp dword [edx] → cost2 stored as float
    // And:  fmul [a2]; fstp [a2] → cost stored as float
    double avg;
    if (curvature_count > 0) {
        avg = (double)curvature_sum / (double)curvature_count;
    } else {
        avg = 1.0;
    }

    // cost2 = max_face_cost * edge_len — both loaded from float, multiplied at x87, fstp dword
    *out_cost2 = (float)((double)max_face_cost * (double)edge_len);

    // cost = avg * max_face_cost — computed at x87, stored as float
    float cost = (float)(avg * (double)max_face_cost);

    // IDA: fld [var_38]; fcomp 0.5 — float promoted to x87 for comparison
    if ((double)min_quality < 0.5) {
        cost = 1.0f;
    }

    // Boundary handling
    bool v_boundary = MeshVertex_IsBoundary(v);
    bool t_boundary = MeshVertex_IsBoundary(target);
    // Diagnostic: log boundary status for critical pairs
    if (s_diag_rt_step >= 3145 && s_diag_rt_step <= 3146 && v->id == 3803 &&
        (target->id == 3804 || target->id == 3805)) {
        // Also log 3805's details if it's a neighbor
        MeshVertex* v3805 = nullptr;
        for (int di = 0; di < v->neighbor_count; di++) {
            if (v->neighbors[di]->id == 3805) { v3805 = v->neighbors[di]; break; }
        }
        bool v3805_bnd = v3805 ? MeshVertex_IsBoundary(v3805) : false;
        if (v3805 && target->id == 3804) {
            // Dump full topology of 3805
            char fbuf[1024] = {0}; int foff = 0;
            foff += snprintf(fbuf+foff, sizeof(fbuf)-foff, "[");
            for (int fi2 = 0; fi2 < v3805->face_count && foff < 900; fi2++) {
                auto* ff = reinterpret_cast<MeshFace*>(v3805->faces_array[fi2]);
                if (fi2) foff += snprintf(fbuf+foff, sizeof(fbuf)-foff, ",");
                foff += snprintf(fbuf+foff, sizeof(fbuf)-foff, "[%d,%d,%d]",
                    ff->v[0]->id, ff->v[1]->id, ff->v[2]->id);
            }
            foff += snprintf(fbuf+foff, sizeof(fbuf)-foff, "]");
            char nbuf[512] = {0}; int noff = 0;
            noff += snprintf(nbuf+noff, sizeof(nbuf)-noff, "[");
            for (int ni2 = 0; ni2 < v3805->neighbor_count; ni2++) {
                if (ni2) noff += snprintf(nbuf+noff, sizeof(nbuf)-noff, ",");
                noff += snprintf(nbuf+noff, sizeof(nbuf)-noff, "%d", v3805->neighbors[ni2]->id);
            }
            noff += snprintf(nbuf+noff, sizeof(nbuf)-noff, "]");
            trace_log("{\"event\":\"diag_v3805_topo\",\"step\":%d,"
                      "\"faces\":%s,\"neighbors\":%s,\"boundary\":%d}",
                      s_diag_rt_step, fbuf, nbuf, v3805_bnd?1:0);
        }
    }
    if (!v_boundary) {
        // Non-boundary vertex (LABEL_45): use cost as-is
    } else if (!g_simp.param || !t_boundary) {
        cost = 1e10f;
    } else {
        // Both boundary with param: check edge compatibility
        bool compatible = false;
        bool _diag_compat = s_diag_rt_step >= 3145 && s_diag_rt_step <= 3146 &&
                            v->id == 3803 && (target->id == 3804 || target->id == 3805);
        for (int ni = 0; ni < v->neighbor_count; ni++) {
            MeshVertex* n = v->neighbors[ni];
            if (n == target) continue;
            if (!MeshVertex_IsBoundary(n)) continue;

            // Normalize via sub_406F70 (calls sub_406F30 with intermediate float truncation)
            float cd[3] = {target->pos[0]-v->pos[0], target->pos[1]-v->pos[1], target->pos[2]-v->pos[2]};
            double cl = vec3_length_original(cd[0], cd[1], cd[2]);
            if (cl > 0.0) { cd[0]=(float)((double)cd[0]/cl); cd[1]=(float)((double)cd[1]/cl); cd[2]=(float)((double)cd[2]/cl); }

            float nd[3] = {n->pos[0]-v->pos[0], n->pos[1]-v->pos[1], n->pos[2]-v->pos[2]};
            double nl = vec3_length_original(nd[0], nd[1], nd[2]);
            if (nl > 0.0) { nd[0]=(float)((double)nd[0]/nl); nd[1]=(float)((double)nd[1]/nl); nd[2]=(float)((double)nd[2]/nl); }

            // IDA: sub_407040 dot (z-first), then fcomp dbl_424878 (double constant 0.999)
            double dot = vec3_dot_original(cd[0], cd[1], cd[2], nd[0], nd[1], nd[2]);
            if (_diag_compat) {
                trace_log("{\"event\":\"diag_compat\",\"step\":%d,\"vid\":%d,\"target\":%d,"
                          "\"neighbor\":%d,\"n_boundary\":1,\"dot\":%.17g,\"pass\":%d}",
                          s_diag_rt_step, v->id, target->id, n->id, dot, (dot > 0.999) ? 1 : 0);
            }
            if (dot > 0.999) {
                compatible = true;
                break;
            }
        }
        if (_diag_compat) {
            trace_log("{\"event\":\"diag_compat_result\",\"step\":%d,\"vid\":%d,\"target\":%d,"
                      "\"compatible\":%d}",
                      s_diag_rt_step, v->id, target->id, compatible ? 1 : 0);
        }
        if (!compatible) {
            cost = 1e10f;
        }
    }

    // IDA: fld [a2]; fmul [var_34] → returned via st(0) as double
    g_simp.temp_face_count = 0;
    return (double)cost * (double)edge_len;
}

// ===== MeshVertex_IsBoundary (sub_408720) =====
// Ported exactly from IDA. A vertex is boundary if any of its NEIGHBORS
// appears in only ONE of the vertex's faces (indicating an unshared edge).
bool MeshVertex_IsBoundary(MeshVertex* v) {
    if (v->neighbor_count <= 0) return false;

    for (int ni = 0; ni < v->neighbor_count; ni++) {
        MeshVertex* neighbor = v->neighbors[ni];
        if (!neighbor) continue;

        // Count how many of v's faces contain this neighbor
        int face_count_with_neighbor = 0;
        for (int fi = 0; fi < v->face_count; fi++) {
            auto* f = reinterpret_cast<MeshFace*>(v->faces_array[fi]);
            if (!f) continue;
            if (neighbor == f->v[0] || neighbor == f->v[1] || neighbor == f->v[2])
                face_count_with_neighbor++;
        }

        // If only 1 face shares this neighbor → edge is unshared → boundary
        if (face_count_with_neighbor == 1)
            return true;
    }
    return false;
}

// ===== MeshVertex_RemoveFromFaceNeighbors (sub_408690) =====
// IDA: Only removes the neighbor if no face of in_vertex still references v_to_remove.
// This preserves neighbor relationships when vertices share multiple faces.
void MeshVertex_RemoveFromFaceNeighbors(MeshVertex* v_to_remove, MeshVertex* in_vertex) {
    // Step 1: Check if v_to_remove is in in_vertex's neighbor list
    bool is_neighbor = false;
    for (int i = 0; i < in_vertex->neighbor_count; i++) {
        if (in_vertex->neighbors[i] == v_to_remove) {
            is_neighbor = true;
            break;
        }
    }
    if (!is_neighbor) return;

    // Step 2: Check if any face of in_vertex still references v_to_remove
    // IDA: scans faces_array (offset 28), checks face->v[0/1/2] against a2
    for (int i = 0; i < in_vertex->face_count; i++) {
        auto* f = reinterpret_cast<MeshFace*>(in_vertex->faces_array[i]);
        if (f->v[0] == v_to_remove || f->v[1] == v_to_remove || f->v[2] == v_to_remove) {
            return;  // Still share a face → keep neighbor relationship
        }
    }

    // Step 3: No shared face → remove v_to_remove from in_vertex's neighbor list
    for (int i = 0; i < in_vertex->neighbor_count; i++) {
        if (in_vertex->neighbors[i] == v_to_remove) {
            for (int j = i; j < in_vertex->neighbor_count - 1; j++) {
                in_vertex->neighbors[j] = in_vertex->neighbors[j + 1];
            }
            in_vertex->neighbor_count--;
            return;
        }
    }
}

// ===== MeshFace_Remove (sub_407480) =====
// Ported faithfully from IDA. In normal mode (simplify_mode==0):
// 1. Remove face from each vertex's face list
// 2. Remove from global face array
// 3. Remove neighbor relationships between the face's vertex pairs
void MeshFace_Remove(MeshFace* f) {
    if (g_simp.simplify_mode) {
        // Fast mode: just remove from global face array
        for (int i = 0; i < g_simp.face_count; i++) {
            if (g_simp.faces[i] == f) {
                for (int j = i; j < g_simp.face_count - 1; j++) {
                    g_simp.faces[j] = g_simp.faces[j + 1];
                }
                g_simp.face_count--;
                break;
            }
        }
        return;
    }

    // Normal mode: full cleanup

    // Step 1: Remove face from each vertex's face list (IDA lines 49-79)
    for (int i = 0; i < 3; i++) {
        MeshVertex* v = f->v[i];
        if (!v) continue;
        for (int j = 0; j < v->face_count; j++) {
            if (reinterpret_cast<MeshFace*>(v->faces_array[j]) == f) {
                for (int k = j; k < v->face_count - 1; k++) {
                    v->faces_array[k] = v->faces_array[k + 1];
                }
                v->face_count--;
                break;
            }
        }
    }

    // Step 2: Remove from global face array (IDA lines 81-104)
    for (int i = 0; i < g_simp.face_count; i++) {
        if (g_simp.faces[i] == f) {
            for (int j = i; j < g_simp.face_count - 1; j++) {
                g_simp.faces[j] = g_simp.faces[j + 1];
            }
            g_simp.face_count--;
            break;
        }
    }

    // Step 3: Remove neighbor relationships between adjacent vertex pairs
    // IDA lines 105-119: for each edge (v[i], v[(i+1)%3]), remove the neighbor link
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        if (f->v[i] && f->v[j]) {
            MeshVertex_RemoveFromFaceNeighbors(f->v[i], f->v[j]);
            MeshVertex_RemoveFromFaceNeighbors(f->v[j], f->v[i]);
        }
    }
}

// ===== MeshVertex_Destroy (sub_4084E0) =====
void MeshVertex_Destroy(MeshVertex* v) {
    if (g_simp.simplify_mode) {
        // Fast mode: just free arrays and remove from global list
        if (v->faces_array) std::free(v->faces_array);
        v->faces_array = nullptr;
        v->face_count = 0;
        v->face_capacity = 0;
        if (v->neighbors) std::free(v->neighbors);
        v->neighbors = nullptr;
        v->neighbor_count = 0;
        v->neighbor_capacity = 0;

        // Remove from global vertex array
        for (int i = 0; i < g_simp.vertex_count; i++) {
            if (g_simp.vertices[i] == v) {
                for (int j = i; j < g_simp.vertex_count - 1; j++) {
                    g_simp.vertices[j] = g_simp.vertices[j + 1];
                }
                g_simp.vertex_count--;
                break;
            }
        }
    } else {
        // Full mode: remove from all neighbor lists first
        // IDA sub_4084E0 does UNCONDITIONAL removal (no face check) —
        // it directly removes 'this' from each neighbor's list
        while (v->neighbor_count > 0) {
            MeshVertex* first_neighbor = v->neighbors[0];

            // Remove v from first_neighbor's neighbor list (unconditional)
            for (int i = 0; i < first_neighbor->neighbor_count; i++) {
                if (first_neighbor->neighbors[i] == v) {
                    for (int j = i; j < first_neighbor->neighbor_count - 1; j++) {
                        first_neighbor->neighbors[j] = first_neighbor->neighbors[j + 1];
                    }
                    first_neighbor->neighbor_count--;
                    break;
                }
            }

            // Remove first_neighbor from v's neighbor list (unconditional)
            for (int i = 0; i < v->neighbor_count; i++) {
                if (v->neighbors[i] == first_neighbor) {
                    for (int j = i; j < v->neighbor_count - 1; j++) {
                        v->neighbors[j] = v->neighbors[j + 1];
                    }
                    v->neighbor_count--;
                    break;
                }
            }
        }

        // Remove from global vertex array
        for (int i = 0; i < g_simp.vertex_count; i++) {
            if (g_simp.vertices[i] == v) {
                for (int j = i; j < g_simp.vertex_count - 1; j++) {
                    g_simp.vertices[j] = g_simp.vertices[j + 1];
                }
                g_simp.vertex_count--;
                break;
            }
        }
    }

    if (v->faces_array) std::free(v->faces_array);
    v->faces_array = nullptr;
    v->face_count = 0;
    v->face_capacity = 0;
    if (v->neighbors) std::free(v->neighbors);
    v->neighbors = nullptr;
    v->neighbor_count = 0;
    v->neighbor_capacity = 0;
}

// ===== MeshSimp_CollapseVertex (sub_408D40) =====
// Ported faithfully from IDA. When target is non-null, performs full edge
// collapse. When target is null, just destroys the vertex.
void MeshSimp_CollapseVertex(MeshVertex* v, MeshVertex* target) {
    if (target) {
        // IDA lines 21-32: save v's neighbors before modification
        std::vector<MeshVertex*> saved_neighbors;
        for (int i = 0; i < v->neighbor_count; i++) {
            saved_neighbors.push_back(v->neighbors[i]);
        }

        // IDA lines 34-42: remove degenerate faces (those containing both v and target)
        for (int i = v->face_count - 1; i >= 0; i--) {
            auto* f = reinterpret_cast<MeshFace*>(v->faces_array[i]);
            if ((target == f->v[0] || target == f->v[1] || target == f->v[2]) && f) {
                MeshFace_Remove(f);
                std::free(f);
            }
        }

        // IDA lines 44-48: replace v with target in remaining faces
        // IDA iterates back-to-front (esi = face_count-1, dec esi each iteration)
        for (int i = v->face_count - 1; i >= 0; i--) {
            MeshEdge_Collapse(reinterpret_cast<MeshFace*>(v->faces_array[i]), v, target);
        }

        // Diagnostic: log saved neighbors at step 3145 tile 2 to trace 3803 recomputation
        int _diag_vid = v->id, _diag_tid = target->id;
        if (s_diag_rt_step == 3145) {
            bool has_3803 = false;
            for (auto* n : saved_neighbors) {
                if (n->id == 3803) has_3803 = true;
            }
            trace_log("{\"event\":\"diag_saved_neighbors\",\"step\":%d,\"collapsed_vid\":%d,"
                      "\"target_vid\":%d,\"count\":%d,\"has_3803\":%d}",
                      s_diag_rt_step, _diag_vid, _diag_tid,
                      (int)saved_neighbors.size(), has_3803 ? 1 : 0);
        }

        // IDA lines 49-52: destroy the collapsed vertex
        MeshVertex_Destroy(v);
        std::free(v);
        for (auto* n : saved_neighbors) {
            MeshSimp_ComputeMinCost(n);
        }
    } else if (v) {
        // Null target: IDA calls MeshVertex_Destroy(v) + free(v).
        // MeshVertex_Destroy removes v from ALL neighbors' lists and from g_simp.vertices.
        // Faces containing v remain in g_simp.faces with stale v pointers.
        // This is safe: IsBoundary compares face->v[] against CURRENT neighbors,
        // and v is no longer anyone's neighbor, so the stale pointer never matches.
        // Face extraction later reads face->v[i]->id — we keep v alive (don't free)
        // so the id field remains readable.
        MeshVertex_Destroy(v);
    }
}

// ===== MeshEdge_Collapse (sub_408160) =====
void MeshEdge_Collapse(MeshFace* f, MeshVertex* v_old, MeshVertex* v_new) {
    // Replace v_old with v_new in this face
    for (int i = 0; i < 3; i++) {
        if (f->v[i] == v_old) {
            f->v[i] = v_new;
            break;
        }
    }

    // Remove face from v_old's face list
    for (int i = 0; i < v_old->face_count; i++) {
        if (reinterpret_cast<MeshFace*>(v_old->faces_array[i]) == f) {
            for (int j = i; j < v_old->face_count - 1; j++) {
                v_old->faces_array[j] = v_old->faces_array[j + 1];
            }
            v_old->face_count--;
            break;
        }
    }

    // Add face to v_new's face list
    if (v_new->face_count == v_new->face_capacity) {
        grow_ptr_array((void***)&v_new->faces_array, &v_new->face_count, &v_new->face_capacity);
    }
    v_new->faces_array[v_new->face_count++] = reinterpret_cast<MeshVertex*>(f);

    // Update neighbor lists
    for (int i = 0; i < 3; i++) {
        MeshVertex_RemoveFromFaceNeighbors(v_old, f->v[i]);
        MeshVertex_RemoveFromFaceNeighbors(f->v[i], v_old);
    }

    // Add new neighbor relationships
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            MeshVertex* a = f->v[i];
            MeshVertex* b = f->v[j];
            // Add if not already neighbors
            bool found = false;
            for (int k = 0; k < a->neighbor_count; k++) {
                if (a->neighbors[k] == b) { found = true; break; }
            }
            if (!found) {
                if (a->neighbor_count == a->neighbor_capacity)
                    grow_ptr_array((void***)&a->neighbors, &a->neighbor_count, &a->neighbor_capacity);
                a->neighbors[a->neighbor_count++] = b;
            }
            found = false;
            for (int k = 0; k < b->neighbor_count; k++) {
                if (b->neighbors[k] == a) { found = true; break; }
            }
            if (!found) {
                if (b->neighbor_count == b->neighbor_capacity)
                    grow_ptr_array((void***)&b->neighbors, &b->neighbor_count, &b->neighbor_capacity);
                b->neighbors[b->neighbor_count++] = a;
            }
        }
    }

    // Recompute face normal
    MeshFace_ComputeNormal(f);
}

// ===== MeshSimp_Init (sub_409080) =====

void MeshSimp_Init(const uint8_t* vertex_data, int vertex_count,
                   const uint8_t* face_data, int face_count, int param) {
    g_simp.param = param;
    g_simp.simplify_mode = 1;

    // Free existing arrays
    if (g_simp.faces) std::free(g_simp.faces);
    g_simp.faces = nullptr;
    g_simp.face_count = 0;
    g_simp.face_capacity = 0;

    if (g_simp.vertices) std::free(g_simp.vertices);
    g_simp.vertices = nullptr;
    g_simp.vertex_count = 0;
    g_simp.vertex_capacity = 0;

    g_simp.simplify_mode = 0;

    MeshSimp_RebuildVertices(vertex_data, vertex_count);
    MeshSimp_RebuildFaces(face_data, face_count);
    MeshSimp_ComputeAllCosts();
}

// ===== MeshSimp_ReduceToTarget (sub_409180) =====

// Ported from sub_409180.
// IDA: checks if v is non-null, then calls CollapseVertex(v, v->collapse_target).
// If target is null, CollapseVertex destroys the vertex (removes from array).
void MeshSimp_ReduceToTarget(int target) {
    int before_v = g_simp.vertex_count;
    int failures = 0;

    // Diagnostic: capture collapse sequence for first 2 leaf tiles' L0 (4225→332)
    static int s_diag_count = 0;
    bool diag_active = s_diag_count < 2 && before_v == 4225 && target == 332;
    int diag_step = 0;

    // Dump initial neighbor list for vertex 2898
    if (diag_active) {
        for (int di = 0; di < g_simp.vertex_count; di++) {
            MeshVertex* dv = g_simp.vertices[di];
            if (dv->id == 2898) {
                char nbuf[512] = {0};
                int noff = 0;
                noff += snprintf(nbuf + noff, sizeof(nbuf) - noff, "[");
                for (int ni = 0; ni < dv->neighbor_count; ni++) {
                    if (ni > 0) noff += snprintf(nbuf + noff, sizeof(nbuf) - noff, ",");
                    noff += snprintf(nbuf + noff, sizeof(nbuf) - noff, "%d", dv->neighbors[ni]->id);
                }
                noff += snprintf(nbuf + noff, sizeof(nbuf) - noff, "]");
                trace_log("{\"event\":\"diag_neighbors\",\"vid\":2898,\"count\":%d,\"neighbors\":%s}",
                          dv->neighbor_count, nbuf);
                break;
            }
        }
    }

    while (g_simp.vertex_count > target) {
        if (failures >= 10) break;
        if (diag_active) s_diag_rt_step = diag_step;
        MeshVertex* v = MeshSimp_FindMinCostVertex();
        if (v) {
            if (diag_active && v->collapse_cost > -1.0f) {
                trace_log("{\"event\":\"diag_collapse\",\"step\":%d,\"vid\":%d,"
                          "\"cost\":%.9g,\"cost2\":%.9g,\"target\":%d}",
                          diag_step, v->id, v->collapse_cost, v->collapse_cost2,
                          v->collapse_target ? v->collapse_target->id : -1);
            }
            // At divergence point, dump top candidates and vertex 1223's cost
            if (diag_active && diag_step == 2708) {
                // Find and log vertex 1223's cost
                for (int di = 0; di < g_simp.vertex_count; di++) {
                    MeshVertex* dv = g_simp.vertices[di];
                    if (dv->id == 1223 || dv->id == 1876) {
                        trace_log("{\"event\":\"diag_vertex\",\"step\":%d,\"vid\":%d,"
                                  "\"cost\":%.9g,\"cost2\":%.9g,\"target_vid\":%d}",
                                  diag_step, dv->id, dv->collapse_cost, dv->collapse_cost2,
                                  dv->collapse_target ? dv->collapse_target->id : -1);
                    }
                }
            }
            if (diag_active) diag_step++;
            MeshSimp_CollapseVertex(v, v->collapse_target);
        } else {
            failures++;
        }
    }
    if (diag_active) s_diag_count++;
    // Compute hash of remaining vertex IDs for divergence detection
    uint32_t vhash = 0;
    for (int i = 0; i < g_simp.vertex_count; i++) {
        uint32_t vid = (uint32_t)g_simp.vertices[i]->id;
        vhash ^= vid * 2654435761u; // Knuth multiplicative hash
    }
    trace_log("{\"event\":\"reduce_target\",\"target\":%d,\"before_v\":%d,\"after_v\":%d,\"vhash\":%u}",
              target, before_v, g_simp.vertex_count, vhash);
}

// ===== MeshSimp_ReduceWithThreshold (sub_409110) =====

void MeshSimp_ReduceWithThreshold(int target, float threshold) {
    int before_v = g_simp.vertex_count;
    int failures = 0;
    bool exceeded = false;
    int step = 0;

    while (!exceeded) {
        if (failures >= 10) break;

        MeshVertex* v = MeshSimp_FindMinCostVertex();
        if (!v) break;

        // Log per-step details for first 20 steps of each threshold call
        if (step < 20) {
            trace_log("{\"event\":\"thresh_step\",\"step\":%d,\"vid\":%d,"
                      "\"cost\":%.9g,\"cost2\":%.9g,\"threshold\":%.9g,"
                      "\"pass\":%d,\"target_vid\":%d}",
                      step, v->id, v->collapse_cost, v->collapse_cost2,
                      threshold, (v->collapse_cost2 <= threshold) ? 1 : 0,
                      v->collapse_target ? v->collapse_target->id : -1);
        }

        // Check cost against threshold (offset +44 = collapse_cost2)
        if (v->collapse_cost2 <= threshold) {
            if (v->collapse_target) {
                MeshSimp_CollapseVertex(v, v->collapse_target);
            } else {
                failures++;
            }
        } else {
            exceeded = true;
        }

        step++;

        // Stop if below 10% of target
        if (g_simp.vertex_count <= static_cast<int>(static_cast<double>(target) * 0.1)) {
            break;
        }
    }
    trace_log("{\"event\":\"reduce_thresh\",\"target\":%d,\"threshold\":%.9g,"
              "\"before_v\":%d,\"after_v\":%d}",
              target, threshold, before_v, g_simp.vertex_count);
}

} // namespace opennova

