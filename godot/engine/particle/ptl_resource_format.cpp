#include "ptl_resource_format.h"

#include "nova_particle_file.h"

using namespace godot;

PackedStringArray ResourceFormatLoaderPTL::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("ptl");
	return exts;
}

bool ResourceFormatLoaderPTL::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaParticleFile") || p_type == StringName("Resource");
}

String ResourceFormatLoaderPTL::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "ptl") {
		return "NovaParticleFile";
	}
	return "";
}

Variant ResourceFormatLoaderPTL::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<NovaParticleFile> file;
	file.instantiate();
	if (file->load_from_file(p_path) != OK) {
		return Variant();
	}
	const String logical_path = p_original_path.is_empty() ? p_path : p_original_path;
	file->set_source_path(logical_path);
	file->set_path(logical_path);
	return file;
}

Error ResourceFormatSaverPTL::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<NovaParticleFile> file = p_resource;
	if (file.is_null()) return ERR_INVALID_PARAMETER;
	return file->save_to_file(p_path);
}

bool ResourceFormatSaverPTL::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaParticleFile>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverPTL::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("ptl");
	}
	return exts;
}
