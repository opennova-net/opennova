#include "nova_terrain_tile_info.h"

#include "nova_terrain_tile_entry.h"

#include <formats/til/til_io.h>

#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>

// Retail: Terrain_LoadTileInfoFile @0x60a740 (jodemo twin Terrain_LoadTileInfoFile @0x5CA730);
// docs/tiles/til-re.md
// docs/engine_spec_tiles.md 4.1

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using namespace godot;

namespace {

static Ref<TerrainTileEntry> tile_entry_to_object(const opennova::TilOverlayEntry &entry) {
	Ref<TerrainTileEntry> object;
	object.instantiate();
	object->copy_from_native(entry);
	return object;
}

static bool tile_entry_from_variant(const Variant &value, opennova::TilOverlayEntry &out_entry) {
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value;
		if (const TerrainTileEntry *entry = Object::cast_to<TerrainTileEntry>(object)) {
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

void TerrainTileInfo::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &TerrainTileInfo::load_from_bytes);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &TerrainTileInfo::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &TerrainTileInfo::save_to_path);
	ClassDB::bind_method(D_METHOD("blocks_foliage", "world_x", "world_z", "radius"), &TerrainTileInfo::blocks_foliage);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &TerrainTileInfo::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_entries"), &TerrainTileInfo::get_entries);
	ClassDB::bind_method(D_METHOD("set_entries", "entries"), &TerrainTileInfo::set_entries);
	ClassDB::bind_method(D_METHOD("get_entry", "index"), &TerrainTileInfo::get_entry);
	ClassDB::bind_method(D_METHOD("add_entry", "entry"), &TerrainTileInfo::add_entry);
	ClassDB::bind_method(D_METHOD("remove_entry", "index"), &TerrainTileInfo::remove_entry);
	ClassDB::bind_method(D_METHOD("clear_entries"), &TerrainTileInfo::clear_entries);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "entries", PROPERTY_HINT_ARRAY_TYPE, "TerrainTileEntry"),
	             "set_entries",
	             "get_entries");

	BIND_CONSTANT(FLAG_FLIP_X);
	BIND_CONSTANT(FLAG_FLIP_Y);
	BIND_CONSTANT(FLAG_ROTATE_90);
	BIND_CONSTANT(FLAG_OUTLINE);
	BIND_CONSTANT(ATLAS_TILE_PIXELS);
	BIND_CONSTANT(CELL_WORLD_SIZE);
}

Error TerrainTileInfo::load_from_bytes(const PackedByteArray &p_bytes) {
	opennova::TilFile parsed;
	std::string error;
	const uint8_t *data = p_bytes.size() > 0 ? p_bytes.ptr() : nullptr;
	if (!opennova::load_til(data, static_cast<size_t>(p_bytes.size()), parsed, error)) {
		UtilityFunctions::push_error("TerrainTileInfo: failed to parse .til: ", error.c_str());
		return ERR_PARSE_ERROR;
	}
	copy_from_native(parsed);
	return OK;
}

bool TerrainTileInfo::blocks_foliage(float world_x,
                                         float world_z,
                                         float radius) const {
	// til_world_z_from_fixed decodes stored-negated z_fixed into the same
	// terrain/Godot plane used by foliage candidates. This API accepts that
	// decoded plane directly. The loaded mission .til is the same
	// g_TerrainTileArray scanned by both foliage generators and surface overrides.
	// [orig: Foliage_PathBlockedByPlacedTile @ 0x606490;
	// Terrain_GetSurfaceTypeAtPosition @ 0x606510]
	return opennova::til_blocks_foliage(til, world_x, world_z, radius);
}

int TerrainTileInfo::get_entry_count() const {
	return static_cast<int>(til.entries.size());
}

Array TerrainTileInfo::get_entries() const {
	Array entries;
	entries.resize(static_cast<int64_t>(til.entries.size()));
	for (size_t i = 0; i < til.entries.size(); ++i) {
		entries[static_cast<int64_t>(i)] = tile_entry_to_object(til.entries[i]);
	}
	return entries;
}

void TerrainTileInfo::set_entries(const Array &p_entries) {
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

Ref<TerrainTileEntry> TerrainTileInfo::get_entry(int index) const {
	if (index < 0 || index >= static_cast<int>(til.entries.size())) {
		return Ref<TerrainTileEntry>();
	}
	return tile_entry_to_object(til.entries[static_cast<size_t>(index)]);
}

void TerrainTileInfo::add_entry(const Ref<TerrainTileEntry> &entry) {
	if (entry.is_null()) {
		return;
	}

	til.entries.push_back(opennova::til_normalize_overlay_entry(entry->to_native()));
	emit_changed();
}

void TerrainTileInfo::remove_entry(int index) {
	if (index < 0 || index >= static_cast<int>(til.entries.size())) {
		return;
	}

	til.entries.erase(til.entries.begin() + index);
	emit_changed();
}

void TerrainTileInfo::clear_entries() {
	til.entries.clear();
	emit_changed();
}

void TerrainTileInfo::copy_from_native(const opennova::TilFile &file) {
	til = opennova::til_normalize_file(file);
	emit_changed();
}

opennova::TilFile TerrainTileInfo::to_native() const {
	return opennova::til_normalize_file(til);
}

Error TerrainTileInfo::load_from_path(const String &p_path) {
	PackedByteArray packed;
	if (!read_nova_payload_file(p_path, packed)) {
		return ERR_FILE_CANT_OPEN;
	}
	return load_from_bytes(packed);
}

Error TerrainTileInfo::save_to_path(const String &p_path) const {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::save_til(to_native(), bytes, error)) {
		UtilityFunctions::push_warning("TerrainTileInfo.save_to_path: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(packed.ptrw(), bytes.data(), bytes.size());
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}
