// MUS .bin ResourceFormatLoader/Saver.
//
// .bin clash hazard: cc.bin (BHD/JO config) and other arbitrary .bin files
// also use this extension. We disambiguate by peeking the first 4 bytes for
// the SCR0 magic (or by trying SCR decryption with each known key when the
// peek fails). Files that match neither path return an empty resource type
// and our loader skips them cleanly so other addons can claim them.

#include "mus_resource_format.h"

#include "nova_music_script.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "mus/mus.h"
#include "mus/mus_sniff.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

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

// Decrypt/recognize a MUS .bin in any on-disk form (plaintext SCR0, SCR-wrapped,
// or the headerless retail-disk form), writing the plaintext SCR0 bytes into
// `out`. Delegates to libs/mus mus_decode_to_scr0 so the loader, the VFS decode
// path, and the resource index share one definition of "is this a MUS".
static bool try_decrypt_to_scr0(const PackedByteArray &in, PackedByteArray &out) {
	if (in.size() <= 0) {
		return false;
	}
	uint8_t *dec = nullptr;
	size_t dec_size = 0;
	if (!mus_decode_to_scr0(in.ptr(), (size_t)in.size(), &dec, &dec_size) || dec == nullptr) {
		return false;
	}
	out.resize((int64_t)dec_size);
	memcpy(out.ptrw(), dec, dec_size);
	free(dec);
	return true;
}

String MusResourceFormatLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() != "bin") {
		return String();
	}
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::READ);
	if (fa.is_null()) {
		return String();
	}
	int64_t total_size = fa->get_length();
	// Real content check (decrypt-probe + header validation) so we don't claim
	// arbitrary .bin (e.g. cc.bin). Shared with the VFS + index via mus_sniff.
	PackedByteArray bytes = fa->get_buffer(total_size);
	if (mus_is_mus(bytes.ptr(), bytes.size())) {
		return "NovaMusicScript";
	}
	return String();
}

Variant MusResourceFormatLoader::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::READ);
	if (fa.is_null()) {
		UtilityFunctions::printerr("MusResourceFormatLoader: cannot open ", p_path);
		return Variant();
	}
	PackedByteArray bytes = fa->get_buffer(fa->get_length());
	fa.unref();

	if (bytes.is_empty()) {
		UtilityFunctions::printerr("MusResourceFormatLoader: empty file ", p_path);
		return Variant();
	}

	// Direct path: file starts with SCR0 magic, so it's an already-decrypted
	// MUS body. Hand straight to load_from_decrypted_bytes.
	if (!peek_is_scr0(bytes.ptr(), bytes.size())) {
		// Try SCR-wrapped: decrypt and check inner magic.
		PackedByteArray decrypted;
		if (!try_decrypt_to_scr0(bytes, decrypted)) {
			UtilityFunctions::printerr(
					"MusResourceFormatLoader: ", p_path,
					" is neither a raw SCR0 MUS file nor a wrapped MUS that decrypts with any known key");
			return Variant();
		}
		bytes = decrypted;
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
	// Saving back the decrypted SCR0 form. Re-encrypting to match the
	// encrypted-on-disk format requires libs/scr encrypt support which lands
	// with the editor spec.
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
