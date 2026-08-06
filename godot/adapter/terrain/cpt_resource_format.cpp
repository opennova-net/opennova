#include "cpt_resource_format.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>

using namespace godot;

PackedStringArray ResourceFormatLoaderCPT::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("cpt");
	return exts;
}

bool ResourceFormatLoaderCPT::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaDataFile") || p_type == StringName("Resource");
}

String ResourceFormatLoaderCPT::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "cpt")
		return "NovaDataFile";
	return "";
}

Variant ResourceFormatLoaderCPT::_load(const String &p_path, const String &p_original_path,
                                        bool p_use_sub_threads, int32_t p_cache_mode) const {
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) return Variant();

	Ref<NovaDataFile> res;
	res.instantiate();
	res->set_data(bytes);
	return res;
}
