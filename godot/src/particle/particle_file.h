#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <formats/particle/particle.h>

#include "particle/particle_effect.h"

namespace godot {

// A parsed native document with its device source locator. The renderer and
// catalog consume native_file(); effect inspection is materialized on demand
// for the visual probes that report authored definitions.
class ParticleFile : public Resource {
	GDCLASS(ParticleFile, Resource)

	String source_path;
	opennova::particle::ParticleFile file_;

protected:
	static void _bind_methods();

public:
	void set_source_path(const String &p);
	String get_source_path() const;
	TypedArray<ParticleEffect> get_effects() const;
	Ref<ParticleEffect> find_effect(const String &id) const;

	Error load_from_file(const String &path);
	Error load_from_buffer(const PackedByteArray &bytes, const String &display_path);
	const opennova::particle::ParticleFile &native_file() const { return file_; }
};

} // namespace godot
