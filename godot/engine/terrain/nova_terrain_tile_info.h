#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <til/til.h>

namespace godot {

class NovaTerrainTileEntry;

class NovaTerrainTileInfo : public Resource {
	GDCLASS(NovaTerrainTileInfo, Resource)

private:
	opennova::TilFile til;

protected:
	static void _bind_methods();

public:
	enum {
		FLAG_FLIP_X = opennova::TIL_FLAG_FLIP_X,
		FLAG_FLIP_Y = opennova::TIL_FLAG_FLIP_Y,
		FLAG_ROTATE_90 = opennova::TIL_FLAG_ROTATE_90,
		FLAG_OUTLINE = opennova::TIL_FLAG_OUTLINE,
		ATLAS_TILE_PIXELS = opennova::TIL_ATLAS_TILE_PIXELS,
		CELL_WORLD_SIZE = opennova::TIL_CELL_WORLD_UNITS,
	};

	int get_entry_count() const;
	Array get_entries() const;
	void set_entries(const Array &p_entries);
	Ref<NovaTerrainTileEntry> get_entry(int index) const;
	void set_entry(int index, const Ref<NovaTerrainTileEntry> &entry);
	void add_entry(const Ref<NovaTerrainTileEntry> &entry);
	void remove_entry(int index);
	void clear_entries();
	int find_entry_index_at_cell(int cell_x, int cell_z) const;
	PackedInt32Array get_entry_indices_at_cell(int cell_x, int cell_z) const;

	void copy_from_native(const opennova::TilFile &file);
	opennova::TilFile to_native() const;
};

} // namespace godot
