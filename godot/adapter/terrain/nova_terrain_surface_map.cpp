#include "nova_terrain_surface_map.h"

#include "util/pcx_texture_bridge.h"

#include <algorithm>
#include <cstring>

using namespace godot;

void NovaTerrainSurfaceMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_width"), &NovaTerrainSurfaceMap::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &NovaTerrainSurfaceMap::get_height);
	ClassDB::bind_method(D_METHOD("get_index", "x", "y"), &NovaTerrainSurfaceMap::get_index);
	ClassDB::bind_method(D_METHOD("paint_circle", "center_x", "center_y", "radius", "hardness", "strength", "index"),
	                     &NovaTerrainSurfaceMap::paint_circle);
	ClassDB::bind_method(D_METHOD("get_palette_bytes"), &NovaTerrainSurfaceMap::get_palette_bytes);
	ClassDB::bind_method(D_METHOD("get_preview_texture"), &NovaTerrainSurfaceMap::get_preview_texture);
	ClassDB::bind_method(D_METHOD("map_x_from_heightmap_x", "heightmap_x"), &NovaTerrainSurfaceMap::map_x_from_heightmap_x);
	ClassDB::bind_method(D_METHOD("map_y_from_heightmap_y", "heightmap_y"), &NovaTerrainSurfaceMap::map_y_from_heightmap_y);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &NovaTerrainSurfaceMap::to_dictionary);
	ClassDB::bind_method(D_METHOD("load_from_dictionary", "state"), &NovaTerrainSurfaceMap::load_from_dictionary);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "width"), "", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height"), "", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "preview_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"),
	             "",
	             "get_preview_texture");

	BIND_CONSTANT(HEIGHTMAP_SIZE);
}

void NovaTerrainSurfaceMap::_refresh_preview_texture() {
	if (!opennova::foliage_has_size(surface_map)) {
		preview_texture.unref();
		return;
	}
	preview_texture = opennova::build_indexed_texture(surface_map.indices, surface_map.palette, surface_map.width, surface_map.height);
}

int NovaTerrainSurfaceMap::get_width() const {
	return surface_map.width;
}

int NovaTerrainSurfaceMap::get_height() const {
	return surface_map.height;
}

uint8_t NovaTerrainSurfaceMap::get_index(int x, int y) const {
	return opennova::foliage_get_index(surface_map, x, y);
}

bool NovaTerrainSurfaceMap::paint_circle(int center_x, int center_y, int radius, double hardness, double strength, int index) {
	const bool changed = opennova::foliage_paint_circle(surface_map,
	                                                     center_x,
	                                                     center_y,
	                                                     radius,
	                                                     static_cast<float>(hardness),
	                                                     static_cast<float>(strength),
	                                                     static_cast<uint8_t>(std::clamp(index, 0, 255)));
	if (changed) {
		_refresh_preview_texture();
		emit_changed();
	}
	return changed;
}

PackedByteArray NovaTerrainSurfaceMap::get_palette_bytes() const {
	PackedByteArray out;
	out.resize(256 * 3);
	std::memcpy(out.ptrw(), surface_map.palette, sizeof(surface_map.palette));
	return out;
}

Ref<Texture2D> NovaTerrainSurfaceMap::get_preview_texture() const {
	return preview_texture;
}

int NovaTerrainSurfaceMap::map_x_from_heightmap_x(double hm_x) const {
	return opennova::foliage_map_x_from_heightmap_x(static_cast<float>(hm_x), surface_map.width);
}

int NovaTerrainSurfaceMap::map_y_from_heightmap_y(double hm_y) const {
	return opennova::foliage_map_y_from_heightmap_y(static_cast<float>(hm_y), surface_map.height);
}

Dictionary NovaTerrainSurfaceMap::to_dictionary() const {
	Dictionary out;
	out["width"] = surface_map.width;
	out["height"] = surface_map.height;

	PackedByteArray indices;
	indices.resize(static_cast<int64_t>(surface_map.indices.size()));
	if (!surface_map.indices.empty()) {
		std::memcpy(indices.ptrw(), surface_map.indices.data(), surface_map.indices.size());
	}
	out["indices"] = indices;
	out["palette"] = get_palette_bytes();
	return out;
}

void NovaTerrainSurfaceMap::load_from_dictionary(const Dictionary &dict) {
	const int width = std::max(static_cast<int>(dict.get("width", 0)), 0);
	const int height = std::max(static_cast<int>(dict.get("height", 0)), 0);
	const PackedByteArray indices = dict.get("indices", PackedByteArray());
	const int expected = width * height;
	if (expected <= 0 || indices.size() < expected) {
		surface_map = opennova::FoliageMap{};
		_refresh_preview_texture();
		emit_changed();
		return;
	}

	surface_map = opennova::foliage_make_default_map(width, height, 0);
	surface_map.indices.resize(static_cast<size_t>(expected));
	std::memcpy(surface_map.indices.data(), indices.ptr(), static_cast<size_t>(expected));

	const PackedByteArray palette = dict.get("palette", PackedByteArray());
	if (palette.size() >= 256 * 3) {
		std::memcpy(surface_map.palette, palette.ptr(), sizeof(surface_map.palette));
	}

	_refresh_preview_texture();
	emit_changed();
}
