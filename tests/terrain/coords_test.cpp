// Terrain world->source coordinate transform kernel (libs/terrain_query/coords).
//
// Characterizes two things the A2 refactor must preserve byte-for-byte:
//   1. The RUNTIME formula currently inlined as resolve_world_sample() in
//      godot/engine/terrain/nova_terrain_data.cpp (float, & 0xF grid wrap, no
//      id clamp, no local clamp). ref_resolve_world_sample() below is a verbatim
//      copy of that formula and is the oracle for the runtime-mode kernel.
//   2. The EDITOR guard divergences ported from
//      godot/modtools/terrain/editor_terrain_mesh.gd world_to_source_coords /
//      world_to_cell_source_coords / get_cell_atlas_rect: bounds-reject (instead
//      of & 0xF wrap), clampi(sector_id, 0, 4), and clampf(local, 0, 512-0.001).
//      These are exercised with hand-derived golden values.

#include "terrain/coords.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace opennova::terrain;

static int g_fail = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        g_fail = 1;
    }
}

static void check_close(double got, double want, double tol, const char *msg) {
    if (!(std::fabs(got - want) <= tol)) {
        std::fprintf(stderr, "FAIL: %s (got %.10f want %.10f)\n", msg, got, want);
        g_fail = 1;
    }
}

// Verbatim copy of the current resolve_world_sample() formula (the runtime
// source-of-truth). Kept independent of the kernel so the kernel can be proven
// equal to it.
struct RefSample {
    bool valid = false;
    int sector_id = 0;
    float source_x = 0.0f;
    float source_z = 0.0f;
};

static RefSample ref_resolve_world_sample(const int *grid, int origin_x, int origin_y,
                                          float world_x, float world_z) {
    RefSample out;
    const int sector_sx = static_cast<int>(std::floor(world_x / 512.0f));
    const int sector_sz = static_cast<int>(std::floor(world_z / 512.0f));
    const int grid_x = sector_sx - origin_x;
    const int grid_z = sector_sz - origin_y;
    const int sector_id = grid[(grid_z & 0xF) * 16 + (grid_x & 0xF)];
    if (sector_id <= 0) {
        return out;
    }
    const float local_x = world_x - static_cast<float>(sector_sx * 512);
    const float local_z = world_z - static_cast<float>(sector_sz * 512);
    const float quadrant_x = (sector_id == 3 || sector_id == 4) ? 512.0f : 0.0f;
    const float quadrant_z = (sector_id == 2 || sector_id == 4) ? 512.0f : 0.0f;
    out.sector_id = sector_id;
    out.source_x = quadrant_x + local_x;
    out.source_z = quadrant_z + local_z;
    out.valid = true;
    return out;
}

