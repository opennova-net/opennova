#include "resource_index/nova_resource_root.h"

#include "cbin/cbin_asset_lookup.h"
#include "fnt/nova_fnt_resource.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace godot;

namespace {

bool case_insensitive_less(const String &a, const String &b) {
	return a.to_lower() < b.to_lower();
}

bool is_flat_filename(const String &name) {
	return name.find("/") == -1 && name.find("\\") == -1;
}

} // namespace

void NovaResourceRoot::_bind_methods() {
	ClassDB::bind_static_method("NovaResourceRoot", D_METHOD("is_valid_root", "path"), &NovaResourceRoot::is_valid_root);
	ClassDB::bind_method(D_METHOD("set_root_dir", "path"), &NovaResourceRoot::set_root_dir);
	ClassDB::bind_method(D_METHOD("mount_game", "path", "expansion"), &NovaResourceRoot::mount_game, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_root_dir"), &NovaResourceRoot::get_root_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaResourceRoot::get_last_error);
	ClassDB::bind_method(D_METHOD("clear"), &NovaResourceRoot::clear);
	ClassDB::bind_method(D_METHOD("resolve_file", "name"), &NovaResourceRoot::resolve_file);
	ClassDB::bind_method(D_METHOD("list_files", "suffix"), &NovaResourceRoot::list_files, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_file_entries", "suffix"), &NovaResourceRoot::list_file_entries, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("has_file", "name"), &NovaResourceRoot::has_file);
	ClassDB::bind_method(D_METHOD("read_file", "name"), &NovaResourceRoot::read_file);
	ClassDB::bind_method(D_METHOD("load_texture", "name"), &NovaResourceRoot::load_texture);
	ClassDB::bind_method(D_METHOD("load_font", "name"), &NovaResourceRoot::load_font);
}

bool NovaResourceRoot::has_virtual_scheme(const String &path) {
	return path.begins_with("res://") || path.begins_with("user://");
}

String NovaResourceRoot::to_native_path(const String &path) {
	if (has_virtual_scheme(path)) {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (settings != nullptr) {
			return settings->globalize_path(path);
		}
	}
	return path;
}

String NovaResourceRoot::normalize_dir(const String &path) {
	return to_native_path(path.strip_edges()).replace("\\", "/").rstrip("/");
}

String NovaResourceRoot::lookup_name(const String &name) {
	return name.strip_edges().replace("\\", "/").get_file();
}

Dictionary NovaResourceRoot::file_entry_to_dictionary(const opennova::ResourceFileEntry &entry) {
	Dictionary out;
	out["kind"] = String(entry.kind.c_str());
	out["path"] = String(entry.path.c_str());
	out["logical_name"] = String(entry.logical_name.c_str());
	out["display_name"] = String(entry.display_name.c_str());
	out["relative_path"] = String(entry.relative_path.c_str());
	out["source_type"] = String(entry.source_type.c_str());
	out["archive_path"] = String(entry.archive_path.c_str());
	return out;
}

bool NovaResourceRoot::is_valid_root(const String &path) {
	const String clean = normalize_dir(path);
	if (clean.is_empty()) {
		return false;
	}
	if (!DirAccess::dir_exists_absolute(clean)) {
		return false;
	}
	OS *os = OS::get_singleton();
	if (os != nullptr && clean.begins_with(os->get_user_data_dir().replace("\\", "/").rstrip("/"))) {
		return false;
	}
	return true;
}

Error NovaResourceRoot::set_root_dir(const String &path) {
	return mount_game(path, String());
}

Error NovaResourceRoot::mount_game(const String &path, const String &expansion) {
	// The resolver's per-session caches are keyed to the previous root; drop them so a
	// new (or re-scanned) resource directory is read fresh. scan_root() in the editor
	// routes through here too, so a rescan picks up on-disk edits.
	opennova::clear_texture_resolver_caches();
	const String clean = normalize_dir(path);
	if (clean.is_empty()) {
		root_dir_ = String();
		last_error_ = "Resource directory is empty";
		return ERR_INVALID_PARAMETER;
	}
	if (!is_valid_root(clean)) {
		root_dir_ = String();
		last_error_ = "Resource directory does not exist or is not allowed: " + clean;
		return ERR_DOES_NOT_EXIST;
	}
	root_dir_ = clean;
	if (!index_.scan(clean.utf8().get_data(), expansion.utf8().get_data())) {
		root_dir_ = String();
		last_error_ = String(index_.last_error().c_str());
		return ERR_CANT_OPEN;
	}
	last_error_ = String();
	return OK;
}

String NovaResourceRoot::get_root_dir() const {
	return root_dir_;
}

String NovaResourceRoot::get_last_error() const {
	return last_error_;
}

void NovaResourceRoot::clear() {
	root_dir_ = String();
	last_error_ = String();
	opennova::clear_texture_resolver_caches();
	index_.clear();
}

String NovaResourceRoot::resolve_file(const String &name) {
	last_error_ = String();
	if (root_dir_.is_empty()) {
		last_error_ = "Resource directory is empty";
		return String();
	}
	const String file = lookup_name(name);
	if (file.is_empty()) {
		last_error_ = "Resource filename is empty";
		return String();
	}
	if (!is_flat_filename(name.strip_edges())) {
		last_error_ = "Resource lookup requires a flat filename: " + name;
		return String();
	}
	const String wanted = file.to_lower();

	Ref<DirAccess> dir = DirAccess::open(root_dir_);
	if (dir.is_null()) {
		last_error_ = "Resource directory cannot be opened: " + root_dir_;
		return String();
	}

	String found;
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (!dir->current_is_dir() && entry.to_lower() == wanted) {
			if (!found.is_empty()) {
				dir->list_dir_end();
				last_error_ = "Duplicate resource filename: " + file;
				return String();
			}
			found = entry;
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
	return found.is_empty() ? String() : root_dir_.path_join(found);
}

PackedStringArray NovaResourceRoot::list_files(const String &suffix) const {
	PackedStringArray out;
	if (root_dir_.is_empty()) {
		return out;
	}
	const String suffix_lower = suffix.to_lower();
	for (const opennova::ResourceFileEntry &entry : index_.resource_files("*")) {
		const String logical_name(entry.logical_name.c_str());
		if (suffix_lower.is_empty() || logical_name.to_lower().ends_with(suffix_lower)) {
			const String path(entry.path.c_str());
			out.push_back(path.is_empty() ? logical_name : path);
		}
	}
	std::sort(out.ptrw(), out.ptrw() + out.size(), case_insensitive_less);
	return out;
}

Array NovaResourceRoot::list_file_entries(const String &suffix) const {
	Array out;
	if (root_dir_.is_empty()) {
		return out;
	}
	const String suffix_lower = suffix.to_lower();
	std::vector<opennova::ResourceFileEntry> entries;
	for (const opennova::ResourceFileEntry &entry : index_.resource_files("*")) {
		const String logical_name(entry.logical_name.c_str());
		if (suffix_lower.is_empty() || logical_name.to_lower().ends_with(suffix_lower)) {
			entries.push_back(entry);
		}
	}
	std::sort(entries.begin(), entries.end(), [](const opennova::ResourceFileEntry &a, const opennova::ResourceFileEntry &b) {
		return String(a.logical_name.c_str()).to_lower() < String(b.logical_name.c_str()).to_lower();
	});
	for (const opennova::ResourceFileEntry &entry : entries) {
		out.push_back(file_entry_to_dictionary(entry));
	}
	return out;
}

bool NovaResourceRoot::has_file(const String &name) const {
	if (root_dir_.is_empty() || name.strip_edges().is_empty() || !is_flat_filename(name.strip_edges())) {
		return false;
	}
	std::vector<uint8_t> bytes;
	return index_.read_file(lookup_name(name).utf8().get_data(), bytes);
}

PackedByteArray NovaResourceRoot::read_file(const String &name) const {
	PackedByteArray out;
	if (root_dir_.is_empty() || name.strip_edges().is_empty() || !is_flat_filename(name.strip_edges())) {
		return out;
	}
	std::vector<uint8_t> bytes;
	if (!index_.read_file(lookup_name(name).utf8().get_data(), bytes)) {
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Ref<Texture2D> NovaResourceRoot::load_texture(const String &name) const {
	if (root_dir_.is_empty()) {
		return Ref<Texture2D>();
	}
	const String file = lookup_name(name);
	if (file.is_empty()) {
		return Ref<Texture2D>();
	}

	Ref<Texture2D> loose = opennova::load_texture_from_dir(root_dir_, file);
	if (loose.is_valid()) {
		return loose;
	}

	for (const String &candidate : opennova::texture_candidate_filenames(file)) {
		const PackedByteArray bytes = read_file(candidate);
		if (bytes.is_empty()) {
			continue;
		}
		Ref<Texture2D> tex = opennova::load_texture_from_bytes(candidate, bytes);
		if (tex.is_valid()) {
			return tex;
		}
	}
	return Ref<Texture2D>();
}

Ref<Resource> NovaResourceRoot::load_font(const String &name) const {
	if (root_dir_.is_empty()) {
		return Ref<Resource>();
	}
	const String file = lookup_name(name);
	if (file.is_empty()) {
		return Ref<Resource>();
	}
	const PackedByteArray bytes = read_file(file.get_extension().to_lower() == "fnt" ? file : file + String(".fnt"));
	if (!bytes.is_empty()) {
		Ref<NovaFntResource> font;
		font.instantiate();
		if (font->load_from_bytes(bytes) == OK) {
			return font;
		}
	}
	return cbin_internal::find_font_by_name(file, root_dir_);
}
