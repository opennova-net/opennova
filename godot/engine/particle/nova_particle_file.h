#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <particle/parser.h>
#include <particle/particle.h>

#include "nova_particle_def.h"
#include "nova_particle_effect.h"
#include "nova_particle_table.h"
#include "nova_particle_table_handles.h"

namespace godot {

// Top-level Resource wrapper for a parsed .ptl file.
// Engine: section dispatcher CEffectWorld_ParseSectionCallback @ 0x5ecb40 +
// CParticleDef_ParseFromConfigMap @ 0x5ed210.
class NovaParticleFile : public Resource {
	GDCLASS(NovaParticleFile, Resource)

private:
	String source_path;
	TypedArray<NovaParticleEffect> effects;
	TypedArray<NovaParticleDef> particles;
	TypedArray<NovaParticleTable> tables;
	TypedArray<NovaParticleTableHandles> table_handles;

protected:
	static void _bind_methods();

public:
	NovaParticleFile();

	void set_source_path(const String &p);
	String get_source_path() const;

	void set_effects(const TypedArray<NovaParticleEffect> &v);
	TypedArray<NovaParticleEffect> get_effects() const;
	void set_particles(const TypedArray<NovaParticleDef> &v);
	TypedArray<NovaParticleDef> get_particles() const;
	void set_tables(const TypedArray<NovaParticleTable> &v);
	TypedArray<NovaParticleTable> get_tables() const;
	void set_table_handles(const TypedArray<NovaParticleTableHandles> &v);
	TypedArray<NovaParticleTableHandles> get_table_handles() const;

	// File I/O — return Error code (OK on success).
	Error load_from_file(const String &path);
	// Parse from in-memory bytes (VFS/PFF-mounted .ptl); display_path becomes
	// source_path for status surfaces without implying a loose file exists.
	Error load_from_buffer(const PackedByteArray &bytes, const String &display_path);
	Error save_to_file(const String &path);

	// Convenience lookups by id (returns null Ref on miss).
	Ref<NovaParticleEffect> find_effect(const String &id) const;
	Ref<NovaParticleDef> find_particle(const String &id) const;
	Ref<NovaParticleTable> find_table(const String &id) const;

	void copy_from_native(const opennova::particle::ParticleFile &file);
	opennova::particle::ParticleFile to_native() const;
};

} // namespace godot
