#ifndef NOVA_MUSIC_SCRIPT_H
#define NOVA_MUSIC_SCRIPT_H

// MUS interactive-music script wrapper. The underlying parser is
// engine/formats/mus (mus_open_memory), mirroring the engine's
// AudioVM_LoadScriptFile @ 0x00672D20 (Jointops.exe) script-load path.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "mus/mus.h"

namespace godot {

class SbfBank;

class MusicScript : public Resource {
	GDCLASS(MusicScript, Resource)

public:
	MusicScript();
	~MusicScript();

	// The bytes must already be in their decrypted SCR0 form.
	void load_from_decrypted_bytes(const PackedByteArray &bytes, const String &p_source);
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;

	int get_script_count() const;
	String get_source_path() const { return source_path; }
	String get_default_script_name() const;
	PackedStringArray get_section_names(const StringName &p_script_name) const;

	// Reusable text codec surface.
	String get_decompiled_text(const StringName &p_script_name);
	String get_decompiled_text_with_bank(const StringName &p_script_name,
			const Ref<SbfBank> &p_bank);
	Dictionary compile_text(const String &p_text);
	void set_compiled_bytecode(const PackedByteArray &p_bytecode);
	void set_compiled_file_bytes(const PackedByteArray &p_file_bytes);

	const MusScript *raw_script(const String &p_name) const;
	PackedByteArray get_raw_file_bytes() const { return _file_bytes; }

protected:
	static void _bind_methods();

private:
	String source_path;
	// Retained so save_to_path can pass through the decrypted SCR0 bytes. The
	// parser owns independent copies of its chunk data.
	PackedByteArray _file_bytes;
	MusFile _mf;
	bool _opened = false;
};

} // namespace godot

#endif // NOVA_MUSIC_SCRIPT_H
