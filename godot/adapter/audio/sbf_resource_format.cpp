#include "sbf_resource_format.h"

#include "nova_sbf_bank.h"

#include <godot_cpp/classes/file_access.hpp>

using namespace godot;

PackedStringArray SbfResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("sbf");
	return exts;
}

bool SbfResourceFormatLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaSbfBank") || p_type == StringName("Resource");
}

String SbfResourceFormatLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "sbf") {
		return "NovaSbfBank";
	}
	return "";
}

Variant SbfResourceFormatLoader::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<NovaSbfBank> bank;
	bank.instantiate();
	bank->load_from_path(p_path);
	// take_over_path == set_path(path, take_over=true) in editor builds:
	// replaces any stale cache entry at the same path so re-imports and hot
	// reloads pick up our newly-loaded bank.
	bank->take_over_path(p_path);
	return bank;
}

Error SbfResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t /*p_flags*/) {
	Ref<NovaSbfBank> bank = p_resource;
	if (bank.is_null()) return ERR_INVALID_PARAMETER;

	if (!bank->is_dirty()) {
		// Lossless passthrough: copy original source bytes through.
		PackedByteArray bytes = bank->get_raw_file_bytes();
		if (bytes.is_empty()) return ERR_FILE_CANT_OPEN;
		Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
		if (fa.is_null()) return ERR_CANT_OPEN;
		fa->store_buffer(bytes);
		return OK;
	}

	// Re-encode path (Phase D / SBF F2): walk current entry table + override
	// PCM map, rebuild the byte stream from scratch.
	PackedByteArray out_bytes;
	Error err = bank->build_encoded_bytes(out_bytes);
	if (err != OK) return err;
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) return ERR_CANT_OPEN;
	fa->store_buffer(out_bytes);
	bank->clear_dirty();
	return OK;
}

bool SbfResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaSbfBank>(p_resource.ptr()) != nullptr;
}

PackedStringArray SbfResourceFormatSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("sbf");
	}
	return exts;
}
