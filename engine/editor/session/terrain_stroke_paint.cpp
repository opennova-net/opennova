// The maps' planner (ADR 0046 S25 B4): a stroke's sample of the Paint tools on the surface, foliage, colour and
// detail blend maps, each dab laid on the map's own texture document (documents/texture_raster.h's plane) at the
// texels the game's routing reads for the mission's ground under it (preview/terrain_map_target.h). UI over the
// game's data: the values a dab lays are the file's own (a class, a foliage code, a colour, the blend's layer
// weights), the map written again in its form at Save.
#include <editor/session/terrain_stroke_planes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <editor/documents/texture_document.h>
#include <editor/preview/terrain_map_target.h>
#include <editor/project/project_files.h>
#include <formats/trn/charmap_legend.h>

namespace opennova::editor {

namespace {

// The texture form a plane's map is read in, in a modder's words, where its plane's texels are not that form. An
// import's source (`source`) is read as the import reads it: a surface map's indices are its classes, a colour
// picture's colours the legend's; a foliage map's indices or grey levels are its codes [S20: runtime/terrain
// decode_charmap_source, decode_foliage_map_source].
bool form_fits(TerrainPlane plane, const RasterPlane &texels, int layer, bool source, std::string &why) {
	switch (plane) {
	case TerrainPlane::Surface:
		if (texels.format() == RasterFormat::U8) return true;
		if (source) return true; // the legend's colours
		why = "the game reads the surface map as an 8-bit PCX of indices";
		return false;
	case TerrainPlane::Foliage:
		if (texels.format() == RasterFormat::U8) return true;
		// A true-colour source whose texels are grey: its import reads their levels as the codes
		// (decode_foliage_map_source), so a code is painted as its grey.
		if (source && (texels.format() == RasterFormat::Rgb8 || texels.format() == RasterFormat::Rgba8)) return true;
		why = "the game reads the foliage map as an 8-bit PCX of indices";
		return false;
	case TerrainPlane::Colour:
		if (texels.format() == RasterFormat::Rgb8 || texels.format() == RasterFormat::Rgba8) return true;
		why = "the colour map is a true-colour image";
		return false;
	case TerrainPlane::Blend:
		if (texels.format() != RasterFormat::Rgb8 && texels.format() != RasterFormat::Rgba8) {
			why = "the detail blend map is a true-colour image, a layer's weight a channel";
			return false;
		}
		// Three layers whatever the depth: the splat weighs the three detail textures by the map's red, green and blue,
		// normalised over those three alone [orig: PolyTrn_InitTextures @ 0x60b216..0x60b24d (the normalisation,
		// texture_preprocess normalize_detail_blend_map); the normal-map splat text @ 0x7dece0
		// (g_PolyTrnPS14SplatNormalMap's: c1 R + c2 G + c3 B)]; a 32-bit map's alpha is read by nothing.
		if (layer < 0 || layer > 2) {
			why = "the game blends three detail layers by the map's red, green and blue (layers 0 to 2)";
			return false;
		}
		return true;
	case TerrainPlane::Heights:
	case TerrainPlane::kCount: break;
	}
	why = "it is no map the Paint tools paint";
	return false;
}

// The game's class where a terrain names no char map [orig: Terrain_LoadCharmapPCX], what an Erase of the surface map lays.
constexpr int kDefaultClass = 1;

// The value a dab of weight `weight` leaves on a texel holding `texel` (bytes of the plane's format). An Erase lays the
// game's default: class 1, code 0, the first detail layer, the colour map's texel as last saved (`saved`).
void lay(TerrainPlane plane, const TerrainBrush &brush, float weight, uint8_t *texel, int bytes,
         const uint8_t *saved = nullptr) {
	switch (plane) {
	case TerrainPlane::Surface: {
		const int surface = brush.erase ? kDefaultClass : brush.surface;
		if (bytes == 1) {
			texel[0] = uint8_t(std::clamp(surface, 0, 255));
		} else {
			// A colour source: the class's legend colour, its alpha kept (the import reads the colours alone).
			const CharmapLegendColour &c = kCharmapLegend[std::clamp(surface, 0, kCharmapLegendCount - 1)];
			texel[0] = c.r;
			texel[1] = c.g;
			texel[2] = c.b;
		}
		return;
	}
	case TerrainPlane::Foliage: {
		// A code, or a grey source's level (its red, green and blue alike, its alpha kept).
		const uint8_t code = uint8_t(brush.erase ? 0 : std::clamp(brush.foliage, 0, 255));
		for (int c = 0; c < std::min(bytes, 3); ++c) texel[c] = code;
		return;
	}
	case TerrainPlane::Colour:
	case TerrainPlane::Blend: {
		// Toward the target by the dab's weight and the brush's strength: the colour (its alpha kept), or the layer's
		// channel full and the others empty.
		uint8_t target[4] = {0, 0, 0, 0};
		const int channels = 3; // a blend map's alpha, which no splat reads, kept as it is
		if (plane == TerrainPlane::Colour) {
			if (brush.erase && saved) {
				target[0] = saved[0];
				target[1] = saved[1];
				target[2] = saved[2];
			} else {
				target[0] = uint8_t((brush.colour >> 16) & 0xFF);
				target[1] = uint8_t((brush.colour >> 8) & 0xFF);
				target[2] = uint8_t(brush.colour & 0xFF);
			}
		} else {
			target[brush.erase ? 0 : std::clamp(brush.layer, 0, 2)] = 255;
		}
		const double share = std::clamp(double(weight) * double(brush.strength), 0.0, 1.0);
		for (int c = 0; c < channels && c < bytes; ++c)
			texel[c] = uint8_t(std::lround(double(texel[c]) + (double(target[c]) - double(texel[c])) * share));
		return;
	}
	case TerrainPlane::Heights:
	case TerrainPlane::kCount: break;
	}
}

} // namespace

bool plan_paint_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error) {
	const std::string file = basename_of(context.file.path);
	auto *texture = dynamic_cast<TextureDocument *>(&context.target);
	if (!texture) {
		error = file + " is no texture: " + terrain_plane_words(context.plane) + " is painted in its image.";
		return false;
	}
	if (!texture->paintable()) {
		error = file + " is " + texture_unpaintable_words(texture->path(), texture->state()) +
		        ": no brush paints it. Save it as an 8-bit PCX (a surface or foliage map) or an uncompressed TGA (a "
		        "colour or blend map) to paint it.";
		return false;
	}
	if (!context.family.trn) {
		error = "The terrain's configuration did not read: the brush cannot place its texels.";
		return false;
	}
	const RasterPlane &texels = texture->plane(0);
	const bool source = !context.file.import_output.empty();
	std::string why;
	if (!form_fits(context.plane, texels, context.brush.layer, source, why)) {
		error = file + " cannot take this paint: " + why + ".";
		return false;
	}
	// An Erase of the colour map lays it as last saved: that state's texels, of this size and form.
	const RasterState &saved_state = texture->saved_state();
	const RasterPlane *saved = context.brush.erase && context.plane == TerrainPlane::Colour && !saved_state.planes.empty()
	                                   ? &saved_state.planes.front()
	                                   : nullptr;
	if (context.brush.erase && context.plane == TerrainPlane::Colour &&
	    (!saved || saved->width() != texels.width() || saved->height() != texels.height() ||
	     saved->format() != texels.format())) {
		error = file + " holds no saved colour map of its size and form to erase to (save it first).";
		return false;
	}
	if (context.plane == TerrainPlane::Surface && texels.format() != RasterFormat::U8 && !context.brush.erase &&
	    (context.brush.surface < 0 || context.brush.surface >= kCharmapLegendCount)) {
		error = file + " holds the legend's colours, its twenty classes: class " + std::to_string(context.brush.surface) +
		        " has no colour there (paint it into an indexed picture).";
		return false;
	}
	// A palette picture's index past its palette's entries is a file the import's reader refuses (formats/png
	// encode_png_indexed writes none).
	const TextureSide *side = texture_side(texture->state());
	if (side && side->form == TextureSide::Form::Png && side->png_colour == 3 &&
	    (context.plane == TerrainPlane::Surface || context.plane == TerrainPlane::Foliage)) {
		const int value = context.brush.erase ? (context.plane == TerrainPlane::Surface ? kDefaultClass : 0)
		                  : context.plane == TerrainPlane::Surface ? context.brush.surface
		                                                           : context.brush.foliage;
		if (value < 0 || uint32_t(value) >= side->png_entries) {
			error = file + " holds a palette of " + std::to_string(side->png_entries) + " entries: " +
			        std::to_string(value) + " is past it (add the entry to its palette in a paint program first).";
			return false;
		}
	}
	const TerrainMapTarget target = terrain_map_target(*context.family.trn, context.plane, texels.width(), texels.height());
	const bool categorical = terrain_plane_categorical(context.plane);
	const int bytes = texels.texel_bytes();
	// The texels reached, in the order first reached, and their values as laid (`bytes` each, in that order).
	std::vector<uint32_t> order;
	std::vector<uint8_t> values;
	bool filled = false;
	if (context.brush.fill && !context.points.empty()) {
		// Fill (S25 B4): the texels 4-connected to the one under the first point that hold its value exactly, each laid
		// at full weight, over the map's own texels (a texel the terrain draws at several places changes at each). The
		// press's alone: a gesture's later samples lay nothing, so a drag fills only where it was pressed.
		filled = true;
		if (context.first) {
			int sx = 0, sy = 0;
			if (!terrain_map_texel(target, context.points.front().first, context.points.front().second, sx, sy)) {
				error = "No texel of " + file + " lies under that point: the terrain's grid maps no sector there.";
				return false;
			}
			const int w = texels.width(), h = texels.height();
			const uint8_t *seed = texels.texel(sx, sy);
			const std::vector<uint8_t> value(seed, seed + bytes);
			std::vector<uint8_t> laid = value;
			lay(context.plane, context.brush, 1.0f, laid.data(), bytes);
			std::vector<uint8_t> seen(size_t(w) * size_t(h), 0);
			std::vector<uint32_t> stack{uint32_t(sy) * uint32_t(w) + uint32_t(sx)};
			seen[stack.back()] = 1;
			while (!stack.empty()) {
				const uint32_t index = stack.back();
				stack.pop_back();
				order.push_back(index);
				const int x = int(index % uint32_t(w)), y = int(index / uint32_t(w));
				const int next[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
				for (const auto &n : next) {
					if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
					const uint32_t at = uint32_t(n[1]) * uint32_t(w) + uint32_t(n[0]);
					if (seen[at] || std::memcmp(texels.texel(n[0], n[1]), value.data(), size_t(bytes)) != 0) continue;
					seen[at] = 1;
					stack.push_back(at);
				}
			}
			std::sort(order.begin(), order.end());
			values.reserve(order.size() * size_t(bytes));
			for (const uint32_t index : order) {
				if (saved) {
					// An Erase of the colour map: each texel toward its own colour as last saved.
					std::vector<uint8_t> own = value;
					lay(context.plane, context.brush, 1.0f, own.data(), bytes,
					    saved->texel(int(index % uint32_t(w)), int(index / uint32_t(w))));
					values.insert(values.end(), own.begin(), own.end());
				} else {
					values.insert(values.end(), laid.begin(), laid.end());
				}
			}
		}
	} else {
		// The dabs in order over the texels as they stand, each dab seeing the ones before it.
		std::unordered_map<uint32_t, size_t> slot_of;
		for (const auto &point : context.points)
			for (const TerrainMapTexel &texel : terrain_map_dab(target, point.first, point.second, context.brush, categorical)) {
				auto found = slot_of.find(texel.index);
				if (found == slot_of.end()) {
					const int x = int(texel.index % uint32_t(texels.width())), y = int(texel.index / uint32_t(texels.width()));
					const uint8_t *now = texels.texel(x, y);
					found = slot_of.emplace(texel.index, order.size()).first;
					order.push_back(texel.index);
					values.insert(values.end(), now, now + bytes);
				}
				const uint8_t *was = saved ? saved->texel(int(texel.index % uint32_t(texels.width())),
				                                          int(texel.index / uint32_t(texels.width())))
				                           : nullptr;
				lay(context.plane, context.brush, texel.weight, &values[found->second * size_t(bytes)], bytes, was);
			}
	}
	auto edit = std::make_shared<TexturePaintEdit>();
	edit->format = texels.format();
	edit->width = texels.width();
	edit->height = texels.height();
	int x0 = texels.width(), y0 = texels.height(), x1 = 0, y1 = 0;
	for (size_t i = 0; i < order.size(); ++i) {
		const uint32_t index = order[i];
		const int x = int(index % uint32_t(texels.width())), y = int(index / uint32_t(texels.width()));
		const uint8_t *value = &values[i * size_t(bytes)];
		if (std::memcmp(texels.texel(x, y), value, size_t(bytes)) == 0) continue;
		edit->at.push_back(index);
		edit->values.insert(edit->values.end(), value, value + bytes);
		x0 = std::min(x0, x);
		y0 = std::min(y0, y);
		x1 = std::max(x1, x + 1);
		y1 = std::max(y1, y + 1);
	}
	plan.words = std::string(context.brush.erase ? (filled ? "Filled with the default " : "Erased ")
	                                             : (filled ? "Filled " : "Painted ")) +
	             terrain_plane_words(context.plane) + " in " + file;
	if (filled) {
		// A map whose quadrants the grid draws at more than one place (or wraps) changes at each of them.
		int places[5] = {0, 0, 0, 0, 0};
		for (const int slot : target.grid)
			if (slot >= 1 && slot <= 4) ++places[slot];
		const bool repeats = target.wrap_x || target.wrap_z ||
		                     std::any_of(std::begin(places) + 1, std::end(places), [](int n) { return n > 1; });
		plan.words += ": " + std::to_string(order.size()) + " texel" + (order.size() == 1 ? "" : "s") +
		              (repeats ? ", at every place the terrain repeats them" : "");
	}
	if (edit->at.empty()) return true; // nothing the dabs reach changes
	plan.edits.push_back(RasterDocument::edit(edit, context.gesture));
	TerrainStrokeRegion region;
	region.path = context.file.path;
	region.plane = context.plane;
	region.x0 = x0;
	region.y0 = y0;
	region.x1 = x1;
	region.y1 = y1;
	plan.changed.push_back(region);
	return true;
}

} // namespace opennova::editor
