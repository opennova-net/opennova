#ifndef NOVA_MUSIC_SCRIPT_H
#define NOVA_MUSIC_SCRIPT_H

// MUS interactive-music script wrapper. The underlying parser is libs/mus
// (mus_open_memory), which mirrors the engine's
// AudioVM_LoadScriptFile @ 0x00672D20 (Jointops.exe) script-load path. Held as a
// godot::Resource so a .bin script file appears in the editor's resource
// browser alongside SBF banks.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "mus/mus.h"

namespace godot {

class NovaSbfBank;

class NovaMusicScript : public Resource {
	GDCLASS(NovaMusicScript, Resource)

public:
	NovaMusicScript();
	~NovaMusicScript();

	// Populated by MusResourceFormatLoader; safe to call directly from script
	// for tests / fixtures that bypass the loader. The bytes must already be
	// in their decrypted SCR0 form (the loader strips the SCR layer first).
	void load_from_decrypted_bytes(const PackedByteArray &bytes, const String &p_source);

	int get_script_count() const;
	String get_source_path() const { return source_path; }
	String get_default_script_name() const;
	Array get_scripts() const;
	bool has_script(const StringName &p_name) const;
	PackedStringArray get_script_names() const;
	PackedStringArray get_section_names(const StringName &p_script_name) const;
	PackedStringArray get_intrinsic_names() const;
	String get_decompiled_text(const StringName &p_script_name);

	// Names-aware decompile: when a NovaSbfBank is supplied, the emitter
	// substitutes real entry names for the synthesised "sound_N"
	// placeholders so the displayed text reads "play GAMINT" instead of
	// "play sound_1". When p_bank is null, falls back to the names-less
	// variant so callers can pass through unconditionally.
	String get_decompiled_text_with_bank(const StringName &p_script_name,
			const Ref<NovaSbfBank> &p_bank);

	// Phase F4: text -> bytecode bridge for the music editor's Script mode.
	// Returns a Dictionary with keys:
	//   "rc"        : int  -- 0 on success, negative on parse/emit failure
	//   "bytecode"  : PackedByteArray -- raw chunk bytes (code only, no SCR0/MU01 wrapper)
	//   "file_bytes": PackedByteArray -- full decrypted SCR0/MU01 bytes
	//   "err_line"  : int  -- 1-based source line for the diagnostic, 0 on success
	//   "err_col"   : int  -- 1-based source column for the diagnostic, 0 on success
	//   "err_msg"   : String -- empty on success
	// Output references aren't viable across the GDExtension boundary; using a
	// Dictionary keeps the GDScript signature trivial (`var d = compile_text(text)`).
	Dictionary compile_text(const String &p_text);

	// Phase F5: install fresh bytecode produced by compile_text without
	// re-parsing the SCR0 wrapper. Replaces the default script's `code`
	// buffer in-place; the saver passes the rewritten file bytes through.
	void set_compiled_bytecode(const PackedByteArray &p_bytecode);
	void set_compiled_file_bytes(const PackedByteArray &p_file_bytes);

	// Internal access for the upcoming Director (Phase G).
	const MusScript *raw_script(const String &p_name) const;
	PackedByteArray get_raw_file_bytes() const { return _file_bytes; }

protected:
	static void _bind_methods();

private:
	String source_path;
	// _file_bytes is kept around solely so the saver can passthrough the
	// original decrypted SCR0 form. The parser (mus_open_memory) memcpys
	// the chunk data into freshly-malloc'd buffers, so MusFile is
	// independent of _file_bytes for runtime access.
	PackedByteArray _file_bytes;
	MusFile _mf;
	bool _opened = false;
};

} // namespace godot

#endif // NOVA_MUSIC_SCRIPT_H
