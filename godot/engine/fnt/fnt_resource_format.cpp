#include "fnt/fnt_resource_format.h"

#include "fnt/nova_fnt_resource.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

PackedStringArray ResourceFormatLoaderFNT::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("fnt");
	exts.push_back("FNT");
	return exts;
}

bool ResourceFormatLoaderFNT::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaFntResource") || p_type == StringName("Resource");
}

String ResourceFormatLoaderFNT::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "fnt") {
		return "NovaFntResource";
	}
	return String();
}

Variant ResourceFormatLoaderFNT::_load(const String &p_path, const String &p_original_path,
                                       bool p_use_sub_threads, int32_t p_cache_mode) const {
	(void)p_original_path;
	(void)p_use_sub_threads;
	(void)p_cache_mode;

	PackedByteArray data;
	if (!read_nova_payload_file(p_path, data)) {
		UtilityFunctions::push_error("ResourceFormatLoaderFNT: cannot open file: ", p_path);
		return Variant();
	}

	Ref<NovaFntResource> resource;
	resource.instantiate();
	Error err = resource->load_from_bytes(data);
	if (err != OK) {
		UtilityFunctions::push_error("ResourceFormatLoaderFNT: failed to load .fnt: ", p_path);
		return Variant();
	}
	resource->set_path(p_original_path);
	return resource;
}

Error ResourceFormatSaverFNT::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	(void)p_flags;
	Ref<NovaFntResource> resource = p_resource;
	if (resource.is_null()) {
		return ERR_INVALID_PARAMETER;
	}

	PackedByteArray data = resource->to_bytes();
	if (data.is_empty()) {
		return ERR_INVALID_DATA;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		UtilityFunctions::push_error("ResourceFormatSaverFNT: cannot open file for writing: ", p_path);
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(data);
	file->close();
	return OK;
}

bool ResourceFormatSaverFNT::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaFntResource>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverFNT::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("fnt");
	}
	return exts;
}

} // namespace godot
