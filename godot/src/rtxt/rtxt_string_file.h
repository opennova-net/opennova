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

#include <memory>

#include <formats/rtxt/rtxt.h>
#include <runtime/hud/game_text_lookup.h> // the engine's ONE game-text seam

#include "util/string_convert.h"

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
// accumulated section string_counts (engine: formats/rtxt/rtxt.h). Loading stays faithful: an ungrouped file is loaded as-is (so
// unedited bytes save back exactly) and flagged via is_grouped().
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
	// Engine-faithful section-scoped lookup (first matching section, first
	// matching key within its contiguous run) (engine: formats/rtxt/rtxt.h).
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
	// Override-table-first section lookup (rtxt::lookup_with_override): a hit in
	// the process's text-override table (rtxt::text_override, the active
	// expansion's text bin) wins outright; a miss in both returns the miss
	// marker. The table may be null (the override table alone answers).
	static String lookup_with_override(const Ref<RtxtStringFile> &p_table,
			const String &p_section, const String &p_key);

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

	// --- The process's text-override table (retail's g_TextOverrideTable) ---
	// install_text_override copies THIS table in as the engine's one override
	// table (rtxt::set_text_override), which every game-text lookup (text_or,
	// has_text, game_text_lookup below) and the key labels search before the
	// table they were asked (D-NET-675); the shell installs the mounted
	// expansion's table (Strings) and clear_text_override forgets it.
	void install_text_override() const;
	static void clear_text_override();
	// The installed table itself (null: none), for the engine lookups that take
	// the override table as an argument (the mission catalog's titles, the cfg
	// defaults' texts).
	static std::shared_ptr<const opennova::rtxt::File> installed_text_override();
	// The section lookup (rtxt::find_with_text_override) over p_table: the
	// installed override's entry first, then the table's, its bytes as the
	// table holds them (the game draws, joins and sends a table's bytes);
	// false with no table or no entry.
	static bool find_text(const RtxtStringFile &p_table, const char *p_section, const char *p_key,
			std::string &r_out);
	// That text, or p_fallback for no table or no entry (GameText_GetStringWithFallback's
	// form, the HUD's and the menu tokens'); whether it finds one.
	static String text_or(const Ref<RtxtStringFile> &p_table, const String &p_section,
			const String &p_key, const String &p_fallback);
	static bool has_text(const Ref<RtxtStringFile> &p_table, const String &p_section,
			const String &p_key);

	// Native access for the loader/saver.
	const opennova::rtxt::File &get_native() const { return file_; }
};

// The engine's game-text seam (hud/game_text_lookup.h) bound over a string
// table: a present key answers its value, an absent key (or no table) the
// fallback. The one factory every engine text argument comes from.
inline opennova::hud::GameTextLookup game_text_lookup(const Ref<RtxtStringFile> &table) {
	return [table](const char *section, const char *key, const char *fallback) {
		// With a table the override table answers first, then the table (no
		// table answers the fallback before any search): GameText_GetString's
		// order, the engine's rtxt::find_with_text_override.
		std::string text;
		if (table.is_valid() && RtxtStringFile::find_text(*table.ptr(), section, key, text))
			return text;
		return std::string(fallback);
	};
}

} // namespace godot
