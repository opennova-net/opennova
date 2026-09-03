#pragma once

#include <godot_cpp/variant/string.hpp>

#include <formats/particle/particle.h>

namespace godot {

// Wraps opennova::particle::TableEditHandles: the [tabledef] authoring
// metadata (handlecount, tightness) a parsed .ptl carries through
// ParticleFile so a write-back preserves it. Runtime ignores these fields;
// a C++-only value since the ADR 0043 d10 env-core sweep (no script ever
// read it).
struct ParticleTableHandles {
	String table_id;
	int handlecount = 0;
	int tightness = 0;

	void copy_from_native(const opennova::particle::TableEditHandles &h);
	opennova::particle::TableEditHandles to_native() const;
};

} // namespace godot
