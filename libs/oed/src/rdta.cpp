#include "oed/rdta.h"

#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <limits>
#include <algorithm>
#include <iterator>
#include <cstring>
#include <type_traits>
#include <cctype>
#include <array>

#include "threedi_model.h"

#include "oed/types.h"
#include "oed/export_3di.h"
#include <io/log.h>

namespace oed {

void normalize_vec3(float (&v)[3]) {
  const float len =
      std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len == 0.0f) {
    v[0] = v[1] = v[2] = 0.0f;
    return;
  }
  const float inv = 1.0f / len;
  v[0] *= inv;
  v[1] *= inv;
  v[2] *= inv;
}

// Matches the game engine's NormalizeVec3 (0x4215e0) which uses x87 FPU:
// - sum computed at 80-bit extended precision without intermediate truncation
// - sqrt called with double argument
// - 1.0/len and inv*component computed at 80-bit, stored as float
// Using double precision reproduces this exactly for float inputs because the
// sum/product of two 23-bit-mantissa values fits in double's 52-bit mantissa.
static void normalize_vec3_x87(float (&v)[3]) {
  const double x = v[0], y = v[1], z = v[2];
  const double sum = x * x + y * y + z * z;
  const float len_f = static_cast<float>(std::sqrt(sum));
  if (len_f == 0.0f) {
    v[0] = v[1] = v[2] = 0.0f;
    return;
  }
  const float inv = static_cast<float>(1.0 / static_cast<double>(len_f));
  v[0] = static_cast<float>(static_cast<double>(inv) * x);
  v[1] = static_cast<float>(static_cast<double>(inv) * y);
  v[2] = static_cast<float>(static_cast<double>(inv) * z);
}

void normalize_vec3(Vec3 &v) {
  const float len =
      std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len == 0.0f) {
    v.x = v.y = v.z = 0.0f;
    return;
  }
  const float inv = 1.0f / len;
  v.x *= inv;
  v.y *= inv;
  v.z *= inv;
}

