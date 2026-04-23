#include "nova_terrain_tile_info.h"

#include "nova_terrain_tile_entry.h"

// Engine: jodemo.exe Terrain_LoadTileInfoFile@0x5CA730
// docs/engine_spec_tiles.md 4.1

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>

using namespace godot;

namespace {

static Ref<NovaTerrainTileEntry> tile_entry_to_object(const opennova::TilOverlayEntry &entry) {
	Ref<NovaTerrainTileEntry> object;
	object.instantiate();
	object->copy_from_native(entry);
	return object;
}

static bool tile_entry_from_variant(const Variant &value, opennova::TilOverlayEntry &out_entry) {
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value;
		if (const NovaTerrainTileEntry *entry = Object::cast_to<NovaTerrainTileEntry>(object)) {
			out_entry = entry->to_native();
			return true;
		}
	}

	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = value;
		out_entry.x_fixed = static_cast<int32_t>(dict.get("x_fixed", 0));
		out_entry.z_fixed = static_cast<int32_t>(dict.get("z_fixed", 0));
		out_entry.tile_index = static_cast<uint8_t>(std::clamp(static_cast<int>(dict.get("tile_index", 0)), 0, 255));
		out_entry.flags = static_cast<uint8_t>(std::clamp(static_cast<int>(dict.get("flags", 0)), 0, 255));
		out_entry = opennova::til_normalize_overlay_entry(out_entry);
		return true;
	}

	return false;
}

} // namespace

void NovaTerrainTileInfo::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_entry_count"), &NovaTerrainTileInfo::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_entries"), &NovaTerrainTileInfo::get_entries);
	ClassDB::bind_method(D_METHOD("set_entries", "entries"), &NovaTerrainTileInfo::set_entries);
	ClassDB::bind_method(D_METHOD("get_entry", "index"), &NovaTerrainTileInfo::get_entry);
	ClassDB::bind_method(D_METHOD("set_entry", "index", "entry"), &NovaTerrainTileInfo::set_entry);
	ClassDB::bind_method(D_METHOD("add_entry", "entry"), &NovaTerrainTileInfo::add_entry);
	ClassDB::bind_method(D_METHOD("remove_entry", "index"), &NovaTerrainTileInfo::remove_entry);
	ClassDB::bind_method(D_METHOD("clear_entries"), &NovaTerrainTileInfo::clear_entries);
	ClassDB::bind_method(D_METHOD("find_entry_index_at_cell", "cell_x", "cell_z"), &NovaTerrainTileInfo::find_entry_index_at_cell);
	ClassDB::bind_method(D_METHOD("get_entry_indices_at_cell", "cell_x", "cell_z"), &NovaTerrainTileInfo::get_entry_indices_at_cell);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "entries", PROPERTY_HINT_ARRAY_TYPE, "NovaTerrainTileEntry"),
	             "set_entries",
	             "get_entries");

	BIND_CONSTANT(FLAG_FLIP_X);
	BIND_CONSTANT(FLAG_FLIP_Y);
	BIND_CONSTANT(FLAG_ROTATE_90);
	BIND_CONSTANT(FLAG_OUTLINE);
	BIND_CONSTANT(ATLAS_TILE_PIXELS);
	BIND_CONSTANT(CELL_WORLD_SIZE);
}

int NovaTerrainTileInfo::get_entry_count() const {
	return static_cast<int>(til.entries.size());
}

Array NovaTerrainTileInfo::get_entries() const {
	Array entries;
	entries.resize(static_cast<int64_t>(til.entries.size()));
	for (size_t i = 0; i < til.entries.size(); ++i) {
		entries[static_cast<int64_t>(i)] = tile_entry_to_object(til.entries[i]);
	}
	return entries;
}

void NovaTerrainTileInfo::set_entries(const Array &p_entries) {
	til.entries.clear();
	til.entries.reserve(static_cast<size_t>(p_entries.size()));
	for (int i = 0; i < p_entries.size(); ++i) {
		opennova::TilOverlayEntry entry;
		if (tile_entry_from_variant(p_entries[i], entry)) {
			til.entries.push_back(opennova::til_normalize_overlay_entry(entry));
		}
	}
	til = opennova::til_normalize_file(til);
	emit_changed();
}

Ref<NovaTerrainTileEntry> NovaTerrainTileInfo::get_entry(int index) const {
	if (index < 0 || index >= static_cast<int>(til.entries.size())) {
		return Ref<NovaTerrainTileEntry>();
	}
	return tile_entry_to_object(til.entries[static_cast<size_t>(index)]);
}

void NovaTerrainTileInfo::set_entry(int index, const Ref<NovaTerrainTileEntry> &entry) {
	if (entry.is_null() || index < 0 || index >= static_cast<int>(til.entries.size())) {
		return;
	}

	til.entries[static_cast<size_t>(index)] = opennova::til_normalize_overlay_entry(entry->to_native());
	emit_changed();
}

void NovaTerrainTileInfo::add_entry(const Ref<NovaTerrainTileEntry> &entry) {
	if (entry.is_null()) {
		return;
	}

	til.entries.push_back(opennova::til_normalize_overlay_entry(entry->to_native()));
	emit_changed();
}

void NovaTerrainTileInfo::remove_entry(int index) {
	if (index < 0 || index >= static_cast<int>(til.entries.size())) {
		return;
	}

	til.entries.erase(til.entries.begin() + index);
	emit_changed();
}

void NovaTerrainTileInfo::clear_entries() {
	til.entries.clear();
	emit_changed();
}

int NovaTerrainTileInfo::find_entry_index_at_cell(int cell_x, int cell_z) const {
	for (size_t i = 0; i < til.entries.size(); ++i) {
		if (opennova::til_entry_matches_cell(til.entries[i], cell_x, cell_z)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

PackedInt32Array NovaTerrainTileInfo::get_entry_indices_at_cell(int cell_x, int cell_z) const {
	PackedInt32Array indices;
	for (size_t i = 0; i < til.entries.size(); ++i) {
		if (opennova::til_entry_matches_cell(til.entries[i], cell_x, cell_z)) {
			indices.push_back(static_cast<int32_t>(i));
		}
	}
	return indices;
}

void NovaTerrainTileInfo::copy_from_native(const opennova::TilFile &file) {
	til = opennova::til_normalize_file(file);
	emit_changed();
}

opennova::TilFile NovaTerrainTileInfo::to_native() const {
	return opennova::til_normalize_file(til);
}
