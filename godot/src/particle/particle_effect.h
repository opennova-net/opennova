#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <formats/particle/particle.h>

namespace godot {

// Read-only authored effect diagnostic used by visual probes. Runtime
// catalog loading reads the document's native records directly.
class ParticleEffect : public RefCounted {
	GDCLASS(ParticleEffect, RefCounted)

	opennova::particle::EffectDef value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::particle::EffectDef &value) { value_ = value; }
	String get_id() const;
	PackedStringArray get_pdefs() const;
};

} // namespace godot
