#include <editor/preview/terrain_map_target.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include <base/io/fixed.h>
#include <formats/trn/trn_io.h>
#include <runtime/terrain_query/atlas_routing.h>

namespace opennova::editor {

namespace {

terrain::TerrainAtlasRouting routing_of(const TerrainMapTarget &target) {
	terrain::TerrainAtlasRouting routing;
	routing.sector_grid = target.grid.data();
	routing.origin_x = target.origin_x;
	routing.origin_y = target.origin_y;
	routing.wrap_x = target.wrap_x;
	routing.wrap_z = target.wrap_z;
	return routing;
}

int32_t fixed_of(double metres) {
	const double scaled = std::floor(metres * io::kFp16OneD);
	return scaled <= -2147483648.0 ? INT32_MIN : scaled >= 2147483647.0 ? INT32_MAX : int32_t(scaled);
}

} // namespace

double TerrainMapTarget::step() const {
	if (shifted) return shift < 0 || shift > 10 ? 1.0 : double(1 << shift);
	return width > 0 ? 1024.0 / double(width) : 1.0;
}

TerrainMapTarget terrain_map_target(const TrnConfig &trn, TerrainPlane plane, int width, int height) {
	TerrainMapTarget out;
	// The grid as the game holds it, extended to 16 x 16 by the admission gate's rule [orig: Terrain_ShiftHeightmapRows
	// @ 0x60F2A5..0x60F317, formats/trn trn_extend_sector_grid].
	TrnConfig grid = trn;
	trn_extend_sector_grid(grid);
	for (int row = 0; row < kTerrainGridSide; ++row)
		for (int col = 0; col < kTerrainGridSide; ++col) out.grid[size_t(row * 16 + col)] = grid.sector_grid[row][col];
	out.origin_x = trn.origin_x;
	out.origin_y = trn.origin_y;
	out.wrap_x = trn.wrap_x != 0;
	out.wrap_z = trn.wrap_y != 0;
	out.width = width;
	out.height = height;
	out.shifted = plane == TerrainPlane::Surface || plane == TerrainPlane::Foliage;
	out.shift = out.shifted ? terrain::detail::surface_sample_shift(width) : 0;
	return out;
}

bool terrain_map_texel(const TerrainMapTarget &target, double x, double y, int &tx, int &ty) {
	if (!target.valid()) return false;
	const terrain::TerrainAtlasRouting routing = routing_of(target);
	const terrain::TerrainAtlasTexel atlas = terrain::terrain_atlas_texel_at(routing, fixed_of(x), fixed_of(y));
	if (!atlas.mapped) return false;
	if (target.shifted) {
		// The samplers' shift as the processor takes it, modulo 32 (a map 2048 wide or more reads its first texel).
		tx = int(uint32_t(atlas.x) >> uint32_t(target.shift & 31));
		ty = int(uint32_t(atlas.z) >> uint32_t(target.shift & 31));
	} else {
		// The atlas position to the unit's fraction (its x east, its z south: the mission y negated), scaled by the
		// map's side over 1024.
		const double fx = x - std::floor(x), fz = -y - std::floor(-y);
		tx = int(std::floor((double(atlas.x) + fx) * double(target.width) / 1024.0));
		ty = int(std::floor((double(atlas.z) + fz) * double(target.height) / 1024.0));
	}
	return tx >= 0 && ty >= 0 && tx < target.width && ty < target.height;
}

std::vector<TerrainMapTexel> terrain_map_dab(const TerrainMapTarget &target, double x, double y,
                                             const TerrainBrush &brush, bool categorical) {
	std::vector<TerrainMapTexel> out;
	if (!target.valid()) return out;
	const double radius = brush.radius;
	// The texels' cells on the plane, each a step aligned to the step, sampled at its centre.
	const double step = target.step();
	const double half = step * 0.5;
	// What each texel reached holds in `out`: a hash for a small dab, a flat index for a large one (a wide brush on a
	// fine map reaches up to a million cells).
	const double across = 2.0 * std::max(radius, 0.0) / step + 2.0;
	const bool flat = across * across > 65536.0;
	std::unordered_map<uint32_t, size_t> seen;
	std::vector<int32_t> slots;
	if (flat) slots.assign(size_t(target.width) * size_t(target.height), -1);
	const auto reach = [&](double px, double py, float weight) {
		int tx = 0, ty = 0;
		if (weight <= 0.0f || !terrain_map_texel(target, px, py, tx, ty)) return;
		const uint32_t index = uint32_t(ty) * uint32_t(target.width) + uint32_t(tx);
		size_t at = SIZE_MAX;
		if (flat) {
			if (slots[index] >= 0) at = size_t(slots[index]);
			else slots[index] = int32_t(out.size());
		} else if (const auto found = seen.find(index); found != seen.end()) {
			at = found->second;
		} else {
			seen.emplace(index, out.size());
		}
		if (at == SIZE_MAX) out.push_back({index, weight});
		else if (out[at].weight < weight) out[at].weight = weight;
	};
	if (radius <= 0.0) {
		reach(x, y, 1.0f);
		return out;
	}
	const int64_t x0 = int64_t(std::floor((x - radius) / step)), x1 = int64_t(std::floor((x + radius) / step));
	const int64_t y0 = int64_t(std::floor((y - radius) / step)), y1 = int64_t(std::floor((y + radius) / step));
	for (int64_t j = y0; j <= y1; ++j)
		for (int64_t i = x0; i <= x1; ++i) {
			const double cx = double(i) * step + half, cy = double(j) * step + half;
			const double distance = std::hypot(cx - x, cy - y);
			if (distance > radius) continue;
			reach(cx, cy, terrain_brush_weight(brush, distance, categorical));
		}
	// A disc smaller than a texel reaches the texel under its centre.
	if (out.empty()) reach(x, y, terrain_brush_weight(brush, 0.0, categorical));
	return out;
}

} // namespace opennova::editor
