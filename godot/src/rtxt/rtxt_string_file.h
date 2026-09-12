#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/rtxt/rtxt.h>

namespace godot {

class ResourceRoot;

// Godot-facing wrapper around an RTXT localized string table (NovaLogic
// strings/*.bin). All format behavior lives in engine/formats/rtxt; this Resource adds the
// editor-facing CRUD surface, change signals, and Godot file I/O.
//
// Read lookups are case-insensitive. Mutating entries/sections keeps the section
// string-count totals and the lookup map in sync, and emits a signal so the
// editor can refresh without polling.
//
// Mutations also maintain the engine's grouping invariant — entries stay
// contiguous per section, because the original derives entry indices from
// accumulated section string_counts [orig: TextResource_FindEntryBySectionAndKey
// @ 0x75D250]. Loading stays faithful: an ungrouped file is loaded as-is (so
// unedited bytes save back exactly) and flagged via is_grouped().
class RtxtStringFile : public Resource {
	GDCLASS(RtxtStringFile, Resource)

private:
	opennova::rtxt::File file_;

	void _refresh();  // rebuild lookup + recompute section counts
	int _section_insert_index(uint32_t p_section_index) const;

protected:
	static void _bind_methods();

public:
	RtxtStringFile();

	// --- Read (case-insensitive key lookup) ---
	String get_string(const StringName &p_key) const;
	bool has_string(const StringName &p_key) const;
	// Engine-faithful section-scoped lookup (first matching section, first
	// matching key within its contiguous run) [orig: 0x75D250 / 0x75D1E0, see docs/interface/rtxt-strings-re.md].
	String get_string_in_section(const String &p_section, const StringName &p_key) const;
	bool has_string_in_section(const String &p_section, const StringName &p_key) const;
	int get_entry_count() const;

	// --- Section read ---
	int get_section_count() const;
	PackedStringArray get_section_names() const;
	String get_section_name(int p_section_index) const;
	PackedStringArray get_section_keys(int p_section_index) const;

	// --- Indexed entry access (editor table) ---
	String get_entry_text(int p_index) const;

	// --- Entry mutations ---
	// add_entry inserts at the end of the section's run and returns the new index.
	int add_entry(const String &p_key, const String &p_text, int p_section_index, const Vector2i &p_position);
	void remove_entry(int p_index);
	void set_entry_text(int p_index, const String &p_text);

	// --- Grouping invariant ---
	bool is_grouped() const;
	void normalize_grouping();

	// --- Section CRUD ---
	int add_section(const String &p_name);

	// --- I/O ---
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;

	// --- Snapshot (for editor undo/redo): faithful byte image of the table. ---
	PackedByteArray to_byte_array() const;
	Error load_from_byte_array(const PackedByteArray &p_bytes);
	// The per-mission MissionText table off a mounted root: <mission>.bin when
	// it exists, else medmssn.bin (the engine's runtime_boot resolver — the
	// fallback fires only when the mission .bin does not EXIST; a present but
	// unparseable file loads to nothing). Null when neither yields a table.
	static Ref<RtxtStringFile> load_mission_table(const Ref<ResourceRoot> &root,
			const String &mission_file_basename);

	// --- Hotkey helpers ---
	static String strip_hotkey(const String &p_text);

	// --- Engine-faithful lookup policy re-exports (engine/formats/rtxt owns
	// the marker text and the ordering; see rtxt.h for the witnesses) ---
	// "??section:key??" — the visible marker a failed lookup renders.
	static String format_miss_marker(const String &p_section, const String &p_key);
	// Override-table-first section lookup: a hit in p_override_table (the
	// active expansion's text bin) wins outright; a miss in both returns the
	// miss marker. Either table may be null.
	static String lookup_with_override(const Ref<RtxtStringFile> &p_override_table,
			const Ref<RtxtStringFile> &p_table, const String &p_section,
			const String &p_key);

	// --- The process-wide "Keys" table (engine controls::key_strings.h) ---
	// install_key_strings copies THIS table in as the binding-label table
	// every key name, modifier prefix and separator resolves through (retail's
	// g_TextKeyHelp: keyhelp.bin, loaded at boot beside gametext.bin; the
	// engine's KeyHelp_GetStringWithFallback lookup). The shell installs the
	// mounted keyhelp.bin at the boot point that registers the text tables;
	// clear_key_strings forgets it (every label falls back to its literal).
	void install_key_strings() const;
	static void clear_key_strings();
	static bool has_key_strings();

	// Native access for the loader/saver.
	void set_native(const opennova::rtxt::File &p_file);
	const opennova::rtxt::File &get_native() const { return file_; }
};

} // namespace godot
