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
	// The detail gate's flat 1024 wrap at a source-atlas position.
	uint8_t sample_detail_flat_wrap(int32_t atlas_x_fixed, int32_t atlas_z_fixed) const;

	void copy_from_native(const opennova::FoliageMap &map);
	// The map as the engine's samplers read it (terrain_query/foliage_mask_raster.h
	// foliage_code_map).
	const opennova::FoliageMap &native() const { return foliage_map; }
	// The codes of a rect of the map written in place (`codes` w x h, row-major, the top row first): an editor's paint
	// (ADR 0046 S25). False, nothing written, for a rect past the map.
	bool write_rect(int x, int y, int w, int h, const uint8_t *codes);
};

} // namespace godot
