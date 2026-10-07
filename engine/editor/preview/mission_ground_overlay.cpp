// The surface classes and the foliage over a mission's terrain, as the game reads them (mission_ground_overlay.h,
// DI-29). Tooling: every value is the game's own sampler's (terrain_query), the picture and its colours the
// editor's.

#include <editor/preview/mission_ground_overlay.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include <editor/preview/mission_ground_facts.h>
#include <formats/mission/bms.h>
#include <formats/trn/charmap_legend.h>
#include <runtime/world/ammo_table.h>

namespace opennova::editor {

using io::JsonValue;
using io::json_number;
using io::json_string;

namespace {

constexpr const char *kOverlays[] = { "none", "surfaces", "foliage" };
// Each foliage definition's colour in the overlay, by its slot (the editor's own: four apart from the char
// map legend's greens).
constexpr uint8_t kSlotColours[4][3] = { { 40, 230, 70 }, { 255, 196, 0 }, { 0, 168, 255 }, { 235, 70, 220 } };
// What a placed tile keeps off, and a tile's outline.
constexpr uint8_t kKeptOff[3] = { 128, 128, 128 };
constexpr uint8_t kOutline[3] = { 255, 255, 255 };
constexpr double kSector = 512.0; // a sector's side, world units (a fixed-point position >> 25)
constexpr int kGrid = 16;

// The picture's extent over the sector grid: its mapped cells, the whole grid on an axis that wraps.
struct Extent {
	bool any = false;
	int col0 = kGrid, col1 = -1, row0 = kGrid, row1 = -1;
};

Extent mapped_cells(const terrain::FoliageMaskMap &grid) {
	Extent out;
	if (!grid.sector_grid) return out;
	for (int row = 0; row < kGrid; ++row)
		for (int col = 0; col < kGrid; ++col) {
			if (grid.sector_grid[kGrid * row + col] < 1) continue;
			out.any = true;
			out.col0 = std::min(out.col0, col);
			out.col1 = std::max(out.col1, col);
			out.row0 = std::min(out.row0, row);
			out.row1 = std::max(out.row1, row);
		}
	if (out.any && grid.wrap_x) out.col0 = 0, out.col1 = kGrid - 1;
	if (out.any && grid.wrap_z) out.row0 = 0, out.row1 = kGrid - 1;
	return out;
}

// The grid as the samplers route it, whichever map is read.
terrain::FoliageMaskMap grid_of(const MissionGround &ground) {
	terrain::FoliageMaskMap grid;
	const terrain::TerrainHeightField *field = ground.height_field();
	if (!field) return grid;
	grid.sector_grid = field->layout.sector_grid;
	grid.origin_x = field->layout.origin_x;
	grid.origin_y = field->layout.origin_y;
	grid.wrap_x = field->wrap_x;
	grid.wrap_z = field->wrap_z;
	return grid;
}

// The picture laid out over the extent at the map's own texel (1024 / its side), made coarser by powers of
// two to fit; false for no extent.
bool lay_out(MissionOverlayImage &image, const terrain::FoliageMaskMap &grid, const Extent &extent, int side) {
	if (!extent.any || side <= 0) return false;
	image.west = (grid.origin_x + extent.col0) * kSector;
	const double east = (grid.origin_x + extent.col1 + 1) * kSector;
	// A grid row is -y >> 25 less the origin: row r spans mission y from -(origin + r) sectors south.
	image.north = -(grid.origin_y + extent.row0) * kSector;
	const double south = -(grid.origin_y + extent.row1 + 1) * kSector;
	image.texel = 1024.0 / double(side);
	const double span = std::max(east - image.west, image.north - south);
	while (span / image.texel > double(kMissionOverlayMaxSide)) image.texel *= 2.0;
	image.width = int(std::ceil((east - image.west) / image.texel));
	image.height = int(std::ceil((image.north - south) / image.texel));
	image.rgba.assign(size_t(image.width) * size_t(image.height) * 4, 0);
	// One value past the picture: no wrap, and the grid's empty cells all round the mapped ones (an off-grid
	// position clamps to an edge cell, which is then empty too).
	image.outside = !grid.wrap_x && !grid.wrap_z && extent.col0 > 0 && extent.col1 < kGrid - 1 && extent.row0 > 0 &&
			extent.row1 < kGrid - 1;
	return true;
}

// The texel's middle, mission metres, and as the game's 16.16.
struct Point {
	double x, y;
	int32_t fx, fy;
};
Point point_of(const MissionOverlayImage &image, int i, int j) {
	Point p;
	p.x = image.west + (double(i) + 0.5) * image.texel;
	p.y = image.north - (double(j) + 0.5) * image.texel;
	p.fx = bms::to_fixed_16_16(p.x);
	p.fy = bms::to_fixed_16_16(p.y);
	return p;
}

// The texels whose middles lie within `pad` units of a placed tile's 16-unit square (its z the negated mission y
// it starts at): where a sampler that walks the tiles is asked; elsewhere the tiles decide nothing.
std::vector<uint8_t> near_tiles(const MissionOverlayImage &image, const std::vector<terrain::SurfaceTileEntry> &tiles,
		double pad) {
	std::vector<uint8_t> out;
	if (tiles.empty()) return out;
	out.assign(size_t(image.width) * size_t(image.height), 0);
	for (const terrain::SurfaceTileEntry &tile : tiles) {
		const double x0 = double(tile.x_fixed) / 65536.0 - pad, x1 = double(tile.x_fixed) / 65536.0 + 16.0 + pad;
		const double y0 = -double(tile.z_fixed) / 65536.0 - pad, y1 = -double(tile.z_fixed) / 65536.0 + 16.0 + pad;
		const int i0 = std::max(0, int(std::floor((x0 - image.west) / image.texel)) - 1);
		const int i1 = std::min(image.width - 1, int(std::floor((x1 - image.west) / image.texel)) + 1);
		const int j0 = std::max(0, int(std::floor((image.north - y1) / image.texel)) - 1);
		const int j1 = std::min(image.height - 1, int(std::floor((image.north - y0) / image.texel)) + 1);
		for (int j = j0; j <= j1; ++j)
			for (int i = i0; i <= i1; ++i) out[size_t(j) * size_t(image.width) + size_t(i)] = 1;
	}
	return out;
}

void put(MissionOverlayImage &image, int i, int j, const uint8_t rgb[3], uint8_t alpha) {
	uint8_t *p = &image.rgba[(size_t(j) * size_t(image.width) + size_t(i)) * 4];
	p[0] = rgb[0];
	p[1] = rgb[1];
	p[2] = rgb[2];
	p[3] = alpha;
}

void class_rgb(int surface, uint8_t out[3]) {
	const CharmapLegendColour &c =
			surface >= 0 && surface < kCharmapLegendCount ? kCharmapLegend[surface] : kCharmapLegendRest;
	out[0] = c.r;
	out[1] = c.g;
	out[2] = c.b;
}

// A class in the legend's words: "Snow (TSD_SNOW): the snow footsteps; a round plays its ammo's snow row".
std::string class_row_words(int surface) {
	std::string out = mission_surface_words(surface);
	if (const char *name = mission_surface_name(surface)) out += std::string(" (") + name + ")";
	if (surface < 0 || surface > 255) return out;
	const int row = world::terrain_impact_effect_tag(surface);
	out += std::string(": the ") + (surface == 3 ? "snow" : "ground") + " footsteps";
	if (row >= 0 && row < int(std::size(world::kImpactEffectTagNames)))
		out += std::string("; a round plays its ammo's ") + world::kImpactEffectTagNames[row] + " row";
	return out;
}

void surfaces(MissionOverlayImage &image, const MissionGround &ground) {
	const int side = ground.surface_side();
	const terrain::SurfaceTypeMap sampler = ground.surface_sampler();
	if (side <= 0) {
		// No char map: the game reads 1 everywhere, before the tiles [orig: Terrain_GetSurfaceTypeAtPosition @
		// 0x606519].
		image.outside = true;
		class_rgb(1, image.outside_rgba);
		image.outside_rgba[3] = kMissionOverlayAlpha;
		image.words = "The terrain names no char map (polytrn_charmap): the game reads " + mission_surface_words(1) +
				" (surface 1) everywhere.";
		MissionOverlayRow row;
		row.key = "1";
		class_rgb(1, row.rgb);
		row.share = 1.0;
		row.words = class_row_words(1);
		image.legend.push_back(std::move(row));
		return;
	}
	image.title = "Surface classes as the game reads them: " + ground.surface_map() + " (" + std::to_string(side) +
			" a side)";
	const terrain::FoliageMaskMap grid = grid_of(ground);
	const Extent extent = mapped_cells(grid);
	if (!lay_out(image, grid, extent, side)) {
		image.words = "The terrain's sector grid maps no sector: the game reads " + mission_surface_words(7) +
				" (surface 7) everywhere.";
		image.outside = true;
		class_rgb(7, image.outside_rgba);
		image.outside_rgba[3] = kMissionOverlayAlpha;
		return;
	}
	// The char map alone everywhere, the tiles walked only where one may decide (an exact answer: past every
	// square the walk returns the char map's class).
	terrain::SurfaceTypeMap bare = sampler;
	bare.tiles = nullptr;
	bare.tile_count = 0;
	const std::vector<uint8_t> near = near_tiles(image, ground.placed_tiles(), 1.0);
	size_t counts[256] = {};
	for (int j = 0; j < image.height; ++j)
		for (int i = 0; i < image.width; ++i) {
			const Point p = point_of(image, i, j);
			const bool walk = !near.empty() && near[size_t(j) * size_t(image.width) + size_t(i)];
			const int surface = terrain::surface_type_at_fixed(walk ? sampler : bare, p.fx, p.fy);
			uint8_t rgb[3];
			class_rgb(surface, rgb);
			put(image, i, j, rgb, kMissionOverlayAlpha);
			++counts[surface & 0xFF];
		}
	// Each placed tile whose square decides the class (DI-07's leg) outlined.
	size_t deciding = 0;
	for (const terrain::SurfaceTileEntry &tile : ground.placed_tiles()) {
		const double x0 = double(tile.x_fixed) / 65536.0, y0 = -double(tile.z_fixed) / 65536.0;
		if (ground.terrain_at(x0 + 8.0, y0 + 8.0, 0.0).from != MissionSurfaceFrom::Tile) continue;
		++deciding;
		const int i0 = int(std::floor((x0 - image.west) / image.texel));
		const int i1 = int(std::floor((x0 + 16.0 - image.west) / image.texel));
		const int j0 = int(std::floor((image.north - (y0 + 16.0)) / image.texel));
		const int j1 = int(std::floor((image.north - y0) / image.texel));
		for (int j = j0; j <= j1; ++j)
			for (int i = i0; i <= i1; ++i) {
				if (i < 0 || j < 0 || i >= image.width || j >= image.height) continue;
				if (i == i0 || i == i1 || j == j0 || j == j1) put(image, i, j, kOutline, 255);
			}
	}
	const double texels = double(image.width) * double(image.height);
	for (int surface = 0; surface < 256; ++surface) {
		if (!counts[surface]) continue;
		MissionOverlayRow row;
		row.key = std::to_string(surface);
		class_rgb(surface, row.rgb);
		row.share = double(counts[surface]) / texels;
		row.words = class_row_words(surface);
		image.legend.push_back(std::move(row));
	}
	if (deciding) {
		MissionOverlayRow row;
		row.key = "tile";
		std::copy(std::begin(kOutline), std::end(kOutline), row.rgb);
		row.words = std::to_string(deciding) + " placed tile(s) of the mission's .til, outlined: each square's class is "
				"its tile set's table entry (TSD_NULL with no .tsd), not the char map's";
		image.legend.push_back(std::move(row));
	}
	if (image.outside) {
		// Past the mapped sectors, an empty cell's value, read where the grid's empty cells begin.
		const Point beyond{ 0.0, 0.0, bms::to_fixed_16_16(image.west - kSector * 0.5),
			bms::to_fixed_16_16(image.north + kSector * 0.5) };
		const int surface = terrain::surface_type_at_fixed(sampler, beyond.fx, beyond.fy);
		class_rgb(surface, image.outside_rgba);
		image.outside_rgba[3] = kMissionOverlayAlpha;
		image.words = "Past the picture, the grid's empty sectors: the game reads " + mission_surface_words(surface) +
				" (surface " + std::to_string(surface) + ") everywhere.";
	}
}

// The definitions a mask names: "onfern1.3di (foliage 1, codes 253)".
std::string slots_words(const std::vector<FoliageDef> &defs, int mask) {
	std::string out;
	for (size_t slot = 0; slot < defs.size() && slot < 4; ++slot) {
		if (!(mask & (1 << slot))) continue;
		std::string codes;
		for (const int code : defs[slot].match)
			if (code > 0) codes += (codes.empty() ? "" : " ") + std::to_string(code);
		out += (out.empty() ? "" : ", ") + defs[slot].graphic + " (foliage " + std::to_string(slot + 1) + ", codes " +
				codes + ")";
	}
	return out;
}

void mask_rgb(int mask, uint8_t out[3]) {
	int r = 0, g = 0, b = 0, n = 0;
	for (int slot = 0; slot < 4; ++slot)
		if (mask & (1 << slot)) r += kSlotColours[slot][0], g += kSlotColours[slot][1], b += kSlotColours[slot][2], ++n;
	out[0] = uint8_t(n ? r / n : 0);
	out[1] = uint8_t(n ? g / n : 0);
	out[2] = uint8_t(n ? b / n : 0);
}

void foliage(MissionOverlayImage &image, const MissionGround &ground) {
	const IndexedImage8 &codes = ground.foliage_codes();
	// Past the mapped sectors, and everywhere with no map, nothing grows: no tint.
	image.outside = true;
	if (codes.empty()) {
		image.words = "The terrain names no foliage map (polytrn_foliagemap), or it does not read: nothing grows.";
		return;
	}
	image.title = "Foliage as the game grows it: " + ground.foliage_map() + " (" + std::to_string(codes.width) +
			" a side), its codes to the terrain's definitions";
	const std::vector<FoliageDef> &defs = ground.foliage_defs();
	const terrain::FoliageMaskMap masks = ground.foliage_sampler(false);
	const Extent extent = mapped_cells(masks);
	if (!lay_out(image, masks, extent, codes.width)) {
		image.words = "The terrain's sector grid maps no sector: nothing grows.";
		return;
	}
	const std::vector<uint8_t> near = near_tiles(image, ground.placed_tiles(), 3.0);
	size_t counts[16] = {};
	size_t kept_texels = 0;
	for (int j = 0; j < image.height; ++j)
		for (int i = 0; i < image.width; ++i) {
			const Point p = point_of(image, i, j);
			const int mask = terrain::foliage_mask_at_fixed(masks, p.fx, p.fy) & 0xF;
			if (!mask) continue;
			const bool walk = !near.empty() && near[size_t(j) * size_t(image.width) + size_t(i)];
			const int kept = walk ? ground.foliage_kept_off(p.x, p.y, mask) : 0;
			const int grows = mask & ~kept;
			if (grows) {
				uint8_t rgb[3];
				mask_rgb(grows, rgb);
				put(image, i, j, rgb, kMissionOverlayAlpha);
				++counts[grows];
			} else {
				put(image, i, j, kKeptOff, kMissionOverlayAlpha);
				++kept_texels;
			}
		}
	const double texels = double(image.width) * double(image.height);
	for (int mask = 1; mask < 16; ++mask) {
		if (!counts[mask]) continue;
		MissionOverlayRow row;
		for (int slot = 0; slot < 4; ++slot)
			if (mask & (1 << slot)) row.key += (row.key.empty() ? "foliage " : " + ") + std::to_string(slot + 1);
		mask_rgb(mask, row.rgb);
		row.share = double(counts[mask]) / texels;
		row.words = "grows " + slots_words(defs, mask);
		image.legend.push_back(std::move(row));
	}
	if (kept_texels) {
		MissionOverlayRow row;
		row.key = "kept off";
		std::copy(std::begin(kKeptOff), std::end(kKeptOff), row.rgb);
		row.share = double(kept_texels) / texels;
		row.words = "a placed tile's square (2 units round it) keeps every definition without forceon off";
		image.legend.push_back(std::move(row));
	}
	if (defs.empty()) image.words = "The terrain holds no foliage definition (its .trn's foliage blocks): nothing grows.";
	else if (image.legend.empty()) image.words = "No code its foliage map holds selects a definition: nothing grows.";
}

} // namespace

const char *mission_ground_overlay_token(MissionGroundOverlay overlay) {
	return size_t(overlay) < std::size(kOverlays) ? kOverlays[size_t(overlay)] : "none";
}

bool mission_ground_overlay_from_token(const std::string &token, MissionGroundOverlay &out) {
	for (size_t i = 0; i < std::size(kOverlays); ++i)
		if (token == kOverlays[i]) {
			out = MissionGroundOverlay(i);
			return true;
		}
	return false;
}

MissionOverlayImage mission_ground_overlay(const MissionGround &ground, MissionGroundOverlay kind) {
	MissionOverlayImage image;
	image.kind = kind;
	if (kind == MissionGroundOverlay::None) return image;
	if (!ground.terrain()) {
		image.words = ground.error().empty() ? std::string("The mission's terrain was not read.") : ground.error();
		return image;
	}
	if (kind == MissionGroundOverlay::Surfaces) surfaces(image, ground);
	else foliage(image, ground);
	return image;
}

io::JsonValue mission_overlay_to_json(const MissionOverlayImage &image) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(mission_ground_overlay_token(image.kind)));
	out.set("title", json_string(image.title));
	JsonValue legend = JsonValue::make_array();
	for (const MissionOverlayRow &row : image.legend) {
		JsonValue one = JsonValue::make_object();
		one.set("key", json_string(row.key));
		JsonValue rgb = JsonValue::make_array();
		for (const uint8_t c : row.rgb) rgb.push(json_number(c));
		one.set("rgb", std::move(rgb));
		one.set("share", json_number(row.share));
		one.set("words", json_string(row.words));
		legend.push(std::move(one));
	}
	out.set("legend", std::move(legend));
	out.set("words", json_string(image.words));
	out.set("west", json_number(image.west));
	out.set("north", json_number(image.north));
	out.set("texel", json_number(image.texel));
	out.set("width", json_number(image.width));
	out.set("height", json_number(image.height));
	if (image.outside) {
		JsonValue rgba = JsonValue::make_array();
		for (const uint8_t c : image.outside_rgba) rgba.push(json_number(c));
		out.set("outside", std::move(rgba));
	} else {
		out.set("outside", JsonValue::make_null());
	}
	return out;
}

} // namespace opennova::editor
