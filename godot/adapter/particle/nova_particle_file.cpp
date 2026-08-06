#include "nova_particle_file.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <sstream>

using namespace godot;

NovaParticleFile::NovaParticleFile() = default;

void NovaParticleFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_path", "p"), &NovaParticleFile::set_source_path);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaParticleFile::get_source_path);
	ClassDB::bind_method(D_METHOD("set_effects", "v"), &NovaParticleFile::set_effects);
	ClassDB::bind_method(D_METHOD("get_effects"), &NovaParticleFile::get_effects);
	ClassDB::bind_method(D_METHOD("set_particles", "v"), &NovaParticleFile::set_particles);
	ClassDB::bind_method(D_METHOD("get_particles"), &NovaParticleFile::get_particles);
	ClassDB::bind_method(D_METHOD("set_tables", "v"), &NovaParticleFile::set_tables);
	ClassDB::bind_method(D_METHOD("get_tables"), &NovaParticleFile::get_tables);
	ClassDB::bind_method(D_METHOD("set_table_handles", "v"), &NovaParticleFile::set_table_handles);
	ClassDB::bind_method(D_METHOD("get_table_handles"), &NovaParticleFile::get_table_handles);

	ClassDB::bind_method(D_METHOD("load_from_file", "path"), &NovaParticleFile::load_from_file);
	ClassDB::bind_method(D_METHOD("load_from_buffer", "bytes", "display_path"), &NovaParticleFile::load_from_buffer);
	ClassDB::bind_method(D_METHOD("save_to_file", "path"), &NovaParticleFile::save_to_file);
	ClassDB::bind_method(D_METHOD("find_effect", "id"), &NovaParticleFile::find_effect);
	ClassDB::bind_method(D_METHOD("find_particle", "id"), &NovaParticleFile::find_particle);
	ClassDB::bind_method(D_METHOD("find_table", "id"), &NovaParticleFile::find_table);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path"), "set_source_path", "get_source_path");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "effects", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleEffect"),
			"set_effects", "get_effects");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "particles", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleDef"),
			"set_particles", "get_particles");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "tables", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleTable"),
			"set_tables", "get_tables");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "table_handles", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleTableHandles"),
			"set_table_handles", "get_table_handles");
}

void NovaParticleFile::set_source_path(const String &p) { source_path = p; }
String NovaParticleFile::get_source_path() const { return source_path; }
void NovaParticleFile::set_effects(const TypedArray<NovaParticleEffect> &v) { effects = v; emit_changed(); }
TypedArray<NovaParticleEffect> NovaParticleFile::get_effects() const { return effects; }
void NovaParticleFile::set_particles(const TypedArray<NovaParticleDef> &v) { particles = v; emit_changed(); }
TypedArray<NovaParticleDef> NovaParticleFile::get_particles() const { return particles; }
void NovaParticleFile::set_tables(const TypedArray<NovaParticleTable> &v) { tables = v; emit_changed(); }
TypedArray<NovaParticleTable> NovaParticleFile::get_tables() const { return tables; }
void NovaParticleFile::set_table_handles(const TypedArray<NovaParticleTableHandles> &v) { table_handles = v; emit_changed(); }
TypedArray<NovaParticleTableHandles> NovaParticleFile::get_table_handles() const { return table_handles; }

