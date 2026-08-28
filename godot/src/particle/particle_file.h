#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <formats/particle/parser.h>
#include <formats/particle/particle.h>

#include "particle/particle_def.h"
#include "particle/particle_effect.h"
#include "particle/particle_table.h"
#include "particle/particle_table_handles.h"

namespace godot {

// Top-level Resource wrapper for a parsed .ptl file.
// Engine: section dispatcher CEffectWorld_ParseSectionCallback @ 0x5ecb40 +
// CParticleDef_ParseFromConfigMap @ 0x5ed210.
class ParticleFile : public Resource {
	GDCLASS(ParticleFile, Resource)

private:
	String source_path;
	TypedArray<ParticleEffect> effects;
	TypedArray<ParticleDef> particles;
	TypedArray<ParticleTable> tables;
	TypedArray<ParticleTableHandles> table_handles;

protected:
	static void _bind_methods();

public:
	ParticleFile();

	void set_source_path(const String &p);
	String get_source_path() const;

	void set_effects(const TypedArray<ParticleEffect> &v);
	TypedArray<ParticleEffect> get_effects() const;
	void set_particles(const TypedArray<ParticleDef> &v);
	TypedArray<ParticleDef> get_particles() const;
	void set_tables(const TypedArray<ParticleTable> &v);
	TypedArray<ParticleTable> get_tables() const;

	// File I/O — return Error code (OK on success).
	Error load_from_file(const String &path);
	// Parse from in-memory bytes (VFS/PFF-mounted .ptl); display_path becomes
	// source_path for status surfaces without implying a loose file exists.
	Error load_from_buffer(const PackedByteArray &bytes, const String &display_path);

	// Convenience lookups by id (returns null Ref on miss).
	Ref<ParticleEffect> find_effect(const String &id) const;
	Ref<ParticleDef> find_particle(const String &id) const;

	void copy_from_native(const opennova::particle::ParticleFile &file);
	opennova::particle::ParticleFile to_native() const;
};

} // namespace godot