void cross3(const float (&a)[3], const float (&b)[3], float (&out)[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

// Vertex structure for deduplication (matches WriteRDTA's 100-byte structure).
struct RenderVertex {
  float position[3];    // 0x00 (12 bytes)
  float normal[3];      // 0x0C (12 bytes)
  float uv0[2];         // 0x18 (8 bytes)
  float uv1[2];         // 0x20 (8 bytes)
  float tangent[3];     // 0x28 (12 bytes)
  float bitangent[3];   // 0x34 (12 bytes)
  float bone_weights[4];   // 0x40 (16 bytes)
  int32_t bone_indices[4]; // 0x50 (16 bytes)
  // Padding to 100 bytes (25 dwords) to match ModSuperOED's vertex cache.
  uint8_t padding[4];   // 0x60 (4 bytes)

  bool operator==(const RenderVertex &other) const {
    return std::memcmp(this, &other, sizeof(RenderVertex)) == 0;
  }
};
static_assert(sizeof(RenderVertex) == 100, "RenderVertex must be 100 bytes");

struct SmoothedVertexVectors {
  float normal[3];
  float tangent[3];
  float bitangent[3];
};

struct SmoothedFace {
  SmoothedVertexVectors verts[3];
};

struct StripBucket {
  std::vector<uint16_t> tris;  // triangle list, 3 indices per tri
  Vec3 bboxMin{1000000000.0f, 1000000000.0f, 1000000000.0f};
  Vec3 bboxMax{-1000000000.0f, -1000000000.0f, -1000000000.0f};
};

struct StripBuild {
  int32_t material_index;
  int32_t index_offset;
  uint16_t num_indices;
  uint16_t num_triangles;
  int32_t is_strip;
  int32_t start_vertex;
  int32_t num_vertices;
  float min[3];
  float max[3];
};

struct StripifyContext {
  std::vector<uint16_t> indices;   // 3 * triCount
  std::vector<int> adjacency;      // triCount * 3 (neighbor tri index)
  std::vector<int> adjacency_edge; // triCount * 3 (neighbor edge)
  std::vector<uint8_t> visited;    // triCount
  int triCount() const { return static_cast<int>(indices.size() / 3); }
};

// Build adjacency for triangle list (reversed-edge matching).
static StripifyContext build_stripify_context(const std::vector<uint16_t> &tris) {
  StripifyContext ctx;
  ctx.indices = tris;
  const int tri_count = ctx.triCount();
  ctx.adjacency.assign(tri_count * 3, -1);
  ctx.adjacency_edge.assign(tri_count * 3, -1);
  ctx.visited.assign(tri_count, 0);

  // IDA sub_4612B0: walk triangle pairs in order, record the first reversed edge match.
  std::vector<uint8_t> edge_seen(static_cast<size_t>(tri_count), 0);
  for (int t = 0; t < tri_count; ++t) {
    for (int e = 0; e < 3; ++e) {
      if (edge_seen[static_cast<size_t>(t)] & (1u << e)) continue;
      const uint16_t a = ctx.indices[static_cast<size_t>(t) * 3 + e];
      const uint16_t b = ctx.indices[static_cast<size_t>(t) * 3 + ((e + 1) % 3)];
      for (int u = t + 1; u < tri_count; ++u) {
        if (edge_seen[static_cast<size_t>(u)] == 7) continue;
        for (int ue = 0; ue < 3; ++ue) {
          // NOTE: The game engine does NOT skip already-matched edges of
          // triangle u.  It can overwrite a previous adjacency entry for
          // (u, ue), matching the first forward triangle that shares the
          // reversed edge.  We must reproduce this to match the output.
          const uint16_t ua = ctx.indices[static_cast<size_t>(u) * 3 + ue];
          const uint16_t ub = ctx.indices[static_cast<size_t>(u) * 3 + ((ue + 1) % 3)];
          if (ua == b && ub == a) {
            ctx.adjacency[static_cast<size_t>(t) * 3 + e] = u;
            ctx.adjacency_edge[static_cast<size_t>(t) * 3 + e] = ue;
            ctx.adjacency[static_cast<size_t>(u) * 3 + ue] = t;
            ctx.adjacency_edge[static_cast<size_t>(u) * 3 + ue] = e;
            edge_seen[static_cast<size_t>(t)] |= static_cast<uint8_t>(1u << e);
            edge_seen[static_cast<size_t>(u)] |= static_cast<uint8_t>(1u << ue);
            goto next_edge;
          }
        }
      }
    next_edge:;
    }
  }
  return ctx;
}

static int count_unvisited_neighbors(const StripifyContext &ctx, int tri) {
  int count = 0;
  for (int e = 0; e < 3; ++e) {
    const int n = ctx.adjacency[static_cast<size_t>(tri) * 3 + e];
    if (n != -1 && ctx.visited[static_cast<size_t>(n)] == 0) {
      ++count;
    }
  }
  return count;
}

struct TraceResult {
  std::vector<int> tri_path;   // triangle indices in strip order
  std::vector<int> edge_path;  // which edge index (0..2) was used to pick vertex for each tri
  int skipped = 0;
};

struct StripifyResult {
  std::vector<uint16_t> indices;  // triangle list indices (3 * triangle_count)
  int triangle_count = 0;
};

struct StripBuildIteration {
  int iteration = 0;
  int seed_tri = -1;
  int seed_edge = -1;
  int neighbor_count = 0;
  int length = 0;
  int skipped = 0;
  double ratio = 0.0;
  bool tri_path_parity = false;
  bool use_adj_before = false;
  bool use_adj_after = false;
};

// Port of Stripify_TraceStrip (IDA 0x460050).
static TraceResult trace_strip(StripifyContext &ctx, int start_tri, int start_edge,
                               int max_len, bool tri_path_parity,
                               bool use_adj_heuristic) {
  TraceResult out;
  if (start_tri < 0 || start_tri >= ctx.triCount()) return out;
  if (ctx.visited[static_cast<size_t>(start_tri)]) return out;

  ctx.visited[static_cast<size_t>(start_tri)] = 1;
  out.tri_path.reserve(static_cast<size_t>(max_len));
  out.edge_path.reserve(static_cast<size_t>(max_len));

  // Edge seeding is keyed off use_adj_heuristic (matches IDA).
  if (use_adj_heuristic) {
    out.edge_path.push_back(start_edge % 3);
    out.edge_path.push_back((start_edge + 1) % 3);
  } else {
    out.edge_path.push_back((start_edge + 1) % 3);
    out.edge_path.push_back(start_edge % 3);
  }
  out.edge_path.push_back((start_edge + 2) % 3);
  out.tri_path.push_back(start_tri);
  out.tri_path.push_back(start_tri);
  out.tri_path.push_back(start_tri);

  bool flip_parity = !use_adj_heuristic;
  const int first_sel = (start_edge + (flip_parity ? 2 : 1)) % 3;
  int next_tri =
      ctx.adjacency[static_cast<size_t>(start_tri) * 3 + first_sel];
  int next_edge =
      ctx.adjacency_edge[static_cast<size_t>(start_tri) * 3 + first_sel];

  int i = 3;
  int skipped = 0;
  while (i < max_len && next_tri != -1 &&
         ctx.visited[static_cast<size_t>(next_tri)] == 0) {
    const int cur_tri = next_tri;
    const int cur_edge = next_edge;
    flip_parity = !flip_parity;
    const int edge_sel = (cur_edge + (flip_parity ? 2 : 1)) % 3;
    next_tri = ctx.adjacency[static_cast<size_t>(cur_tri) * 3 + edge_sel];
    next_edge =
        ctx.adjacency_edge[static_cast<size_t>(cur_tri) * 3 + edge_sel];

    bool jump_edge = false;
    if (next_tri == -1 || ctx.visited[static_cast<size_t>(next_tri)]) {
      jump_edge = true;
    } else if (tri_path_parity) {
      const int alt_edge = (cur_edge + (flip_parity ? 1 : 2)) % 3;
      const int alt_tri =
          ctx.adjacency[static_cast<size_t>(cur_tri) * 3 + alt_edge];
      const int alt_edge_idx =
          ctx.adjacency_edge[static_cast<size_t>(cur_tri) * 3 + alt_edge];
      if (alt_tri != -1 && ctx.visited[static_cast<size_t>(alt_tri)] == 0) {
        const int next_val = count_unvisited_neighbors(ctx, next_tri);
        const int alt_val = count_unvisited_neighbors(ctx, alt_tri);
        if (alt_val < next_val) {
          jump_edge = true;
        } else if (alt_val == next_val) {
          const int tri_through_alt =
              ctx.adjacency[static_cast<size_t>(alt_tri) * 3 +
                            ((alt_edge_idx + (flip_parity ? 2 : 1)) % 3)];
          const int tri_through_next =
              ctx.adjacency[static_cast<size_t>(next_tri) * 3 +
                            ((next_edge + (flip_parity ? 1 : 2)) % 3)];
          if (tri_through_next == -1 ||
              ctx.visited[static_cast<size_t>(tri_through_next)] != 0) {
            jump_edge = true;
          } else if (tri_through_alt != -1 &&
                     ctx.visited[static_cast<size_t>(tri_through_alt)] == 0) {
            const int alt2_val =
                count_unvisited_neighbors(ctx, tri_through_alt);
            const int next2_val =
                count_unvisited_neighbors(ctx, tri_through_next);
            if (alt2_val < next2_val) {
              jump_edge = true;
            }
          }
        }
      }
    }

    if (jump_edge) {
      const int alt_edge = (cur_edge + (flip_parity ? 1 : 2)) % 3;
      next_tri = ctx.adjacency[static_cast<size_t>(cur_tri) * 3 + alt_edge];
      next_edge =
          ctx.adjacency_edge[static_cast<size_t>(cur_tri) * 3 + alt_edge];
      if (next_tri != -1 && ctx.visited[static_cast<size_t>(next_tri)] == 0) {
        out.tri_path.push_back(out.tri_path[i - 2]);
        out.edge_path.push_back(out.edge_path[i - 2]);
        ++i;
        ++skipped;
        flip_parity = !flip_parity;
      }
    }

    out.tri_path.push_back(cur_tri);
    out.edge_path.push_back((cur_edge + 2) % 3);
    ctx.visited[static_cast<size_t>(cur_tri)] = 1;
    ++i;
  }

  for (size_t j = 2; j < out.tri_path.size(); ++j) {
    ctx.visited[static_cast<size_t>(out.tri_path[j])] = 0;
  }
  out.skipped = skipped;
  return out;
}

// Build strip list (triangle order) and return flattened triangle list after stripify.
StripifyResult stripify_triangles(const std::vector<uint16_t> &tris,
                                         int subobject_index,
                                         int material_index) {
  StripifyResult out;
  if (tris.empty()) return out;

  const char *debug_stripify = std::getenv("OED_STRIPIFY_DEBUG");

  StripifyContext ctx = build_stripify_context(tris);
  constexpr int kMaxStripLenPassA = 1024;
  constexpr int kMaxStripLenPassB = 1024;

  struct CandidateStrip {
    std::vector<uint16_t> indices;
    bool flip_parity = false;  // corresponds to useAdjacencyHeuristic in IDA
    int seed_tri = -1;
    int seed_edge = -1;
    std::vector<int> tri_path;   // debug: triangle walk
    std::vector<int> edge_path;  // debug: edge choices per tri
  };

  struct BuildResult {
    std::vector<CandidateStrip> strips;
    std::vector<StripBuildIteration> iterations;
    std::vector<std::string> seed_diag;
  };

  auto build_strip_list = [&](bool tri_path_parity, int max_len) {
    BuildResult result;
    std::vector<CandidateStrip> strips;
    std::fill(ctx.visited.begin(), ctx.visited.end(), 0);
    bool use_adj_heuristic = true;  // corresponds to useAdjacencyHeuristic in IDA, toggled on odd-length strips

    // Optional diagnostics: emit per-seed edge stats when env is set.
    // Off by default: the per-seed diag strings are expensive to build and are
    // pure diagnostics; opt in with OED_STRIPIFY_SEED_DIAG=1.
    const char *diag_env = std::getenv("OED_STRIPIFY_SEED_DIAG");
    const bool diag_enabled = diag_env != nullptr && std::strcmp(diag_env, "0") != 0;
    std::vector<std::string> seed_diag;

    while (true) {
      int best_neighbor_score = std::numeric_limits<int>::max();
      float best_ratio = 2.0f;
      int best_tri = -1;
      int best_edge = 0;

      for (int tri = 0; tri < ctx.triCount(); ++tri) {
        if (ctx.visited[static_cast<size_t>(tri)] != 0) continue;

        int neighbor_count = count_unvisited_neighbors(ctx, tri);
        if (neighbor_count == 0) neighbor_count = 4;
        if (neighbor_count > best_neighbor_score) continue;

        for (int edge = 0; edge < 3; ++edge) {
          TraceResult trace = trace_strip(
              ctx, tri, edge, max_len, tri_path_parity, use_adj_heuristic);
          float ratio =
              (trace.tri_path.size() == 3)
                  ? 1.0f
                  : static_cast<float>(trace.skipped) /
                        static_cast<float>(trace.tri_path.empty() ? 1 : trace.tri_path.size());
          if (diag_enabled) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "seed tri=%d edge=%d neigh=%d len=%zu skipped=%d ratio=%.6f",
                          tri, edge, neighbor_count, trace.tri_path.size(), trace.skipped, ratio);
            seed_diag.emplace_back(buf);
          }
          if (neighbor_count < best_neighbor_score ||
              (neighbor_count == best_neighbor_score && ratio < best_ratio)) {
            best_neighbor_score = neighbor_count;
            best_ratio = ratio;
            best_tri = tri;
            best_edge = edge;
          }
        }
      }

      if (best_neighbor_score == std::numeric_limits<int>::max()) break;

      TraceResult chosen =
          trace_strip(ctx, best_tri, best_edge, max_len, tri_path_parity,
                      use_adj_heuristic);

      StripBuildIteration iter{};
      iter.iteration = static_cast<int>(result.iterations.size());
      iter.seed_tri = best_tri;
      iter.seed_edge = best_edge;
      iter.neighbor_count = best_neighbor_score;
      iter.length = static_cast<int>(chosen.tri_path.size());
      iter.skipped = chosen.skipped;
      iter.ratio =
          (chosen.tri_path.size() == 3)
              ? 1.0
              : static_cast<double>(chosen.skipped) /
                    static_cast<double>(chosen.tri_path.empty() ? 1
                                                                : chosen.tri_path.size());
      iter.tri_path_parity = tri_path_parity;
      iter.use_adj_before = use_adj_heuristic;

      for (size_t i = 0; i < chosen.tri_path.size(); ++i) {
        ctx.visited[static_cast<size_t>(chosen.tri_path[i])] = 1;
      }

      CandidateStrip strip;
      strip.flip_parity = use_adj_heuristic;
      strip.indices.reserve(chosen.tri_path.size());
      for (size_t i = 0; i < chosen.tri_path.size(); ++i) {
        const size_t tri_idx = static_cast<size_t>(chosen.tri_path[i]);
        const size_t edge_idx = static_cast<size_t>(chosen.edge_path[i]);
        strip.indices.push_back(ctx.indices[tri_idx * 3 + edge_idx]);
      }
      strip.seed_tri = best_tri;
      strip.seed_edge = best_edge;
      strip.tri_path.assign(chosen.tri_path.begin(), chosen.tri_path.end());
      strip.edge_path.assign(chosen.edge_path.begin(), chosen.edge_path.end());
      strips.push_back(std::move(strip));

      if ((chosen.tri_path.size() & 1u) != 0) {
        use_adj_heuristic = !use_adj_heuristic;
      }
      iter.use_adj_after = use_adj_heuristic;
      result.iterations.push_back(iter);
    }

    result.strips = std::move(strips);
    if (diag_enabled && !seed_diag.empty()) {
      seed_diag.insert(seed_diag.begin(),
                       std::string("tri_path_parity=") +
                           (tri_path_parity ? "1" : "0"));
      result.seed_diag.swap(seed_diag);
    }

    return result;
  };

  auto compute_index_count = [](const std::vector<CandidateStrip> &strips) {
    if (strips.empty()) return 0;
    int count = static_cast<int>(strips.size()) * 2 - 2;
    for (const auto &s : strips) count += static_cast<int>(s.indices.size());
    return count;
  };

  // Mirrors StripifyScore_Update (IDA 0x4615E0): tracks the last 16 indices and
  // increments a score when the same index is seen from a different owner.
  struct StripifyScoreState {
    uint32_t score = 0;
    uint16_t values[16];
    uint32_t owners[16];
    uint32_t cursor = 0;
  };

  auto update_score = [](StripifyScoreState &ctx, uint32_t owner,
                         uint16_t idx) {
    for (size_t i = 0; i < 16; ++i) {
      if (ctx.values[i] == idx) {
        if (ctx.owners[i] == owner) {
          return false;
        }
        ++ctx.score;
        ctx.owners[i] = owner;
        return true;
      }
    }
    const size_t pos = ctx.cursor & 0xF;
    ctx.values[pos] = idx;
    ctx.owners[pos] = owner;
    ctx.cursor = (ctx.cursor + 1) & 0xF;
    return true;
  };

  const bool debug_diag = debug_stripify && std::strcmp(debug_stripify, "diag") == 0;

  auto select_strip_in_place = [&](std::vector<CandidateStrip> &list,
                                   StripifyScoreState &ctx) {
    if (list.empty()) return;
    const bool base_flip = list[0].flip_parity;
    const bool base_parity = (list[0].indices.size() & 1u) != 0;

    int best_delta = -1;
    size_t best_index = 0;
    bool best_reverse = false;

    for (size_t i = 0; i < list.size(); ++i) {
      const auto &strip = list[i];
      if (strip.flip_parity != base_flip) continue;
      if (((strip.indices.size() & 1u) != 0) != base_parity) continue;

      StripifyScoreState forward = ctx;
      for (uint16_t idx : strip.indices) {
        update_score(forward, 2, idx);
      }

      bool use_reverse = false;
      if ((strip.indices.size() & 1u) == 0) {
        StripifyScoreState reversed = ctx;
        for (auto it = strip.indices.rbegin(); it != strip.indices.rend(); ++it) {
          update_score(reversed, 2, *it);
        }
        if (reversed.score > forward.score) {
          forward = reversed;
          use_reverse = true;
        }
      }

      const int delta =
          static_cast<int>(forward.score) - static_cast<int>(ctx.score);
      if (debug_diag) {
        opennova::io::logf(opennova::io::LogLevel::kDebug,
		"  cand idx=%zu len=%zu flip=%d rev=%d delta=%d score=%u",
                     i, strip.indices.size(), strip.flip_parity ? 1 : 0,
                     use_reverse ? 1 : 0, delta, forward.score);
      }
      if (delta > best_delta) {
        best_delta = delta;
        best_index = i;
        best_reverse = use_reverse;
      }
    }

    // Apply chosen reversal and move to front (matches sub_460620 behavior).
    if (best_reverse && ((list[best_index].indices.size() & 1u) == 0)) {
      std::reverse(list[best_index].indices.begin(),
                   list[best_index].indices.end());
    }
    if (best_index != 0) {
      std::swap(list[0], list[best_index]);
    }
  };

  auto flatten_strips = [&](std::vector<CandidateStrip> strips) {
    std::vector<uint16_t> flat;
    if (strips.empty()) return flat;

    // Allocate/output like Stripify_FlattenStrips: start with first strip as-is.
    StripifyScoreState ctx{};
    std::fill(std::begin(ctx.values), std::end(ctx.values), 0xFFFF);
    std::fill(std::begin(ctx.owners), std::end(ctx.owners), 0);

    flat.insert(flat.end(), strips[0].indices.begin(), strips[0].indices.end());
    for (uint16_t idx : strips[0].indices) {
      update_score(ctx, 1, idx);
    }
    strips.erase(strips.begin());

    if (strips.empty()) {
      return flat;
    }

    while (!strips.empty()) {
      select_strip_in_place(strips, ctx);
      if (debug_diag) {
        opennova::io::logf(opennova::io::LogLevel::kDebug,
		"select idx=0 reverse_applied=%d remaining=%zu",
                     0, strips.size());
      }

      CandidateStrip chosen = strips.front();
      strips.erase(strips.begin());

      const uint16_t last = flat.back();
      const uint16_t first = chosen.indices.front();
      if (first != last) {
        flat.push_back(last);
        flat.push_back(first);
      }
      const bool even_parity = (flat.size() % 2) == 0;
      if (chosen.flip_parity != even_parity) {
        flat.push_back(first);
      }
      // Only update score for actual strip indices (matching game engine
      // Stripify_FlattenStrips which does NOT score degenerate connectors).
      for (uint16_t idx : chosen.indices) {
        flat.push_back(idx);
        update_score(ctx, 1, idx);
      }
    }
    return flat;
  };

  // Two candidates: startTri parity = 1, and parity = 0 (mirrors Stripify passes).
  const auto build_a =
      build_strip_list(true /*tri_path_parity*/, kMaxStripLenPassA);
  const auto build_b =
      build_strip_list(false /*tri_path_parity*/, kMaxStripLenPassB);
  const auto &strips_a = build_a.strips;
  const auto &strips_b = build_b.strips;
  if (debug_diag) {
    auto dump_strips = [&](const char *name,
                           const std::vector<CandidateStrip> &strips) {
      opennova::io::logf(opennova::io::LogLevel::kDebug,
		"pass %s strip_count=%zu", name, strips.size());
      for (size_t i = 0; i < strips.size(); ++i) {
        const auto &s = strips[i];
        std::string line;
        for (uint16_t idx : s.indices) line += " " + std::to_string(idx);
        opennova::io::logf(opennova::io::LogLevel::kDebug,
		"  %s[%zu] flip=%d len=%zu:%s",
                     name, i, s.flip_parity ? 1 : 0, s.indices.size(),
                     line.c_str());
      }
    };
    dump_strips("A", strips_a);
    dump_strips("B", strips_b);
  }

  const int count_a = compute_index_count(strips_a);
  const int count_b = compute_index_count(strips_b);

  auto flatten_copy = [&](const std::vector<CandidateStrip> &strips) {
    return flatten_strips(strips);
  };

  const bool force_a = debug_stripify && std::strcmp(debug_stripify, "forceA") == 0;
  const bool force_b = debug_stripify && std::strcmp(debug_stripify, "forceB") == 0;
  const bool use_b =
      !force_a && (force_b || (!strips_b.empty() && (count_a == 0 || count_b < count_a)));
  if (debug_stripify) {
    opennova::io::logf(opennova::io::LogLevel::kDebug,
		"stripify tri_count=%zu stripsA=%zu stripsB=%zu countA=%d "
                 "countB=%d choose=%c",
                 tris.size() / 3, strips_a.size(), strips_b.size(), count_a, count_b,
                 use_b ? 'B' : 'A');
  }


  std::vector<uint16_t> strip_indices =
      use_b ? flatten_strips(strips_b) : flatten_strips(strips_a);
  if (debug_stripify) {
    const size_t limit = debug_diag ? strip_indices.size() : std::min<size_t>(30, strip_indices.size());
    std::string line;
    for (size_t i = 0; i < limit; ++i)
      line += " " + std::to_string(strip_indices[i]);
    opennova::io::logf(opennova::io::LogLevel::kDebug,
		"strip_indices size=%zu (first %zu):%s", strip_indices.size(), limit,
		line.c_str());
  }

  // Convert strip indices back to a triangle list (discard degenerate tris).
  std::vector<uint16_t> tri_list;
  tri_list.reserve(strip_indices.size());
  int tri_count = 0;
  for (size_t jj = 0; jj + 2 < strip_indices.size(); ++jj) {
    const uint16_t v0 = strip_indices[jj];
    const bool odd = (jj & 1u) != 0;
    const uint16_t v1 = strip_indices[jj + (odd ? 2 : 1)];
    const uint16_t v2 = strip_indices[jj + (odd ? 1 : 2)];
    if (v0 == v1 || v1 == v2 || v2 == v0) {
      continue;
    }
    tri_list.push_back(v0);
    tri_list.push_back(v1);
    tri_list.push_back(v2);
    ++tri_count;
  }

  out.indices.swap(tri_list);
  out.triangle_count = tri_count;
  return out;
}