Error NovaParticleFile::load_from_file(const String &path) {
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

Error NovaParticleFile::load_from_buffer(const PackedByteArray &bytes, const String &display_path) {
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

Error NovaParticleFile::save_to_file(const String &path) {
	opennova::particle::ParticleFile native = to_native();
	std::string err;
	std::ostringstream stream;
	if (!opennova::particle::save_particles(stream, native, err)) {
		UtilityFunctions::push_warning(String::utf8(("ptl save failed: " + err).c_str()));
		return ERR_FILE_CANT_WRITE;
	}
	const std::string serialized = stream.str();
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(serialized.size()));
	if (!serialized.empty()) {
		std::memcpy(bytes.ptrw(), serialized.data(), serialized.size());
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		UtilityFunctions::push_warning("ptl save failed: cannot open ", path);
		return ERR_FILE_CANT_WRITE;
	}
	if (!file->store_buffer(bytes)) {
		file->close();
		UtilityFunctions::push_warning("ptl save failed: short write to ", path);
		return ERR_FILE_CANT_WRITE;
	}
	file->close();
	source_path = path;
	return OK;
}

Ref<NovaParticleEffect> NovaParticleFile::find_effect(const String &id) const {
	// Case-insensitive like every by-name walk in the effect system
	// [orig: CEffectWorld_FindEffectDefByName @ 0x5e34f0 → _stricmp @ 0x5e352c].
	for (int i = 0; i < effects.size(); ++i) {
		Ref<NovaParticleEffect> e = effects[i];
		if (e.is_valid() && e->get_id().nocasecmp_to(id) == 0) return e;
	}
	return Ref<NovaParticleEffect>();
}

Ref<NovaParticleDef> NovaParticleFile::find_particle(const String &id) const {
	// Case-insensitive like every by-name walk in the effect system
	// [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0 → _stricmp @ 0x5e420c].
	for (int i = 0; i < particles.size(); ++i) {
		Ref<NovaParticleDef> p = particles[i];
		if (p.is_valid() && p->get_id().nocasecmp_to(id) == 0) return p;
	}
	return Ref<NovaParticleDef>();
}

Ref<NovaParticleTable> NovaParticleFile::find_table(const String &id) const {
	// Case-insensitive to match the engine's _stricmp table resolve
	// [orig: table find @ 0x5e9540 → _stricmp @ 0x76fdf6]; shipped data mixes
	// cases (ambfx.ptl `green_func = Table11Alt` vs `id = table11Alt`).
	for (int i = 0; i < tables.size(); ++i) {
		Ref<NovaParticleTable> t = tables[i];
		if (t.is_valid() && t->get_id().nocasecmp_to(id) == 0) return t;
	}
	return Ref<NovaParticleTable>();
}

void NovaParticleFile::copy_from_native(const opennova::particle::ParticleFile &file) {
	effects.clear();
	effects.resize(static_cast<int>(file.effects.size()));
	for (int i = 0; i < static_cast<int>(file.effects.size()); ++i) {
		Ref<NovaParticleEffect> e;
		e.instantiate();
		e->copy_from_native(file.effects[static_cast<size_t>(i)]);
		effects[i] = e;
	}
	particles.clear();
	particles.resize(static_cast<int>(file.particles.size()));
	for (int i = 0; i < static_cast<int>(file.particles.size()); ++i) {
		Ref<NovaParticleDef> p;
		p.instantiate();
		p->copy_from_native(file.particles[static_cast<size_t>(i)]);
		particles[i] = p;
	}
	tables.clear();
	tables.resize(static_cast<int>(file.tables.size()));
	for (int i = 0; i < static_cast<int>(file.tables.size()); ++i) {
		Ref<NovaParticleTable> t;
		t.instantiate();
		t->copy_from_native(file.tables[static_cast<size_t>(i)]);
		tables[i] = t;
	}
	table_handles.clear();
	table_handles.resize(static_cast<int>(file.table_handles.size()));
	for (int i = 0; i < static_cast<int>(file.table_handles.size()); ++i) {
		Ref<NovaParticleTableHandles> h;
		h.instantiate();
		h->copy_from_native(file.table_handles[static_cast<size_t>(i)]);
		table_handles[i] = h;
	}
	emit_changed();
}

opennova::particle::ParticleFile NovaParticleFile::to_native() const {
	opennova::particle::ParticleFile out;
	out.effects.reserve(static_cast<size_t>(effects.size()));
	for (int i = 0; i < effects.size(); ++i) {
		Ref<NovaParticleEffect> e = effects[i];
		if (e.is_valid()) out.effects.push_back(e->to_native());
	}
	out.particles.reserve(static_cast<size_t>(particles.size()));
	for (int i = 0; i < particles.size(); ++i) {
		Ref<NovaParticleDef> p = particles[i];
		if (p.is_valid()) out.particles.push_back(p->to_native());
	}
	out.tables.reserve(static_cast<size_t>(tables.size()));
	for (int i = 0; i < tables.size(); ++i) {
		Ref<NovaParticleTable> t = tables[i];
		if (t.is_valid()) out.tables.push_back(t->to_native());
	}
	out.table_handles.reserve(static_cast<size_t>(table_handles.size()));
	for (int i = 0; i < table_handles.size(); ++i) {
		Ref<NovaParticleTableHandles> h = table_handles[i];
		if (h.is_valid()) out.table_handles.push_back(h->to_native());
	}
	return out;
}
