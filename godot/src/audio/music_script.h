#pragma once

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

#include <formats/mus/mus.h>

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
	// The script from its model alone (formats/mus: mus_encode_file over the parsed scripts,
	// ADR 0003), never the bytes it was read from, each program as the game reads it: its editor
	// debug section (source path, names, line table) written empty, which the game never reads.
	// Refused when the written bytes would not read back as the model in what the game reads (a
	// model the encoder refuses: mus.h, mus_encode_file).
	Error save_to_path(const String &p_path) const;

	int get_script_count() const;
	String get_source_path() const { return source_path; }
	String get_default_script_name() const;
	PackedStringArray get_section_names(const StringName &p_script_name) const;

	// Reusable text codec surface.
	String get_decompiled_text(const StringName &p_script_name);
	String get_decompiled_text_with_bank(const StringName &p_script_name,
			const Ref<SbfBank> &p_bank);

	const opennova::mus::MusScript *raw_script(const String &p_name) const;
	PackedByteArray get_raw_file_bytes() const { return _file_bytes; }

protected:
	static void _bind_methods();

private:
	String source_path;
	// The decrypted SCR0 bytes the script was read from (get_raw_file_bytes); the parser
	// owns independent copies of its chunk data, and the saver never writes these.
	PackedByteArray _file_bytes;
	opennova::mus::MusFile _mf;
	bool _opened = false;
};

} // namespace godot

