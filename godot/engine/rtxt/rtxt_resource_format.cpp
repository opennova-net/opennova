#include "rtxt_resource_format.h"

#include "rtxt_string_file.h"
#include "util/nova_data_format.h"

#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

namespace {

// True when the file at p_path begins with the 'RTXT' magic.
bool has_rtxt_magic(const String &p_path) {
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes) || bytes.size() < 4) {
		return false;
	}
	return bytes[0] == 'R' && bytes[1] == 'T' && bytes[2] == 'X' && bytes[3] == 'T';
}

} // namespace

PackedStringArray ResourceFormatLoaderRTXT::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("bin");
	return exts;
}

bool ResourceFormatLoaderRTXT::_handles_type(const StringName &p_type) const {
	return p_type == StringName("RtxtStringFile") || p_type == StringName("Resource");
}

String ResourceFormatLoaderRTXT::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "bin" && has_rtxt_magic(p_path)) {
		return "RtxtStringFile";
	}
	return "";
}

Variant ResourceFormatLoaderRTXT::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	if (!has_rtxt_magic(p_path)) {
		return Variant();
	}
	Ref<RtxtStringFile> resource;
	resource.instantiate();
	if (resource->load_from_path(p_path) != OK) {
		return Variant();
	}
	resource->set_path(p_original_path.is_empty() ? p_path : p_original_path);
	return resource;
}

Error ResourceFormatSaverRTXT::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<RtxtStringFile> table = p_resource;
	if (table.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	return table->save_to_path(p_path);
}

bool ResourceFormatSaverRTXT::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<RtxtStringFile>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverRTXT::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("bin");
	}
	return exts;
}
