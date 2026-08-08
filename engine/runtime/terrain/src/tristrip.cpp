#include "tristrip.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace opennova {

// Fail loudly before a size_t overflow turns into a heap smash. 256 MiB is
// far above what any realistic terrain tile produces — we just want a sane
// ceiling so a corrupt/nonsense input gets caught with a message instead of
// a silent heap corruption + SEH.
static void _guard_alloc_size(const char *site, int64_t element_count, size_t element_size) {
    constexpr int64_t kMaxBytes = 1LL << 28; // 256 MiB
    if (element_count < 0) {
        throw std::runtime_error(std::string("tristrip: ") + site
                                 + ": negative element count "
                                 + std::to_string(element_count));
    }
    int64_t bytes = element_count * static_cast<int64_t>(element_size);
    if (bytes > kMaxBytes) {
        throw std::runtime_error(std::string("tristrip: ") + site
                                 + ": allocation too large ("
                                 + std::to_string(element_count) + " elements, "
                                 + std::to_string(bytes) + " bytes)");
    }
}

// ============================================================================
// Internal data structures matching IDA structures
// ============================================================================

// [orig: sub_406760 @ 0x406760, sub_406860 @ 0x406860]
// Ported from sub_406760 initialization.
// Per-face adjacency: 6 ints per face =
//   [adj_face_e0, adj_face_e1, adj_face_e2,  (at +0, +1, +2)
//    adj_edge_e0, adj_edge_e1, adj_edge_e2]  (at +3, +4, +5)
struct StripContext {
    int num_faces;
    const uint16_t* face_data;  // 3 uint16 per face
    int* adj;                    // 6 ints per face
    int* flags;                  // 1 int per face
};

// Individual strip record (12 bytes in original: data_ptr, count, winding)
struct StripRecord {
    uint16_t* indices;
    int count;
    int winding;
};

// Vertex cache for strip ordering (sub_406860).
// Layout matches IDA: score(4) + verts[16](32) + faces[16](64) + write_pos(4) = 104 bytes
struct VertexCache {
    int score;
    uint16_t verts[16];
    int faces[16];
    int write_pos;
};

// ============================================================================
// Ported from sub_406620
// Find and set adjacency for face/edge pair
// ============================================================================
static void find_adjacent_edge(StripContext* ctx, int face, int edge) {
    const uint16_t* fd = ctx->face_data;
    int v_a = fd[3 * face + edge];
    int v_b = fd[3 * face + (edge + 1) % 3];

    for (int j = face + 1; j < ctx->num_faces; j++) {
        if (ctx->flags[j] == 7) continue; // all 3 edges already matched

        const uint16_t* fj = fd + 3 * j;
        for (int e = 0; e < 3; e++) {
            if (fj[e] == v_b && fj[(e + 1) % 3] == v_a) {
                ctx->adj[6 * face + edge] = j;
                ctx->adj[6 * face + edge + 3] = e;
                ctx->flags[face] |= (1 << edge);

                ctx->adj[6 * j + e] = face;
                ctx->adj[6 * j + e + 3] = edge;
                ctx->flags[j] |= (1 << e);
                return;
            }
        }
    }
}

// ============================================================================
// Ported from sub_406760
// Initialize strip context with adjacency
// ============================================================================
static void init_context(StripContext* ctx, int face_count, const uint16_t* face_data) {
    _guard_alloc_size("init_context.adj", static_cast<int64_t>(6) * face_count, sizeof(int));
    _guard_alloc_size("init_context.flags", face_count, sizeof(int));
    ctx->num_faces = face_count;
    ctx->face_data = face_data;
    ctx->adj = new int[6 * face_count];
    ctx->flags = new int[face_count];

    // Initialize adjacency face entries to -1
    for (int i = 0; i < face_count; i++) {
        ctx->adj[6 * i + 0] = -1;
        ctx->adj[6 * i + 1] = -1;
        ctx->adj[6 * i + 2] = -1;
    }
    std::memset(ctx->flags, 0, 4 * face_count);

    // Build adjacency
    for (int i = 0; i < face_count; i++) {
        for (int j = 0; j < 3; j++) {
            if (((1 << j) & ctx->flags[i]) == 0)
                find_adjacent_edge(ctx, i, j);
        }
    }

    // Reset flags for strip building phase
    std::memset(ctx->flags, 0, 4 * face_count);
}

