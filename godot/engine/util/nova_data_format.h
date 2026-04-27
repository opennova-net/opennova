#pragma once

// Minimal Resource wrapper for raw NovaLogic data files.
// Used by the individual format loaders (CPT, BAD, DEF) to make
// these files visible to Godot's resource/export system.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class NovaDataFile : public Resource {
	GDCLASS(NovaDataFile, Resource)

	PackedByteArray data;

protected:
	static void _bind_methods();

public:
	void set_data(const PackedByteArray &p_data);
	PackedByteArray get_data() const;
};

} // namespace godot
