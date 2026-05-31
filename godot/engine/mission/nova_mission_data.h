#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mission/mission.h>

namespace godot {

// Thin GDExtension wrapper over opennova::mission::MissionDocument (libs/mission).
// Parses a NovaLogic .bms mission file and exposes its header (terrain/env refs,
// metadata) and its placed entities as Godot dictionaries. The read surface is
// loading + getters; the mutate surface (Phase 1 authoring) is set_entity_transform
// + save, calling the already byte-faithful writer in libs/mission.
class NovaMissionData : public RefCounted {
	GDCLASS(NovaMissionData, RefCounted)

private:
	opennova::mission::MissionDocument document;
	String source_path;
	String last_error;
	// True once an in-memory mutation lands and before the next successful save/load.
	// MissionDocument carries no dirty bit of its own, so the wrapper owns it.
	bool modified = false;

	Dictionary entity_to_dictionary(const opennova::mission::EntityRecord &record) const;

protected:
	static void _bind_methods();

public:
	// Mirrors opennova::mission::EntityKind. Bound as plain constants.
	enum {
		KIND_MARKER = 0,
		KIND_ITEM = 1,
		KIND_BUILDING = 2,
		KIND_ORGANIC = 3,
	};

	Error open_file(const String &path);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	String get_mission_name() const;
	String get_designer() const;
	// Header references (basenames, no extension): e.g. "dvxi5", "full_00".
	String get_terrain_ref() const;
	String get_environment_ref() const;
	Dictionary get_info() const;

	int get_entity_count(int kind) const;
	// Array of dictionaries; see entity_to_dictionary() for the fields.
	Array get_entities(int kind) const;
	// One entity by (kind, index) as a dictionary (same fields as get_entities), or {}
	// if out of range. O(1) — prefer this over scanning get_entities for a single hit.
	Dictionary get_entity(int kind, int index) const;
	Array get_all_entities() const;

	// --- Authoring (Phase 1) --------------------------------------------------
	// Move an existing entity. `position` is mission-space (x, y, z); `rotation_deg`
	// is (pitch, yaw, roll) in degrees, rounded to the int fields the format stores.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool set_entity_transform(int kind, int index, const Vector3 &position, const Vector3 &rotation_deg);
	// Mutate a single editable scalar property ("team" or "group") on the entity at
	// (kind, index). The underlying lib setter (set_entity_properties) replaces all
	// 13 property fields at once, so this reads the entity's current properties,
	// overwrites only the named one, then writes the lot back. Returns false if
	// (kind, index) is out of range or `property` is not a known editable name. Sets
	// the dirty flag on success.
	bool set_entity_property_int(int kind, int index, const String &property, int value);
	// Place a new entity of `kind` for `item_id` (an items.def id) at `position`
	// (mission-space x, y, z) with `rotation_deg` (pitch, yaw, roll), rounded to the
	// int fields the format stores. The lib seeds the rest of the record with sane
	// defaults (see make_default_entity). Returns the new entity as a dictionary
	// (same fields as get_entity, including its assigned "index"), or {} if no mission
	// is loaded or the kind is invalid. Sets the dirty flag on success.
	Dictionary add_entity(int kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg);
	// Write the document back to disk. save_file() targets the path it was opened
	// from; save_as() targets a new path and adopts it. Both clear the dirty flag and
	// go through the byte-faithful writer in libs/mission. save_file() returns
	// ERR_INVALID_PARAMETER when there is no current path (shell then offers Save As).
	Error save_file();
	Error save_as(const String &path);
	bool is_modified() const;
};

} // namespace godot
