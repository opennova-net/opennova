#include "terrain/terrain_tile_entry.h"

#include <algorithm>

using namespace godot;

namespace {

template <typename T>
static T clamp_int(int value, int min_value, int max_value) {
	return static_cast<T>(std::clamp(value, min_value, max_value));
}

} // namespace

void TerrainTileEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_x_fixed", "value"), &TerrainTileEntry::set_x_fixed);
	ClassDB::bind_method(D_METHOD("get_x_fixed"), &TerrainTileEntry::get_x_fixed);
	ClassDB::bind_method(D_METHOD("set_z_fixed", "value"), &TerrainTileEntry::set_z_fixed);
	ClassDB::bind_method(D_METHOD("get_z_fixed"), &TerrainTileEntry::get_z_fixed);
	ClassDB::bind_method(D_METHOD("set_tile_index", "value"), &TerrainTileEntry::set_tile_index);
	ClassDB::bind_method(D_METHOD("set_flags", "value"), &TerrainTileEntry::set_flags);
	ClassDB::bind_method(D_METHOD("get_flags"), &TerrainTileEntry::get_flags);
	ClassDB::bind_method(D_METHOD("set_cell_x", "value"), &TerrainTileEntry::set_cell_x);
	ClassDB::bind_method(D_METHOD("get_cell_x"), &TerrainTileEntry::get_cell_x);
	ClassDB::bind_method(D_METHOD("set_cell_z", "value"), &TerrainTileEntry::set_cell_z);
	ClassDB::bind_method(D_METHOD("get_cell_z"), &TerrainTileEntry::get_cell_z);
	ClassDB::bind_method(D_METHOD("set_cell", "cell_x", "cell_z"), &TerrainTileEntry::set_cell);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "x_fixed"), "set_x_fixed", "get_x_fixed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "z_fixed"), "set_z_fixed", "get_z_fixed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags"), "set_flags", "get_flags");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_x"), "set_cell_x", "get_cell_x");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_z"), "set_cell_z", "get_cell_z");
}

void TerrainTileEntry::set_x_fixed(int value) {
	x_fixed = value;
}

int TerrainTileEntry::get_x_fixed() const {
	return x_fixed;
}

void TerrainTileEntry::set_z_fixed(int value) {
	z_fixed = value;
}

int TerrainTileEntry::get_z_fixed() const {
	return z_fixed;
}

void TerrainTileEntry::set_tile_index(int value) {
	tile_index = std::clamp(value, 0, 255);
}

void TerrainTileEntry::set_flags(int value) {
	flags = clamp_int<int>(opennova::til_normalize_authored_flags(static_cast<uint8_t>(std::clamp(value, 0, 255))), 0, 255);
}

int TerrainTileEntry::get_flags() const {
	return flags;
}

void TerrainTileEntry::set_cell_x(int value) {
	x_fixed = opennova::til_x_fixed_from_cell(value);
}

int TerrainTileEntry::get_cell_x() const {
	return opennova::til_cell_x_from_fixed(x_fixed);
}

void TerrainTileEntry::set_cell_z(int value) {
	z_fixed = opennova::til_z_fixed_from_cell(value);
}

int TerrainTileEntry::get_cell_z() const {
	return opennova::til_cell_z_from_fixed(z_fixed);
}

void TerrainTileEntry::set_cell(int cell_x, int cell_z) {
	x_fixed = opennova::til_x_fixed_from_cell(cell_x);
	z_fixed = opennova::til_z_fixed_from_cell(cell_z);
}

void TerrainTileEntry::copy_from_native(const opennova::TilOverlayEntry &entry) {
	const opennova::TilOverlayEntry normalized = opennova::til_normalize_overlay_entry(entry);
	x_fixed = normalized.x_fixed;
	z_fixed = normalized.z_fixed;
	tile_index = clamp_int<int>(normalized.tile_index, 0, 255);
	flags = clamp_int<int>(normalized.flags, 0, 255);
}

opennova::TilOverlayEntry TerrainTileEntry::to_native() const {
	opennova::TilOverlayEntry entry;
	entry.x_fixed = x_fixed;
	entry.z_fixed = z_fixed;
	entry.tile_index = clamp_int<uint8_t>(tile_index, 0, 255);
	entry.flags = clamp_int<uint8_t>(flags, 0, 255);
	return opennova::til_normalize_overlay_entry(entry);
}