int main() {
    // 16x16 row-major grid. Authored region is 8x8 (origin -4,-4). Specific cells:
    //   (row 0, col 0) id 1   -> quadrant (0,0)
    //   (row 0, col 1) id 2   -> quadrant (0,512)
    //   (row 1, col 0) id 3   -> quadrant (512,0)
    //   (row 1, col 1) id 4   -> quadrant (512,512)
    //   (row 0, col 2) id 0   -> empty (reject)
    //   (row 0, col 3) id 7   -> over-range (editor clamps to 4; runtime keeps 7)
    // every other cell is id 1.
    std::vector<int> grid(256, 1);
    grid[0 * 16 + 0] = 1;
    grid[0 * 16 + 1] = 2;
    grid[1 * 16 + 0] = 3;
    grid[1 * 16 + 1] = 4;
    grid[0 * 16 + 2] = 0;
    grid[0 * 16 + 3] = 7;

    SectorLayout layout;
    layout.sector_grid = grid.data();
    layout.origin_x = -4;
    layout.origin_y = -4;
    layout.sector_count = 8;
    layout.sector_rows = 8;

    // Missing sector storage is an empty layout across every public lookup.
    SectorLayout missing;
    missing.sector_count = 1;
    missing.sector_rows = 1;
    check(coords_sector_id_at_cell(missing, 0, 0) == 0,
          "null sector grid cell lookup is empty");
    check(!coords_world_to_source<float>(missing, 0.0f, 0.0f,
                                         coords_runtime_options()).valid,
          "null sector grid world lookup is invalid");
    check(!coords_world_to_cell_source<float>(missing, 0.0f, 0.0f, 0, 0).valid,
          "null sector grid explicit-cell lookup is invalid");
    CoordsRect missing_rect = coords_cell_atlas_rect(missing, 0, 0);
    check(missing_rect.w == 0 && missing_rect.h == 0,
          "null sector grid atlas rect is empty");

    // --- Runtime mode equals the verbatim resolve_world_sample oracle ---
    struct Pt { float x, z; const char *name; };
    const Pt pts[] = {
        {-2000.0f, -2000.0f, "cell(0,0) id1"},
        {-1500.0f, -2000.0f, "cell(0,1) id2"},
        {-2000.0f, -1500.0f, "cell(1,0) id3"},
        {-1500.0f, -1500.0f, "cell(1,1) id4"},
        {-800.0f, -2000.0f, "cell(0,2) id0 empty"},
        {-300.0f, -2000.0f, "cell(0,3) id7 raw"},
        {-5000.0f, -2000.0f, "out-of-extent -> & 0xF wrap"},
        {-1536.0005f, -2000.0f, "near sector boundary"},
    };
    for (const Pt &p : pts) {
        RefSample ref = ref_resolve_world_sample(grid.data(), -4, -4, p.x, p.z);
        CoordsResult<float> k = coords_world_to_source<float>(layout, p.x, p.z, coords_runtime_options());
        check(k.valid == ref.valid, p.name);
        if (ref.valid) {
            check(k.sector_id == ref.sector_id, p.name);
            // Bit-exact: same float ops in the same order.
            check(k.source_x == ref.source_x, p.name);
            check(k.source_z == ref.source_z, p.name);
        }
    }

    // Runtime spot values (documents the oracle's expectations explicitly).
    {
        CoordsResult<float> r1 = coords_world_to_source<float>(layout, -2000.0f, -2000.0f, coords_runtime_options());
        check(r1.valid && r1.sector_id == 1, "R1 valid id1");
        check_close(r1.source_x, 48.0, 0.0, "R1 source_x");
        check_close(r1.source_z, 48.0, 0.0, "R1 source_z");

        CoordsResult<float> r6 = coords_world_to_source<float>(layout, -300.0f, -2000.0f, coords_runtime_options());
        check(r6.valid && r6.sector_id == 7, "R6 raw id7 kept");
        check_close(r6.source_x, 212.0, 0.0, "R6 source_x (quadrant 0 for raw 7)");
        check_close(r6.source_z, 48.0, 0.0, "R6 source_z");

        CoordsResult<float> r7 = coords_world_to_source<float>(layout, -5000.0f, -2000.0f, coords_runtime_options());
        check(r7.valid && r7.sector_id == 1, "R7 wrap finds id1");
        check_close(r7.source_x, 120.0, 0.0, "R7 source_x");
    }

    // --- Editor mode: bounds-reject, id-clamp, local-clamp ---
    const CoordsOptions ed = coords_editor_options();

    // E1: in-bounds in-range matches runtime.
    {
        CoordsResult<double> e = coords_world_to_source<double>(layout, -2000.0, -2000.0, ed);
        check(e.valid && e.sector_id == 1, "E1 valid id1");
        check_close(e.source_x, 48.0, 0.0, "E1 source_x");
        check_close(e.source_z, 48.0, 0.0, "E1 source_z");
    }

    // E2: id-clamp. cell(0,3) raw id 7 clamps to 4 -> quadrant (512,512).
    {
        CoordsResult<double> e = coords_world_to_source<double>(layout, -300.0, -2000.0, ed);
        check(e.valid && e.sector_id == 4, "E2 id clamped 7->4");
        check_close(e.source_x, 724.0, 0.0, "E2 source_x (212 + 512)");
        check_close(e.source_z, 560.0, 0.0, "E2 source_z (48 + 512)");
    }

    // E3: bounds-reject where runtime wrapped.
    {
        CoordsResult<double> e = coords_world_to_source<double>(layout, -5000.0, -2000.0, ed);
        check(!e.valid, "E3 out-of-extent rejected (no & 0xF wrap)");
    }

    // E4: empty cell rejected.
    {
        CoordsResult<double> e = coords_world_to_source<double>(layout, -800.0, -2000.0, ed);
        check(!e.valid, "E4 empty cell (id 0) rejected");
    }

    // E5: local-clamp. local_x = 511.9995 -> clamped to exactly 512 - 0.001.
    {
        CoordsResult<double> e = coords_world_to_source<double>(layout, -1536.0005, -2000.0, ed);
        check(e.valid && e.sector_id == 1, "E5 valid id1");
        check(e.source_x < 512.0, "E5 local clamped below 512");
        check_close(e.source_x, 512.0 - 0.001, 1e-9, "E5 source_x clamped to 511.999");
        check_close(e.source_z, 48.0, 0.0, "E5 source_z unaffected");
    }

    // --- world_to_cell_source (explicit cell, UNCLAMPED local) ---
    {
        // C1: in-cell point equals world_to_source for that cell.
        CellCoordsResult<double> c1 = coords_world_to_cell_source<double>(layout, -1500.0, -1500.0, 1, 1);
        check(c1.valid && c1.sector_id == 4, "C1 cell(1,1) id4");
        check_close(c1.source_x, 548.0, 0.0, "C1 source_x");
        check_close(c1.source_z, 548.0, 0.0, "C1 source_z");

        // C2: empty cell rejected with the -1e9-style sentinel handled by caller.
        CellCoordsResult<double> c2 = coords_world_to_cell_source<double>(layout, -1500.0, -1500.0, 0, 2);
        check(!c2.valid, "C2 empty cell rejected");

        // C3: UNCLAMPED -- a point well outside the cell yields local > 512.
        CellCoordsResult<double> c3 = coords_world_to_cell_source<double>(layout, -1000.0, -2000.0, 0, 0);
        check(c3.valid && c3.sector_id == 1, "C3 cell(0,0) id1");
        check_close(c3.source_x, 1048.0, 0.0, "C3 source_x unclamped (>512)");
        check_close(c3.source_z, 48.0, 0.0, "C3 source_z");

        // Out-of-extent explicit cell rejected (bounds-reject in cell lookup).
        CellCoordsResult<double> c4 = coords_world_to_cell_source<double>(layout, 0.0, 0.0, 9, 0);
        check(!c4.valid, "C4 row 9 >= sector_rows rejected");
    }

    // --- cell_atlas_rect ---
    {
        CoordsRect a4 = coords_cell_atlas_rect(layout, 0, 0); // id1 -> (0,0)
        check(a4.x == 0 && a4.z == 0 && a4.w == 512 && a4.h == 512, "A: cell(0,0) id1 rect");

        CoordsRect a2 = coords_cell_atlas_rect(layout, 0, 1); // id2 -> (0,512)
        check(a2.x == 0 && a2.z == 512 && a2.w == 512 && a2.h == 512, "A: cell(0,1) id2 rect");

        CoordsRect a3 = coords_cell_atlas_rect(layout, 1, 0); // id3 -> (512,0)
        check(a3.x == 512 && a3.z == 0 && a3.w == 512 && a3.h == 512, "A: cell(1,0) id3 rect");

        CoordsRect a1 = coords_cell_atlas_rect(layout, 1, 1); // id4 -> (512,512)
        check(a1.x == 512 && a1.z == 512 && a1.w == 512 && a1.h == 512, "A: cell(1,1) id4 rect");

        CoordsRect a0 = coords_cell_atlas_rect(layout, 0, 2); // id0 -> empty
        check(a0.x == 0 && a0.z == 0 && a0.w == 0 && a0.h == 0, "A: cell(0,2) id0 empty rect");

        CoordsRect a7 = coords_cell_atlas_rect(layout, 0, 3); // id7 clamps 4 -> (512,512)
        check(a7.x == 512 && a7.z == 512 && a7.w == 512 && a7.h == 512, "A: cell(0,3) id7->4 rect");
    }

    if (g_fail) {
        std::fprintf(stderr, "coords_test: FAILED\n");
        return 1;
    }
    std::printf("coords_test: OK\n");
    return 0;
}
