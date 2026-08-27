#include "nova_terrain_foliage_map.h"

#include <cmath>
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

void TerrainFoliageMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_width"), &TerrainFoliageMap::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &TerrainFoliageMap::get_height);
	ClassDB::bind_method(D_METHOD("get_index", "x", "y"), &TerrainFoliageMap::get_index);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "width"), "", "get_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height"), "", "get_height");
}

int TerrainFoliageMap::get_width() const {
	return foliage_map.width;
}

int TerrainFoliageMap::get_height() const {
	return foliage_map.height;
}

uint8_t TerrainFoliageMap::get_index(int x, int y) const {
	return opennova::foliage_get_index(foliage_map, x, y);
}

int TerrainFoliageMap::map_x_from_heightmap_x(double hm_x) const {
	return opennova::foliage_map_x_from_heightmap_x(static_cast<float>(hm_x), foliage_map.width);
}

int TerrainFoliageMap::map_y_from_heightmap_y(double hm_y) const {
	return opennova::foliage_map_y_from_heightmap_y(static_cast<float>(hm_y), foliage_map.height);
}

uint8_t TerrainFoliageMap::sample_detail_flat_wrap(int32_t world_x_fixed,
                                                       int32_t world_z_fixed) const {
	return opennova::foliage_sample_detail_flat_wrap(
			foliage_map, world_x_fixed, world_z_fixed);
}

int TerrainFoliageMap::sample_detail_index_world(double world_x,
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

void TerrainFoliageMap::copy_from_native(const opennova::FoliageMap &map) {
	foliage_map = ensure_valid_map(map);
}
