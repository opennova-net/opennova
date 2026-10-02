#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/mns/mns.h>
#include <formats/mns/mns_document.h>
#include <runtime/menu/menu_style.h>

#include <unordered_map>
#include <string>
#include <vector>

namespace godot {

class ResourceRoot;

// Godot-facing wrapper around an MNS stylesheet: a table of %VAR% -> value
// substitutions referenced by .mnu menus (e.g. %DEF_FONTNAME%, %TRIM_COLOR%).
// All format behavior lives in engine/formats/mns. Keys are case-insensitive (stored
// uppercase in the flattened view).
//
// Document-backed (ADR 0014): the source of truth is a lossless opennova::mns::Document
// (comments, grouping, alignment, conditionals, authored case all survive a
// load -> save), and the StyleSheet the runtime substitutes through is the game's own
// read of it (Document::evaluate: what the retail reader reads, up to where it stops).
// Lookup/substitution serve that evaluated view; mutations and serialization go
// through the document, so to_byte_array()/save_to_path() are byte-faithful for
// untouched files and minimal-delta after edits.
//
// The runtime loads the shell's stylesheets with load_shell (menu_style.mns, then
// brand.mns onto it, as the game does): that sheet is a runtime view, its variables
// the merged list and its document empty (nothing to edit or save).
class MnsStyleSheet : public Resource {
	GDCLASS(MnsStyleSheet, Resource)

private:
	opennova::mns::Document doc_;
	opennova::mns::StyleSheet sheet_; // the game's read (a runtime view: the shell's merged list)
	std::vector<opennova::mns::Diagnostic> evaluation_diagnostics_;
	bool runtime_valid_ = true;

	void _refresh();

protected:
	static void _bind_methods();

public:
	// --- The shell's stylesheets (the runtime path) ---
	// menu_style.mns then brand.mns from the root, as the game loads them
	// (runtime/menu/menu_style.h); null when neither file is there.
	static Ref<MnsStyleSheet> load_shell(const Ref<ResourceRoot> &p_root);
	// The runtime view of a loaded shell style; native only.
	static Ref<MnsStyleSheet> from_shell_style(const opennova::menu::ShellStyle &p_style);

	// --- Lookup / substitution (flattened view) ---
	String get_variable(const String &p_name) const;
	bool has_variable(const String &p_name) const;
	String substitute(const String &p_text) const;
	// The parsed variable table (name -> value) as the native map the menu
	// frame compiler reads; NOT ClassDB-bound.
	const std::unordered_map<std::string, std::string> &variables() const;

	// --- Mutation (each successful mutation emits changed exactly once;
	//     failures and no-ops emit nothing) ---
	void set_variable(const String &p_name, const String &p_value);
	bool remove_variable(const String &p_name);
	void set_variables(const Dictionary &p_variables);
	void clear();

	// --- Document view (native format inspection) ---
	// Ordered active defines: {name, value, raw_value, inline_comment, line,
	// node_index, multiline, group, preceding_comments}.
	Array get_entries() const;
	// Number of entries (duplicates listed separately).
	int get_entry_count() const;
	// {line, severity: "error"|"warning", code, message}
	Array get_diagnostics() const;
	// True when the game reads the whole sheet (for a shell view: every sheet there);
	// false when its reader stops, or would stop responding, part way.
	bool is_runtime_valid() const { return runtime_valid_; }
	Array get_evaluation_diagnostics() const;
	String get_source_text() const;
	void set_source_text(const String &p_text);
	// p_after_name "" appends at the end of the document.
	bool add_variable(const String &p_name, const String &p_value, const String &p_after_name = String());
	bool rename_variable(const String &p_old_name, const String &p_new_name);
	// p_to_entry_index is a position in entries() order; clamped to the end.
	bool move_variable(const String &p_name, int p_to_entry_index);
	// Plain text or a "//"-prefixed comment; empty clears.
	bool set_inline_comment(const String &p_name, const String &p_comment);
	bool is_valid_variable_name(const String &p_name) const;
	bool is_valid_variable_value(const String &p_value) const;

	// --- I/O ---
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_byte_array() const;
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;

	// Native access.
	const opennova::mns::StyleSheet &get_native() const { return sheet_; }
};

} // namespace godot
