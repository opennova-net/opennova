#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mns/mns.h>
#include <mns/mns_document.h>

#include <vector>

namespace godot {

// Godot-facing wrapper around an MNS stylesheet: a table of %VAR% -> value
// substitutions referenced by .mnu menus (e.g. %DEF_FONTNAME%, %TRIM_COLOR%).
// All format behavior lives in libs/mns. Keys are case-insensitive (stored
// uppercase in the flattened view).
//
// Document-backed (ADR 0014): the source of truth is a lossless mns::Document
// (comments, grouping, alignment, conditionals, authored case all survive a
// load -> save), and the StyleSheet the runtime substitutes through is its
// separately evaluated cache. Lookup/substitution serve that evaluated view;
// mutations and serialization go through the document, so
// to_byte_array()/save_to_path() are byte-faithful for untouched files and
// minimal-delta after edits. Runtime callers reject is_runtime_valid()==false;
// the editor deliberately keeps that source open for repair.
class MnsStyleSheet : public Resource {
	GDCLASS(MnsStyleSheet, Resource)

private:
	mns::Document doc_;
	mns::StyleSheet sheet_; // successful/partial runtime evaluation cache
	std::vector<mns::Diagnostic> evaluation_diagnostics_;
	bool runtime_valid_ = true;

	void _refresh();

protected:
	static void _bind_methods();

public:
	// --- Lookup / substitution (flattened view) ---
	String get_variable(const String &p_name) const;
	bool has_variable(const String &p_name) const;
	String substitute(const String &p_text) const;
	int get_variable_count() const;
	Dictionary get_variables() const;

	// --- Mutation (each successful mutation emits changed exactly once;
	//     failures and no-ops emit nothing) ---
	void set_variable(const String &p_name, const String &p_value);
	bool remove_variable(const String &p_name);
	void set_variables(const Dictionary &p_variables);
	void clear();

	// --- Document view (ONED authoring surface) ---
	// Ordered active defines: {name, value, raw_value, inline_comment, line,
	// node_index, multiline, group, preceding_comments}.
	Array get_entries() const;
	// Number of entries (duplicates listed separately; get_variable_count is
	// the collapsed flatten count).
	int get_entry_count() const;
	// {line, severity: "error"|"warning", code, message}
	Array get_diagnostics() const;
	// Runtime evaluation is strict even though document loading stays permissive
	// for the repairable editor workflow.
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

	// Native access. The flat-sheet setter rebuilds a canonical document from
	// the map (documented lossy); the document accessors are the lossless path.
	void set_native(const mns::StyleSheet &p_sheet);
	const mns::StyleSheet &get_native() const { return sheet_; }
	void set_native_document(const mns::Document &p_doc);
	const mns::Document &get_native_document() const { return doc_; }
};

} // namespace godot
