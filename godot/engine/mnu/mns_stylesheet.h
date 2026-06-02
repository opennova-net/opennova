#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mns/mns.h>

namespace godot {

// Godot-facing wrapper around an MNS stylesheet: a table of %VAR% -> value
// substitutions referenced by .mnu menus (e.g. %DEF_FONTNAME%, %TRIM_COLOR%).
// All format behavior lives in libs/mns. Keys are case-insensitive (stored
// uppercase).
class MnsStyleSheet : public Resource {
	GDCLASS(MnsStyleSheet, Resource)

private:
	mns::StyleSheet sheet_;

protected:
	static void _bind_methods();

public:
	// --- Lookup / substitution ---
	String get_variable(const String &p_name) const;
	bool has_variable(const String &p_name) const;
	String substitute(const String &p_text) const;
	int get_variable_count() const;
	Dictionary get_variables() const;

	// --- Mutation (emits changed) ---
	void set_variable(const String &p_name, const String &p_value);
	void remove_variable(const String &p_name);
	void set_variables(const Dictionary &p_variables);
	void clear();

	// --- I/O ---
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_byte_array() const;
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;

	// Native access for the loader/saver.
	void set_native(const mns::StyleSheet &p_sheet);
	const mns::StyleSheet &get_native() const { return sheet_; }
};

} // namespace godot
