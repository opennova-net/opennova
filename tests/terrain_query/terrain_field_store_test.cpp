// TerrainFieldStore — the ONE owning cpt/trn(+charmap) -> TerrainHeightField +
// SurfaceTypeMap builder (ADR 0042 d4, amending ADR 0020 d4).
//
// Covers, over small synthetic cpt/trn/charmap buffers:
//   1. Dims and sector origins land in the height-field view.
//   2. The per-quadrant neighbour-tap locks (.trn lock_*) fold into the view.
//   3. A world-remap bilinear height sample reads the copied heightmap.
//   4. A surface index round-trips through the charmap view
//      [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
//   5. The store OWNS its copies: wiping the source documents changes nothing.
//   6. No charmap -> the zeroed map (the sampler's "no charmap -> surface 1"
//      leg); clear() invalidates.

#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/surface_type_map.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

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

namespace {

constexpr int kDim = 8; // small square atlas; real terrain uses 1024

// Synthetic parsed documents, minted in memory the way the cpt roundtrip test
// mints buffers: heightmap texel (z, x) = (z * kDim + x) height units.
CptFile make_cpt() {
	CptFile cpt;
	cpt.depth_buffer.resize(static_cast<size_t>(kDim) * kDim);
	for (int z = 0; z < kDim; ++z)
		for (int x = 0; x < kDim; ++x)
			cpt.depth_buffer[static_cast<size_t>(z) * kDim + x] =
					static_cast<uint16_t>((z * kDim + x) * 256);
	return cpt;
}

TrnConfig make_trn() {
	TrnConfig trn;
	trn.origin_x = -4;
	trn.origin_y = -4;
	for (int r = 0; r < 16; ++r)
		for (int c = 0; c < 16; ++c)
			trn.sector_grid[r][c] = 1; // every cell maps quadrant 1 (top-left)
	trn.lock_topright = {1, 0};    // quadrant 1: X locked, Z open
	trn.lock_bottomright = {0, 1}; // quadrant 3: X open, Z locked
	return trn;
}

} // namespace

int main() {
	// --- the format-typed build over parsed documents ------------------------
	CptFile cpt = make_cpt();
	TrnConfig trn = make_trn();
	// 4x4 charmap raster with distinct surface classes per texel.
	std::vector<uint8_t> charmap(16);
	for (int i = 0; i < 16; ++i) charmap[i] = static_cast<uint8_t>(i + 2);

	terrain::TerrainFieldStore store;
	terrain::terrain_field_store_build(store, cpt, trn, charmap.data(), 4, 4);
	check(store.valid(), "store builds valid from synthetic cpt/trn");

	const terrain::TerrainHeightField &field = store.height_field();
	check(field.dim == kDim, "dim = sqrt(depth-buffer sample count)");
	check(field.layout.origin_x == -4 && field.layout.origin_y == -4,
			"sector origins stamp into the layout");
	check(field.locks.locked_x(1) && !field.locks.locked_z(1),
			"lock_topright folds to quadrant 1's X lock");
	check(!field.locks.locked_x(3) && field.locks.locked_z(3),
			"lock_bottomright folds to quadrant 3's Z lock");
	check(!field.has_water, "the water clamp stays deferred (has_water off)");
	check(field.heightmap != cpt.depth_buffer.data(),
			"the heightmap view points at the store's own copy");

	// World (3, 2): sector cell (0,0) - origin (-4,-4) -> grid (4,4) = 1 ->
	// quadrant offset (0,0) -> source texel (3, 2) = 2*8+3 = 19 height units.
	check_close(terrain::height_field_height_world_bilinear(field, 3.0f, 2.0f),
			19.0, 1e-4, "world-remap bilinear height sample");

	// Surface round-trip at fixed-point mission (300, -200): same sector cell,
	// fine (300, 200), 4-wide raster shift 8 -> texel (1, 0) = charmap[1] = 3.
	const terrain::SurfaceTypeMap &surface = store.surface_map();
	check(surface.width == 4 && surface.height == 4, "charmap dims land in the view");
	check(surface.origin_x == -4 && surface.origin_y == -4,
			"the surface view shares the sector origins");
	check(surface.data != charmap.data(),
			"the charmap view points at the store's own copy");
	check(terrain::surface_type_at_fixed(surface, 300 << 16, -(200 << 16)) == 3,
			"surface index round-trips through the charmap view");
	// Texel (1, 1) = charmap[4*1+1] = 7 via mission (300, -300): fine (300,
	// 300), both >> 8 = 1 (quadrant 1 reaches raster texels 0..1 per axis).
	check(terrain::surface_type_at_fixed(surface, 300 << 16, -(300 << 16)) ==
					charmap[4 * 1 + 1],
			"a second texel round-trips (row advance)");

	// --- ownership: wiping the sources changes nothing ----------------------
	cpt.depth_buffer.assign(cpt.depth_buffer.size(), 0);
	cpt.depth_buffer.clear();
	trn = TrnConfig{};
	charmap.assign(charmap.size(), 0);
	charmap.clear();
	check_close(terrain::height_field_height_world_bilinear(store.height_field(), 3.0f, 2.0f),
			19.0, 1e-4, "height sample survives the source documents being freed");
	check(terrain::surface_type_at_fixed(store.surface_map(), 300 << 16, -(200 << 16)) == 3,
			"surface sample survives the source charmap being freed");

	// --- the format-free build path, no charmap ------------------------------
	{
		std::vector<uint16_t> hm(static_cast<size_t>(kDim) * kDim, 7 * 256);
		std::vector<int> grid(256, 1);
		terrain::CoordsQuadrantLocks locks{};
		locks.set(0, true, true);
		terrain::TerrainFieldStore bare;
		bare.build(hm.data(), hm.size(), grid.data(), 0, 0, locks);
		check(bare.valid(), "format-free build over raw buffers");
		check(bare.height_field().locks.locked_x(0) && bare.height_field().locks.locked_z(0),
				"raw locks pass through");
		check(bare.surface_map().data == nullptr,
				"no charmap leaves the surface view zeroed");
		check(terrain::surface_type_at_fixed(bare.surface_map(), 300 << 16, -(200 << 16)) == 1,
				"the zeroed view is the sampler's 'no charmap -> surface 1' leg");
		// The occupant water clamp's plane rides beside the field in the same
		// 16.16 units the ground solve compares [orig: worldY @0x26C6454]; a
		// rebuild starts without one.
		check(!bare.height_field().has_water, "a fresh build carries no water plane");
		bare.set_water_plane(12 << 16);
		check(bare.height_field().has_water && bare.height_field().water_y == (12 << 16),
				"set_water_plane feeds the clamp plane");
		bare.set_water_plane(0);
		check(!bare.height_field().has_water, "a zero plane is no authored water");
		bare.clear();
		check(!bare.valid(), "clear() invalidates the store");
	}

	// --- degenerate input ----------------------------------------------------
	{
		terrain::TerrainFieldStore empty;
		terrain::terrain_field_store_build(empty, CptFile{}, TrnConfig{});
		check(!empty.valid(), "an empty depth buffer builds an invalid store");
	}

	if (g_fail == 0) std::printf("terrain_field_store_test: all checks passed\n");
	return g_fail;
}
