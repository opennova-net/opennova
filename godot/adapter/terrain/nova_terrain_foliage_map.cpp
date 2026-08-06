#include "nova_terrain_foliage_map.h"

#include "util/pcx_texture_bridge.h"

#include <godot_cpp/variant/array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace godot;

namespace {

static opennova::FoliageMap ensure_valid_map(const opennova::FoliageMap &map) {
	if (opennova::foliage_has_size(map)) {
		return map;
	}
	return opennova::foliage_make_default_map(opennova::FOLIAGE_HEIGHTMAP_SIZE, opennova::FOLIAGE_HEIGHTMAP_SIZE, 0);
}

static bool world_position_to_fixed(double world_x, double world_z,
                                    int32_t &world_x_fixed,
                                    int32_t &world_z_fixed) {
	if (!std::isfinite(world_x) || !std::isfinite(world_z)) {
		return false;
	}
	const double x_scaled = std::trunc(world_x * 65536.0);
	const double z_scaled = std::trunc(world_z * 65536.0);
	const double fixed_min =
			static_cast<double>(std::numeric_limits<int32_t>::min());
	const double fixed_max =
			static_cast<double>(std::numeric_limits<int32_t>::max());
	if (x_scaled < fixed_min || x_scaled > fixed_max ||
	    z_scaled < fixed_min || z_scaled > fixed_max) {
		return false;
	}
	world_x_fixed = static_cast<int32_t>(x_scaled);
	world_z_fixed = static_cast<int32_t>(z_scaled);
	return true;
}

} // namespace

void NovaTerrainFoliageMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "width", "height"), &NovaTerrainFoliageMap::set_size);
	ClassDB::bind_method(D_METHOD("get_width"), &NovaTerrainFoliageMap::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &NovaTerrainFoliageMap::get_height);
	ClassDB::bind_method(D_METHOD("clear", "fill_index"), &NovaTerrainFoliageMap::clear, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_index", "x", "y"), &NovaTerrainFoliageMap::get_index);
	ClassDB::bind_method(D_METHOD("set_index", "x", "y", "index"), &NovaTerrainFoliageMap::set_index);
	ClassDB::bind_method(D_METHOD("paint_circle", "center_x", "center_y", "radius", "hardness", "strength", "index"),
	                     &NovaTerrainFoliageMap::paint_circle);
	ClassDB::bind_method(D_METHOD("paint_detail_circle_wrap", "center_x", "center_y", "radius", "hardness", "strength", "index"),
	                     &NovaTerrainFoliageMap::paint_detail_circle_wrap);
	ClassDB::bind_method(D_METHOD("count_index", "index"), &NovaTerrainFoliageMap::count_index);
	ClassDB::bind_method(D_METHOD("remap_index", "from_index", "to_index"), &NovaTerrainFoliageMap::remap_index);
	ClassDB::bind_method(D_METHOD("remap_indices", "from_to"), &NovaTerrainFoliageMap::remap_indices);
	ClassDB::bind_method(D_METHOD("clear_index", "index"), &NovaTerrainFoliageMap::clear_index);
	ClassDB::bind_method(D_METHOD("get_indices"), &NovaTerrainFoliageMap::get_indices);
	ClassDB::bind_method(D_METHOD("set_indices", "data"), &NovaTerrainFoliageMap::set_indices);
	ClassDB::bind_method(D_METHOD("get_palette_bytes"), &NovaTerrainFoliageMap::get_palette_bytes);
	ClassDB::bind_method(D_METHOD("set_palette_bytes", "data"), &NovaTerrainFoliageMap::set_palette_bytes);
	ClassDB::bind_method(D_METHOD("get_preview_texture"), &NovaTerrainFoliageMap::get_preview_texture);
	ClassDB::bind_method(D_METHOD("map_x_from_heightmap_x", "heightmap_x"), &NovaTerrainFoliageMap::map_x_from_heightmap_x);
	ClassDB::bind_method(D_METHOD("map_y_from_heightmap_y", "heightmap_y"), &NovaTerrainFoliageMap::map_y_from_heightmap_y);
	ClassDB::bind_method(D_METHOD("sample_detail_index_world", "world_x", "world_z"),
	                     &NovaTerrainFoliageMap::sample_detail_index_world);
	ClassDB::bind_method(D_METHOD("get_detail_map_position_world", "world_x", "world_z"),
	                     &NovaTerrainFoliageMap::get_detail_map_position_world);
	ClassDB::bind_method(D_METHOD("get_detail_sample_resolution"),
	                     &NovaTerrainFoliageMap::get_detail_sample_resolution);
	ClassDB::bind_method(D_METHOD("heightmap_x_from_map_x", "map_x"), &NovaTerrainFoliageMap::heightmap_x_from_map_x);
	ClassDB::bind_method(D_METHOD("heightmap_y_from_map_y", "map_y"), &NovaTerrainFoliageMap::heightmap_y_from_map_y);
	ClassDB::bind_method(D_METHOD("get_sector_id_at", "map_x", "map_y"), &NovaTerrainFoliageMap::get_sector_id_at);
	ClassDB::bind_method(D_METHOD("get_heightmap_position", "map_x", "map_y"), &NovaTerrainFoliageMap::get_heightmap_position);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &NovaTerrainFoliageMap::to_dictionary);
	ClassDB::bind_method(D_METHOD("load_from_dictionary", "state"), &NovaTerrainFoliageMap::load_from_dictionary);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "width"), "", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height"), "", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "indices"), "set_indices", "get_indices");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "palette_bytes"), "set_palette_bytes", "get_palette_bytes");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "preview_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"),
	             "",
	             "get_preview_texture");

	BIND_CONSTANT(HEIGHTMAP_SIZE);
}

