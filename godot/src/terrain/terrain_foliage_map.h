#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <formats/foliage/foliage.h>

namespace godot {

class TerrainFoliageMap : public Resource {
	GDCLASS(TerrainFoliageMap, Resource)

private:
	opennova::FoliageMap foliage_map;

protected:
	static void _bind_methods();

public:
	int get_width() const;
	int get_height() const;
	uint8_t get_index(int x, int y) const;

	int map_x_from_heightmap_x(double hm_x) const;
	int map_y_from_heightmap_y(double hm_y) const;
	uint8_t sample_detail_flat_wrap(int32_t world_x_fixed, int32_t world_z_fixed) const;
	int sample_detail_index_world(double world_x, double world_z) const;

	void copy_from_native(const opennova::FoliageMap &map);
};

} // namespace godot
