// A terrain's char map and foliage map made from a modder's picture (terrain_map_source.h). Tooling, not a port,
// but for the rules the maps are made to (each cited where it binds).
#include <runtime/terrain/terrain_map_source.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include <base/io/strutil.h>
#include <formats/png/png_decode.h>
#include <formats/trn/charmap_legend.h>
#include <runtime/renderer/texture_authoring.h>
#include <runtime/terrain_query/surface_type_map.h>

namespace opennova::terrain {

namespace {

// The texels as an image program shows them, an indexed image's indices beside its colours.
bool read_picture(const std::string &name, const std::vector<uint8_t> &bytes, RgbaImage &colours,
                  IndexedImage8 &indices, std::string &why) {
	if (strutil::ends_with_icase(name, ".png")) {
		if (!png::decode_png(bytes, colours, why, &indices)) {
			why = name + ": " + why;
			return false;
		}
		return true;
	}
	renderer::ImageSource source;
	if (!renderer::decode_image_source(name, bytes, source, why)) {
		why = name + ": " + why;
		return false;
	}
	colours = std::move(source.image);
	if (source.indexed) indices = std::move(source.indices);
	return true;
}

} // namespace

bool decode_charmap_source(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                           std::string &why) {
	out = IndexedImage8();
	RgbaImage colours;
	IndexedImage8 indices;
	if (!read_picture(name, bytes, colours, indices, why)) return false;
	// The game keeps the side alone, the rows' length and the power of two it samples the 1024-unit
	// heightmap by, so the map is square and a power of two no wider than the heightmap [orig: sub_605A10 @
	// 0x605A82..0x605AA0; Terrain_GetSurfaceTypeAtPosition @ 0x6065C6].
	const int w = colours.width, h = colours.height;
	if (w != h || w < kCharmapSourceSideMin || surface_sample_extent(w) != w) {
		why = name + " is " + std::to_string(w) + " x " + std::to_string(h) +
		      ": a surface map is square, 256, 512 or 1024 texels a side, laid over the heightmap";
		return false;
	}
	const bool indexed = !indices.empty();
	const size_t texels = size_t(w) * size_t(h);
	out.width = w;
	out.height = h;
	out.indices.assign(texels, 0);
	// The legend its palette, every entry past it white as the shipped legend leaves its own.
	for (int i = 0; i < 256; ++i) {
		const CharmapLegendColour &c = i < kCharmapLegendCount ? kCharmapLegend[i] : kCharmapLegendRest;
		out.palette[i][0] = c.r;
		out.palette[i][1] = c.g;
		out.palette[i][2] = c.b;
	}
	size_t strays = 0, first = 0;
	for (size_t i = 0; i < texels; ++i) {
		const uint8_t *p = &colours.pixels[i * 4];
		const int surface = indexed ? int(indices.indices[i]) : charmap_legend_class(p[0], p[1], p[2]);
		if (surface < 0 || surface >= kCharmapLegendCount) {
			if (strays++ == 0) first = i;
			continue;
		}
		out.indices[i] = uint8_t(surface);
	}
	if (strays == 0) return true;
	const std::string at = "(" + std::to_string(first % size_t(w)) + ", " + std::to_string(first / size_t(w)) + ")";
	const std::string more = strays > 1 ? " (" + std::to_string(strays) + " texels in all)" : std::string();
	if (indexed) {
		why = name + "'s texel " + at + " holds index " + std::to_string(indices.indices[first]) + more +
		      ": an indexed surface map's indices are the surface classes, 0 to 19";
	} else {
		const uint8_t *p = &colours.pixels[first * 4];
		char colour[8];
		std::snprintf(colour, sizeof(colour), "#%02X%02X%02X", p[0], p[1], p[2]);
		why = name + "'s texel " + at + " is " + colour + more +
		      ", the colour of no surface class: a colour surface map paints each class in its legend colour exactly";
	}
	out = IndexedImage8();
	return false;
}

bool decode_foliage_map_source(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                               std::string &why) {
	out = IndexedImage8();
	RgbaImage colours;
	IndexedImage8 indices;
	if (!read_picture(name, bytes, colours, indices, why)) return false;
	// The game keeps the width alone, the rows' length and the power of two it shifts the atlas position by
	// [orig: Foliage_LoadFoliageMapPCX @ 0x605B44..0x605B60; Foliage_SampleFoliageMapMask @ 0x60662D]: a
	// map shorter than that is read past its last row, one wider than 1024 at its first texel alone.
	const int w = colours.width, h = colours.height;
	if (w != h || surface_sample_extent(w) != w) {
		why = name + " is " + std::to_string(w) + " x " + std::to_string(h) +
		      ": a foliage map is square, a power of two at most 1024 texels a side, laid over the heightmap";
		return false;
	}
	const size_t texels = size_t(w) * size_t(h);
	out.width = w;
	out.height = h;
	if (!indices.empty()) {
		out.indices = std::move(indices.indices);
		std::memcpy(out.palette, indices.palette, sizeof(out.palette));
		return true;
	}
	// A grey image: its levels the codes, a grey ramp the palette.
	out.indices.assign(texels, 0);
	for (size_t i = 0; i < texels; ++i) {
		const uint8_t *p = &colours.pixels[i * 4];
		if (p[0] != p[1] || p[1] != p[2]) {
			char colour[8];
			std::snprintf(colour, sizeof(colour), "#%02X%02X%02X", p[0], p[1], p[2]);
			why = name + "'s texel (" + std::to_string(i % size_t(w)) + ", " + std::to_string(i / size_t(w)) + ") is " +
			      colour + ": a foliage map's texels are codes, an indexed image's indices or a grey image's levels";
			out = IndexedImage8();
			return false;
		}
		out.indices[i] = p[0];
	}
	for (int i = 0; i < 256; ++i) out.palette[i][0] = out.palette[i][1] = out.palette[i][2] = uint8_t(i);
	return true;
}

} // namespace opennova::terrain
