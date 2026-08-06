// MUS .bin ResourceFormatLoader/Saver.
//
// .bin clash hazard: cc.bin (BHD/JO config) and other arbitrary .bin files
// also use this extension. We disambiguate by peeking the first 4 bytes for
// the SCR0 magic after the shared generic VFS payload decoder has run. Files
// that do not decode to SCR0 return an empty resource type and our loader skips
// them cleanly so other addons can claim them.

#include "mus_resource_format.h"

#include "nova_music_script.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "mus/mus.h"

#include <cstdint>

using namespace godot;

PackedStringArray MusResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray exts;
	exts.push_back("bin");
	return exts;
}

bool MusResourceFormatLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("NovaMusicScript") || p_type == StringName("Resource");
}

static bool peek_is_scr0(const uint8_t *bytes, int64_t size) {
	return size >= 4 && bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'R' && bytes[3] == '0';
}

static bool is_plain_mus(const PackedByteArray &bytes) {
	return peek_is_scr0(bytes.ptr(), bytes.size()) && mus_validate(bytes.ptr(), (size_t)bytes.size()) == 0;
}

String MusResourceFormatLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() != "bin") {
		return String();
	}
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) {
		return String();
	}
	if (is_plain_mus(bytes)) {
		return "NovaMusicScript";
	}
	return String();
}

Variant MusResourceFormatLoader::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) {
		UtilityFunctions::push_warning("MusResourceFormatLoader: cannot open ", p_path);
		return Variant();
	}

	if (bytes.is_empty()) {
		UtilityFunctions::push_warning("MusResourceFormatLoader: empty file ", p_path);
		return Variant();
	}

	if (!is_plain_mus(bytes)) {
		UtilityFunctions::push_warning(
				"MusResourceFormatLoader: ", p_path,
				" is not a valid SCR0 MUS file after generic payload decode");
		return Variant();
	}

	Ref<NovaMusicScript> script;
	script.instantiate();
	script->load_from_decrypted_bytes(bytes, p_path);
	// take_over_path replaces any stale cache entry at this path so re-loads
	// pick up our newly-instantiated script. Mirrors NovaSbfBank.
	script->take_over_path(p_path);
	return script;
}

Error MusResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t /*p_flags*/) {
	Ref<NovaMusicScript> ms = p_resource;
	if (ms.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	PackedByteArray bytes = ms->get_raw_file_bytes();
	if (bytes.is_empty()) {
		return ERR_FILE_CANT_OPEN;
	}
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) {
		return ERR_CANT_OPEN;
	}
	fa->store_buffer(bytes);
	return OK;
}

bool MusResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<NovaMusicScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray MusResourceFormatSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray exts;
	if (_recognize(p_resource)) {
		exts.push_back("bin");
	}
	return exts;
}
