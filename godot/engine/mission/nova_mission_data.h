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
// metadata) and its placed entities as Godot dictionaries. Read-only for now;
// mission authoring is a later phase.
class NovaMissionData : public RefCounted {
	GDCLASS(NovaMissionData, RefCounted)

private:
	opennova::mission::MissionDocument document;
	String source_path;
	String last_error;

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
	Array get_all_entities() const;
};

} // namespace godot
