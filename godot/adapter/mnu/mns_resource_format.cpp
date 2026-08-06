#include "mns_resource_format.h"

#include "mns_stylesheet.h"

using namespace godot;

PackedStringArray ResourceFormatLoaderMNS::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("mns");
	return exts;
}

bool ResourceFormatLoaderMNS::_handles_type(const StringName &p_type) const {
	return p_type == StringName("MnsStyleSheet") || p_type == StringName("Resource");
}

String ResourceFormatLoaderMNS::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "mns") {
		return "MnsStyleSheet";
	}
	return "";
}

Variant ResourceFormatLoaderMNS::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<MnsStyleSheet> resource;
	resource.instantiate();
	if (resource->load_from_path(p_path) != OK) {
		return Variant();
	}
	resource->set_path(p_original_path.is_empty() ? p_path : p_original_path);
	return resource;
}

Error ResourceFormatSaverMNS::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<MnsStyleSheet> sheet = p_resource;
	if (sheet.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	return sheet->save_to_path(p_path);
}

bool ResourceFormatSaverMNS::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<MnsStyleSheet>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverMNS::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("mns");
	}
	return exts;
}
