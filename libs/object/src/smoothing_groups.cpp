#include "object/smoothing_groups.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Edge key: ordered (lo, hi) vertex pair.
struct EdgeKey {
    int a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};
struct EdgeKeyHash {
    size_t operator()(const EdgeKey& k) const noexcept {
        return std::hash<long long>{}((static_cast<long long>(k.a) << 32) | (unsigned)k.b);
    }
};

inline EdgeKey edge_key(int a, int b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

}  // namespace

extern "C" void object_compute_smoothing_groups(int face_count,
                                                 const int* faces,
                                                 const float* face_corner_normals,
                                                 uint32_t* out) {
    if (face_count <= 0) return;
    const float eps = 1e-4f;
    const float eps_sq = eps * eps;

    // 1. edge_faces: each edge -> list of (up to 2) face indices.
    std::unordered_map<EdgeKey, std::vector<int>, EdgeKeyHash> edge_faces;
    edge_faces.reserve(static_cast<size_t>(face_count * 2));
    for (int fi = 0; fi < face_count; ++fi) {
        int v0 = faces[fi * 3 + 0], v1 = faces[fi * 3 + 1], v2 = faces[fi * 3 + 2];
        edge_faces[edge_key(v0, v1)].push_back(fi);
        edge_faces[edge_key(v1, v2)].push_back(fi);
        edge_faces[edge_key(v2, v0)].push_back(fi);
    }

    // 2. smooth_adj: faces sharing an edge where BOTH shared verts have matching normals.
    std::vector<std::unordered_set<int>> smooth_adj(face_count);
    auto corner_index_for = [&](int fi, int vert_idx) -> int {
        for (int c = 0; c < 3; ++c) {
            if (faces[fi * 3 + c] == vert_idx) return c;
        }
        return -1;
    };
    auto normal_at = [&](int fi, int corner) -> const float* {
        return &face_corner_normals[(fi * 3 + corner) * 3];
    };
    for (const auto& [ekey, flist] : edge_faces) {
        if (flist.size() != 2) continue;
        int fi_a = flist[0], fi_b = flist[1];
        bool smooth = true;
        for (int sv : {ekey.a, ekey.b}) {
            int ca = corner_index_for(fi_a, sv);
            int cb = corner_index_for(fi_b, sv);
            if (ca < 0 || cb < 0) { smooth = false; break; }
            const float* na = normal_at(fi_a, ca);
            const float* nb = normal_at(fi_b, cb);
            float dx = na[0] - nb[0], dy = na[1] - nb[1], dz = na[2] - nb[2];
            if (dx*dx + dy*dy + dz*dz > eps_sq) { smooth = false; break; }
        }
        if (smooth) {
            smooth_adj[fi_a].insert(fi_b);
            smooth_adj[fi_b].insert(fi_a);
        }
    }

    // 3. Flood-fill connected components via smooth_adj.
    std::vector<int> comp(face_count, -1);
    int cid = 0;
    for (int fi = 0; fi < face_count; ++fi) {
        if (comp[fi] >= 0) continue;
        std::deque<int> queue; queue.push_back(fi);
        while (!queue.empty()) {
            int f = queue.front(); queue.pop_front();
            if (comp[f] >= 0) continue;
            comp[f] = cid;
            for (int adj : smooth_adj[f]) {
                if (comp[adj] < 0) queue.push_back(adj);
            }
        }
        ++cid;
    }

    // 4. Detect flat components (all per-corner normals identical within eps).
    std::vector<char> comp_varies(cid, 0);
    std::vector<int> comp_ref_fi(cid, -1);  // first face's first-corner normal as reference
    for (int fi = 0; fi < face_count; ++fi) {
        int c = comp[fi];
        if (c < 0 || comp_varies[c]) continue;
        for (int j = 0; j < 3; ++j) {
            const float* n = normal_at(fi, j);
            if (comp_ref_fi[c] < 0) {
                comp_ref_fi[c] = fi * 3 + j;  // encode (fi, corner) as flat index
            } else {
                const float* ref = &face_corner_normals[comp_ref_fi[c] * 3];
                float dx = n[0] - ref[0], dy = n[1] - ref[1], dz = n[2] - ref[2];
                if (dx*dx + dy*dy + dz*dz > eps_sq) {
                    comp_varies[c] = 1;
                    break;
                }
            }
        }
    }
    std::unordered_set<int> flat_comps;
    for (int c = 0; c < cid; ++c) {
        if (!comp_varies[c]) flat_comps.insert(c);
    }

    // 5. Build comp_adj: adjacency between distinct components via shared edges.
    std::vector<std::unordered_set<int>> comp_adj(cid);
    for (const auto& [ekey, flist] : edge_faces) {
        if (flist.size() != 2) continue;
        int ca = comp[flist[0]], cb = comp[flist[1]];
        if (ca != cb && ca >= 0 && cb >= 0) {
            comp_adj[ca].insert(cb);
            comp_adj[cb].insert(ca);
        }
    }

    // 6. Greedy color the component-adjacency graph. Flat comps get color=-1 (sentinel).
    std::vector<int> comp_color(cid, 0);
    for (int c = 0; c < cid; ++c) {
        if (flat_comps.count(c)) {
            comp_color[c] = -1;
            continue;
        }
        std::unordered_set<int> used;
        for (int neighbor : comp_adj[c]) {
            if (comp_color[neighbor] >= 0) used.insert(comp_color[neighbor]);
        }
        int color = 0;
        while (used.count(color)) ++color;
        comp_color[c] = color;
    }

    // 7. Output bitmasks: 1 << (color % 31) for non-flat, 0 for flat or unassigned.
    for (int fi = 0; fi < face_count; ++fi) {
        int c = comp[fi];
        if (c < 0) { out[fi] = 0; continue; }
        int color = comp_color[c];
        out[fi] = (color < 0) ? 0u : (1u << (color % 31));
    }
}
