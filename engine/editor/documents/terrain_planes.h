#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::editor {

// The planes of a terrain a brush edits (ADR 0046 S25, the shared seams), each the file the game reads it from
// for a mission (session/terrain_family resolves which): the heights (the .cpt's CDEP atlas, or an imported
// terrain's heightmap image: what TrnGen's bake reads), the surface classes (the char map the .trn names, the
// game's footsteps and impacts), the foliage codes (the foliage map), the colour map and the detail blend map.
// The .til's tiles are records, never a plane (session/terrain_family's tiles file).
enum class TerrainPlane : uint8_t {
	Heights,
	Surface,
	Foliage,
	Colour,
	Blend,
	kCount,
};
inline constexpr size_t kTerrainPlaneCount = static_cast<size_t>(TerrainPlane::kCount);

// A plane's token on the wire ("heights", "surface", "foliage", "colour", "blend"), and the plane a token names.
inline const char *terrain_plane_token(TerrainPlane plane) {
	switch (plane) {
	case TerrainPlane::Heights: return "heights";
	case TerrainPlane::Surface: return "surface";
	case TerrainPlane::Foliage: return "foliage";
	case TerrainPlane::Colour: return "colour";
	case TerrainPlane::Blend: return "blend";
	case TerrainPlane::kCount: break;
	}
	return "";
}
inline bool terrain_plane_from_token(const std::string &token, TerrainPlane &out) {
	for (size_t i = 0; i < kTerrainPlaneCount; ++i)
		if (token == terrain_plane_token(static_cast<TerrainPlane>(i))) {
			out = static_cast<TerrainPlane>(i);
			return true;
		}
	return false;
}
// A plane in a modder's words ("the heights", "the surface map", ...), as the pane and the status line say it.
inline const char *terrain_plane_words(TerrainPlane plane) {
	switch (plane) {
	case TerrainPlane::Heights: return "the heights";
	case TerrainPlane::Surface: return "the surface map";
	case TerrainPlane::Foliage: return "the foliage map";
	case TerrainPlane::Colour: return "the colour map";
	case TerrainPlane::Blend: return "the detail blend map";
	case TerrainPlane::kCount: break;
	}
	return "";
}
// Whether a plane's values are categories (a surface class, a foliage code): a dab sets the texels whose centre
// lies inside its radius, hard-edged, never blending two classes (preview/terrain_brush).
inline bool terrain_plane_categorical(TerrainPlane plane) {
	return plane == TerrainPlane::Surface || plane == TerrainPlane::Foliage;
}

} // namespace opennova::editor
