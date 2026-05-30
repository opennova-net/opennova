#include "resource_index/nova_resource_root.h"

#include "cbin/cbin_asset_lookup.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>

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
	ClassDB::bind_method(D_METHOD("get_root_dir"), &NovaResourceRoot::get_root_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaResourceRoot::get_last_error);
	ClassDB::bind_method(D_METHOD("clear"), &NovaResourceRoot::clear);
	ClassDB::bind_method(D_METHOD("resolve_file", "name"), &NovaResourceRoot::resolve_file);
	ClassDB::bind_method(D_METHOD("list_files", "suffix"), &NovaResourceRoot::list_files, DEFVAL(String()));
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
	Ref<DirAccess> dir = DirAccess::open(root_dir_);
	if (dir.is_null()) {
		return out;
	}
	const String suffix_lower = suffix.to_lower();
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (!dir->current_is_dir() && (suffix_lower.is_empty() || entry.to_lower().ends_with(suffix_lower))) {
			out.push_back(root_dir_.path_join(entry));
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
	std::sort(out.ptrw(), out.ptrw() + out.size(), case_insensitive_less);
	return out;
}

Ref<Texture2D> NovaResourceRoot::load_texture(const String &name) const {
	if (root_dir_.is_empty()) {
		return Ref<Texture2D>();
	}
	const String file = lookup_name(name);
	return file.is_empty() ? Ref<Texture2D>() : opennova::load_texture_from_dir(root_dir_, file);
}

Ref<Resource> NovaResourceRoot::load_font(const String &name) const {
	if (root_dir_.is_empty()) {
		return Ref<Resource>();
	}
	const String file = lookup_name(name);
	return file.is_empty() ? Ref<Resource>() : cbin_internal::find_font_by_name(file, root_dir_);
}
