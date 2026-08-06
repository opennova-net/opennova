#include "resource_index/nova_resource_index.h"

#include <godot_cpp/classes/project_settings.hpp>

using namespace godot;

namespace {

bool has_virtual_scheme(const String &path) {
	return path.begins_with("res://") || path.begins_with("user://");
}

} // namespace

void NovaResourceIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("scan", "path"), &NovaResourceIndex::scan);
	ClassDB::bind_method(D_METHOD("clear"), &NovaResourceIndex::clear);
	ClassDB::bind_method(D_METHOD("get_resource_files", "kind"), &NovaResourceIndex::get_resource_files);
	ClassDB::bind_method(D_METHOD("get_root_dir"), &NovaResourceIndex::get_root_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaResourceIndex::get_last_error);
}

String NovaResourceIndex::to_native_path(const String &path) {
	if (has_virtual_scheme(path)) {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (settings != nullptr) {
			return settings->globalize_path(path);
		}
	}
	return path;
}

Dictionary NovaResourceIndex::file_entry_to_dictionary(const opennova::ResourceFileEntry &entry) {
	Dictionary out;
	out["kind"] = String(entry.kind.c_str());
	out["path"] = String(entry.path.c_str());
	out["logical_name"] = String(entry.logical_name.c_str());
	out["display_name"] = String(entry.display_name.c_str());
	out["relative_path"] = String(entry.relative_path.c_str());
	out["source_type"] = String(entry.source_type.c_str());
	out["archive_path"] = String(entry.archive_path.c_str());
	// Godot ints are 64-bit signed; real resource sizes never approach the range
	// where the uint64->int64 narrowing would matter.
	out["size_bytes"] = static_cast<int64_t>(entry.size_bytes);
	out["modified_time"] = static_cast<int64_t>(entry.modified_time);
	return out;
}

Error NovaResourceIndex::scan(const String &path) {
	// The editor's resource browser indexes loose files only; the PFF archives are a
	// runtime concern. See opennova::VfsMountMode.
	const String native_path = to_native_path(path);
	return index_.scan(native_path.utf8().get_data(), std::string(), opennova::VfsMountMode::LooseOnly)
			? OK
			: ERR_CANT_OPEN;
}

void NovaResourceIndex::clear() {
	index_.clear();
}

Array NovaResourceIndex::get_resource_files(const String &kind) const {
	Array out;
	for (const opennova::ResourceFileEntry &entry : index_.resource_files(kind.utf8().get_data())) {
		out.push_back(file_entry_to_dictionary(entry));
	}
	return out;
}

String NovaResourceIndex::get_root_dir() const {
	return String(index_.root_dir().c_str());
}

String NovaResourceIndex::get_last_error() const {
	return String(index_.last_error().c_str());
}