// Mirrors sub_457360: average per-face vectors across smoothing groups for faces sharing a
// vertex and compatible smoothing masks. Outputs 9 floats per face (normal/tangent/bitangent
// for each of the 3 vertices).
std::vector<SmoothedFace> compute_smoothed_vectors(const SubObject &subobj) {
  std::vector<SmoothedFace> out;
  if (subobj.faceCount <= 0 || !subobj.faces) {
    return out;
  }

  out.resize(static_cast<size_t>(subobj.faceCount));

  for (int32_t face_idx = 0; face_idx < subobj.faceCount; ++face_idx) {
    const Face &face = subobj.faces[face_idx];

    for (int v_idx = 0; v_idx < 3; ++v_idx) {
      const uint32_t vert_index = face.vert[v_idx];

      // x87 FPU stores accumulator as float each step (fld/fadd/fstp dword).
      // Emulate with: acc = (float)((double)acc + (double)value).
      float normal[3]{};
      float tangent[3]{};
      float bitangent[3]{};

      if (face.smoothingGroup != 0) {
        for (int32_t k = 0; k < subobj.faceCount; ++k) {
          const Face &other = subobj.faces[k];
          if ((face.smoothingGroup & other.smoothingGroup) == 0) {
            continue;
          }

          if (other.vert[0] == vert_index || other.vert[1] == vert_index ||
              other.vert[2] == vert_index) {
            normal[0] = static_cast<float>((double)normal[0] + (double)other.faceBasis[0][0]);
            normal[1] = static_cast<float>((double)normal[1] + (double)other.faceBasis[0][1]);
            normal[2] = static_cast<float>((double)normal[2] + (double)other.faceBasis[0][2]);

            tangent[0] = static_cast<float>((double)tangent[0] + (double)other.faceBasis[1][0]);
            tangent[1] = static_cast<float>((double)tangent[1] + (double)other.faceBasis[1][1]);
            tangent[2] = static_cast<float>((double)tangent[2] + (double)other.faceBasis[1][2]);

            bitangent[0] = static_cast<float>((double)bitangent[0] + (double)other.faceBasis[2][0]);
            bitangent[1] = static_cast<float>((double)bitangent[1] + (double)other.faceBasis[2][1]);
            bitangent[2] = static_cast<float>((double)bitangent[2] + (double)other.faceBasis[2][2]);
          }
        }
      } else {
        normal[0] = static_cast<float>((double)normal[0] + (double)face.faceBasis[0][0]);
        normal[1] = static_cast<float>((double)normal[1] + (double)face.faceBasis[0][1]);
        normal[2] = static_cast<float>((double)normal[2] + (double)face.faceBasis[0][2]);

        tangent[0] = static_cast<float>((double)tangent[0] + (double)face.faceBasis[1][0]);
        tangent[1] = static_cast<float>((double)tangent[1] + (double)face.faceBasis[1][1]);
        tangent[2] = static_cast<float>((double)tangent[2] + (double)face.faceBasis[1][2]);

        bitangent[0] = static_cast<float>((double)bitangent[0] + (double)face.faceBasis[2][0]);
        bitangent[1] = static_cast<float>((double)bitangent[1] + (double)face.faceBasis[2][1]);
        bitangent[2] = static_cast<float>((double)bitangent[2] + (double)face.faceBasis[2][2]);
      }

      normalize_vec3_x87(normal);
      normalize_vec3_x87(tangent);
      normalize_vec3_x87(bitangent);

      SmoothedVertexVectors &dst = out[static_cast<size_t>(face_idx)].verts[v_idx];
      dst.normal[0] = normal[0];
      dst.normal[1] = normal[1];
      dst.normal[2] = normal[2];

      dst.tangent[0] = tangent[0];
      dst.tangent[1] = tangent[1];
      dst.tangent[2] = tangent[2];

      dst.bitangent[0] = bitangent[0];
      dst.bitangent[1] = bitangent[1];
      dst.bitangent[2] = bitangent[2];
    }
  }

  return out;
}

