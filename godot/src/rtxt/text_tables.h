#pragma once

// The process's named string tables (engine formats/rtxt/text_tables.h) for the shell's
// scripts: registration, the section lookups and the menu tokens' chain are the engine's; this
// class marshals the strings and keeps the bindings it hands out, so a registered table reads
// back as the same object.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "rtxt/rtxt_string_file.h"

namespace godot {

class ResourceRoot;

class TextTables : public Object {
	GDCLASS(TextTables, Object)

protected:
	static void _bind_methods();

public:
	// Register `p_table` under `p_name` (null unregisters; names without case). keyhelp.bin's
	// registration installs it as the binding labels' table (controls/key_strings.h), its
	// unregistration forgets it.
	static void register_table(const String &p_name, const Ref<RtxtStringFile> &p_table);
	// The table registered under `p_name`, or null.
	static Ref<RtxtStringFile> get_table(const String &p_name);
	// Every table unregistered, the override table forgotten and no root followed; the binding
	// labels' table goes with them.
	static void clear();
	// One RTXT .bin read off a mounted root; null with no root, no file or no parse.
	static Ref<RtxtStringFile> load_rtxt(const Ref<ResourceRoot> &p_root, const String &p_file);

	// The engine's lookups (text_tables.h): the miss marker's, the fallback's, the presence
	// test, the display form and the menu tokens' chain.
	static String lookup(const String &p_table, const String &p_section, const String &p_key);
	static String lookup_or(const String &p_table, const String &p_section, const String &p_key,
			const String &p_fallback);
	static bool has_text(const String &p_table, const String &p_section, const String &p_key);
	static String lookup_display(const String &p_table, const String &p_section,
			const String &p_key);
	static String menu_text(const String &p_key, const String &p_fallback);

	// Install the override table `p_root` serves and follow the root: every later mount of it
	// installs the table that mount left (follow_mount). A null root stops
	// following and clears the table.
	static void track_expansion_override(const Ref<ResourceRoot> &p_root);
	// A mount of `p_root` finished (ResourceRoot, ahead of its `mounted`): the followed root
	// installs the override table the mount left.
	static void follow_mount(const ResourceRoot &p_root);
};

} // namespace godot