// ============================================================================
// Ported from sub_406830
// Destroy strip context
// ============================================================================
static void destroy_context(StripContext* ctx) {
    delete[] ctx->flags;
    ctx->flags = nullptr;
    delete[] ctx->adj;
    ctx->adj = nullptr;
}

// ============================================================================
// Ported from sub_406860
// Update vertex cache. Returns 1 if cache state changed.
// ============================================================================
static int cache_update(VertexCache* cache, int face_idx, uint16_t vertex_idx) {
    for (int i = 0; i < 16; i++) {
        if (vertex_idx == cache->verts[i]) {
            if (face_idx == cache->faces[i])
                return 0;
            cache->score++;
            cache->faces[i] = face_idx;
            return 1;
        }
    }
    // Not found — insert at write_pos (circular)
    cache->verts[cache->write_pos] = vertex_idx;
    cache->faces[cache->write_pos] = face_idx;
    cache->write_pos = (cache->write_pos + 1) % 16;
    return 1;
}

// ============================================================================
// Helper extracted from sub_406380 loop body
// Count unvisited adjacent faces for a given face
// ============================================================================
static int count_unvisited_neighbors(StripContext* ctx, int face) {
    int n = 0;
    for (int e = 0; e < 3; e++) {
        int af = ctx->adj[6 * face + e];
        if (af != -1 && !ctx->flags[af]) n++;
    }
    return n;
}

