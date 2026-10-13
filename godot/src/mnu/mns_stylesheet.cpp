#include "mnu/mns_stylesheet.h"
#include "resource_index/resource_root.h"
#include "util/data_format.h"

#include "util/string_convert.h"

#include <base/io/strutil.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace godot;

namespace {

using opennova::to_gd;
using opennova::to_std;

} // namespace

void MnsStyleSheet::_refresh() {
	sheet_ = list_.sheet();
}

int MnsStyleSheet::_find(const std::string &p_name) const {
	for (size_t i = 0; i < list_.nodes.size(); ++i)
		if (opennova::strutil::iequals(list_.nodes[i].name, p_name)) return static_cast<int>(i);
	return -1;
}

void MnsStyleSheet::_read(const std::string &p_text) {
	list_ = opennova::mns::KeyValueList();
	stops_.clear();
	const opennova::mns::ReadResult read = opennova::mns::parse_key_value_buffer(p_text.data(), p_text.size(), list_);
	runtime_valid_ = read.status == opennova::mns::ReadStatus::Read;
	if (!runtime_valid_)
		stops_.push_back({opennova::mns::line_at_offset(p_text.data(), p_text.size(), read.offset),
		                  read.status == opennova::mns::ReadStatus::Hangs, opennova::mns::stop_message(read.status)});
	_refresh();
}

Ref<MnsStyleSheet> MnsStyleSheet::load_shell(const Ref<ResourceRoot> &p_root) {
	if (p_root.is_null()) return Ref<MnsStyleSheet>();
	const opennova::menu::ShellStyle style = opennova::menu::load_shell_style(
	        [&](const std::string &name, std::string &bytes) {
		        const PackedByteArray packed = p_root->read_file(to_gd(name));
		        if (packed.is_empty()) return false;
		        bytes.assign(reinterpret_cast<const char *>(packed.ptr()), size_t(packed.size()));
		        return true;
	        });
	return style.any_present() ? from_shell_style(style) : Ref<MnsStyleSheet>();
}

Ref<MnsStyleSheet> MnsStyleSheet::from_shell_style(const opennova::menu::ShellStyle &p_style) {
	Ref<MnsStyleSheet> sheet;
	sheet.instantiate();
	sheet->sheet_ = p_style.list.sheet();
	sheet->runtime_valid_ = true;
	for (const opennova::menu::ShellSheetRead &read : p_style.sheets) {
		if (!read.present || read.read.status == opennova::mns::ReadStatus::Read) continue;
		sheet->runtime_valid_ = false;
		sheet->stops_.push_back({read.stopped_line, read.read.status == opennova::mns::ReadStatus::Hangs,
		                         read.name + ": " + opennova::mns::stop_message(read.read.status)});
	}
	return sheet;
}

String MnsStyleSheet::get_variable(const String &p_name) const {
	return to_gd(sheet_.get(to_std(p_name)));
}

bool MnsStyleSheet::has_variable(const String &p_name) const {
	return sheet_.has(to_std(p_name));
}

// A text-level %VAR% helper for tools and tests, not the runtime path: the original
// expands %VAR% over the whole raw .mnu buffer before the XML parse [orig:
// NapiXML_ExpandVariablesInText @ 0x63a000]; the menus resolve whole field values
// through MenuFrameCompiler::resolve_var (D-MNU-1, ADR 0005; docs/mnu/menu-re.md).
String MnsStyleSheet::substitute(const String &p_text) const {
	return to_gd(sheet_.substitute(to_std(p_text)));
}

const std::unordered_map<std::string, std::string> &MnsStyleSheet::variables() const {
	return sheet_.variables;
}

void MnsStyleSheet::set_variable(const String &p_name, const String &p_value) {
	const std::string name = to_std(p_name);
	const std::string value = to_std(p_value);
	if (!opennova::mns::is_valid_name(name) || !opennova::mns::is_valid_value(value)) {
		UtilityFunctions::push_warning("MnsStyleSheet: set_variable: '", p_name, "' = '", p_value,
		                               "' is no name and value the reader reads back");
		return;
	}
	const int at = _find(name);
	if (at >= 0) {
		if (list_.nodes[size_t(at)].value == value) return; // no-op: equal value, no dirty flip
		list_.nodes[size_t(at)].value = value;
	} else {
		list_.nodes.push_back({name, value});
	}
	_refresh();
	emit_changed();
}

