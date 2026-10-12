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

namespace opennova::editor {

namespace {

// The texture form a plane's map is read in, in a modder's words, where its plane's texels are not that form.
bool form_fits(TerrainPlane plane, const RasterPlane &texels, int layer, std::string &why) {
	switch (plane) {
	case TerrainPlane::Surface:
	case TerrainPlane::Foliage:
		if (texels.format() == RasterFormat::U8) return true;
		why = std::string("the game reads ") + terrain_plane_words(plane) + " as an 8-bit PCX of indices";
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
		if (layer < 0 || layer >= texels.texel_bytes()) {
			why = "this blend map holds " + std::to_string(texels.texel_bytes()) + " layers (0 to " +
			      std::to_string(texels.texel_bytes() - 1) + ")";
			return false;
		}
		return true;
	case TerrainPlane::Heights:
	case TerrainPlane::kCount: break;
	}
	why = "it is no map the Paint tools paint";
	return false;
}

// The value a dab of weight `weight` leaves on a texel holding `texel` (bytes of the plane's format).
void lay(TerrainPlane plane, const TerrainBrush &brush, float weight, uint8_t *texel, int bytes) {
	switch (plane) {
	case TerrainPlane::Surface: texel[0] = uint8_t(std::clamp(brush.surface, 0, 255)); return;
	case TerrainPlane::Foliage: texel[0] = uint8_t(std::clamp(brush.foliage, 0, 255)); return;
	case TerrainPlane::Colour:
	case TerrainPlane::Blend: {
		// Toward the target by the dab's weight and the brush's strength: the colour (its alpha kept), or the layer's
		// channel full and the others empty.
		uint8_t target[4] = {0, 0, 0, 0};
		int channels = 3;
		if (plane == TerrainPlane::Colour) {
			target[0] = uint8_t((brush.colour >> 16) & 0xFF);
			target[1] = uint8_t((brush.colour >> 8) & 0xFF);
			target[2] = uint8_t(brush.colour & 0xFF);
		} else {
			channels = bytes;
			target[std::clamp(brush.layer, 0, 3)] = 255;
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
	std::string why;
	if (!form_fits(context.plane, texels, context.brush.layer, why)) {
		error = file + " cannot take this paint: " + why + ".";
		return false;
	}
	const TerrainMapTarget target = terrain_map_target(*context.family.trn, context.plane, texels.width(), texels.height());
	const bool categorical = terrain_plane_categorical(context.plane);
	const int bytes = texels.texel_bytes();
	// The dabs in order over the texels as they stand, each dab seeing the ones before it.
	std::unordered_map<uint32_t, std::vector<uint8_t>> painted;
	std::vector<uint32_t> order;
	for (const auto &point : context.points)
		for (const TerrainMapTexel &texel : terrain_map_dab(target, point.first, point.second, context.brush, categorical)) {
			auto found = painted.find(texel.index);
			if (found == painted.end()) {
				const int x = int(texel.index % uint32_t(texels.width())), y = int(texel.index / uint32_t(texels.width()));
				const uint8_t *now = texels.texel(x, y);
				found = painted.emplace(texel.index, std::vector<uint8_t>(now, now + bytes)).first;
				order.push_back(texel.index);
			}
			lay(context.plane, context.brush, texel.weight, found->second.data(), bytes);
		}
	auto edit = std::make_shared<TexturePaintEdit>();
	edit->format = texels.format();
	edit->width = texels.width();
	edit->height = texels.height();
	int x0 = texels.width(), y0 = texels.height(), x1 = 0, y1 = 0;
	for (const uint32_t index : order) {
		const int x = int(index % uint32_t(texels.width())), y = int(index / uint32_t(texels.width()));
		const std::vector<uint8_t> &value = painted[index];
		if (std::memcmp(texels.texel(x, y), value.data(), size_t(bytes)) == 0) continue;
		edit->at.push_back(index);
		edit->values.insert(edit->values.end(), value.begin(), value.end());
		x0 = std::min(x0, x);
		y0 = std::min(y0, y);
		x1 = std::max(x1, x + 1);
		y1 = std::max(y1, y + 1);
	}
	plan.words = "Painted " + std::string(terrain_plane_words(context.plane)) + " in " + file;
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
