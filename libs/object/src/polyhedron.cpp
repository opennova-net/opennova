// libs/object/src/polyhedron.cpp
//
// Port of pyopennova/polyhedron.py::compute_polyhedron_controlled
// and its internal helpers (_build_faces_from_halfspaces, _dot, _cross,
// _add, _sub, _scale, _dist_sq, _normalize).
//
// Halfspace convention: n . x + d <= 0.
// Python: offsets[i] = -hs[3]  and  check: dot(n, p) > offset + eps  (outside)
//
// C mapping:
//   halfspaces[] = { nx, ny, nz, d }   (4 floats per plane)
//   offset[i]    = -halfspaces[i*4 + 3]
//   inside check: dot(n, p) > offset + eps  => outside

#include "object/polyhedron.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

static const double EPSILON = 1e-6;

// ---------------------------------------------------------------------------
// Internal vector helpers (double precision, matching Python float arithmetic)
// ---------------------------------------------------------------------------

static inline void v3_cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline double v3_dot(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void v3_add(const double a[3], const double b[3], double out[3]) {
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
}

static inline void v3_scale(const double a[3], double s, double out[3]) {
    out[0] = a[0] * s;
    out[1] = a[1] * s;
    out[2] = a[2] * s;
}

static inline double v3_dist_sq(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx*dx + dy*dy + dz*dz;
}

static inline void v3_normalize(const double v[3], double out[3]) {
    double n = std::sqrt(v3_dot(v, v));
    if (n == 0.0) { out[0] = out[1] = out[2] = 0.0; return; }
    out[0] = v[0] / n; out[1] = v[1] / n; out[2] = v[2] / n;
}

// ---------------------------------------------------------------------------
// _build_faces_from_halfspaces
//
// Port of pyopennova/polyhedron.py lines 85-128.
// Groups hull vertices by halfspace plane; emits one wound polygon per plane.
//
// pts        — array of vert_count vertices (3 doubles each, row-major)
// normals    — hs_count normals (3 doubles each)
// offsets    — hs_count offsets = -d
// Writes face data into faces_out and face_sizes_out.
// Returns number of faces emitted.
// ---------------------------------------------------------------------------

static int build_faces_from_halfspaces(
    const std::vector<std::array<double, 3>>& pts,
    const std::vector<std::array<double, 3>>& normals,
    const std::vector<double>& offsets,
    std::vector<std::vector<int>>& out_faces)
{
    const double plane_eps = EPSILON * 10.0;
    const int hs_count = (int)normals.size();

    out_faces.clear();

    for (int i = 0; i < hs_count; ++i) {
        const double* n = normals[i].data();
        double d = offsets[i];

        // Collect indices of pts on this plane.
        std::vector<int> on_plane;
        for (int vi = 0; vi < (int)pts.size(); ++vi) {
            const double* p = pts[vi].data();
            if (std::fabs(v3_dot(n, p) - d) < plane_eps)
                on_plane.push_back(vi);
        }
        if ((int)on_plane.size() < 3)
            continue;

        // Local 2D basis on the plane.
        // ref: (1,0,0) if |n.x| < 0.9 else (0,1,0).
        double ref[3] = { (std::fabs(n[0]) < 0.9) ? 1.0 : 0.0,
                          (std::fabs(n[0]) < 0.9) ? 0.0 : 1.0,
                          0.0 };
        double u_raw[3], u[3], v[3];
        v3_cross(n, ref, u_raw);
        v3_normalize(u_raw, u);
        v3_cross(n, u, v);

        // 2D projected coords.
        int sz = (int)on_plane.size();
        std::vector<double> px(sz), py(sz);
        double cx = 0.0, cy = 0.0;
        for (int j = 0; j < sz; ++j) {
            const double* p = pts[on_plane[j]].data();
            px[j] = v3_dot(p, u);
            py[j] = v3_dot(p, v);
            cx += px[j];
            cy += py[j];
        }
        cx /= sz;
        cy /= sz;

        // Sort by angle (Python: sorted by atan2).
        std::vector<int> order(sz);
        for (int j = 0; j < sz; ++j) order[j] = j;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return std::atan2(py[a] - cy, px[a] - cx)
                 < std::atan2(py[b] - cy, px[b] - cx);
        });

        std::vector<int> winding(sz);
        for (int j = 0; j < sz; ++j)
            winding[j] = on_plane[order[j]];

        // Verify winding vs halfspace normal.
        const double* p0 = pts[winding[0]].data();
        const double* p1 = pts[winding[1]].data();
        const double* p2 = pts[winding[2]].data();
        double e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        double e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        double fn[3];
        v3_cross(e1, e2, fn);
        if (v3_dot(fn, n) < 0.0)
            std::reverse(winding.begin(), winding.end());

        out_faces.push_back(std::move(winding));
    }

    return (int)out_faces.size();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" int polyhedron_compute(
    const float* halfspaces,
    int hs_count,
    float* out_verts,
    int* out_vert_count,
    int* out_faces,
    int* out_face_ints,
    int* out_face_count)
{
    if (hs_count < 4 || !halfspaces || !out_verts || !out_vert_count
        || !out_faces || !out_face_ints || !out_face_count)
        return 0;

    // Build normals and offsets (offset = -d, Python line 34).
    std::vector<std::array<double, 3>> normals(hs_count);
    std::vector<double> offsets(hs_count);
    for (int i = 0; i < hs_count; ++i) {
        normals[i][0] = (double)halfspaces[i * 4 + 0];
        normals[i][1] = (double)halfspaces[i * 4 + 1];
        normals[i][2] = (double)halfspaces[i * 4 + 2];
        offsets[i]    = -(double)halfspaces[i * 4 + 3];
    }

    // Intersect every triplet of planes.
    // Python lines 39-64.
    std::vector<std::array<double, 3>> hull_points;

    for (int a = 0; a < hs_count - 2; ++a) {
        for (int b = a + 1; b < hs_count - 1; ++b) {
            for (int c = b + 1; c < hs_count; ++c) {
                const double* n1 = normals[a].data();
                const double* n2 = normals[b].data();
                const double* n3 = normals[c].data();
                double d1 = offsets[a], d2 = offsets[b], d3 = offsets[c];

                double cross23[3], cross31[3], cross12[3];
                v3_cross(n2, n3, cross23);
                double denom = v3_dot(n1, cross23);
                if (std::fabs(denom) < EPSILON)
                    continue;

                v3_cross(n3, n1, cross31);
                v3_cross(n1, n2, cross12);

                // point = (cross23*d1 + cross31*d2 + cross12*d3) / denom
                double t1[3], t2[3], t3[3], sum[3];
                v3_scale(cross23, d1, t1);
                v3_scale(cross31, d2, t2);
                v3_scale(cross12, d3, t3);
                v3_add(t1, t2, sum);
                double tmp[3];
                v3_add(sum, t3, tmp);
                std::array<double, 3> point;
                v3_scale(tmp, 1.0 / denom, point.data());

                // Check inside all halfspaces.
                bool inside = true;
                for (int h = 0; h < hs_count; ++h) {
                    if (v3_dot(normals[h].data(), point.data()) > offsets[h] + EPSILON) {
                        inside = false;
                        break;
                    }
                }
                if (inside)
                    hull_points.push_back(point);
            }
        }
    }

    // Deduplicate (Python lines 67-71).
    if ((int)hull_points.size() >= 4) {
        std::vector<std::array<double, 3>> unique;
        unique.push_back(hull_points[0]);
        for (int i = 1; i < (int)hull_points.size(); ++i) {
            bool dup = false;
            for (const auto& u : unique) {
                if (v3_dist_sq(hull_points[i].data(), u.data()) <= EPSILON * EPSILON) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                unique.push_back(hull_points[i]);
        }
        hull_points = std::move(unique);
    }

    if ((int)hull_points.size() < 4)
        return 0;

    // Build faces.
    std::vector<std::vector<int>> faces;
    int face_count = build_faces_from_halfspaces(hull_points, normals, offsets, faces);
    if (face_count == 0)
        return 0;

    // Write vertices.
    *out_vert_count = (int)hull_points.size();
    for (int i = 0; i < (int)hull_points.size(); ++i) {
        out_verts[i * 3 + 0] = (float)hull_points[i][0];
        out_verts[i * 3 + 1] = (float)hull_points[i][1];
        out_verts[i * 3 + 2] = (float)hull_points[i][2];
    }

    // Write faces: packed as [count, i0, i1, ..., count, i0, ...]
    int face_int_cursor = 0;
    for (const auto& face : faces) {
        out_faces[face_int_cursor++] = (int)face.size();
        for (int idx : face)
            out_faces[face_int_cursor++] = idx;
    }
    *out_face_ints = face_int_cursor;
    *out_face_count = face_count;

    return 1;
}
