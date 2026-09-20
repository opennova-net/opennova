#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <formats/til/til.h>

namespace godot {

class TerrainTileEntry : public RefCounted {
	GDCLASS(TerrainTileEntry, RefCounted)

private:
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
	int tile_index = 0;
	int flags = 0;

protected:
	static void _bind_methods();

public:
	void set_x_fixed(int value);
	int get_x_fixed() const;

	void set_z_fixed(int value);
	int get_z_fixed() const;

	void set_tile_index(int value);

	void set_flags(int value);
	int get_flags() const;

	void set_cell_x(int value);
	int get_cell_x() const;

	void set_cell_z(int value);
	int get_cell_z() const;

	void set_cell(int cell_x, int cell_z);


	void copy_from_native(const opennova::TilOverlayEntry &entry);
	opennova::TilOverlayEntry to_native() const;
};

} // namespace godot
