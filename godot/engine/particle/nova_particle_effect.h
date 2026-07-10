#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <particle/particle.h>

namespace godot {

// Wraps opennova::particle::EffectDef — a named effect that composes one or
// more particle defs by id. Engine: CParticleEffectDef_WriteToFile @ 0x5e0fe0.
class NovaParticleEffect : public Resource {
	GDCLASS(NovaParticleEffect, Resource)

private:
	String id;
	PackedStringArray pdefs;

protected:
	static void _bind_methods();

public:
	void set_id(const String &p_value);
	String get_id() const;
	void set_pdefs(const PackedStringArray &p_value);
	PackedStringArray get_pdefs() const;

	void copy_from_native(const opennova::particle::EffectDef &effect);
	opennova::particle::EffectDef to_native() const;
};

} // namespace godot