void NovaTerrainFoliageMap::_refresh_preview_texture() {
	foliage_map = ensure_valid_map(foliage_map);
	preview_texture = opennova::build_indexed_texture(foliage_map.indices, foliage_map.palette, foliage_map.width, foliage_map.height);
}

void NovaTerrainFoliageMap::set_size(int width, int height) {
	foliage_map = opennova::foliage_make_default_map(width, height, 0);
	_refresh_preview_texture();
	emit_changed();
}

int NovaTerrainFoliageMap::get_width() const {
	return foliage_map.width;
}

int NovaTerrainFoliageMap::get_height() const {
	return foliage_map.height;
}

void NovaTerrainFoliageMap::clear(int fill_index) {
	foliage_map = opennova::foliage_make_default_map(
			std::max(foliage_map.width, 1),
			std::max(foliage_map.height, 1),
			static_cast<uint8_t>(std::clamp(fill_index, 0, 255)));
	_refresh_preview_texture();
	emit_changed();
}

uint8_t NovaTerrainFoliageMap::get_index(int x, int y) const {
	return opennova::foliage_get_index(foliage_map, x, y);
}

void NovaTerrainFoliageMap::set_index(int x, int y, int index) {
	if (!opennova::foliage_set_index(foliage_map, x, y, static_cast<uint8_t>(std::clamp(index, 0, 255)))) {
		return;
	}
	_refresh_preview_texture();
	emit_changed();
}

