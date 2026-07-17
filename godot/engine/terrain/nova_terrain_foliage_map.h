#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <foliage/foliage.h>

namespace godot {

class NovaTerrainFoliageMap : public Resource {
	GDCLASS(NovaTerrainFoliageMap, Resource)

private:
	opennova::FoliageMap foliage_map;
	Ref<Texture2D> preview_texture;

	void _refresh_preview_texture();

protected:
	static void _bind_methods();

public:
	enum {
		HEIGHTMAP_SIZE = opennova::FOLIAGE_HEIGHTMAP_SIZE,
	};

	void set_size(int width, int height);
	int get_width() const;
	int get_height() const;

	void clear(int fill_index = 0);

	uint8_t get_index(int x, int y) const;
	void set_index(int x, int y, int index);
	bool paint_circle(int center_x, int center_y, int radius, double hardness, double strength, int index);
	bool paint_detail_circle_wrap(int center_x, int center_y, int radius,
	                              double hardness, double strength, int index);
	int count_index(int index) const;
	int remap_index(int from_index, int to_index);
	int remap_indices(const Dictionary &from_to);
	int clear_index(int index);

	PackedByteArray get_indices() const;
	void set_indices(const PackedByteArray &data);

	PackedByteArray get_palette_bytes() const;
	void set_palette_bytes(const PackedByteArray &data);

	Ref<Texture2D> get_preview_texture() const;

	int map_x_from_heightmap_x(double hm_x) const;
	int map_y_from_heightmap_y(double hm_y) const;
	uint8_t sample_detail_flat_wrap(int32_t world_x_fixed, int32_t world_z_fixed) const;
	int sample_detail_index_world(double world_x, double world_z) const;
	Vector2i get_detail_map_position_world(double world_x, double world_z) const;
	int get_detail_sample_resolution() const;
	double heightmap_x_from_map_x(int map_x) const;
	double heightmap_y_from_map_y(int map_y) const;
	int get_sector_id_at(int map_x, int map_y) const;
	Vector2 get_heightmap_position(int map_x, int map_y) const;

	Dictionary to_dictionary() const;
	void load_from_dictionary(const Dictionary &dict);

	void copy_from_native(const opennova::FoliageMap &map);
	opennova::FoliageMap to_native() const;
};

} // namespace godot
