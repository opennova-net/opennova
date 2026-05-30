#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <rtxt/rtxt.h>

namespace godot {

// Godot-facing wrapper around an RTXT localized string table (NovaLogic
// strings/*.bin). All format behavior lives in libs/rtxt; this Resource adds the
// editor-facing CRUD surface, change signals, and Godot file I/O.
//
// Read lookups are case-insensitive. Mutating entries/sections keeps the section
// string-count totals and the lookup map in sync, and emits a signal so the
// editor can refresh without polling.
class RtxtStringFile : public Resource {
	GDCLASS(RtxtStringFile, Resource)

private:
	opennova::rtxt::File file_;

	void _refresh();  // rebuild lookup + recompute section counts

protected:
	static void _bind_methods();

public:
	RtxtStringFile();

	// --- Read (case-insensitive key lookup) ---
	String get_string(const StringName &p_key) const;
	bool has_string(const StringName &p_key) const;
	Vector2i get_position(const StringName &p_key) const;
	int get_section_index_for_key(const StringName &p_key) const;
	PackedStringArray get_keys() const;
	int get_entry_count() const;

	// --- Section read ---
	int get_section_count() const;
	PackedStringArray get_section_names() const;
	String get_section_name(int p_section_index) const;
	int get_section_string_count(int p_section_index) const;
	PackedStringArray get_section_keys(int p_section_index) const;

	// --- Indexed entry access (editor table) ---
	String get_entry_key(int p_index) const;
	String get_entry_text(int p_index) const;
	Vector2i get_entry_position(int p_index) const;
	int get_entry_section_index(int p_index) const;
	int find_entry_by_key(const StringName &p_key) const;

	// --- Entry mutations ---
	int add_entry(const String &p_key, const String &p_text, int p_section_index, const Vector2i &p_position);
	void remove_entry(int p_index);
	void set_entry_key(int p_index, const String &p_key);
	void set_entry_text(int p_index, const String &p_text);
	void set_entry_position(int p_index, const Vector2i &p_position);
	void set_entry_section_index(int p_index, int p_section_index);

	// --- Section CRUD ---
	int add_section(const String &p_name);
	// Removes section p_index. Entries in it are reassigned to p_reassign_to when
	// that is a valid section, otherwise they are removed. Higher section indices
	// (and entry references to them) shift down by one.
	void remove_section(int p_index, int p_reassign_to = -1);
	void rename_section(int p_index, const String &p_name);

	// --- I/O ---
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;
	void reset_empty();

	// --- Snapshot (for editor undo/redo): faithful byte image of the table. ---
	PackedByteArray to_byte_array() const;
	Error load_from_byte_array(const PackedByteArray &p_bytes);

	// --- Hotkey helpers ---
	static String strip_hotkey(const String &p_text);
	static Dictionary strip_hotkey_with_index(const String &p_text);

	// Native access for the loader/saver.
	void set_native(const opennova::rtxt::File &p_file);
	const opennova::rtxt::File &get_native() const { return file_; }
};

} // namespace godot
