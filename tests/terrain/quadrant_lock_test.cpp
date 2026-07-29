// Per-quadrant neighbour-tap locks (the .trn lock_topleft/topright/bottomleft/
// bottomright pairs) in the shared heightmap tap kernel.
//
// Sector tiling repeats one 512x512 quadrant of the 1024 atlas next to itself, so
// the vertex row that lands ON a sector boundary has to tap the quadrant's own
// row/column 0, not the next quadrant across the atlas-internal seam. Terrains
// whose lock declares that wrap are only seamless when it is honored: on Dvxc2
// (05TR's terrain, locks tl/tr = 0 1, bl/br = 1 1) the locked wrap breaks by a
// median 0.004 u across the seam while the atlas-crossing tap breaks by 13.5 u,
// dropping the boundary row to the neighbouring quadrant's shoreline.
//
// Covers:
//   1. coords_locked_tap against a verbatim copy of the original's masking.
//   2. height_field_height_world_bilinear at a sector seam, locked and unlocked.
//   3. The default (all-zero locks) field still taps across the full atlas.
//
// [orig: sub_402D20 @0x402D20, ported in libs/terrain/src/terrain_mesh.cpp.]

#include "terrain/coords.h"
#include "terrain/height_field.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
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
        std::fprintf(stderr, "FAIL: %s (got %.6f want %.6f)\n", msg, got, want);
        g_fail = 1;
    }
}

// Verbatim copy of the original masking, kept independent of the kernel so the
// kernel can be proven equal to it:
//   if (lock) { mask = 511; offset = base & 0x200; }
//   buf = (offset + (abs & mask)) & 0x3FF;
static int ref_tap(int abs_coord, int tile_base, bool locked) {
    int mask = 1023;
    int offset = 0;
    if (locked) {
        mask = 511;
        offset = tile_base & 0x200;
    }
    return (offset + (abs_coord & mask)) & 0x3FF;
}

namespace {

constexpr int kDim = 1024;

// The Dvxc2 shape, reduced to what the seam depends on: quadrant 1 (atlas rows and
// columns 0..511) is a flat plateau at 16.0 units, everything below/right of the
// internal seam is at 0.0 like the open sea Dvxc2 keeps there. A boundary tap that
// wraps inside quadrant 1 therefore reads 16.0; one that crosses reads 0.0, and the
// bilinear halfway across the last cell separates them cleanly.
struct SeamAtlas {
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    SeamAtlas() : heightmap(static_cast<size_t>(kDim) * kDim, 0), sector_grid(256, 1) {
        for (int z = 0; z < 512; ++z) {
            for (int x = 0; x < 512; ++x) {
                heightmap[static_cast<size_t>(z) * kDim + x] = 16 * 256; // 16.0 units
            }
        }
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = -4;
        field.layout.origin_y = -4;
    }
};

} // namespace

