#include "model/anim_def_document.h"

#include "util/string_convert.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <cstring>
#include <string>

using namespace godot;
using namespace opennova::adm;

namespace {

std::string native_path(const String &p_path) {
	String global = p_path;
	if (ProjectSettings::get_singleton() != nullptr) {
		global = ProjectSettings::get_singleton()->globalize_path(p_path);
	}
	return opennova::to_std(global);
}

} // namespace

void AnimDefDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &AnimDefDocument::load_from_path);
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &AnimDefDocument::load_from_bytes);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &AnimDefDocument::save_to_path);
	ClassDB::bind_method(D_METHOD("to_bytes"), &AnimDefDocument::to_bytes);
	ClassDB::bind_method(D_METHOD("clear"), &AnimDefDocument::clear);
	ClassDB::bind_method(D_METHOD("add_row", "key", "variants"), &AnimDefDocument::add_row);
	ClassDB::bind_method(D_METHOD("get_row_count"), &AnimDefDocument::get_row_count);
	ClassDB::bind_method(D_METHOD("get_row_key", "row"), &AnimDefDocument::get_row_key);
	ClassDB::bind_method(D_METHOD("get_row_variants", "row"), &AnimDefDocument::get_row_variants);
	ClassDB::bind_method(D_METHOD("find_row", "key"), &AnimDefDocument::find_row);
	ClassDB::bind_method(D_METHOD("get_source_path"), &AnimDefDocument::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &AnimDefDocument::get_last_error);
}

Error AnimDefDocument::load_from_path(const String &p_path) {
	last_error_ = "";
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_path);
	if (bytes.is_empty()) {
		last_error_ = "cannot read " + p_path;
		return ERR_FILE_CANT_READ;
	}
	const Error err = load_from_bytes(bytes);
	if (err == OK) {
		source_path_ = p_path;
	}
	return err;
}

Error AnimDefDocument::load_from_bytes(const PackedByteArray &p_bytes) {
	last_error_ = "";
	entries_.clear();
	source_path_ = "";
	AdmFile parsed = {};
	if (adm_parse_buffer(reinterpret_cast<const char *>(p_bytes.ptr()), static_cast<size_t>(p_bytes.size()),
				&parsed) != 0) {
		last_error_ = "not a .adm table";
		return ERR_FILE_CORRUPT;
	}
	entries_.assign(parsed.entries, parsed.entries + parsed.count);
	adm_free(&parsed);
	return OK;
}

Error AnimDefDocument::save_to_path(const String &p_path) {
	last_error_ = "";
	AdmFile file = {};
	file.entries = entries_.data();
	file.count = entries_.size();
	if (adm_write(native_path(p_path).c_str(), &file) != 0) {
		last_error_ = "cannot write " + p_path;
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

PackedByteArray AnimDefDocument::to_bytes() {
	last_error_ = "";
	PackedByteArray out;
	AdmFile file = {};
	file.entries = entries_.data();
	file.count = entries_.size();
	std::string text;
	if (adm_write_buffer(&file, text) != 0) {
		last_error_ = "the table cannot be written";
		return out;
	}
	out.resize(static_cast<int64_t>(text.size()));
	if (!text.empty()) {
		std::memcpy(out.ptrw(), text.data(), text.size());
	}
	return out;
}

void AnimDefDocument::clear() {
	entries_.clear();
	source_path_ = "";
}

bool AnimDefDocument::add_row(const String &p_key, const PackedStringArray &p_variants) {
	last_error_ = "";
	const std::string key = opennova::to_std(p_key);
	if (key.rfind("anim_", 0) != 0 || key.size() >= sizeof(AdmEntry::key)) {
		last_error_ = "a row key is anim_<name>, under 64 characters: " + p_key;
		return false;
	}
	if (p_variants.is_empty() || p_variants.size() > ADM_MAX_VARIANTS) {
		last_error_ = "a row carries 1 to 8 clip names: " + p_key;
		return false;
	}
	AdmEntry entry = {};
	std::strncpy(entry.key, key.c_str(), sizeof(entry.key) - 1);
	entry.variant_count = 0;
	for (int i = 0; i < p_variants.size(); ++i) {
		const std::string variant = opennova::to_std(p_variants[i]);
		if (variant.empty() || variant.size() >= sizeof(entry.variants[0]) || variant.find('"') != std::string::npos) {
			last_error_ = "a clip name is 1 to 63 characters without quotes: " + p_key;
			return false;
		}
		std::strncpy(entry.variants[entry.variant_count], variant.c_str(), sizeof(entry.variants[0]) - 1);
		++entry.variant_count;
	}
	entries_.push_back(entry);
	return true;
}

String AnimDefDocument::get_row_key(int p_row) const {
	if (p_row < 0 || static_cast<size_t>(p_row) >= entries_.size()) return String();
	return String(entries_[p_row].key);
}

PackedStringArray AnimDefDocument::get_row_variants(int p_row) const {
	PackedStringArray out;
	if (p_row < 0 || static_cast<size_t>(p_row) >= entries_.size()) return out;
	const AdmEntry &entry = entries_[p_row];
	for (size_t v = 0; v < entry.variant_count; ++v) {
		out.append(String(entry.variants[v]));
	}
	return out;
}

int AnimDefDocument::find_row(const String &p_key) const {
	const std::string key = opennova::to_std(p_key.to_lower());
	for (size_t i = 0; i < entries_.size(); ++i) {
		std::string row = entries_[i].key;
		for (char &c : row) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (row == key) return static_cast<int>(i);
	}
	return -1;
}
