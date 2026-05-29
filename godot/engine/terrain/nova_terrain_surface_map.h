#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <foliage/foliage.h>

namespace godot {

// A paintable 8-bit indexed surface-type (charmap) raster — the editor's
// working view of NovaTerrainData's "charmap" PCX slot. Mirrors
// NovaTerrainFoliageMap and deliberately shares the same generic index-grid
// kernel from libs/foliage (circle paint + heightmap<->map coordinate
// mapping): the charmap is the same shape as the foliage map, a palette-indexed
// grid aligned to the 1024-unit heightmap space, so the paint math lives in
// exactly one place. The persistent charmap bytes stay owned by
// NovaTerrainData; this resource is bridged via get/set_pcx_slot_state.
class NovaTerrainSurfaceMap : public Resource {
	GDCLASS(NovaTerrainSurfaceMap, Resource)

private:
	opennova::FoliageMap surface_map;
	Ref<Texture2D> preview_texture;

	void _refresh_preview_texture();

protected:
	static void _bind_methods();

public:
	enum {
		HEIGHTMAP_SIZE = opennova::FOLIAGE_HEIGHTMAP_SIZE,
	};

	int get_width() const;
	int get_height() const;

	uint8_t get_index(int x, int y) const;
	bool paint_circle(int center_x, int center_y, int radius, double hardness, double strength, int index);

	PackedByteArray get_palette_bytes() const;
	Ref<Texture2D> get_preview_texture() const;

	int map_x_from_heightmap_x(double hm_x) const;
	int map_y_from_heightmap_y(double hm_y) const;

	Dictionary to_dictionary() const;
	void load_from_dictionary(const Dictionary &dict);
};

} // namespace godot
