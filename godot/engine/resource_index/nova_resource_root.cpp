#include "resource_index/nova_resource_root.h"

#include "cbin/cbin_asset_lookup.h"
#include "fnt/nova_fnt_resource.h"
#include "util/engine_caches.h"
#include "util/texture_path_resolver.h"

#include <gameprofile/gameprofile.h>
#include <gameprofile/required_resources.h>
#include <vfs/vfs.h>

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
	ClassDB::bind_static_method("NovaResourceRoot", D_METHOD("cache_epoch"), &NovaResourceRoot::cache_epoch);
	ClassDB::bind_static_method("NovaResourceRoot", D_METHOD("bump_cache_epoch"), &NovaResourceRoot::bump_cache_epoch);
	ClassDB::bind_method(D_METHOD("set_root_dir", "path"), &NovaResourceRoot::set_root_dir);
	ClassDB::bind_method(D_METHOD("mount_runtime", "path", "expansion", "allow_loose_override", "game_code"),
			&NovaResourceRoot::mount_runtime, DEFVAL(String()), DEFVAL(false), DEFVAL("jo"));
	ClassDB::bind_method(D_METHOD("list_expansions", "path"), &NovaResourceRoot::list_expansions);
	ClassDB::bind_method(D_METHOD("get_expansion"), &NovaResourceRoot::get_expansion);
	ClassDB::bind_method(D_METHOD("is_runtime_mount"), &NovaResourceRoot::is_runtime_mount);
	ClassDB::bind_method(D_METHOD("get_root_dir"), &NovaResourceRoot::get_root_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaResourceRoot::get_last_error);
	ClassDB::bind_method(D_METHOD("clear"), &NovaResourceRoot::clear);
	ClassDB::bind_method(D_METHOD("resolve_file", "name"), &NovaResourceRoot::resolve_file);
	ClassDB::bind_method(D_METHOD("list_files", "suffix"), &NovaResourceRoot::list_files, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_file_entries", "suffix"), &NovaResourceRoot::list_file_entries, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("has_file", "name", "policy"), &NovaResourceRoot::has_file, DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("read_file", "name", "policy"), &NovaResourceRoot::read_file, DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("load_texture", "name", "policy"), &NovaResourceRoot::load_texture, DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("load_font", "name"), &NovaResourceRoot::load_font);
	ClassDB::bind_method(D_METHOD("list_missing_boot_resources"), &NovaResourceRoot::list_missing_boot_resources);
	ClassDB::bind_method(D_METHOD("boot_resource_failure_text", "name"), &NovaResourceRoot::boot_resource_failure_text);

	BIND_ENUM_CONSTANT(LOOKUP_SESSION_DEFAULT);
	BIND_ENUM_CONSTANT(LOOKUP_FORCE_LOOSE_FIRST);
	BIND_ENUM_CONSTANT(LOOKUP_FORCE_ARCHIVE_ONLY);
}

PackedStringArray NovaResourceRoot::list_missing_boot_resources() const {
	PackedStringArray missing;
	if (root_dir_.is_empty()) {
		return missing;
	}
	for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
		const NovaRequiredResource *row = gameprofile_required_resource_at(i);
		if (row->severity != NOVA_RES_FATAL) {
			continue;
		}
		// The archive-table trio's all-missing gate belongs to mount_runtime
		// itself [orig: fatal check @ 0x4a6f44]; pattern rows carry no
		// probeable literal name.
		if ((row->flags & (NOVA_RES_F_PFF_TABLE_ANY | NOVA_RES_F_PATTERN)) != 0) {
			continue;
		}
		const String name = String::utf8(row->name);
		if (!has_file(name)) {
			missing.push_back(name);
		}
	}
	return missing;
}