bool NovaTerrainFoliageMap::paint_circle(int center_x, int center_y, int radius, double hardness, double strength, int index) {
	const bool changed = opennova::foliage_paint_circle(foliage_map,
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

bool NovaTerrainFoliageMap::paint_detail_circle_wrap(
		int center_x, int center_y, int radius, double hardness,
		double strength, int index) {
	const bool changed = opennova::foliage_paint_detail_circle_wrap(
			foliage_map,
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

int NovaTerrainFoliageMap::count_index(int index) const {
	return opennova::foliage_count_index(foliage_map, static_cast<uint8_t>(std::clamp(index, 0, 255)));
}

int NovaTerrainFoliageMap::remap_index(int from_index, int to_index) {
	const int changed = opennova::foliage_remap_index(
			foliage_map,
			static_cast<uint8_t>(std::clamp(from_index, 0, 255)),
			static_cast<uint8_t>(std::clamp(to_index, 0, 255)));
	if (changed > 0) {
		_refresh_preview_texture();
		emit_changed();
	}
	return changed;
}

int NovaTerrainFoliageMap::remap_indices(const Dictionary &from_to) {
	std::array<uint8_t, 256> lut;
	for (int i = 0; i < 256; ++i) {
		lut[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
	}
	const Array keys = from_to.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const Variant key = keys[i];
		const int from = std::clamp(static_cast<int>(key), 0, 255);
		const int to = std::clamp(static_cast<int>(from_to[key]), 0, 255);
		lut[static_cast<size_t>(from)] = static_cast<uint8_t>(to);
	}
	const int changed = opennova::foliage_remap_indices(foliage_map, lut);
	if (changed > 0) {
		_refresh_preview_texture();
		emit_changed();
	}
	return changed;
}

int NovaTerrainFoliageMap::clear_index(int index) {
	return remap_index(index, 0);
}

PackedByteArray NovaTerrainFoliageMap::get_indices() const {
	PackedByteArray out;
	out.resize(static_cast<int64_t>(foliage_map.indices.size()));
	if (!foliage_map.indices.empty()) {
		std::memcpy(out.ptrw(), foliage_map.indices.data(), foliage_map.indices.size());
	}
	return out;
}

void NovaTerrainFoliageMap::set_indices(const PackedByteArray &data) {
	foliage_map = ensure_valid_map(foliage_map);
	const int expected = foliage_map.width * foliage_map.height;
	if (expected <= 0 || data.size() < expected) {
		return;
	}
	foliage_map.indices.resize(static_cast<size_t>(expected));
	std::memcpy(foliage_map.indices.data(), data.ptr(), static_cast<size_t>(expected));
	_refresh_preview_texture();
	emit_changed();
}

PackedByteArray NovaTerrainFoliageMap::get_palette_bytes() const {
	PackedByteArray out;
	out.resize(256 * 3);
	std::memcpy(out.ptrw(), foliage_map.palette, sizeof(foliage_map.palette));
	return out;
}

void NovaTerrainFoliageMap::set_palette_bytes(const PackedByteArray &data) {
	if (data.size() < 256 * 3) {
		return;
	}
	foliage_map = ensure_valid_map(foliage_map);
	std::memcpy(foliage_map.palette, data.ptr(), sizeof(foliage_map.palette));
	_refresh_preview_texture();
	emit_changed();
}

Ref<Texture2D> NovaTerrainFoliageMap::get_preview_texture() const {
	return preview_texture;
}

int NovaTerrainFoliageMap::map_x_from_heightmap_x(double hm_x) const {
	return opennova::foliage_map_x_from_heightmap_x(static_cast<float>(hm_x), foliage_map.width);
}

int NovaTerrainFoliageMap::map_y_from_heightmap_y(double hm_y) const {
	return opennova::foliage_map_y_from_heightmap_y(static_cast<float>(hm_y), foliage_map.height);
}

uint8_t NovaTerrainFoliageMap::sample_detail_flat_wrap(int32_t world_x_fixed,
                                                       int32_t world_z_fixed) const {
	return opennova::foliage_sample_detail_flat_wrap(
			foliage_map, world_x_fixed, world_z_fixed);
}

int NovaTerrainFoliageMap::sample_detail_index_world(double world_x,
                                                     double world_z) const {
	int32_t world_x_fixed = 0;
	int32_t world_z_fixed = 0;
	if (!world_position_to_fixed(
	        world_x, world_z, world_x_fixed, world_z_fixed)) {
		return 0;
	}
	return static_cast<int>(sample_detail_flat_wrap(
			world_x_fixed, world_z_fixed));
}

Vector2i NovaTerrainFoliageMap::get_detail_map_position_world(
		double world_x, double world_z) const {
	int32_t world_x_fixed = 0;
	int32_t world_z_fixed = 0;
	if (!world_position_to_fixed(
	        world_x, world_z, world_x_fixed, world_z_fixed)) {
		return Vector2i(-1, -1);
	}
	int map_x = -1;
	int map_y = -1;
	if (!opennova::foliage_detail_flat_wrap_position(
	        foliage_map, world_x_fixed, world_z_fixed, map_x, map_y)) {
		return Vector2i(-1, -1);
	}
	return Vector2i(map_x, map_y);
}

int NovaTerrainFoliageMap::get_detail_sample_resolution() const {
	return opennova::foliage_detail_sample_resolution(foliage_map);
}

double NovaTerrainFoliageMap::heightmap_x_from_map_x(int map_x) const {
	return static_cast<double>(opennova::foliage_heightmap_x_from_map_x(map_x, foliage_map.width));
}

double NovaTerrainFoliageMap::heightmap_y_from_map_y(int map_y) const {
	return static_cast<double>(opennova::foliage_heightmap_y_from_map_y(map_y, foliage_map.height));
}

int NovaTerrainFoliageMap::get_sector_id_at(int map_x, int map_y) const {
	if (!opennova::foliage_has_size(foliage_map)) {
		return 0;
	}
	const double step_x = static_cast<double>(HEIGHTMAP_SIZE) / static_cast<double>(std::max(foliage_map.width, 1));
	const double step_y = static_cast<double>(HEIGHTMAP_SIZE) / static_cast<double>(std::max(foliage_map.height, 1));
	const float hm_x = static_cast<float>(heightmap_x_from_map_x(map_x) + step_x * 0.5);
	const float hm_y = static_cast<float>(heightmap_y_from_map_y(map_y) + step_y * 0.5);
	return opennova::foliage_sector_id_from_heightmap(hm_x, hm_y);
}

Vector2 NovaTerrainFoliageMap::get_heightmap_position(int map_x, int map_y) const {
	if (!opennova::foliage_has_size(foliage_map)) {
		return Vector2();
	}
	const double step_x = static_cast<double>(HEIGHTMAP_SIZE) / static_cast<double>(std::max(foliage_map.width, 1));
	const double step_y = static_cast<double>(HEIGHTMAP_SIZE) / static_cast<double>(std::max(foliage_map.height, 1));
	return Vector2(
			static_cast<float>(heightmap_x_from_map_x(map_x) + step_x * 0.5),
			static_cast<float>(heightmap_y_from_map_y(map_y) + step_y * 0.5));
}

Dictionary NovaTerrainFoliageMap::to_dictionary() const {
	Dictionary out;
	out["width"] = foliage_map.width;
	out["height"] = foliage_map.height;
	out["indices"] = get_indices();
	out["palette"] = get_palette_bytes();
	return out;
}

void NovaTerrainFoliageMap::load_from_dictionary(const Dictionary &dict) {
	const int width = std::max(static_cast<int>(dict.get("width", opennova::FOLIAGE_HEIGHTMAP_SIZE)), 1);
	const int height = std::max(static_cast<int>(dict.get("height", opennova::FOLIAGE_HEIGHTMAP_SIZE)), 1);
	foliage_map = opennova::foliage_make_default_map(width, height, 0);

	const PackedByteArray indices = dict.get("indices", PackedByteArray());
	const int expected = width * height;
	if (indices.size() >= expected) {
		foliage_map.indices.resize(static_cast<size_t>(expected));
		std::memcpy(foliage_map.indices.data(), indices.ptr(), static_cast<size_t>(expected));
	}

	const PackedByteArray palette = dict.get("palette", PackedByteArray());
	if (palette.size() >= 256 * 3) {
		std::memcpy(foliage_map.palette, palette.ptr(), sizeof(foliage_map.palette));
	}

	_refresh_preview_texture();
	emit_changed();
}

void NovaTerrainFoliageMap::copy_from_native(const opennova::FoliageMap &map) {
	foliage_map = ensure_valid_map(map);
	_refresh_preview_texture();
	emit_changed();
}

opennova::FoliageMap NovaTerrainFoliageMap::to_native() const {
	return ensure_valid_map(foliage_map);
}