// ============================================================================
// Ported from sub_405A20
// Build a single strip from a face+edge.
// Returns strip length. Sets *out_degen to degenerate count.
// face_list/edge_list must have at least max_length entries.
// ============================================================================
static int build_one_strip(
    StripContext* ctx,
    int start_face,    // a2
    int start_edge,    // a3
    int max_length,    // a4
    int* out_degen,    // a5
    int look_ahead,    // a6
    int winding,       // a7
    int* face_list,    // a8
    int* edge_list)    // a9
{
    *out_degen = 0;

    // Check if start face already visited
    if (ctx->flags[start_face])
        return 0;

    // Mark start face visited
    ctx->flags[start_face] = 1;

    // Initialize output arrays — first 3 entries all refer to start face
    face_list[0] = start_face;
    face_list[1] = start_face;
    face_list[2] = start_face;

    int* face_unmark = face_list + 2; // v82 — for unmarking after trial
    int degen_count = 0;              // v76

    // Set initial vertex ordering based on winding
    int v12;
    if (winding) {
        edge_list[0] = start_edge % 3;
        v12 = start_edge + 1;
        edge_list[1] = (start_edge + 1) % 3;
    } else {
        edge_list[0] = (start_edge + 1) % 3;
        v12 = start_edge + 1;
        edge_list[1] = start_edge % 3;
    }
    edge_list[2] = (start_edge + 2) % 3;

    int inv_wind = winding ? 0 : 1; // v14
    if (!winding)
        v12 = start_edge + 2;

    int strip_len = 3;       // v16
    int strip_len_saved = 3; // v71

    // Get first next face through exit edge
    int exit_idx = v12 % 3 + 6 * start_face;
    int v20 = ctx->adj[exit_idx];           // next_face
    int v21 = ctx->adj[exit_idx + 3];       // next_edge (int, treated as edge index)

    if (max_length > 3) {
        int* fw2 = face_list + 3;  // v74
        int* ew2 = edge_list + 3;  // v75
        int* fr  = face_list + 1;  // v72
        int* er  = edge_list + 1;  // v73

        while (v20 != -1) {
            if (ctx->flags[v20])
                break;

            int a2a = v20;             // current face to add
            int v22 = !inv_wind;       // toggled winding
            int a3a = v21;             // current edge in that face
            int a7a = v22;             // saved winding for next iteration
            int v78 = 6 * v20;        // saved 6*face offset

            // Compute exit direction → face_ahead
            int v23 = inv_wind ? (a3a + 1) : (a3a + 2);
            int eidx = v78 + v23 % 3;
            int v27 = ctx->adj[eidx];       // face_ahead
            int v70 = v27;                   // next face for end of iteration
            int a8a = ctx->adj[eidx + 3];   // edge_ahead

            if (v27 != -1 && !ctx->flags[v27]) {
                if (!look_ahead)
                    goto add_face;

                // Look-ahead: compute alt direction
                int alt_off = v22 ? (a3a + 1) : (a3a + 2);
                int alt_idx = v78 + alt_off % 3;
                int v81 = ctx->adj[alt_idx];        // alt_face
                int v77 = ctx->adj[alt_idx + 3];    // alt_edge

                if (v81 == -1 || ctx->flags[v81])
                    goto add_face;

                // Count unvisited neighbors
                int alt_n = count_unvisited_neighbors(ctx, v81);
                int ahead_n = count_unvisited_neighbors(ctx, v27);

                if (alt_n >= ahead_n) {
                    // Recount (matches IDA exactly — duplicate counting)
                    int alt_n2 = count_unvisited_neighbors(ctx, v81);
                    int ahead_n2 = count_unvisited_neighbors(ctx, v27);

                    if (alt_n2 != ahead_n2)
                        goto add_face; // LABEL_80: go straight

                    // Look 2 steps ahead
                    int next_ahead_off = a7a ? (a8a + 1) : (a8a + 2);
                    int next_alt_off   = a7a ? (v77 + 2) : (v77 + 1);

                    int next_ahead_f = ctx->adj[6 * v27 + next_ahead_off % 3];
                    int next_alt_f   = ctx->adj[6 * v81 + next_alt_off % 3];

                    if (next_ahead_f != -1 && !ctx->flags[next_ahead_f]) {
                        if (next_alt_f == -1 || ctx->flags[next_alt_f])
                            goto add_face; // LABEL_80

                        int nn_alt = count_unvisited_neighbors(ctx, next_alt_f);
                        int nn_ahead = count_unvisited_neighbors(ctx, next_ahead_f);

                        if (nn_alt >= nn_ahead)
                            goto add_face; // LABEL_80
                    }
                }

                // Take alternate path: restore state
                strip_len = strip_len_saved;
                v22 = a7a;
                // v78 already correct (v24 = v78 in IDA)
            }

            // Alt direction code (runs when face_ahead invalid or look-ahead chose alt)
            {
                int try_off = v22 ? (a3a + 1) : (a3a + 2);
                int try_idx = v78 + try_off % 3;
                v70 = ctx->adj[try_idx];
                a8a = ctx->adj[try_idx + 3];

                if (v70 != -1 && !ctx->flags[v70]) {
                    // Insert degenerate vertex
                    strip_len++;
                    *fw2++ = *fr;
                    int te = *er++;
                    *ew2 = te;
                    fr++;
                    ew2++;
                    degen_count++;
                    a7a = !v22;
                }
            }

        add_face:
            *fw2 = a2a;
            strip_len_saved = ++strip_len;
            *ew2 = (a3a + 2) % 3;
            ctx->flags[a2a] = 1;
            ew2++;
            fr++;
            er++;
            fw2++;

            if (strip_len >= max_length)
                break;

            // Advance to next face
            v21 = a8a;
            v20 = v70;
            inv_wind = a7a;  // IDA: v14 = a7a (direct assignment, NOT inverted)
        }
    }

    // Unmark faces visited during trial (from position 2 onwards)
    if (strip_len > 2) {
        int* ptr = face_unmark;
        int remaining = strip_len - 2;
        while (remaining > 0) {
            ctx->flags[*ptr] = 0;
            ptr++;
            remaining--;
        }
    }

    *out_degen = degen_count;
    return strip_len;
}