std::vector<SmoothedFace> compute_smoothed_vectors_float(
    const SubObject &subobj) {
  std::vector<SmoothedFace> out;
  if (subobj.faceCount <= 0 || !subobj.faces) {
    return out;
  }
  out.resize(static_cast<size_t>(subobj.faceCount));
  for (int32_t face_idx = 0; face_idx < subobj.faceCount; ++face_idx) {
    const Face &face = subobj.faces[face_idx];
    if (face.hasDollar) continue;
    for (int v_idx = 0; v_idx < 3; ++v_idx) {
      const uint32_t vert_index = face.vert[v_idx];
      float normal[3]{};
      float tangent[3]{};
      float bitangent[3]{};
      if (face.smoothingGroup != 0) {
        for (int32_t k = 0; k < subobj.faceCount; ++k) {
          const Face &other = subobj.faces[k];
          if (other.hasDollar) continue;
          if ((face.smoothingGroup & other.smoothingGroup) == 0) continue;
          if (other.vert[0] == vert_index || other.vert[1] == vert_index ||
              other.vert[2] == vert_index) {
            normal[0] = static_cast<float>((double)normal[0] + (double)other.faceBasis[0][0]);
            normal[1] = static_cast<float>((double)normal[1] + (double)other.faceBasis[0][1]);
            normal[2] = static_cast<float>((double)normal[2] + (double)other.faceBasis[0][2]);
            tangent[0] = static_cast<float>((double)tangent[0] + (double)other.faceBasis[1][0]);
            tangent[1] = static_cast<float>((double)tangent[1] + (double)other.faceBasis[1][1]);
            tangent[2] = static_cast<float>((double)tangent[2] + (double)other.faceBasis[1][2]);
            bitangent[0] = static_cast<float>((double)bitangent[0] + (double)other.faceBasis[2][0]);
            bitangent[1] = static_cast<float>((double)bitangent[1] + (double)other.faceBasis[2][1]);
            bitangent[2] = static_cast<float>((double)bitangent[2] + (double)other.faceBasis[2][2]);
          }
        }
      } else {
        normal[0] = static_cast<float>((double)normal[0] + (double)face.faceBasis[0][0]);
        normal[1] = static_cast<float>((double)normal[1] + (double)face.faceBasis[0][1]);
        normal[2] = static_cast<float>((double)normal[2] + (double)face.faceBasis[0][2]);
        tangent[0] = static_cast<float>((double)tangent[0] + (double)face.faceBasis[1][0]);
        tangent[1] = static_cast<float>((double)tangent[1] + (double)face.faceBasis[1][1]);
        tangent[2] = static_cast<float>((double)tangent[2] + (double)face.faceBasis[1][2]);
        bitangent[0] = static_cast<float>((double)bitangent[0] + (double)face.faceBasis[2][0]);
        bitangent[1] = static_cast<float>((double)bitangent[1] + (double)face.faceBasis[2][1]);
        bitangent[2] = static_cast<float>((double)bitangent[2] + (double)face.faceBasis[2][2]);
      }
      normalize_vec3_x87(normal);
      normalize_vec3_x87(tangent);
      normalize_vec3_x87(bitangent);

      SmoothedVertexVectors &dst = out[static_cast<size_t>(face_idx)].verts[v_idx];
      dst.normal[0] = normal[0];
      dst.normal[1] = normal[1];
      dst.normal[2] = normal[2];
      dst.tangent[0] = tangent[0];
      dst.tangent[1] = tangent[1];
      dst.tangent[2] = tangent[2];
      dst.bitangent[0] = bitangent[0];
      dst.bitangent[1] = bitangent[1];
      dst.bitangent[2] = bitangent[2];
    }
  }
  return out;
}

void sort_bone_weights(RenderVertex &rv) {
  bool swapped;
  do {
    swapped = false;
    for (int j = 0; j < 3; ++j) {
      const int a = j;
      const int b = j + 1;
      if (rv.bone_weights[a] < rv.bone_weights[b]) {
        std::swap(rv.bone_weights[a], rv.bone_weights[b]);
        std::swap(rv.bone_indices[a], rv.bone_indices[b]);
        swapped = true;
      }
    }
  } while (swapped);
}

struct SkinnedGroup {
  std::vector<uint16_t> triangles;   // flattened triangle list (3 * tri_count)
  std::vector<int32_t> bone_table;   // unique bones referenced by this group
};