bool MnsStyleSheet::remove_variable(const String &p_name) {
	const int at = _find(to_std(p_name));
	if (at < 0) return false; // unchanged: no dirty signal
	list_.nodes.erase(list_.nodes.begin() + at);
	_refresh();
	emit_changed();
	return true;
}

void MnsStyleSheet::set_variables(const Dictionary &p_variables) {
	list_ = opennova::mns::KeyValueList();
	const Array keys = p_variables.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		const std::string name = to_std(key);
		const std::string value = to_std(p_variables[key]);
		if (!opennova::mns::is_valid_name(name) || !opennova::mns::is_valid_value(value) || _find(name) >= 0) {
			UtilityFunctions::push_warning("MnsStyleSheet: set_variables skipped '", key, "'");
			continue;
		}
		list_.nodes.push_back({name, value});
	}
	_refresh();
	emit_changed();
}

void MnsStyleSheet::clear() {
	if (list_.nodes.empty() && sheet_.variables.empty()) {
		return;
	}
	list_ = opennova::mns::KeyValueList();
	_refresh();
	emit_changed();
}

Array MnsStyleSheet::get_entries() const {
	Array out;
	for (const opennova::mns::KeyValue &node : list_.nodes) {
		Dictionary row;
		row["name"] = to_gd(node.name);
		row["value"] = to_gd(node.value);
		out.append(row);
	}
	return out;
}

int MnsStyleSheet::get_entry_count() const {
	return static_cast<int>(list_.nodes.size());
}

Array MnsStyleSheet::get_evaluation_diagnostics() const {
	Array out;
	for (const Stop &stop : stops_) {
		Dictionary row;
		row["line"] = stop.line;
		row["severity"] = "error";
		row["code"] = stop.hangs ? "hangs" : "stops";
		row["message"] = to_gd(stop.message);
		out.append(row);
	}
	return out;
}

String MnsStyleSheet::get_source_text() const {
	std::string text, error;
	if (!opennova::mns::write(list_, text, error)) return String();
	return to_gd(text);
}

void MnsStyleSheet::set_source_text(const String &p_text) {
	const opennova::mns::KeyValueList before = list_;
	const bool was_valid = runtime_valid_;
	_read(to_std(p_text));
	bool same = was_valid == runtime_valid_ && before.nodes.size() == list_.nodes.size();
	for (size_t i = 0; same && i < list_.nodes.size(); ++i)
		same = before.nodes[i].name == list_.nodes[i].name && before.nodes[i].value == list_.nodes[i].value;
	if (!same) emit_changed();
}

