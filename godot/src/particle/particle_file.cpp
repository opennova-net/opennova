#include "particle/particle_file.h"

#include <formats/particle/parser.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <utility>

using namespace godot;

void ParticleFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_path", "path"), &ParticleFile::set_source_path);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ParticleFile::get_source_path);
	ClassDB::bind_method(D_METHOD("get_effects"), &ParticleFile::get_effects);
	ClassDB::bind_method(D_METHOD("find_effect", "id"), &ParticleFile::find_effect);
	ClassDB::bind_method(D_METHOD("load_from_file", "path"), &ParticleFile::load_from_file);
	ClassDB::bind_method(D_METHOD("load_from_buffer", "bytes", "display_path"),
			&ParticleFile::load_from_buffer, DEFVAL(String()));
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path"), "set_source_path", "get_source_path");
}

void ParticleFile::set_source_path(const String &p) { source_path = p; emit_changed(); }
String ParticleFile::get_source_path() const { return source_path; }

TypedArray<ParticleEffect> ParticleFile::get_effects() const {
	TypedArray<ParticleEffect> out;
	for (const auto &effect : file_.effects) {
		Ref<ParticleEffect> row;
		row.instantiate();
		row->assign(effect);
		out.push_back(row);
	}
	return out;
}

Ref<ParticleEffect> ParticleFile::find_effect(const String &id) const {
	const auto *effect = file_.find_effect(id.utf8().get_data());
	if (effect == nullptr) return {};
	Ref<ParticleEffect> out;
	out.instantiate();
	out->assign(*effect);
	return out;
}

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
	opennova::particle::ParticleFile parsed;
	opennova::particle::ParseError err;
	if (!opennova::particle::load_particles_from_buffer(
			reinterpret_cast<const char *>(bytes.ptr()), static_cast<std::size_t>(bytes.size()), parsed, err)) {
		UtilityFunctions::push_warning(String::utf8(("ptl load failed at line " + std::to_string(err.line) + ": " + err.message).c_str()));
		return ERR_FILE_CANT_READ;
	}
	file_ = std::move(parsed);
	source_path = display_path;
	emit_changed();
	return OK;
}