bool build_render_geometry_skinned(const LodHeader &lod,
                                          const MaterialTable *materials,
                                          ThreediLod &out_lod,
                                          std::string &error) {
  int max_mat_index = -1;
  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    for (int32_t fi = 0; fi < so.faceCount; ++fi) {
      if (so.faces[fi].matIndex > max_mat_index) {
        max_mat_index = so.faces[fi].matIndex;
      }
    }
  }

  const int material_count = std::max<int>(
      1, std::max<int>(lod.materialCount, max_mat_index + 1));

  auto material_name = [&](int idx) -> const char * {
    if (materials && idx >= 0 &&
        static_cast<uint32_t>(idx) < materials->count) {
      return materials->slots[idx].shader_name;
    }
    if (idx >= 0 && idx < lod.materialCount) {
      return lod.materials[idx].name;
    }
    return "";
  };

  std::vector<bool> material_alpha(static_cast<size_t>(material_count), false);
  for (int m = 0; m < material_count; ++m) {
    const auto flags =
        lookup_material_info_flags(material_name(m));
    material_alpha[static_cast<size_t>(m)] =
        (flags & MATERIAL_FLAG_BLENDING) != 0;
  }

  std::vector<std::vector<SmoothedFace>> smoothed_per_sub;
  smoothed_per_sub.reserve(static_cast<size_t>(lod.subobjectCount));
  for (int32_t sub_idx = 0; sub_idx < lod.subobjectCount; ++sub_idx) {
    smoothed_per_sub.push_back(
        compute_smoothed_vectors_float(lod.subobjects[sub_idx]));
  }

  std::vector<RenderVertex> final_vertices;
  std::vector<uint16_t> final_indices;
  std::vector<ThreediTriangleStrip> normal_strips;
  std::vector<ThreediTriangleStrip> alpha_strips;

  auto apply_tiling = [](float u, float v, float u_tiling, float v_tiling,
                         float &out_u, float &out_v) {
    if (u_tiling != 0.0f) {
      out_u = (u - 0.5f) * u_tiling + 0.5f;
    } else {
      out_u = u;
    }
    if (v_tiling != 0.0f) {
      out_v = (v - 0.5f) * v_tiling + 0.5f;
    } else {
      out_v = v;
    }
  };

  for (int mat_idx = 0; mat_idx < material_count; ++mat_idx) {
    std::vector<RenderVertex> mat_vertices;
    std::vector<uint16_t> mat_indices;

    for (int32_t sub_idx = 0; sub_idx < lod.subobjectCount; ++sub_idx) {
      const SubObject &subobj = lod.subobjects[sub_idx];
      const auto &smoothed = smoothed_per_sub[static_cast<size_t>(sub_idx)];

      for (int32_t face_idx = 0; face_idx < subobj.faceCount; ++face_idx) {
        const Face &face = subobj.faces[face_idx];
        if (face.hasDollar) continue;
        if (face.matIndex != mat_idx) continue;

        uint16_t tri_idx[3]{};
        for (int v_idx = 0; v_idx < 3; ++v_idx) {
          RenderVertex rv{};
          const uint32_t vert_index = face.vert[v_idx];
          if (vert_index >= subobj.vertCount) {
            error = "vertex index out of bounds";
            return false;
          }
          const Vertex &src_vert = subobj.verts[vert_index];

          rv.position[0] = -src_vert.pos.y;
          rv.position[1] = src_vert.pos.z;
          rv.position[2] = src_vert.pos.x;

          for (int b = 0; b < 4; ++b) {
            rv.bone_weights[b] = src_vert.boneWeight[b];
            rv.bone_indices[b] = src_vert.boneIndex[b];
          }

          const SmoothedFace *smoothed_face =
              (face_idx < static_cast<int32_t>(smoothed.size()))
                  ? &smoothed[static_cast<size_t>(face_idx)]
                  : nullptr;
          const SmoothedVertexVectors *smoothed_vecs =
              smoothed_face ? &smoothed_face->verts[v_idx] : nullptr;

          if (smoothed_vecs) {
            rv.normal[0] = -smoothed_vecs->normal[1];
            rv.normal[1] = smoothed_vecs->normal[2];
            rv.normal[2] = smoothed_vecs->normal[0];

            rv.tangent[0] = smoothed_vecs->tangent[1];
            rv.tangent[1] = -smoothed_vecs->tangent[2];
            rv.tangent[2] = -smoothed_vecs->tangent[0];

            rv.bitangent[0] = smoothed_vecs->bitangent[1];
            rv.bitangent[1] = -smoothed_vecs->bitangent[2];
            rv.bitangent[2] = -smoothed_vecs->bitangent[0];
          } else {
            rv.normal[0] = -face.faceBasis[0][1];
            rv.normal[1] = face.faceBasis[0][2];
            rv.normal[2] = face.faceBasis[0][0];
            normalize_vec3_x87(rv.normal);

            float tangent[3] = {rv.normal[2], 0.0f, -rv.normal[0]};
            normalize_vec3_x87(tangent);
            rv.tangent[0] = tangent[0];
            rv.tangent[1] = tangent[1];
            rv.tangent[2] = tangent[2];

            float bitangent[3];
            cross3(rv.normal, tangent, bitangent);
            normalize_vec3_x87(bitangent);
            rv.bitangent[0] = bitangent[0];
            rv.bitangent[1] = bitangent[1];
            rv.bitangent[2] = bitangent[2];
          }

          float base_u = 0.0f, base_v = 0.0f;
          if (subobj.uvs && face.uv[v_idx] < subobj.uvCount) {
            const Uv &src_uv = subobj.uvs[face.uv[v_idx]];
            base_u = src_uv.uv[0];
            base_v = src_uv.uv[1];
          }

          const MaterialBucketSlot *slot = nullptr;
          if (materials && mat_idx >= 0 &&
              static_cast<uint32_t>(mat_idx) < materials->count) {
            slot = &materials->slots[mat_idx];
          }

          float uv0_u_tiling = slot ? slot->uv0_u_tiling
                                    : ((mat_idx < lod.materialCount)
                                           ? lod.materials[mat_idx].uv_u_tiling[0]
                                           : 0.0f);
          float uv0_v_tiling = slot ? slot->uv0_v_tiling
                                    : ((mat_idx < lod.materialCount)
                                           ? lod.materials[mat_idx].uv_v_tiling[0]
                                           : 0.0f);
          float uv1_u_tiling = slot ? slot->uv1_u_tiling
                                    : ((mat_idx < lod.materialCount)
                                           ? lod.materials[mat_idx].uv_u_tiling[1]
                                           : 0.0f);
          float uv1_v_tiling = slot ? slot->uv1_v_tiling
                                    : ((mat_idx < lod.materialCount)
                                           ? lod.materials[mat_idx].uv_v_tiling[1]
                                           : 0.0f);

          apply_tiling(base_u, base_v, uv0_u_tiling, uv0_v_tiling, rv.uv0[0],
                       rv.uv0[1]);
          apply_tiling(base_u, base_v, uv1_u_tiling, uv1_v_tiling, rv.uv1[0],
                       rv.uv1[1]);

          std::memset(rv.padding, 0, sizeof(rv.padding));
          sort_bone_weights(rv);

          uint16_t vertex_idx = 0;
          bool found = false;
          for (size_t i = 0; i < mat_vertices.size(); ++i) {
            if (mat_vertices[i] == rv) {
              vertex_idx = static_cast<uint16_t>(i);
              found = true;
              break;
            }
          }
          if (!found) {
            if (mat_vertices.size() >= 65535) {
              error = "vertex count exceeds 65535";
              return false;
            }
            vertex_idx = static_cast<uint16_t>(mat_vertices.size());
            mat_vertices.push_back(rv);
          }
          tri_idx[v_idx] = vertex_idx;
        }

        mat_indices.push_back(tri_idx[2]);
        mat_indices.push_back(tri_idx[1]);
        mat_indices.push_back(tri_idx[0]);
      }
    }

    if (mat_indices.empty()) continue;

    std::vector<SkinnedGroup> groups;
    auto bones_for_vertex = [&](const RenderVertex &rv,
                                std::vector<int32_t> &out) {
      out.clear();
      out.reserve(4);
      for (int b = 0; b < 4; ++b) {
        const int32_t bone = rv.bone_indices[b];
        bool exists = false;
        for (int32_t v : out) {
          if (v == bone) {
            exists = true;
            break;
          }
        }
        if (!exists) out.push_back(bone);
      }
    };

    for (size_t t = 0; t + 2 < mat_indices.size(); t += 3) {
      // Gather bones per vertex (unique within a vertex) but keep duplicates
      // across vertices to mirror the original palette splitter's accounting.
      std::vector<int32_t> tri_bones_occurrence;
      std::vector<int32_t> temp;
      bones_for_vertex(mat_vertices[mat_indices[t]], temp);
      tri_bones_occurrence.insert(tri_bones_occurrence.end(), temp.begin(),
                                  temp.end());
      bones_for_vertex(mat_vertices[mat_indices[t + 1]], temp);
      tri_bones_occurrence.insert(tri_bones_occurrence.end(), temp.begin(),
                                  temp.end());
      bones_for_vertex(mat_vertices[mat_indices[t + 2]], temp);
      tri_bones_occurrence.insert(tri_bones_occurrence.end(), temp.begin(),
                                  temp.end());

      int target = -1;
      // First-fit: walk palettes in order and pick the first that can accept
      // all of the triangle's bones without exceeding 16 entries.
      for (size_t gi = 0; gi < groups.size(); ++gi) {
        auto &bt = groups[gi].bone_table;
        size_t count = bt.size();
        for (int32_t b : tri_bones_occurrence) {
          if (std::find(bt.begin(), bt.end(), b) == bt.end()) {
            ++count;
          }
        }
        if (count <= 16) {
          target = static_cast<int>(gi);
          for (int32_t b : tri_bones_occurrence) {
            if (std::find(bt.begin(), bt.end(), b) == bt.end()) {
              bt.push_back(b);
            }
          }
          break;
        }
      }
      if (target == -1) {
        SkinnedGroup g;
        for (int32_t b : tri_bones_occurrence) {
          if (std::find(g.bone_table.begin(), g.bone_table.end(), b) ==
              g.bone_table.end()) {
            g.bone_table.push_back(b);
          }
        }
        groups.push_back(std::move(g));
        target = static_cast<int>(groups.size() - 1);
      }
      groups[static_cast<size_t>(target)].triangles.push_back(mat_indices[t]);
      groups[static_cast<size_t>(target)].triangles.push_back(mat_indices[t + 1]);
      groups[static_cast<size_t>(target)].triangles.push_back(mat_indices[t + 2]);
    }

    for (const auto &group : groups) {
      if (group.triangles.empty()) continue;
      if (group.bone_table.size() > 16) {
        error = "bone table exceeds 16 entries";
        return false;
      }

      std::vector<uint8_t> used(mat_vertices.size(), 0);
      for (uint16_t idx : group.triangles) {
        if (idx < used.size()) used[idx] = 1;
      }
      std::vector<uint16_t> old_to_new(mat_vertices.size(), 0xFFFF);
      std::vector<uint16_t> used_order;
      used_order.reserve(mat_vertices.size());
      for (size_t i = 0; i < mat_vertices.size(); ++i) {
        if (used[i]) {
          old_to_new[i] = static_cast<uint16_t>(used_order.size());
          used_order.push_back(static_cast<uint16_t>(i));
        }
      }

      std::vector<RenderVertex> group_vertices;
      group_vertices.reserve(used_order.size());
      for (uint16_t idx : used_order) {
        group_vertices.push_back(mat_vertices[idx]);
      }

      std::vector<uint16_t> remapped_tris;
      remapped_tris.reserve(group.triangles.size());
      for (uint16_t idx : group.triangles) {
        remapped_tris.push_back(old_to_new[idx]);
      }

      StripifyResult stripified =
          stripify_triangles(remapped_tris, /*subobject_index=*/0, mat_idx);
      std::vector<uint16_t> tri_list = std::move(stripified.indices);
      const int tri_count = stripified.triangle_count;

      std::vector<uint16_t> remap(group_vertices.size(), 0xFFFF);
      std::vector<RenderVertex> compact_vertices;
      compact_vertices.reserve(group_vertices.size());
      for (uint16_t idx : tri_list) {
        if (idx >= remap.size()) {
          error = "triangle index out of range during skinned remap";
          return false;
        }
        if (remap[idx] == 0xFFFF) {
          remap[idx] = static_cast<uint16_t>(compact_vertices.size());
          compact_vertices.push_back(group_vertices[idx]);
        }
      }

      std::vector<uint16_t> final_tris;
      final_tris.reserve(tri_list.size());
      for (uint16_t idx : tri_list) {
        final_tris.push_back(remap[idx]);
      }

      std::array<int32_t, 256> bone_map{};
      bone_map.fill(-1);
      for (size_t i = 0; i < group.bone_table.size(); ++i) {
        const int32_t bone = group.bone_table[i];
        if (bone >= 0 && bone < static_cast<int32_t>(bone_map.size())) {
          bone_map[static_cast<size_t>(bone)] = static_cast<int32_t>(i);
        }
      }

      for (auto &v : compact_vertices) {
        for (int b = 0; b < 4; ++b) {
          const int32_t bone = v.bone_indices[b];
          if (bone >= 0 && bone < static_cast<int32_t>(bone_map.size())) {
            v.bone_indices[b] = bone_map[static_cast<size_t>(bone)];
          } else {
            v.bone_indices[b] = 0;
          }
        }
      }

      if (final_vertices.size() + compact_vertices.size() > 0xFFFF) {
        error = "vertex count exceeds 65535";
        return false;
      }

      const uint16_t start_vertex = static_cast<uint16_t>(final_vertices.size());
      const size_t index_offset = final_indices.size();

      final_vertices.insert(final_vertices.end(), compact_vertices.begin(),
                            compact_vertices.end());
      final_indices.reserve(final_indices.size() + final_tris.size());
      for (uint16_t idx : final_tris) {
        final_indices.push_back(idx);
      }

      ThreediTriangleStrip strip{};
      strip.material_index = mat_idx;
      strip.index_offset = static_cast<int32_t>(index_offset);
      strip.num_indices = static_cast<uint16_t>(final_tris.size());
      strip.num_triangles = static_cast<uint16_t>(tri_count);
      strip.is_strip = 0;
      strip.start_vertex = start_vertex;
      strip.num_vertices = static_cast<int32_t>(compact_vertices.size());
      strip.min[0] = strip.min[1] = strip.min[2] = 0.0f;
      strip.max[0] = strip.max[1] = strip.max[2] = 0.0f;
      strip.bone_table_length = static_cast<int32_t>(group.bone_table.size());
      std::memset(strip.bone_table, 0, sizeof(strip.bone_table));
      for (size_t i = 0; i < group.bone_table.size() && i < 16; ++i) {
        strip.bone_table[i] =
            static_cast<uint8_t>(group.bone_table[i] & 0xFF);
      }

      if (material_alpha[static_cast<size_t>(mat_idx)]) {
        alpha_strips.push_back(strip);
      } else {
        normal_strips.push_back(strip);
      }
    }
  }

  auto adjust_indices_relative = [&](const std::vector<ThreediTriangleStrip> &strips) -> bool {
    for (const auto &s : strips) {
      const size_t start = static_cast<size_t>(s.index_offset);
      const size_t end = start + static_cast<size_t>(s.num_indices);
      if (end > final_indices.size()) {
        error = "strip index range out of bounds for skinned mesh";
        return false;
      }
      uint16_t min_val = 0xFFFF;
      for (size_t i = start; i < end; ++i) {
        if (final_indices[i] < min_val) {
          min_val = final_indices[i];
        }
      }
      for (size_t i = start; i < end; ++i) {
        uint16_t &val = final_indices[i];
        val = static_cast<uint16_t>(val - min_val);
      }
    }
    return true;
  };

  if (!adjust_indices_relative(normal_strips) ||
      !adjust_indices_relative(alpha_strips)) {
    return false;
  }

  out_lod.vertices.count = static_cast<uint32_t>(final_vertices.size());
  out_lod.vertices.stride = 56;
  out_lod.vertices.flags =
      static_cast<uint32_t>(THREEDI_VERTEX_FLAG_SKINNED | 1u);

  if (out_lod.vertices.count > 0) {
    out_lod.vertices.items = static_cast<ThreediVertex *>(
        std::calloc(out_lod.vertices.count, sizeof(ThreediVertex)));
    if (!out_lod.vertices.items) {
      error = "failed to allocate skinned vertex buffer";
      return false;
    }
    for (uint32_t i = 0; i < out_lod.vertices.count; ++i) {
      const RenderVertex &rv = final_vertices[i];
      ThreediVertex &tv = out_lod.vertices.items[i];
      tv.position[0] = rv.position[0];
      tv.position[1] = rv.position[1];
      tv.position[2] = rv.position[2];
      tv.bone_weights[0] = rv.bone_weights[0];
      tv.bone_weights[1] = rv.bone_weights[1];
      tv.bone_weights[2] = rv.bone_weights[2];
      tv.bone_indices[0] = static_cast<uint8_t>(rv.bone_indices[0]);
      tv.bone_indices[1] = static_cast<uint8_t>(rv.bone_indices[1]);
      tv.bone_indices[2] = static_cast<uint8_t>(rv.bone_indices[2]);
      tv.bone_indices[3] = static_cast<uint8_t>(rv.bone_indices[3]);
      tv.normal[0] = rv.normal[0];
      tv.normal[1] = rv.normal[1];
      tv.normal[2] = rv.normal[2];
      tv.uv0[0] = rv.uv0[0];
      tv.uv0[1] = rv.uv0[1];
      tv.uv1[0] = rv.uv1[0];
      tv.uv1[1] = rv.uv1[1];
      tv.flags = out_lod.vertices.flags;
      tv.has_tangents = 0;
      tv.is_skinned = 1;
    }
  } else {
    out_lod.vertices.items = nullptr;
  }

  out_lod.indices.count = static_cast<uint32_t>(final_indices.size());
  if (out_lod.indices.count > 0) {
    out_lod.indices.indices = static_cast<uint16_t *>(
        std::calloc(out_lod.indices.count, sizeof(uint16_t)));
    if (!out_lod.indices.indices) {
      error = "failed to allocate skinned index buffer";
      return false;
    }
    std::memcpy(out_lod.indices.indices, final_indices.data(),
                final_indices.size() * sizeof(uint16_t));
  } else {
    out_lod.indices.indices = nullptr;
  }

  out_lod.strip_record_size = 68;
  out_lod.strip_count = normal_strips.size() + alpha_strips.size();
  if (out_lod.strip_count > 0) {
    out_lod.strips = static_cast<ThreediTriangleStrip *>(
        std::calloc(out_lod.strip_count, sizeof(ThreediTriangleStrip)));
    if (!out_lod.strips) {
      error = "failed to allocate skinned strips";
      return false;
    }
    size_t cursor = 0;
    for (const auto &s : normal_strips) {
      out_lod.strips[cursor++] = s;
    }
    for (const auto &s : alpha_strips) {
      out_lod.strips[cursor++] = s;
    }
  } else {
    out_lod.strips = nullptr;
  }

  out_lod.render_object_count = lod.subobjectCount;
  if (out_lod.render_object_count > 0) {
    out_lod.render_objects = static_cast<ThreediRenderObject *>(
        std::calloc(out_lod.render_object_count, sizeof(ThreediRenderObject)));
    if (!out_lod.render_objects) {
      error = "failed to allocate skinned render_objects";
      return false;
    }
    for (int32_t sub = 0; sub < lod.subobjectCount; ++sub) {
      const SubObject &subobj = lod.subobjects[sub];
      ThreediRenderObject &ro = out_lod.render_objects[sub];
      ro.num_strips = (sub == 0) ? static_cast<int32_t>(normal_strips.size()) : 0;
      ro.num_alpha_strips =
          (sub == 0) ? static_cast<int32_t>(alpha_strips.size()) : 0;
      ro.parent_index = static_cast<int32_t>(lod.subobjects[sub].attachIndex);

      const bool has_center =
          lod.centerPoints && sub < static_cast<int>(lod.centerCount) &&
          ro.parent_index >= 0 &&
          ro.parent_index < static_cast<int32_t>(lod.centerCount);
      if (sub == 0) {
        ro.rel[0] = has_center ? -(lod.centerPoints[0].pos[1] -
                                   lod.centerPoints[ro.parent_index].pos[1])
                               : 0.0f;
        ro.rel[1] = has_center ? (lod.centerPoints[0].pos[2] -
                                   lod.centerPoints[ro.parent_index].pos[2])
                               : 0.0f;
        ro.rel[2] = has_center ? (lod.centerPoints[0].pos[0] -
                                   lod.centerPoints[ro.parent_index].pos[0])
                               : 0.0f;
        ro.abs[0] = has_center ? -lod.centerPoints[0].pos[1] : 0.0f;
        ro.abs[1] = has_center ? lod.centerPoints[0].pos[2] : 0.0f;
        ro.abs[2] = has_center ? lod.centerPoints[0].pos[0] : 0.0f;
      } else {
        ro.rel[0] = has_center ? -(lod.centerPoints[sub].pos[1] -
                                   lod.centerPoints[ro.parent_index].pos[1])
                               : 0.0f;
        ro.rel[1] = has_center ? (lod.centerPoints[sub].pos[2] -
                                   lod.centerPoints[ro.parent_index].pos[2])
                               : 0.0f;
        ro.rel[2] = has_center ? (lod.centerPoints[sub].pos[0] -
                                   lod.centerPoints[ro.parent_index].pos[0])
                               : 0.0f;
        ro.abs[0] = has_center ? -lod.centerPoints[sub].pos[1] : 0.0f;
        ro.abs[1] = has_center ? lod.centerPoints[sub].pos[2] : 0.0f;
        ro.abs[2] = has_center ? lod.centerPoints[sub].pos[0] : 0.0f;
      }

      float minX = 10000.0f, minY = 10000.0f, minZ = 10000.0f;
      float maxX = -10000.0f, maxY = -10000.0f, maxZ = -10000.0f;
      for (uint32_t v = 0; v < subobj.vertCount; ++v) {
        const Vertex &sv = subobj.verts[v];
        const float x = -sv.pos.y;
        const float y = sv.pos.z;
        const float z = sv.pos.x;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        minZ = std::min(minZ, z);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        maxZ = std::max(maxZ, z);
      }
      const float cx = (minX + maxX) * 0.5f;
      const float cy = (minY + maxY) * 0.5f;
      const float cz = (minZ + maxZ) * 0.5f;
      ro.bounding_center[0] = cx;
      ro.bounding_center[1] = cy;
      ro.bounding_center[2] = cz;

      float radius = 0.0f;
      for (uint32_t v = 0; v < subobj.vertCount; ++v) {
        const Vertex &sv = subobj.verts[v];
        const float x = -sv.pos.y;
        const float y = sv.pos.z;
        const float z = sv.pos.x;
        const float dx = x - cx;
        const float dy = y - cy;
        const float dz = z - cz;
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > radius) {
          radius = dist2;
        }
      }
      ro.bounding_radius = radius > 0.0f ? std::sqrt(radius) : 0.0f;
    }
  } else {
    out_lod.render_objects = nullptr;
  }

  return true;
}
bool build_render_geometry(const LodHeader &lod,
                           const MaterialTable *materials,
                           bool force_skinned_path,
                           ThreediLod &out_lod, std::string &error) {
  const bool is_skinned = force_skinned_path || ((lod.flags & 4u) != 0);

  if (is_skinned) {
    return build_render_geometry_skinned(lod, materials, out_lod, error);
  }

  std::vector<RenderVertex> vertex_cache;

  int max_mat_index = -1;
  for (int32_t si = 0; si < lod.subobjectCount; ++si) {
    const SubObject &so = lod.subobjects[si];
    for (int32_t fi = 0; fi < so.faceCount; ++fi) {
      if (so.faces[fi].matIndex > max_mat_index) {
        max_mat_index = so.faces[fi].matIndex;
      }
    }
  }
  const int material_count =
      std::max<int>(1, std::max<int>(lod.materialCount, max_mat_index + 1));
  auto material_name = [&](int idx) -> const char * {
    if (materials && idx >= 0 &&
        static_cast<uint32_t>(idx) < materials->count) {
      return materials->slots[idx].shader_name;
    }
    if (idx >= 0 && idx < lod.materialCount) {
      return lod.materials[idx].name;
    }
    return "";
  };
  std::vector<bool> material_alpha(static_cast<size_t>(material_count), false);
  for (int m = 0; m < material_count; ++m) {
    const auto flags = lookup_material_info_flags(material_name(m));
    material_alpha[static_cast<size_t>(m)] =
        (flags & MATERIAL_FLAG_BLENDING) != 0;
  }
  auto for_materials = [&](auto &&fn) {
    for (int pass = 0; pass < 2; ++pass) {
      for (int mat = 0; mat < material_count; ++mat) {
        const bool is_alpha = material_alpha[static_cast<size_t>(mat)];
        if ((is_alpha ? 1 : 0) != pass) continue;
        fn(mat, is_alpha);
      }
    }
  };
  bool has_tangents = false;
  for (int m = 0; m < material_count; ++m) {
    const auto flags =
        lookup_material_info_flags(material_name(m));
    if ((flags & (MATERIAL_FLAG_NORMAL_A | MATERIAL_FLAG_NORMAL_B)) != 0) {
      has_tangents = true;
      break;
    }
  }
  std::vector<std::vector<StripBucket>> buckets(
      static_cast<size_t>(lod.subobjectCount),
      std::vector<StripBucket>(static_cast<size_t>(material_count)));

  // Process all subobjects and faces, building deduplicated vertices.
  for (int32_t subobj_idx = 0; subobj_idx < lod.subobjectCount; ++subobj_idx) {
    const SubObject &subobj = lod.subobjects[subobj_idx];

    // When tangents are in the output, override SG=0 faces to SG=1 so that
    // compute_smoothed_vectors averages tangent/bitangent correctly.
    // SG=0 faces come from the flat-component heuristic in scene_builder/meshes.py
    // which is only useful for stride-40 (no-tangent) dedup accuracy.
    std::vector<Face> face_overrides;
    SubObject subobj_for_smooth = subobj;
    if (has_tangents) {
      bool need_override = false;
      for (int32_t fi = 0; fi < subobj.faceCount; ++fi) {
        if (subobj.faces[fi].smoothingGroup == 0) { need_override = true; break; }
      }
      if (need_override) {
        face_overrides.assign(subobj.faces, subobj.faces + subobj.faceCount);
        for (auto &f : face_overrides) {
          if (f.smoothingGroup == 0) f.smoothingGroup = 1;
        }
        subobj_for_smooth.faces = face_overrides.data();
      }
    }

    const auto smoothed = compute_smoothed_vectors(subobj_for_smooth);
    const bool has_pre_normals = (subobj.preSmoothedNormals != nullptr);

    for (int32_t face_idx = 0; face_idx < subobj.faceCount; ++face_idx) {
      const Face &face = subobj.faces[face_idx];

      // Process the 3 vertices of this triangle.
      uint16_t tri_idx[3]{};
      for (int v_idx = 0; v_idx < 3; ++v_idx) {
        RenderVertex rv{};
        const SmoothedFace *smoothed_face =
            (face_idx < static_cast<int32_t>(smoothed.size()))
                ? &smoothed[static_cast<size_t>(face_idx)]
                : nullptr;
        const SmoothedVertexVectors *smoothed_vecs =
            smoothed_face ? &smoothed_face->verts[v_idx] : nullptr;

        // Position: negate Y coordinate (engine coord system conversion).
        const uint32_t vert_index = face.vert[v_idx];
        if (vert_index >= subobj.vertCount) {
          error = "vertex index out of bounds";
          return false;
        }
        const Vertex &src_vert = subobj.verts[vert_index];
        rv.position[0] = -src_vert.pos.y;
        rv.position[1] = src_vert.pos.z;
        rv.position[2] = src_vert.pos.x;
        if (is_skinned) {
          rv.bone_weights[0] = src_vert.boneWeight[0];
          rv.bone_weights[1] = src_vert.boneWeight[1];
          rv.bone_weights[2] = src_vert.boneWeight[2];
          rv.bone_indices[0] = static_cast<uint8_t>(src_vert.boneIndex[0]);
          rv.bone_indices[1] = static_cast<uint8_t>(src_vert.boneIndex[1]);
          rv.bone_indices[2] = static_cast<uint8_t>(src_vert.boneIndex[2]);
          rv.bone_indices[3] = static_cast<uint8_t>(src_vert.boneIndex[3]);
        } else {
          rv.bone_weights[0] = rv.bone_weights[1] = rv.bone_weights[2] = 0.0f;
          rv.bone_indices[0] = rv.bone_indices[1] = rv.bone_indices[2] =
              rv.bone_indices[3] = 0;
        }

        if (has_pre_normals) {
          // Use pre-computed normals from IR, passed through the ASE roundtrip.
          // The normals are in parsed coordinate space (same as vertex positions
          // after the ASE parser's swizzle). Apply the standard output swizzle.
          const float *pn = &subobj.preSmoothedNormals[
              static_cast<size_t>(face_idx) * 9 + static_cast<size_t>(v_idx) * 3];
          rv.normal[0] = -pn[1];
          rv.normal[1] = pn[2];
          rv.normal[2] = pn[0];

          // Use smoothed tangent/bitangent from compute_smoothed_vectors.
          if (smoothed_vecs) {
            rv.tangent[0] = smoothed_vecs->tangent[1];
            rv.tangent[1] = -smoothed_vecs->tangent[2];
            rv.tangent[2] = -smoothed_vecs->tangent[0];

            rv.bitangent[0] = smoothed_vecs->bitangent[1];
            rv.bitangent[1] = -smoothed_vecs->bitangent[2];
            rv.bitangent[2] = -smoothed_vecs->bitangent[0];
          }
        } else if (smoothed_vecs) {
          // Smoothed normals/tangents/bitangents (sub_457360 output).
          rv.normal[0] = -smoothed_vecs->normal[1];
          rv.normal[1] = smoothed_vecs->normal[2];
          rv.normal[2] = smoothed_vecs->normal[0];

          // Tangent/binormal match WriteRDTA sign swizzles.
          rv.tangent[0] = smoothed_vecs->tangent[1];
          rv.tangent[1] = -smoothed_vecs->tangent[2];
          rv.tangent[2] = -smoothed_vecs->tangent[0];

          rv.bitangent[0] = smoothed_vecs->bitangent[1];
          rv.bitangent[1] = -smoothed_vecs->bitangent[2];
          rv.bitangent[2] = -smoothed_vecs->bitangent[0];
        } else {
          // Fallback to per-face data when smoothing is unavailable.
          rv.normal[0] = -face.faceBasis[0][1];
          rv.normal[1] = face.faceBasis[0][2];
          rv.normal[2] = face.faceBasis[0][0];
          normalize_vec3_x87(rv.normal);

          float tangent[3] = {rv.normal[2], 0.0f, -rv.normal[0]};
          normalize_vec3_x87(tangent);
          rv.tangent[0] = tangent[0];
          rv.tangent[1] = tangent[1];
          rv.tangent[2] = tangent[2];

          float bitangent[3];
          cross3(rv.normal, tangent, bitangent);
          normalize_vec3_x87(bitangent);
          rv.bitangent[0] = bitangent[0];
          rv.bitangent[1] = bitangent[1];
          rv.bitangent[2] = bitangent[2];
        }

        // UV coordinates with per-material tiling/offsets.
        float base_u = 0.0f, base_v = 0.0f;
        if (subobj.uvs && face.uv[v_idx] < subobj.uvCount) {
          const Uv &src_uv = subobj.uvs[face.uv[v_idx]];
          base_u = src_uv.uv[0];
          base_v = src_uv.uv[1];
        }

        const MaterialBucketSlot *slot = nullptr;
        if (materials && face.matIndex >= 0 &&
            static_cast<uint32_t>(face.matIndex) < materials->count) {
          slot = &materials->slots[face.matIndex];
        }

        // UV tiling matches WriteRDTA (IDA 0x459000):
        //   - Only apply tiling when u_tiling != 0 (no offset, no rattrib check)
        //   - Formula: (uv - 0.5) * tiling + 0.5
        //   - UV0 always uses uv0 tiling, UV1 always uses uv1 tiling
        rv.uv0[0] = base_u;
        rv.uv0[1] = base_v;
        rv.uv1[0] = base_u;
        rv.uv1[1] = base_v;
        // Use double intermediates to match x87 extended-precision tiling.
        if (slot) {
          if (slot->uv0_u_tiling != 0.0f) {
            rv.uv0[0] = static_cast<float>(((double)base_u - 0.5) * (double)slot->uv0_u_tiling + 0.5);
            rv.uv0[1] = static_cast<float>(((double)base_v - 0.5) * (double)slot->uv0_v_tiling + 0.5);
          }
          if (slot->uv1_u_tiling != 0.0f) {
            rv.uv1[0] = static_cast<float>(((double)base_u - 0.5) * (double)slot->uv1_u_tiling + 0.5);
            rv.uv1[1] = static_cast<float>(((double)base_v - 0.5) * (double)slot->uv1_v_tiling + 0.5);
          }
        }

        // Zero out padding for consistent memcmp.
        std::memset(rv.padding, 0, sizeof(rv.padding));

        // Deduplicate: search for existing vertex.
        uint16_t vertex_idx = 0;
        bool found = false;
        for (size_t i = 0; i < vertex_cache.size(); ++i) {
          if (vertex_cache[i] == rv) {
            vertex_idx = static_cast<uint16_t>(i);
            found = true;
            break;
          }
        }

        if (!found) {
          if (vertex_cache.size() >= 65535) {
            error = "vertex count exceeds 65535";
            return false;
          }
          vertex_idx = static_cast<uint16_t>(vertex_cache.size());
          vertex_cache.push_back(rv);
        }

        tri_idx[v_idx] = vertex_idx;
      }

      // Skip faces with the dollar flag (non-rendered geometry).
      if (face.hasDollar) {
        continue;
      }

      // Accumulate into material bucket (triangles stored with reversed order).
      const int mat_idx = (face.matIndex >= 0 && face.matIndex < material_count)
                              ? face.matIndex
                              : 0;
      auto &bucket = buckets[static_cast<size_t>(subobj_idx)]
                           [static_cast<size_t>(mat_idx)];
      bucket.tris.push_back(tri_idx[2]);
      bucket.tris.push_back(tri_idx[1]);
      bucket.tris.push_back(tri_idx[0]);

      // Update bucket bounds.
      for (int v = 0; v < 3; ++v) {
        const RenderVertex &rv = vertex_cache[tri_idx[v]];
        bucket.bboxMin.x = std::min(bucket.bboxMin.x, rv.position[0]);
        bucket.bboxMin.y = std::min(bucket.bboxMin.y, rv.position[1]);
        bucket.bboxMin.z = std::min(bucket.bboxMin.z, rv.position[2]);
        bucket.bboxMax.x = std::max(bucket.bboxMax.x, rv.position[0]);
        bucket.bboxMax.y = std::max(bucket.bboxMax.y, rv.position[1]);
        bucket.bboxMax.z = std::max(bucket.bboxMax.z, rv.position[2]);
      }
    }
  }

  // Stripify/reorder triangles per bucket to mirror WriteRDTA.
  std::vector<std::vector<StripifyResult>> bucket_tris(
      buckets.size(), std::vector<StripifyResult>(material_count));
  for (size_t sub_idx = 0; sub_idx < buckets.size(); ++sub_idx) {
    for (int mat = 0; mat < material_count; ++mat) {
      bucket_tris[sub_idx][mat] = stripify_triangles(
          buckets[sub_idx][mat].tris,
          static_cast<int>(sub_idx),
          mat);
    }
  }

  // Remap vertices following stripified order.
  std::vector<uint16_t> remap(vertex_cache.size(), 0xFFFF);
  uint16_t next_idx = 0;
  bool remap_failed = false;
  for (size_t sub_idx = 0; sub_idx < bucket_tris.size(); ++sub_idx) {
    for (int mat = 0; mat < material_count; ++mat) {
      if (remap_failed) break;
      const auto &stripified = bucket_tris[sub_idx][mat];
      for (size_t i = 0; i < stripified.indices.size(); ++i) {
        const uint16_t orig = stripified.indices[i];
        if (orig >= remap.size()) {
          error = "triangle index out of range during remap";
          remap_failed = true;
          break;
        }
        if (remap[orig] == 0xFFFF) {
          if (next_idx == 0xFFFF) {
            error = "vertex remap exceeded 65535";
            remap_failed = true;
            break;
          }
          remap[orig] = next_idx++;
        }
      }
    }
    if (remap_failed) return false;
  }

  // Apply remap to vertex cache.
  std::vector<RenderVertex> remapped_vertices(next_idx);
  for (size_t i = 0; i < vertex_cache.size(); ++i) {
    if (remap[i] != 0xFFFF) {
      remapped_vertices[remap[i]] = vertex_cache[i];
    }
  }
  vertex_cache.swap(remapped_vertices);

  // Build strip params and index buffer (indices relative to strip start).
  std::vector<uint16_t> index_buffer;
  std::vector<StripBuild> strips;
  for (size_t sub_idx = 0; sub_idx < bucket_tris.size(); ++sub_idx) {
    for_materials([&](int mat, bool /*is_alpha*/) {
      const auto &stripified = bucket_tris[sub_idx][mat];
      if (stripified.indices.empty()) return;
      uint16_t min_idx = 0xFFFF;
      uint16_t max_idx = 0;
      for (uint16_t idx : stripified.indices) {
        const uint16_t mapped = remap[idx];
        min_idx = std::min(min_idx, mapped);
        max_idx = std::max(max_idx, mapped);
      }
      StripBuild sb{};
      sb.material_index = mat;
      sb.index_offset = static_cast<int32_t>(index_buffer.size());
      sb.num_indices = static_cast<uint16_t>(stripified.indices.size());
      sb.num_triangles = static_cast<uint16_t>(stripified.triangle_count);
      sb.is_strip = 0;
      sb.start_vertex = min_idx;
      sb.num_vertices = static_cast<int32_t>(max_idx - min_idx + 1);
      const auto &bbox = buckets[sub_idx][mat];
      sb.min[0] = bbox.bboxMin.x;
      sb.min[1] = bbox.bboxMin.y;
      sb.min[2] = bbox.bboxMin.z;
      sb.max[0] = bbox.bboxMax.x;
      sb.max[1] = bbox.bboxMax.y;
      sb.max[2] = bbox.bboxMax.z;

      for (uint16_t idx : stripified.indices) {
        index_buffer.push_back(static_cast<uint16_t>(remap[idx] - min_idx));
      }
      strips.push_back(sb);
    });
  }

  // Allocate and populate the output vertex buffer.
  out_lod.vertices.count = static_cast<uint32_t>(vertex_cache.size());
  out_lod.vertices.stride =
      is_skinned ? 56 : (has_tangents ? 64 : 40);  // add weights/indices/tangents when present
  out_lod.vertices.flags = static_cast<uint32_t>(
      (is_skinned ? THREEDI_VERTEX_FLAG_SKINNED : 0u) |
      (has_tangents ? THREEDI_VERTEX_FLAG_TANGENTS : 0u) | 1u);

  if (out_lod.vertices.count > 0) {
    out_lod.vertices.items = static_cast<ThreediVertex *>(
        std::calloc(out_lod.vertices.count, sizeof(ThreediVertex)));
    if (!out_lod.vertices.items) {
      error = "failed to allocate vertex buffer";
      return false;
    }

    for (uint32_t i = 0; i < out_lod.vertices.count; ++i) {
      const RenderVertex &rv = vertex_cache[i];
      ThreediVertex &tv = out_lod.vertices.items[i];

      tv.position[0] = rv.position[0];
      tv.position[1] = rv.position[1];
      tv.position[2] = rv.position[2];

      tv.bone_weights[0] = rv.bone_weights[0];
      tv.bone_weights[1] = rv.bone_weights[1];
      tv.bone_weights[2] = rv.bone_weights[2];
      tv.bone_indices[0] = rv.bone_indices[0];
      tv.bone_indices[1] = rv.bone_indices[1];
      tv.bone_indices[2] = rv.bone_indices[2];
      tv.bone_indices[3] = rv.bone_indices[3];

      tv.normal[0] = rv.normal[0];
      tv.normal[1] = rv.normal[1];
      tv.normal[2] = rv.normal[2];

      tv.uv0[0] = rv.uv0[0];
      tv.uv0[1] = rv.uv0[1];
      tv.uv1[0] = rv.uv1[0];
      tv.uv1[1] = rv.uv1[1];

      tv.flags = out_lod.vertices.flags;
      tv.has_tangents = has_tangents ? 1 : 0;
      if (has_tangents) {
        tv.tangent[0] = rv.tangent[0];
        tv.tangent[1] = rv.tangent[1];
        tv.tangent[2] = rv.tangent[2];
        tv.bitangent[0] = rv.bitangent[0];
        tv.bitangent[1] = rv.bitangent[1];
        tv.bitangent[2] = rv.bitangent[2];
      }
      tv.is_skinned = is_skinned ? 1 : 0;
    }
  } else {
    out_lod.vertices.items = nullptr;
  }

  // Allocate and populate the index buffer.
  out_lod.indices.count = static_cast<uint32_t>(index_buffer.size());

  if (out_lod.indices.count > 0) {
    out_lod.indices.indices = static_cast<uint16_t *>(
        std::calloc(out_lod.indices.count, sizeof(uint16_t)));
    if (!out_lod.indices.indices) {
      error = "failed to allocate index buffer";
      return false;
    }
    std::memcpy(out_lod.indices.indices, index_buffer.data(),
                index_buffer.size() * sizeof(uint16_t));
  } else {
    out_lod.indices.indices = nullptr;
  }

  // STRP
  out_lod.strip_count = strips.size();
  out_lod.strip_record_size = is_skinned ? 68 : 48;
  if (out_lod.strip_count > 0) {
    out_lod.strips = static_cast<ThreediTriangleStrip *>(
        std::calloc(out_lod.strip_count, sizeof(ThreediTriangleStrip)));
    if (!out_lod.strips) {
      error = "failed to allocate strips";
      return false;
    }
    for (size_t i = 0; i < strips.size(); ++i) {
      const auto &s = strips[i];
      ThreediTriangleStrip &dst = out_lod.strips[i];
      dst.material_index = s.material_index;
      dst.index_offset = s.index_offset;
      dst.num_indices = s.num_indices;
      dst.num_triangles = s.num_triangles;
      dst.is_strip = s.is_strip;
      dst.start_vertex = s.start_vertex;
      dst.num_vertices = s.num_vertices;
      dst.min[0] = s.min[0];
      dst.min[1] = s.min[1];
      dst.min[2] = s.min[2];
      dst.max[0] = s.max[0];
      dst.max[1] = s.max[1];
      dst.max[2] = s.max[2];
      dst.bone_table_length = 0;
      std::memset(dst.bone_table, 0, sizeof(dst.bone_table));
    }
  } else {
    out_lod.strips = nullptr;
  }

  // ROBJ: one render object per subobject.
  out_lod.render_object_count = lod.subobjectCount;
  if (out_lod.render_object_count > 0) {
    out_lod.render_objects = static_cast<ThreediRenderObject *>(
        std::calloc(out_lod.render_object_count, sizeof(ThreediRenderObject)));
    if (!out_lod.render_objects) {
      error = "failed to allocate render_objects";
      return false;
    }
    for (int32_t sub = 0; sub < lod.subobjectCount; ++sub) {
      const SubObject &subobj = lod.subobjects[sub];
      ThreediRenderObject &ro = out_lod.render_objects[sub];
      // Count strips for this subobject (all non-alpha for now).
      int num_strips = 0;
      int num_alpha_strips = 0;
      for (int mat = 0; mat < material_count; ++mat) {
        if (!bucket_tris[sub][mat].indices.empty()) ++num_strips;
      }
      for (int mat = 0; mat < material_count; ++mat) {
        if (bucket_tris[sub][mat].indices.empty()) continue;
        const bool is_alpha =
            material_alpha[static_cast<size_t>(mat)];
        if (is_alpha) ++num_alpha_strips;
      }
      num_strips -= num_alpha_strips;
      ro.num_strips = num_strips;
      ro.num_alpha_strips = num_alpha_strips;
      ro.parent_index = static_cast<int32_t>(subobj.attachIndex);
      const bool has_centers =
          lod.centerPoints && sub < static_cast<int>(lod.centerCount);
      const Vec3 abs{
          has_centers ? -lod.centerPoints[sub].pos[1] : 0.0f,
          has_centers ? lod.centerPoints[sub].pos[2] : 0.0f,
          has_centers ? lod.centerPoints[sub].pos[0] : 0.0f,
      };
      ro.abs[0] = abs.x;
      ro.abs[1] = abs.y;
      ro.abs[2] = abs.z;
      if (ro.parent_index >= 0 &&
          ro.parent_index < static_cast<int32_t>(sub)) {
        const ThreediRenderObject &parent = out_lod.render_objects[ro.parent_index];
        ro.rel[0] = abs.x - parent.abs[0];
        ro.rel[1] = abs.y - parent.abs[1];
        ro.rel[2] = abs.z - parent.abs[2];
        if (ro.rel[0] == 0.0f) ro.rel[0] = std::copysign(0.0f, abs.x);
        if (ro.rel[1] == 0.0f) ro.rel[1] = std::copysign(0.0f, abs.y);
        if (ro.rel[2] == 0.0f) ro.rel[2] = std::copysign(0.0f, abs.z);
      } else {
        ro.rel[0] = abs.x;
        ro.rel[1] = abs.y;
        ro.rel[2] = abs.z;
      }
      // Bounding center/radius from all subobject verts (matches WriteRDTA).
      Vec3 bmin{10000.0f, 10000.0f, 10000.0f};
      Vec3 bmax{-10000.0f, -10000.0f, -10000.0f};
      for (uint32_t v = 0; v < subobj.vertCount; ++v) {
        const Vertex &sv = subobj.verts[v];
        const float x = -sv.pos.y;
        const float y = sv.pos.z;
        const float z = sv.pos.x;
        bmin.x = std::min(bmin.x, x);
        bmin.y = std::min(bmin.y, y);
        bmin.z = std::min(bmin.z, z);
        bmax.x = std::max(bmax.x, x);
        bmax.y = std::max(bmax.y, y);
        bmax.z = std::max(bmax.z, z);
      }
      const Vec3 center{(bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f,
                        (bmin.z + bmax.z) * 0.5f};
      ro.bounding_center[0] = center.x;
      ro.bounding_center[1] = center.y;
      ro.bounding_center[2] = center.z;
      float radius2 = 0.0f;
      for (uint32_t v = 0; v < subobj.vertCount; ++v) {
        const Vertex &sv = subobj.verts[v];
        const float x = -sv.pos.y;
        const float y = sv.pos.z;
        const float z = sv.pos.x;
        const float dx = x - center.x;
        const float dy = y - center.y;
        const float dz = z - center.z;
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > radius2) radius2 = dist2;
      }
      float radius = sqrtf(radius2);
      // Roots beyond index 0 in the legacy writer lost 1 ULP from the radius;
      // mirror that for subobjects parented to the root.
      if (ro.parent_index == 0 && sub > 0 && radius != 0.0f &&
          (ro.rel[0] != 0.0f || ro.rel[1] != 0.0f || ro.rel[2] != 0.0f)) {
        radius = std::nextafter(radius, -std::numeric_limits<float>::infinity());
      }
      ro.bounding_radius = radius;
    }
  } else {
    out_lod.render_objects = nullptr;
  }

  return true;
}

} // namespace oed
