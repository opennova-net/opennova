#include "mns_stylesheet.h"

#include "util/nova_string_convert.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace godot;

namespace {

using opennova::to_gd;
using opennova::to_std;

} // namespace

void MnsStyleSheet::_refresh() {
	const mns::EvaluationResult result = doc_.evaluate();
	sheet_ = result.sheet;
	evaluation_diagnostics_ = result.diagnostics;
	runtime_valid_ = result.success;
}

String MnsStyleSheet::get_variable(const String &p_name) const {
	return to_gd(sheet_.get(to_std(p_name)));
}

bool MnsStyleSheet::has_variable(const String &p_name) const {
	return sheet_.has(to_std(p_name));
}

// [orig: NapiXML_ExpandVariablesInText @ 0x63a000]  ARCHITECTURE DIVERGENCE: the
// original expands %VAR% over the whole raw .mnu byte buffer BEFORE the XML parse;
// the reimpl substitutes per-field, post-parse, on every consumed field the
// engine's whole-buffer pass would cover -- colors, fonts, textures, and literal
// text (see nova_mnu_builder.cpp substitute_var). Matching for the shipped corpus;
// the remaining gap is shell-supplied variables in non-themed fields (D-MNU-1,
// ADR 0005).
String MnsStyleSheet::substitute(const String &p_text) const {
	return to_gd(sheet_.substitute(to_std(p_text)));
}

int MnsStyleSheet::get_variable_count() const {
	return static_cast<int>(sheet_.variables.size());
}

Dictionary MnsStyleSheet::get_variables() const {
	Dictionary out;
	for (const auto &kv : sheet_.variables) {
		out[to_gd(kv.first)] = to_gd(kv.second);
	}
	return out;
}

void MnsStyleSheet::set_variable(const String &p_name, const String &p_value) {
	const std::string name = to_std(p_name);
	const std::string value = to_std(p_value);
	std::string error;
	if (doc_.find_entry(name) >= 0) {
		if (sheet_.get(name) == value) {
			return; // no-op: equal value, no dirty flip
		}
		if (!doc_.set_value(name, value, &error)) {
			UtilityFunctions::push_warning("MnsStyleSheet: set_variable failed: ", error.c_str());
			return;
		}
	} else {
		if (!doc_.add_define(name, value, -1, std::string(), &error)) {
			UtilityFunctions::push_warning("MnsStyleSheet: set_variable failed: ", error.c_str());
			return;
		}
	}
	_refresh();
	emit_changed();
}

bool MnsStyleSheet::remove_variable(const String &p_name) {
	std::string error;
	if (!doc_.remove_define(to_std(p_name), &error)) {
		if (!error.empty()) {
			UtilityFunctions::push_warning(
					"MnsStyleSheet: remove_variable failed: ", error.c_str());
		}
		return false; // unchanged: no dirty signal
	}
	_refresh();
	emit_changed();
	return true;
}

void MnsStyleSheet::set_variables(const Dictionary &p_variables) {
	doc_ = mns::Document();
	const Array keys = p_variables.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		std::string error;
		if (!doc_.add_define(to_std(key), to_std(p_variables[key]), -1, std::string(), &error)) {
			UtilityFunctions::push_warning("MnsStyleSheet: set_variables skipped '", key, "': ", error.c_str());
		}
	}
	_refresh();
	emit_changed();
}

void MnsStyleSheet::clear() {
	if (doc_.nodes().empty() && sheet_.variables.empty()) {
		return;
	}
	doc_ = mns::Document();
	_refresh();
	emit_changed();
}

Array MnsStyleSheet::get_entries() const {
	Array out;
	for (const mns::Document::Entry &entry : doc_.entries()) {
		Dictionary row;
		row["name"] = to_gd(entry.name);
		row["value"] = to_gd(entry.value);
		row["raw_value"] = to_gd(entry.raw_value);
		row["inline_comment"] = to_gd(entry.inline_comment);
		row["line"] = entry.line;
		row["node_index"] = entry.node_index;
		row["multiline"] = entry.multiline;
		row["group"] = entry.group;
		PackedStringArray comments;
		for (const std::string &c : entry.preceding_comments) {
			comments.append(to_gd(c));
		}
		row["preceding_comments"] = comments;
		out.append(row);
	}
	return out;
}

