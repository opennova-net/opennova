#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <formats/til/til.h>

namespace godot {

class TerrainTileEntry;

class TerrainTileInfo : public Resource {
	GDCLASS(TerrainTileInfo, Resource)

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

	Error load_from_bytes(const PackedByteArray &p_bytes);
	// The .til disk pair (ADR 0032 direct document I/O, replacing the deleted
	// ResourceFormat pair): load_from_path runs the generic payload decode
	// before the parse; save_to_path writes engine save_til over to_native().
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;
	// Coordinates are on the decoded terrain/Godot X,Z plane. The format helper
	// has already applied the stored-negated z_fixed decode; never mirror Z again.
	bool blocks_foliage(float world_x, float world_z, float radius) const;
	int get_entry_count() const;
	Array get_entries() const;
	void set_entries(const Array &p_entries);
	Ref<TerrainTileEntry> get_entry(int index) const;
	void add_entry(const Ref<TerrainTileEntry> &entry);
	void remove_entry(int index);
	void clear_entries();

	void copy_from_native(const opennova::TilFile &file);
	opennova::TilFile to_native() const;
};

} // namespace godot
