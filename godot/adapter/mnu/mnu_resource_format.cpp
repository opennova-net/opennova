#include "mnu_resource_format.h"

#include "nova_mnu_document.h"

using namespace godot;

PackedStringArray ResourceFormatLoaderMNU::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("mnu");
	return exts;
}

bool ResourceFormatLoaderMNU::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaMnuDocument") || p_type == StringName("Resource");
}

String ResourceFormatLoaderMNU::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "mnu") {
		return "NovaMnuDocument";
	}
	return "";
}

Variant ResourceFormatLoaderMNU::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<NovaMnuDocument> resource;
	resource.instantiate();
	if (resource->load_from_path(p_path) != OK) {
		return Variant();
	}
	resource->set_path(p_original_path.is_empty() ? p_path : p_original_path);
	return resource;
}

Error ResourceFormatSaverMNU::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<NovaMnuDocument> doc = p_resource;
	if (doc.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	return doc->save_to_path(p_path);
}

bool ResourceFormatSaverMNU::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaMnuDocument>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverMNU::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("mnu");
	}
	return exts;
}