String NovaResourceRoot::boot_resource_failure_text(const String &name) const {
	const NovaRequiredResource *row =
			gameprofile_required_resource_find(name.utf8().get_data());
	return row ? String::utf8(row->failure) : String();
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

String NovaResourceRoot::get_expansion() const {
	return expansion_;
}

bool NovaResourceRoot::is_runtime_mount() const {
	return mount_kind_ == MountKind::Runtime;
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

opennova::VfsLookupPolicy NovaResourceRoot::to_vfs_lookup_policy(LookupPolicy policy) {
	switch (policy) {
		case LOOKUP_FORCE_LOOSE_FIRST:
			return opennova::VfsLookupPolicy::ForceLooseFirst;
		case LOOKUP_FORCE_ARCHIVE_ONLY:
			return opennova::VfsLookupPolicy::ForceArchiveOnly;
		case LOOKUP_SESSION_DEFAULT:
		default:
			return opennova::VfsLookupPolicy::SessionDefault;
	}
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
	// Editor / authoring: loose files only, never the PFF archives. Loose files aren't SCR-wrapped,
	// so the JO default (version-detect) is correct here.
	expansion_ = String();
	mount_kind_ = MountKind::None;
	const Error err = mount_with_mode(path, String(), opennova::VfsMountMode::LooseOnly, "jo",
			opennova::VfsArchiveDiscovery::ScanAll);
	if (err == OK) {
		mount_kind_ = MountKind::EditorLoose;
	}
	return err;
}

Error NovaResourceRoot::mount_runtime(const String &path, const String &expansion, bool allow_loose_override,
		const String &game_code) {
	// Runtime: the packed PFFs are the game data; loose files only shadow them under `/d`.
	mount_kind_ = MountKind::None;
	const opennova::VfsMountMode mode = allow_loose_override
			? opennova::VfsMountMode::PackedWithLooseOverride
			: opennova::VfsMountMode::Packed;
	// The game mounts the witnessed fixed boot table - extra .pff files in the
	// root never mount in retail (docs/vfs/vfs-pff-mount-re.md D-VFS-2).
	const Error err = mount_with_mode(path, expansion, mode, game_code,
			opennova::VfsArchiveDiscovery::RetailTable);
	if (err != OK) {
		expansion_ = String();
		return err;
	}
	// Retail aborts subsystem initialization when the fixed boot table opens no archives.
	// [orig: PFF_OpenAllArchives @ 0x4a4310; Game_InitSubsystems @ 0x4a6f44]
	if (!index_.has_mounted_archive()) {
		clear();
		last_error_ = "No game data archives could be opened";
		return ERR_FILE_NOT_FOUND;
	}
	// The expansion that actually mounted, which is NOT necessarily the requested one:
	// opennova::Vfs::mount_game falls back to base-game mounting for a missing/unknown
	// expansion and still succeeds. Reporting the request back would make every caller-side
	// "did my expansion take?" check tautological (D-NET-178).
	expansion_ = String(index_.mounted_expansion().c_str());
	mount_kind_ = MountKind::Runtime;
	return OK;
}

Error NovaResourceRoot::mount_with_mode(const String &path, const String &expansion, opennova::VfsMountMode mode,
		const String &game_code, opennova::VfsArchiveDiscovery discovery) {
	// The resolver's per-session caches are keyed to the previous root; drop them so a
	// new (or re-scanned) resource directory is read fresh. scan_root() in the editor
	// routes through here too, so a rescan picks up on-disk edits. The epoch bump tells
	// GDScript-side cache holders (placer, veg assets) the same thing.
	opennova::clear_texture_resolver_caches();
	opennova::bump_cache_epoch();
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
	if (!index_.scan(clean.utf8().get_data(), expansion.utf8().get_data(), mode, discovery)) {
		root_dir_ = String();
		last_error_ = String(index_.last_error().c_str());
		return ERR_CANT_OPEN;
	}
	// Game-aware SCR keying: resolve the chosen game's policy once (gameprofile is the single
	// source) and apply it for subsequent read_file calls. An empty/unknown code is the JO default.
	index_.set_scr_policy(gameprofile_scr_policy_for_code(game_code.utf8().get_data()));
	last_error_ = String();
	return OK;
}

PackedStringArray NovaResourceRoot::list_expansions(const String &path) const {
	PackedStringArray out;
	const String clean = normalize_dir(path);
	if (clean.is_empty()) {
		return out;
	}
	for (const std::string &name : opennova::vfs_list_expansions(clean.utf8().get_data())) {
		out.push_back(String(name.c_str()));
	}
	return out;
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
	expansion_ = String();
	mount_kind_ = MountKind::None;
	// Release Godot resources while RenderingServer is still alive. Waiting for
	// the next epoch-checked lookup (or this RefCounted's destructor) retains
	// cached ImageTextures through shutdown and leaks their renderer RIDs.
	texture_cache_.clear();
	texture_cache_epoch_ = 0;
	resolve_memo_.clear();
	resolve_memo_epoch_ = 0;
	resolve_memo_built_ = false;
	opennova::clear_texture_resolver_caches();
	opennova::bump_cache_epoch();
	index_.clear();
}

int64_t NovaResourceRoot::cache_epoch() {
	return static_cast<int64_t>(opennova::cache_epoch());
}

void NovaResourceRoot::bump_cache_epoch() {
	opennova::clear_texture_resolver_caches();
	opennova::bump_cache_epoch();
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

	const uint64_t epoch = opennova::cache_epoch();
	if (!resolve_memo_built_ || resolve_memo_epoch_ != epoch) {
		Ref<DirAccess> dir = DirAccess::open(root_dir_);
		if (dir.is_null()) {
			last_error_ = "Resource directory cannot be opened: " + root_dir_;
			return String();
		}
		resolve_memo_.clear();
		dir->list_dir_begin();
		String entry = dir->get_next();
		while (!entry.is_empty()) {
			if (!dir->current_is_dir()) {
				const String key_string = entry.to_lower();
				const std::string key(key_string.utf8().get_data());
				auto it = resolve_memo_.find(key);
				if (it != resolve_memo_.end()) {
					// Case-variant duplicates poison the name: resolving it is an error.
					it->second = String();
				} else {
					resolve_memo_.emplace(key, root_dir_.path_join(entry));
				}
			}
			entry = dir->get_next();
		}
		dir->list_dir_end();
		resolve_memo_built_ = true;
		resolve_memo_epoch_ = epoch;
	}

	const auto found = resolve_memo_.find(std::string(wanted.utf8().get_data()));
	if (found == resolve_memo_.end()) {
		return String();
	}
	if (found->second.is_empty()) {
		last_error_ = "Duplicate resource filename: " + file;
		return String();
	}
	return found->second;
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

bool NovaResourceRoot::has_file(const String &name, LookupPolicy policy) const {
	if (root_dir_.is_empty()) {
		return false;
	}
	if (mount_kind_ == MountKind::Runtime) {
		if (name.is_empty()) {
			return false;
		}
		// Retail receives the caller's complete relative query. In particular, a
		// qualified loose query must not alias a flat archive entry (D-VFS-3).
		return index_.has_file(
				std::string(name.utf8().get_data()), to_vfs_lookup_policy(policy));
	}
	const String clean = name.strip_edges();
	if (clean.is_empty() || !is_flat_filename(clean)) {
		return false;
	}
	return index_.has_file(std::string(lookup_name(name).utf8().get_data()));
}

PackedByteArray NovaResourceRoot::read_file(const String &name, LookupPolicy policy) const {
	PackedByteArray out;
	if (root_dir_.is_empty()) {
		return out;
	}
	std::vector<uint8_t> bytes;
	bool found = false;
	if (mount_kind_ == MountKind::Runtime) {
		if (name.is_empty()) {
			return out;
		}
		found = index_.read_file(
				std::string(name.utf8().get_data()), bytes, to_vfs_lookup_policy(policy));
	} else {
		const String clean = name.strip_edges();
		if (clean.is_empty() || !is_flat_filename(clean)) {
			return out;
		}
		found = index_.read_file(std::string(lookup_name(name).utf8().get_data()), bytes);
	}
	if (!found) {
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Ref<Texture2D> NovaResourceRoot::load_texture(const String &name, LookupPolicy policy) const {
	if (root_dir_.is_empty() || name.is_empty()) {
		return Ref<Texture2D>();
	}
	const String file = lookup_name(name);
	if (file.is_empty()) {
		return Ref<Texture2D>();
	}

	const uint64_t epoch = opennova::cache_epoch();
	if (texture_cache_epoch_ != epoch) {
		texture_cache_.clear();
		texture_cache_epoch_ = epoch;
	}
	// Source policy and the normalized qualified texture query are part of texture identity.
	// A prior archive/default decode must never poison a later forced-loose lookup (or vice versa).
	const String cache_query = name.strip_edges().to_lower();
	const std::string cache_key = std::to_string(static_cast<int>(policy)) + ":" +
			std::string(cache_query.utf8().get_data());
	const auto cached = texture_cache_.find(cache_key);
	if (cached != texture_cache_.end()) {
		return cached->second;
	}

	Ref<Texture2D> result;
	// texture_candidate_filenames intentionally returns basenames. Reattach a runtime
	// query's directory so every candidate keeps the retail path-qualified lookup.
	const String runtime_dir = mount_kind_ == MountKind::Runtime
			? name.replace("\\", "/").get_base_dir()
			: String();
	for (const String &candidate : opennova::texture_candidate_filenames(file)) {
		const String query = runtime_dir.is_empty()
				? candidate
				: runtime_dir.path_join(candidate);
		const PackedByteArray bytes = read_file(query, policy);
		if (bytes.is_empty()) {
			continue;
		}
		Ref<Texture2D> tex = opennova::load_texture_from_bytes(candidate, bytes);
		if (tex.is_valid()) {
			result = tex;
			break;
		}
	}
	texture_cache_.emplace(cache_key, result);
	return result;
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
