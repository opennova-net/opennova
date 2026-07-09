// The witnessed axis form and tile-key bias
// [orig: Foliage_GenerateModelTileInstances @ 0x600980;
// Foliage_UpdateModelTiles @ 0x601f50]:
//   worldX = (keyHigh + localA) * 65536, worldZ = (keyLow - localB) * 65536 -
//   the local B axis runs NEGATIVE world Z, and the +16u key bias makes the
//   tile cover [snap, snap+16]^2 anyway.
#include <foliage/model_placement.h>

#include <cstdio>

using namespace opennova;
using namespace opennova::foliage;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

PlacementSamplers permissive() {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };
	return s;
}

} // namespace

int main() {
	ModelPlacementConfig cfg{};
	cfg.footprint[0] = 0.25f;  // tiny footprint keeps corners near the centers
	auto s = permissive();

	// --- Coverage: every emitted center lies inside [snap, snap+16]^2 -------
	const Fixed16_16 snaps[][2] = {
	    {0x00100000, 0x00200000},
	    {0x00000000, 0x00000000},
	    {-0x00100000, -0x00200000},  // negative tile exercises the 15-bit sext
	};
	for (const auto &snap : snaps) {
		const uint32_t key = pack_model_tile_key(snap[0], snap[1]);
		// Anchor at the tile center, huge radius: all 36 candidates accepted
		// up to the cap.
		const auto r = generate_model_tile_instances(0, key,
		                                             snap[0] + 0x80000, snap[1] + 0x80000,
		                                             0x40000000, cfg, s);
		if (!expect(r.count == MODEL_TILE_CAP, "permissive tile fills to the cap")) return 1;
		for (int i = 0; i < r.count; ++i) {
			const auto &inst = r.instances[i];
			const bool inside =
			    inst.center_x_fixed >= snap[0] && inst.center_x_fixed <= snap[0] + MODEL_TILE_SIZE &&
			    inst.center_z_fixed >= snap[1] && inst.center_z_fixed <= snap[1] + MODEL_TILE_SIZE;
			if (!expect(inside, "every center lies inside the snapped 16u tile square")) {
				std::fprintf(stderr, "  snap (%d, %d), center (%d, %d)\n",
				             snap[0], snap[1], inst.center_x_fixed, inst.center_z_fixed);
				return 1;
			}
		}

		// --- The B axis runs NEGATIVE world Z: row i/6 grows, worldZ falls.
		// Candidates arrive in index order under permissive gates, so
		// instance[c] is candidate c for c < cap. Compare column-0 candidates
		// of rows 0..3 (candidates 0, 6, 12, 18).
		for (int row = 0; row + 1 < 4; ++row) {
			const auto &lo = r.instances[row * 6];
			const auto &hi = r.instances[(row + 1) * 6];
			if (!expect(hi.center_z_fixed < lo.center_z_fixed,
			            "increasing localB rows emit strictly decreasing world Z")) return 1;
		}
		// Along a row, localA grows and world X grows with it.
		for (int col = 0; col + 1 < 6; ++col) {
			const auto &lo = r.instances[col];
			const auto &hi = r.instances[col + 1];
			if (!expect(hi.center_x_fixed > lo.center_x_fixed,
			            "increasing localA columns emit strictly increasing world X")) return 1;
		}
	}

	// --- Quadrant walk: 4 distinct keys covering the 2x2 tile set -----------
	{
		// Anchor inside a tile: +-8u reaches the 4 surrounding 16u snaps.
		const Fixed16_16 ax = 0x00185000;  // 24.3125
		const Fixed16_16 az = 0x00294000;  // 41.25
		const auto tiles = model_quadrant_tiles(ax, az);
		for (int i = 0; i < 4; ++i) {
			for (int j = i + 1; j < 4; ++j) {
				if (!expect(tiles[i].key != tiles[j].key, "the 4 quadrant keys are distinct")) return 1;
			}
			// Each snap is 16u-aligned and within 8u of the anchor on each axis.
			if (!expect((tiles[i].snap_x_fixed & 0xFFFFF) == 0 && (tiles[i].snap_z_fixed & 0xFFFFF) == 0,
			            "quadrant snaps are 16u aligned")) return 1;
			if (!expect(tiles[i].key == pack_model_tile_key(tiles[i].snap_x_fixed, tiles[i].snap_z_fixed),
			            "quadrant key matches its snap origin")) return 1;
		}
		// Coverage: the 4 snaps are exactly the 16u cells containing
		// anchor - 8 and anchor + 8 per axis (2x2).
		const Fixed16_16 lo_x = (ax - MODEL_QUADRANT_OFFSET) & MODEL_TILE_SNAP_MASK;
		const Fixed16_16 hi_x = (ax + MODEL_QUADRANT_OFFSET) & MODEL_TILE_SNAP_MASK;
		const Fixed16_16 lo_z = (az - MODEL_QUADRANT_OFFSET) & MODEL_TILE_SNAP_MASK;
		const Fixed16_16 hi_z = (az + MODEL_QUADRANT_OFFSET) & MODEL_TILE_SNAP_MASK;
		bool seen[2][2] = {};
		for (const auto &t : tiles) {
			const int xi = t.snap_x_fixed == lo_x ? 0 : (t.snap_x_fixed == hi_x ? 1 : -1);
			const int zi = t.snap_z_fixed == lo_z ? 0 : (t.snap_z_fixed == hi_z ? 1 : -1);
			if (!expect(xi >= 0 && zi >= 0, "each quadrant snap is one of the 2x2 covering cells")) return 1;
			seen[xi][zi] = true;
		}
		if (!expect(seen[0][0] && seen[0][1] && seen[1][0] && seen[1][1],
		            "the walk covers all four 16u cells overlapping anchor +-8u")) return 1;

		// Each quadrant tile's instances stay inside that tile's square.
		for (const auto &t : tiles) {
			const auto r = generate_model_tile_instances(0, t.key, ax, az, 0x40000000, cfg, s);
			for (int i = 0; i < r.count; ++i) {
				const auto &inst = r.instances[i];
				const bool inside =
				    inst.center_x_fixed >= t.snap_x_fixed &&
				    inst.center_x_fixed <= t.snap_x_fixed + MODEL_TILE_SIZE &&
				    inst.center_z_fixed >= t.snap_z_fixed &&
				    inst.center_z_fixed <= t.snap_z_fixed + MODEL_TILE_SIZE;
				if (!expect(inside, "quadrant-tile instances stay inside their own square")) return 1;
			}
		}
	}

	std::printf("OK: axis form (B -> -Z), +16u key bias, quadrant coverage\n");
	return 0;
}