int MnsStyleSheet::get_entry_count() const {
	return static_cast<int>(doc_.entries().size());
}

Array MnsStyleSheet::get_diagnostics() const {
	Array out;
	for (const mns::Diagnostic &d : doc_.diagnostics()) {
		Dictionary row;
		row["line"] = d.line;
		row["severity"] = (d.severity == mns::Severity::Error) ? "error" : "warning";
		row["code"] = to_gd(d.code);
		row["message"] = to_gd(d.message);
		out.append(row);
	}
	return out;
}

Array MnsStyleSheet::get_evaluation_diagnostics() const {
	Array out;
	for (const mns::Diagnostic &d : evaluation_diagnostics_) {
		Dictionary row;
		row["line"] = d.line;
		row["severity"] = (d.severity == mns::Severity::Error) ? "error" : "warning";
		row["code"] = to_gd(d.code);
		row["message"] = to_gd(d.message);
		out.append(row);
	}
	return out;
}

String MnsStyleSheet::get_source_text() const {
	return to_gd(doc_.source_text());
}

void MnsStyleSheet::set_source_text(const String &p_text) {
	const std::string text = to_std(p_text);
	if (text == doc_.source_text()) {
		return; // no-op
	}
	doc_.set_source_text(text);
	_refresh();
	emit_changed();
}