bool MnsStyleSheet::add_variable(const String &p_name, const String &p_value, const String &p_after_name) {
	const std::string name = to_std(p_name);
	const std::string value = to_std(p_value);
	if (!opennova::mns::is_valid_name(name) || !opennova::mns::is_valid_value(value)) {
		UtilityFunctions::push_warning("MnsStyleSheet: add_variable: '", p_name, "' = '", p_value,
		                               "' is no name and value the reader reads back");
		return false;
	}
	if (_find(name) >= 0) {
		UtilityFunctions::push_warning("MnsStyleSheet: add_variable: '", p_name, "' is already a variable");
		return false;
	}
	size_t at = list_.nodes.size();
	if (!p_after_name.is_empty()) {
		const int after = _find(to_std(p_after_name));
		if (after < 0) {
			UtilityFunctions::push_warning("MnsStyleSheet: add_variable: no variable named '", p_after_name, "'");
			return false;
		}
		at = size_t(after) + 1;
	}
	list_.nodes.insert(list_.nodes.begin() + std::ptrdiff_t(at), opennova::mns::KeyValue{name, value});
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::rename_variable(const String &p_old_name, const String &p_new_name) {
	const std::string name = to_std(p_new_name);
	const int at = _find(to_std(p_old_name));
	const int other = _find(name);
	if (at < 0 || !opennova::mns::is_valid_name(name) || (other >= 0 && other != at)) {
		UtilityFunctions::push_warning("MnsStyleSheet: rename_variable: cannot rename '", p_old_name, "' to '",
		                               p_new_name, "'");
		return false;
	}
	list_.nodes[size_t(at)].name = name;
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::move_variable(const String &p_name, int p_to_entry_index) {
	const int at = _find(to_std(p_name));
	if (at < 0) {
		UtilityFunctions::push_warning("MnsStyleSheet: move_variable: no variable named '", p_name, "'");
		return false;
	}
	const opennova::mns::KeyValue node = list_.nodes[size_t(at)];
	list_.nodes.erase(list_.nodes.begin() + at);
	const size_t to = p_to_entry_index < 0 ? list_.nodes.size()
	                                       : std::min(size_t(p_to_entry_index), list_.nodes.size());
	list_.nodes.insert(list_.nodes.begin() + std::ptrdiff_t(to), node);
	_refresh();
	emit_changed();
	return true;
}

bool MnsStyleSheet::is_valid_variable_name(const String &p_name) const {
	return opennova::mns::is_valid_name(to_std(p_name));
}

bool MnsStyleSheet::is_valid_variable_value(const String &p_value) const {
	return opennova::mns::is_valid_value(to_std(p_value));
}

Error MnsStyleSheet::load_from_bytes(const PackedByteArray &p_bytes) {
	// What the game reads of the bytes, up to where it stops (is_runtime_valid: all of it).
	_read(std::string(reinterpret_cast<const char *>(p_bytes.ptr()), static_cast<size_t>(p_bytes.size())));
	return OK;
}

PackedByteArray MnsStyleSheet::to_byte_array() const {
	std::string text, error;
	if (!opennova::mns::write(list_, text, error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: the list has no written form: ", error.c_str());
		return PackedByteArray();
	}
	return to_packed_bytes(std::vector<uint8_t>(text.begin(), text.end()));
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
	std::string text, error;
	if (!opennova::mns::write(list_, text, error)) {
		UtilityFunctions::push_warning("MnsStyleSheet: the list has no written form: ", error.c_str());
		return ERR_INVALID_DATA;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(to_packed_bytes(std::vector<uint8_t>(text.begin(), text.end())));
	file->close();
	return OK;
}

void MnsStyleSheet::_bind_methods() {
	ClassDB::bind_static_method("MnsStyleSheet", D_METHOD("load_shell", "root"), &MnsStyleSheet::load_shell);
	ClassDB::bind_method(D_METHOD("get_variable", "name"), &MnsStyleSheet::get_variable);
	ClassDB::bind_method(D_METHOD("has_variable", "name"), &MnsStyleSheet::has_variable);
	ClassDB::bind_method(D_METHOD("substitute", "text"), &MnsStyleSheet::substitute);

	ClassDB::bind_method(D_METHOD("set_variable", "name", "value"), &MnsStyleSheet::set_variable);
	ClassDB::bind_method(D_METHOD("remove_variable", "name"), &MnsStyleSheet::remove_variable);
	ClassDB::bind_method(D_METHOD("clear"), &MnsStyleSheet::clear);

	ClassDB::bind_method(D_METHOD("get_entries"), &MnsStyleSheet::get_entries);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &MnsStyleSheet::get_entry_count);
	ClassDB::bind_method(D_METHOD("is_runtime_valid"), &MnsStyleSheet::is_runtime_valid);
	ClassDB::bind_method(D_METHOD("get_evaluation_diagnostics"),
			&MnsStyleSheet::get_evaluation_diagnostics);
	ClassDB::bind_method(D_METHOD("get_source_text"), &MnsStyleSheet::get_source_text);
	ClassDB::bind_method(D_METHOD("set_source_text", "text"), &MnsStyleSheet::set_source_text);
	ClassDB::bind_method(D_METHOD("add_variable", "name", "value", "after_name"), &MnsStyleSheet::add_variable, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("rename_variable", "old_name", "new_name"), &MnsStyleSheet::rename_variable);
	ClassDB::bind_method(D_METHOD("move_variable", "name", "to_entry_index"), &MnsStyleSheet::move_variable);
	ClassDB::bind_method(D_METHOD("is_valid_variable_name", "name"), &MnsStyleSheet::is_valid_variable_name);
	ClassDB::bind_method(D_METHOD("is_valid_variable_value", "value"), &MnsStyleSheet::is_valid_variable_value);

	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &MnsStyleSheet::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_byte_array"), &MnsStyleSheet::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &MnsStyleSheet::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MnsStyleSheet::save_to_path);
}