// ============================================================================
// Ported from sub_406380
// Build all strips from face data
// ============================================================================
static void build_all_strips(
    StripContext* ctx,
    std::vector<StripRecord*>& strip_list,
    int max_length,
    int look_ahead)
{
    int face_buf[1025];
    int edge_buf[1027];

    std::memset(ctx->flags, 0, 4 * ctx->num_faces);
    int winding = 1; // a7 = TRUE

    while (true) {
        int best_connectivity = 0x7FFFFFFF; // v25
        float best_ratio = 2.0f;            // v28
        int best_face = -1;                 // v29
        int best_edge = -1;                 // v30

        for (int f = 0; f < ctx->num_faces; f++) {
            if (ctx->flags[f]) continue;

            // Count unvisited neighbors
            int neighbors = 0;
            for (int e = 0; e < 3; e++) {
                int af = ctx->adj[6 * f + e];
                if (af != -1 && !ctx->flags[af])
                    neighbors++;
            }
            if (neighbors == 0)
                neighbors = 4;

            if (neighbors > best_connectivity)
                continue;

            // Try each edge as starting point
            for (int e = 0; e < 3; e++) {
                int degen;
                int len = build_one_strip(ctx, f, e, max_length, &degen,
                                          look_ahead, winding, face_buf, edge_buf);

                // IDA asm: fild [degen] / fidiv [len] → double precision on x87 (PC=2)
                // Then fcom dword ptr [best_ratio] → compare double vs float-extended-to-double
                // Then fstp dword ptr [best_ratio] → truncate double to float for storage
                double ratio;
                if (len == 3)
                    ratio = 1.0;
                else
                    ratio = (double)degen / (double)len;

                if (neighbors < best_connectivity ||
                    (neighbors == best_connectivity && ratio < (double)best_ratio)) {
                    best_ratio = (float)ratio; // fstp dword ptr — truncate to float
                    best_connectivity = neighbors;
                    best_face = f;
                    best_edge = e;
                }
            }
        }

        if (best_connectivity == 0x7FFFFFFF)
            break;

        // Build the actual strip with best starting face/edge
        int degen;
        int len = build_one_strip(ctx, best_face, best_edge, max_length,
                                   &degen, look_ahead, winding, face_buf, edge_buf);

        // Permanently mark faces as visited
        if (len > 0) {
            for (int i = 0; i < len; i++)
                ctx->flags[face_buf[i]] = 1;
        }

        // Create strip record
        _guard_alloc_size("strip_record.indices", len, sizeof(uint16_t));
        StripRecord* rec = new StripRecord;
        rec->count = len;
        rec->indices = new uint16_t[len];
        for (int i = 0; i < len; i++) {
            int idx = edge_buf[i] + 3 * face_buf[i];
            rec->indices[i] = ctx->face_data[idx];
        }
        rec->winding = winding;

        strip_list.push_back(rec);

        // Flip winding if strip has odd length
        if ((len & 1) != 0)
            winding = !winding;
    }
}

// ============================================================================
// Ported from sub_4065F0
// Score a set of strips (total indices after concatenation)
// ============================================================================
static unsigned int score_strips(const std::vector<StripRecord*>& strips) {
    unsigned int count = (unsigned int)strips.size();
    unsigned int result = 2 * count - 2;
    for (auto* sr : strips)
        result += sr->count;
    return result;
}

// ============================================================================
// Ported from sub_405EF0
// Find best next strip for concatenation (cache-aware).
// Swaps best strip to index 0. May reverse strip indices in-place.
// ============================================================================
static void find_best_next(
    std::vector<StripRecord*>& strips,
    VertexCache* cache)
{
    if (strips.empty()) return;

    int best_idx = 0;
    int best_delta = -1;
    int best_reverse = 0;
    int ref_count = strips[0]->count;    // v28 (low byte tracks parity)
    int ref_winding = strips[0]->winding; // v29

    for (int s = 0; s < (int)strips.size(); s++) {
        StripRecord* sr = strips[s];
        int len = sr->count;

        // Compatibility check: same winding + matching parity
        if (sr->winding != ref_winding ||
            (((unsigned char)ref_count ^ (unsigned char)len) & 1) != 0)
            continue;

        int should_reverse = 0;

        // Simulate adding forward
        VertexCache fwd_cache;
        std::memcpy(&fwd_cache, cache, sizeof(VertexCache));
        for (int i = 0; i < len; i++)
            cache_update(&fwd_cache, 2, sr->indices[i]);
        int fwd_score = fwd_cache.score;

        // Try reversed if even length
        if ((len & 1) == 0) {
            VertexCache rev_cache;
            std::memcpy(&rev_cache, cache, sizeof(VertexCache));
            for (int i = len - 1; i >= 0; i--)
                cache_update(&rev_cache, 2, sr->indices[i]);

            if (rev_cache.score > fwd_score) {
                should_reverse = 1;
                fwd_score = rev_cache.score;
            }
        }

        int delta = fwd_score - cache->score;
        if (delta > best_delta) {
            best_delta = delta;
            ref_count = (unsigned char)len; // LOBYTE update
            best_idx = s;
            best_reverse = should_reverse;
        }
    }

    // Reverse best strip's indices if needed
    if (best_reverse) {
        StripRecord* sr = strips[best_idx];
        int lo = 0, hi = sr->count - 1;
        while (lo < hi) {
            uint16_t tmp = sr->indices[lo];
            sr->indices[lo] = sr->indices[hi];
            sr->indices[hi] = tmp;
            lo++; hi--;
        }
    }

    // Swap best to position 0
    if (best_idx != 0) {
        StripRecord* tmp = strips[0];
        strips[0] = strips[best_idx];
        strips[best_idx] = tmp;
    }
}