int main() {
    // 1. The tap kernel matches the original's masking.
    {
        // Unlocked: plain full-atlas wrap, the behavior every call site had before.
        for (int abs_coord : {0, 1, 255, 511, 512, 513, 1023, 1024, 2047}) {
            check(coords_locked_tap(abs_coord, 0, false, kDim) == ref_tap(abs_coord, 0, false),
                  "unlocked tap == reference");
            check(coords_locked_tap(abs_coord, 512, false, kDim) == ref_tap(abs_coord, 512, false),
                  "unlocked tap ignores the quadrant offset");
        }
        // Locked, in each quadrant half.
        for (int abs_coord : {0, 1, 511, 512, 513, 1023, 1024}) {
            check(coords_locked_tap(abs_coord, 0, true, kDim) == ref_tap(abs_coord, 0, true),
                  "locked tap (low quadrant) == reference");
            check(coords_locked_tap(abs_coord, 512, true, kDim) == ref_tap(abs_coord, 512, true),
                  "locked tap (high quadrant) == reference");
        }
        // The crossing itself, spelled out.
        check(coords_locked_tap(511, 0, true, kDim) == 511, "locked: 511 stays 511");
        check(coords_locked_tap(512, 0, true, kDim) == 0, "locked: 512 wraps to the quadrant's row 0");
        check(coords_locked_tap(512, 0, false, kDim) == 512, "unlocked: 512 crosses the seam");
        check(coords_locked_tap(1024, 512, true, kDim) == 512,
              "locked: the high quadrant wraps to ITS own row 0, not the atlas's");
    }

    // 2. Quadrant selection: the lock table is indexed by the tap's own quadrant.
    {
        CoordsQuadrantLocks locks{};   // Dvxc2: tl/tr = 0 1, bl/br = 1 1
        locks.set(0, false, true);  // top-left
        locks.set(1, false, true);  // top-right
        locks.set(2, true, true);   // bottom-left
        locks.set(3, true, true);   // bottom-right

        check(!locks.locked_x(0) && locks.locked_z(0), "packed: top-left is z-only");
        check(locks.locked_x(3) && locks.locked_z(3), "packed: bottom-right is both");
        check(!locks.locked_x(1), "packed: bits do not bleed between quadrants");

        check(coords_quadrant_index(0, 0) == 0, "quadrant index: top-left");
        check(coords_quadrant_index(512, 0) == 1, "quadrant index: top-right");
        check(coords_quadrant_index(0, 512) == 2, "quadrant index: bottom-left");
        check(coords_quadrant_index(512, 512) == 3, "quadrant index: bottom-right");

        // Sector id 1 -> quadrant (0,0) -> top-left -> z locked, x not.
        const CoordsTaps top_left = coords_taps_for_sector(locks, 1, kDim);
        check(!top_left.lock_x && top_left.lock_z, "sector 1 picks the top-left lock");
        check(top_left.z(512) == 0, "sector 1: the z seam wraps inside the quadrant");
        check(top_left.x(512) == 512, "sector 1: the x seam still crosses");

        // Sector id 4 -> quadrant (512,512) -> bottom-right -> both axes locked.
        const CoordsTaps bottom_right = coords_taps_for_sector(locks, 4, kDim);
        check(bottom_right.lock_x && bottom_right.lock_z, "sector 4 picks the bottom-right lock");
        check(bottom_right.x(1024) == 512 && bottom_right.z(1024) == 512,
              "sector 4 wraps both axes inside its own quadrant");
    }

    // 3. The sampler at a sector seam. world_z = 511.5 sits half a cell short of the
    //    boundary, so the bilinear straddles atlas rows 511 and 512.
    {
        SeamAtlas atlas;

        // Unlocked (the pre-fix behavior): row 512 is the neighbouring quadrant's
        // sea floor, so the seam cell dives to the midpoint of 16 and 0.
        check_close(height_field_height_world_bilinear(atlas.field, 100.5f, 511.5f), 8.0, 1e-4,
                    "unlocked: the seam cell dives toward the next quadrant");

        // Locked on z (Dvxc2's top-left lock): the tap wraps to the quadrant's own
        // row 0 and the plateau stays flat across the boundary.
        atlas.field.locks.set(0, false, true);
        check_close(height_field_height_world_bilinear(atlas.field, 100.5f, 511.5f), 16.0, 1e-4,
                    "locked: the seam cell stays on the plateau");

        // Interior samples are untouched by the lock either way.
        check_close(height_field_height_world_bilinear(atlas.field, 100.5f, 100.5f), 16.0, 1e-4,
                    "locked: interior sample unchanged");

        // The x axis is still unlocked here, so its seam still crosses.
        check_close(height_field_height_world_bilinear(atlas.field, 511.5f, 100.5f), 8.0, 1e-4,
                    "x unlocked: the x seam still crosses");
        atlas.field.locks.set(0, true, true);
        check_close(height_field_height_world_bilinear(atlas.field, 511.5f, 100.5f), 16.0, 1e-4,
                    "x locked: the x seam wraps too");

        // The nearest-tap variant never straddles a boundary, so it reads the
        // plateau on both sides regardless.
        check_close(height_field_height_world(atlas.field, 100.5f, 511.5f), 16.0, 1e-4,
                    "nearest tap: plateau");
    }

    // 4. A default-constructed field keeps the old full-atlas wrap, so terrains that
    //    declare no lock (Dvxg1 and friends) render exactly as before.
    {
        SeamAtlas atlas;
        check(atlas.field.locks.x == 0 && atlas.field.locks.z == 0, "default locks are all zero");
        check_close(height_field_height_world_bilinear(atlas.field, 100.5f, 511.5f), 8.0, 1e-4,
                    "default: unchanged from the pre-lock behavior");
    }

    if (g_fail) {
        std::fprintf(stderr, "quadrant_lock_test: FAILED\n");
        return 1;
    }
    std::printf("quadrant_lock_test: OK\n");
    return 0;
}
