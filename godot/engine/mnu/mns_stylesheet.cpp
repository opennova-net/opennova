#include "mns_stylesheet.h"

#include "util/nova_string_convert.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using namespace godot;

namespace {

using opennova::to_gd;
using opennova::to_std;

std::string to_upper(const std::string &s) {
	std::string out = s;
	for (char &c : out) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return out;
}

} // namespace

String MnsStyleSheet::get_variable(const String &p_name) const {
	return to_gd(sheet_.get(to_std(p_name)));
}

bool MnsStyleSheet::has_variable(const String &p_name) const {
	return sheet_.has(to_std(p_name));
}

// [orig: NapiXML_ExpandVariablesInText @ 0x63a000]  ARCHITECTURE DIVERGENCE: the
// original expands %VAR% over the whole raw .mnu byte buffer BEFORE the XML parse
// (covers text, POSITION/SIZE and arbitrary attributes); the reimpl substitutes
// per-field, post-parse, only on color/texture/font strings (see
// nova_mnu_builder.cpp substitute_var). Matching for the shipped corpus, which only
// uses %VAR% in color/font. See notes/mnu/divergence-backlog.md.
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
	sheet_.variables[to_upper(to_std(p_name))] = to_std(p_value);
	emit_changed();
}

void MnsStyleSheet::remove_variable(const String &p_name) {
	sheet_.variables.erase(to_upper(to_std(p_name)));
	emit_changed();
}

void MnsStyleSheet::set_variables(const Dictionary &p_variables) {
	sheet_.variables.clear();
	const Array keys = p_variables.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		sheet_.variables[to_upper(to_std(key))] = to_std(p_variables[key]);
	}
	emit_changed();
}

void MnsStyleSheet::clear() {
	sheet_.variables.clear();
	emit_changed();
}

Error MnsStyleSheet::load_from_bytes(const PackedByteArray &p_bytes) {
	mns::StyleSheet parsed;
	std::string error;
	const char *data = reinterpret_cast<const char *>(p_bytes.ptr());
	if (!mns::parse(data, static_cast<size_t>(p_bytes.size()), parsed, error)) {
		UtilityFunctions::printerr("MnsStyleSheet: parse failed: ", error.c_str());
		return ERR_FILE_CORRUPT;
	}
	sheet_ = std::move(parsed);
	return OK;
}

PackedByteArray MnsStyleSheet::to_byte_array() const {
	std::vector<uint8_t> bytes;
	std::string error;
	PackedByteArray out;
	if (!mns::write(sheet_, bytes, error)) {
		UtilityFunctions::printerr("MnsStyleSheet: serialize failed: ", error.c_str());
		return out;
	}
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
	sheet_ = p_sheet;
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

	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &MnsStyleSheet::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_byte_array"), &MnsStyleSheet::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &MnsStyleSheet::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MnsStyleSheet::save_to_path);
}