// ============================================================================
// Ported from sub_406140
// Concatenate strips into final output buffer.
// Returns total index count, sets *out_indices to allocated buffer.
// Consumes (frees) all strip records.
// ============================================================================
static int concatenate_strips(
    std::vector<StripRecord*>& strips,
    uint16_t** out_indices)
{
    if (strips.empty()) {
        *out_indices = nullptr;
        return 0;
    }

    // Compute max output size
    int total_alloc = 3 * (int)strips.size() + 2;
    for (auto* sr : strips)
        total_alloc += sr->count;

    _guard_alloc_size("stripify.output", total_alloc, sizeof(uint16_t));
    uint16_t* output = new uint16_t[total_alloc];

    // Initialize vertex cache
    VertexCache cache;
    cache.score = 0;
    std::memset(cache.verts, 0xFF, sizeof(cache.verts));
    cache.write_pos = 0;

    int wp = 0; // write position (v8)

    // Process first strip
    StripRecord* first = strips[0];
    for (int i = 0; i < first->count; i++) {
        output[wp] = first->indices[i];
        cache_update(&cache, 1, first->indices[i]);
        wp++;
    }

    // Remove first strip
    delete[] first->indices;
    delete first;
    strips.erase(strips.begin());

    // Process remaining strips with cache-aware ordering
    while (!strips.empty()) {
        find_best_next(strips, &cache);

        StripRecord* sr = strips[0];

        uint16_t last_v = output[wp - 1];
        uint16_t first_v = sr->indices[0];

        // Insert degenerate connectors
        if (first_v != last_v) {
            output[wp++] = last_v;
            output[wp++] = first_v;
        }

        // Fix winding: strip's winding must match current output parity
        if (sr->winding != ((wp & 1) == 0 ? 1 : 0))
            output[wp++] = first_v;

        // Copy strip indices
        for (int i = 0; i < sr->count; i++) {
            output[wp] = sr->indices[i];
            cache_update(&cache, 1, sr->indices[i]);
            wp++;
        }

        // Remove processed strip
        delete[] sr->indices;
        delete sr;
        strips.erase(strips.begin());
    }

    *out_indices = output;
    return wp;
}

// Cleanup helper (no direct IDA counterpart)
static void free_strips(std::vector<StripRecord*>& strips) {
    for (auto* sr : strips) {
        delete[] sr->indices;
        delete sr;
    }
    strips.clear();
}

// ============================================================================
// Ported from sub_4068E0
// Top-level strip builder.
// Tries two strategies (with/without look-ahead), picks the one with
// fewer total indices, then concatenates with cache-aware ordering.
// ============================================================================
TriStripResult build_triangle_strips(
    const uint16_t* face_indices, int face_count, int /*vertex_count*/)
{
    TriStripResult result;
    result.is_strip = true;

    if (face_count == 0 || !face_indices) {
        result.total_indices = 0;
        return result;
    }

    // Initialize context with adjacency
    StripContext ctx;
    init_context(&ctx, face_count, face_indices);

    // Two strategies: {max_length, look_ahead}
    // IDA: v15[0]=1024, v15[1]=1, v15[2]=1024, v15[3]=0
    struct { int max_length; int look_ahead; } strategies[2] = {
        {1024, 1},
        {1024, 0}
    };

    std::vector<StripRecord*> best_strips;
    unsigned int best_score = 0;

    for (int s = 0; s < 2; s++) {
        std::vector<StripRecord*> strips;
        build_all_strips(&ctx, strips, strategies[s].max_length, strategies[s].look_ahead);

        unsigned int score = score_strips(strips);

        if (best_score == 0 || score < best_score) {
            free_strips(best_strips);
            best_strips = std::move(strips);
            best_score = score;
        } else {
            free_strips(strips);
        }
    }

    // Concatenate best strips into final output
    uint16_t* output = nullptr;
    int total = concatenate_strips(best_strips, &output);

    // Cleanup context
    destroy_context(&ctx);

    // Copy to result
    if (output) {
        result.indices.assign(output, output + total);
        delete[] output;
    }
    result.total_indices = total;

    return result;
}

} // namespace opennova

