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
#include "scr/scr.h"

#include <cstdint>
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

// Verify a candidate decrypt result actually parses as a MUS file. Cheap
// signal: SCR0 magic + version 0x00000100 at offset 4 + a sane chunk count.
// MusFileHeader is 44 bytes so we need at least that much data.
static bool peek_is_valid_mus(const uint8_t *bytes, int64_t size) {
	if (!peek_is_scr0(bytes, size)) {
		return false;
	}
	if (size < 44) {
		return false;
	}
	// Little-endian uint32 at offset 4: SCR0 version stamp. Witnessed
	// 0x00000100 across menumus.bin, gamemus.bin, and the BHD variants.
	const uint32_t version = (uint32_t)bytes[4] |
			((uint32_t)bytes[5] << 8) |
			((uint32_t)bytes[6] << 16) |
			((uint32_t)bytes[7] << 24);
	if (version != 0x00000100u) {
		return false;
	}
	// chunk_count at offset 8: typically 1, but cap loosely at 16 to
	// reject obvious garbage from a wrong-key decrypt.
	const uint32_t chunk_count = (uint32_t)bytes[8] |
			((uint32_t)bytes[9] << 8) |
			((uint32_t)bytes[10] << 16) |
			((uint32_t)bytes[11] << 24);
	if (chunk_count == 0 || chunk_count > 16) {
		return false;
	}
	return true;
}

// Try each known SCR key against `in`, writing the decrypted result into
// `out` when one yields a buffer that parses as a SCR0 MUS file. Returns
// true on success and resizes `out` to the actual decrypted size.
//
// First pass: regular SCR-wrapped form ("SCR\xVV" + ciphertext). Falls
// through cleanly when the input has no SCR header (scr_decrypt_buf returns
// -1).
// Second pass: MUS-style retail-disk form (no leading magic; cipher applied
// to the entire on-disk buffer; output gets a literal "SCR0" prepended by
// scr_decrypt_mus). Witnessed: dfvas!Scr_DecryptBuffer @ 0x4cc250 with
// SCR_KEY_JO_DFX2.
//
// Both passes use the stronger peek_is_valid_mus check (SCR0 magic +
// version stamp + chunk_count sanity). The bare SCR0 magic is unreliable
// for pass 2 because scr_decrypt_mus always prepends literal "SCR0" no
// matter which key was used, so all three candidate decrypts trip the
// magic check; we have to look further into the header to identify the
// correct key.
static bool try_decrypt_to_scr0(const PackedByteArray &in, PackedByteArray &out) {
	if (in.size() < 4) {
		return false;
	}
	const uint32_t keys[] = { SCR_KEY_DEFAULT, SCR_KEY_JO_DFX2, SCR_KEY_SHADERS };

	// Pass 1: SCR-wrapped (header-prefixed). scr_decrypt_buf strips a
	// 4-byte header so the max possible payload is in.size() - 4.
	{
		const size_t cap = (size_t)(in.size() - 4);
		PackedByteArray tmp;
		tmp.resize((int64_t)cap);
		for (uint32_t key : keys) {
			size_t sz = cap;
			int rc = scr_decrypt_buf(in.ptr(), (size_t)in.size(), tmp.ptrw(), &sz, key);
			if (rc == 0 && peek_is_valid_mus(tmp.ptr(), (int64_t)sz)) {
				tmp.resize((int64_t)sz);
				out = tmp;
				return true;
			}
		}
	}

	// Pass 2: MUS-style (no header). scr_decrypt_mus prepends "SCR0" so a
	// successful decrypt yields out_size == in.size() + 4 with magic intact.
	for (uint32_t key : keys) {
		uint8_t *mus_out = nullptr;
		size_t mus_out_size = 0;
		int rc = scr_decrypt_mus(in.ptr(), (size_t)in.size(), &mus_out, &mus_out_size, key);
		if (rc != 0 || mus_out == nullptr) {
			continue;
		}
		bool ok = peek_is_valid_mus(mus_out, (int64_t)mus_out_size);
		if (ok) {
			out.resize((int64_t)mus_out_size);
			memcpy(out.ptrw(), mus_out, mus_out_size);
		}
		scr_free_buffer(mus_out);
		if (ok) {
			return true;
		}
	}
	return false;
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
	PackedByteArray peek = fa->get_buffer(4);
	if (peek_is_scr0(peek.ptr(), peek.size())) {
		return "NovaMusicScript";
	}
	// Encrypted MUS still gets recognised when size is in the typical range.
	// The actual SCR-decrypt magic check only runs in _load (it requires the
	// full file). Coarse size filter avoids claiming arbitrary cc.bin etc.
	if (total_size > 64 && total_size < 200000) {
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
