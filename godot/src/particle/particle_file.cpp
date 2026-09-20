#include "particle/particle_file.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <sstream>

using namespace godot;

ParticleFile::ParticleFile() = default;

void ParticleFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_path", "p"), &ParticleFile::set_source_path);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ParticleFile::get_source_path);
	ClassDB::bind_method(D_METHOD("set_effects", "v"), &ParticleFile::set_effects);
	ClassDB::bind_method(D_METHOD("get_effects"), &ParticleFile::get_effects);
	ClassDB::bind_method(D_METHOD("set_particles", "v"), &ParticleFile::set_particles);
	ClassDB::bind_method(D_METHOD("get_particles"), &ParticleFile::get_particles);
	ClassDB::bind_method(D_METHOD("set_tables", "v"), &ParticleFile::set_tables);
	ClassDB::bind_method(D_METHOD("get_tables"), &ParticleFile::get_tables);

	ClassDB::bind_method(D_METHOD("load_from_file", "path"), &ParticleFile::load_from_file);
	ClassDB::bind_method(D_METHOD("find_effect", "id"), &ParticleFile::find_effect);
	ClassDB::bind_method(D_METHOD("find_particle", "id"), &ParticleFile::find_particle);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path"), "set_source_path", "get_source_path");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "effects", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":ParticleEffect"),
			"set_effects", "get_effects");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "particles", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":ParticleDef"),
			"set_particles", "get_particles");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "tables", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":ParticleTable"),
			"set_tables", "get_tables");
}

void ParticleFile::set_source_path(const String &p) { source_path = p; }
String ParticleFile::get_source_path() const { return source_path; }
void ParticleFile::set_effects(const TypedArray<ParticleEffect> &v) { effects = v; emit_changed(); }
TypedArray<ParticleEffect> ParticleFile::get_effects() const { return effects; }
void ParticleFile::set_particles(const TypedArray<ParticleDef> &v) { particles = v; emit_changed(); }
TypedArray<ParticleDef> ParticleFile::get_particles() const { return particles; }
void ParticleFile::set_tables(const TypedArray<ParticleTable> &v) { tables = v; emit_changed(); }
TypedArray<ParticleTable> ParticleFile::get_tables() const { return tables; }

Error ParticleFile::load_from_file(const String &path) {
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		UtilityFunctions::push_warning("ptl load failed: cannot open ", path);
		return ERR_FILE_CANT_READ;
	}
	const int64_t length = file->get_length();
	const PackedByteArray bytes = file->get_buffer(length);
	file->close();
	if (bytes.size() != length) {
		UtilityFunctions::push_warning("ptl load failed: short read from ", path);
		return ERR_FILE_CANT_READ;
	}
	return load_from_buffer(bytes, path);
}

Error ParticleFile::load_from_buffer(const PackedByteArray &bytes, const String &display_path) {
	opennova::particle::ParticleFile native;
	opennova::particle::ParseError err;
	if (!opennova::particle::load_particles_from_buffer(
				reinterpret_cast<const char *>(bytes.ptr()), static_cast<std::size_t>(bytes.size()), native, err)) {
		UtilityFunctions::push_warning(String::utf8(("ptl load failed at line " + std::to_string(err.line) + ": " + err.message).c_str()));
		return ERR_FILE_CANT_READ;
	}
	source_path = display_path;
	copy_from_native(native);
	return OK;
}

Ref<ParticleEffect> ParticleFile::find_effect(const String &id) const {
	// Case-insensitive like every by-name walk in the effect system
	// [orig: CEffectWorld_FindEffectDefByName @ 0x5e34f0 → _stricmp @ 0x5e352c, see docs/particles/ptl-format-re.md].
	for (int i = 0; i < effects.size(); ++i) {
		Ref<ParticleEffect> e = effects[i];
		if (e.is_valid() && e->get_id().nocasecmp_to(id) == 0) return e;
	}
	return Ref<ParticleEffect>();
}

Ref<ParticleDef> ParticleFile::find_particle(const String &id) const {
	// Case-insensitive like every by-name walk in the effect system
	// [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0 → _stricmp @ 0x5e420c, see docs/particles/ptl-format-re.md].
	for (int i = 0; i < particles.size(); ++i) {
		Ref<ParticleDef> p = particles[i];
		if (p.is_valid() && p->get_id().nocasecmp_to(id) == 0) return p;
	}
	return Ref<ParticleDef>();
}

void ParticleFile::copy_from_native(const opennova::particle::ParticleFile &file) {
	effects.clear();
	effects.resize(static_cast<int>(file.effects.size()));
	for (int i = 0; i < static_cast<int>(file.effects.size()); ++i) {
		Ref<ParticleEffect> e;
		e.instantiate();
		e->copy_from_native(file.effects[static_cast<size_t>(i)]);
		effects[i] = e;
	}
	particles.clear();
	particles.resize(static_cast<int>(file.particles.size()));
	for (int i = 0; i < static_cast<int>(file.particles.size()); ++i) {
		Ref<ParticleDef> p;
		p.instantiate();
		p->copy_from_native(file.particles[static_cast<size_t>(i)]);
		particles[i] = p;
	}
	tables.clear();
	tables.resize(static_cast<int>(file.tables.size()));
	for (int i = 0; i < static_cast<int>(file.tables.size()); ++i) {
		Ref<ParticleTable> t;
		t.instantiate();
		t->copy_from_native(file.tables[static_cast<size_t>(i)]);
		tables[i] = t;
	}
	table_handles.clear();
	table_handles.resize(file.table_handles.size());
	for (size_t i = 0; i < file.table_handles.size(); ++i) {
		table_handles[i].copy_from_native(file.table_handles[i]);
	}
	emit_changed();
}

opennova::particle::ParticleFile ParticleFile::to_native() const {
	opennova::particle::ParticleFile out;
	out.effects.reserve(static_cast<size_t>(effects.size()));
	for (int i = 0; i < effects.size(); ++i) {
		Ref<ParticleEffect> e = effects[i];
		if (e.is_valid()) out.effects.push_back(e->to_native());
	}
	out.particles.reserve(static_cast<size_t>(particles.size()));
	for (int i = 0; i < particles.size(); ++i) {
		Ref<ParticleDef> p = particles[i];
		if (p.is_valid()) out.particles.push_back(p->to_native());
	}
	out.tables.reserve(static_cast<size_t>(tables.size()));
	for (int i = 0; i < tables.size(); ++i) {
		Ref<ParticleTable> t = tables[i];
		if (t.is_valid()) out.tables.push_back(t->to_native());
	}
	out.table_handles.reserve(table_handles.size());
	for (const ParticleTableHandles &h : table_handles) {
		out.table_handles.push_back(h.to_native());
	}
	return out;
}