bool MnsStyleSheet::add_variable(const String &p_name, const String &p_value, const String &p_after_name) {
	int before_node = -1;
	if (!p_after_name.is_empty()) {
		const int after = doc_.find_entry(to_std(p_after_name));
		if (after < 0) {
			UtilityFunctions::push_warning("MnsStyleSheet: add_variable: no variable named '", p_after_name, "'");
			return false;
		}
		before_node = doc_.entries()[static_cast<size_t>(after)].node_index + 1;
	}
	std::string error;
	if (!doc_.add_define(to_std(p_name), to_std(p_value), before_node, std::string(), &error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: add_variable failed: ", error.c_str());
		return false;
	}
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::rename_variable(const String &p_old_name, const String &p_new_name) {
	std::string error;
	if (!doc_.rename_define(to_std(p_old_name), to_std(p_new_name), &error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: rename_variable failed: ", error.c_str());
		return false;
	}
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::move_variable(const String &p_name, int p_to_entry_index) {
	const std::vector<mns::Document::Entry> entries = doc_.entries();
	const int before_node = (p_to_entry_index < 0 || p_to_entry_index >= static_cast<int>(entries.size()))
			? static_cast<int>(doc_.nodes().size())
			: entries[static_cast<size_t>(p_to_entry_index)].node_index;
	std::string error;
	if (!doc_.move_define(to_std(p_name), before_node, &error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: move_variable failed: ", error.c_str());
		return false;
	}
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::set_inline_comment(const String &p_name, const String &p_comment) {
	std::string error;
	if (!doc_.set_inline_comment(to_std(p_name), to_std(p_comment), &error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: set_inline_comment failed: ", error.c_str());
		return false;
	}
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::is_valid_variable_name(const String &p_name) const {
	return mns::Document::is_valid_name(to_std(p_name));
}

bool MnsStyleSheet::is_valid_variable_value(const String &p_value) const {
	return mns::Document::is_valid_value(to_std(p_value));
}

Error MnsStyleSheet::load_from_bytes(const PackedByteArray &p_bytes) {
	// Keep the lossless document load permissive so malformed source remains
	// repairable in the editor. Runtime callers must check is_runtime_valid().
	doc_ = mns::Document::parse(reinterpret_cast<const char *>(p_bytes.ptr()),
			static_cast<size_t>(p_bytes.size()));
	_refresh();
	return OK;
}

PackedByteArray MnsStyleSheet::to_byte_array() const {
	const std::vector<uint8_t> bytes = doc_.serialize();
	PackedByteArray out;
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Error MnsStyleSheet::load_from_path(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}
	const PackedByteArray packed = file->get_buffer(file->get_length());
	file->close();
	return load_from_bytes(packed);
}

Error MnsStyleSheet::save_to_path(const String &p_path) const {
	const PackedByteArray packed = to_byte_array();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}

void MnsStyleSheet::set_native(const mns::StyleSheet &p_sheet) {
	doc_ = mns::Document();
	// Sorted for determinism (unordered_map iteration order is arbitrary),
	// matching the legacy mns::write canonical dump.
	std::vector<std::string> keys;
	keys.reserve(p_sheet.variables.size());
	for (const auto &kv : p_sheet.variables) {
		keys.push_back(kv.first);
	}
	std::sort(keys.begin(), keys.end());
	for (const std::string &key : keys) {
		std::string error;
		doc_.add_define(key, p_sheet.variables.at(key), -1, std::string(), &error);
	}
	_refresh();
}

void MnsStyleSheet::set_native_document(const mns::Document &p_doc) {
	doc_ = p_doc;
	_refresh();
}

void MnsStyleSheet::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_variable", "name"), &MnsStyleSheet::get_variable);
	ClassDB::bind_method(D_METHOD("has_variable", "name"), &MnsStyleSheet::has_variable);
	ClassDB::bind_method(D_METHOD("substitute", "text"), &MnsStyleSheet::substitute);
	ClassDB::bind_method(D_METHOD("get_variable_count"), &MnsStyleSheet::get_variable_count);
	ClassDB::bind_method(D_METHOD("get_variables"), &MnsStyleSheet::get_variables);

	ClassDB::bind_method(D_METHOD("set_variable", "name", "value"), &MnsStyleSheet::set_variable);
	ClassDB::bind_method(D_METHOD("remove_variable", "name"), &MnsStyleSheet::remove_variable);
	ClassDB::bind_method(D_METHOD("set_variables", "variables"), &MnsStyleSheet::set_variables);
	ClassDB::bind_method(D_METHOD("clear"), &MnsStyleSheet::clear);

	ClassDB::bind_method(D_METHOD("get_entries"), &MnsStyleSheet::get_entries);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &MnsStyleSheet::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_diagnostics"), &MnsStyleSheet::get_diagnostics);
	ClassDB::bind_method(D_METHOD("is_runtime_valid"), &MnsStyleSheet::is_runtime_valid);
	ClassDB::bind_method(D_METHOD("get_evaluation_diagnostics"),
			&MnsStyleSheet::get_evaluation_diagnostics);
	ClassDB::bind_method(D_METHOD("get_source_text"), &MnsStyleSheet::get_source_text);
	ClassDB::bind_method(D_METHOD("set_source_text", "text"), &MnsStyleSheet::set_source_text);
	ClassDB::bind_method(D_METHOD("add_variable", "name", "value", "after_name"), &MnsStyleSheet::add_variable, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("rename_variable", "old_name", "new_name"), &MnsStyleSheet::rename_variable);
	ClassDB::bind_method(D_METHOD("move_variable", "name", "to_entry_index"), &MnsStyleSheet::move_variable);
	ClassDB::bind_method(D_METHOD("set_inline_comment", "name", "comment"), &MnsStyleSheet::set_inline_comment);
	ClassDB::bind_method(D_METHOD("is_valid_variable_name", "name"), &MnsStyleSheet::is_valid_variable_name);
	ClassDB::bind_method(D_METHOD("is_valid_variable_value", "value"), &MnsStyleSheet::is_valid_variable_value);

	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &MnsStyleSheet::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_byte_array"), &MnsStyleSheet::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &MnsStyleSheet::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MnsStyleSheet::save_to_path);
}
