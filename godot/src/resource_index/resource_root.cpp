#include <runtime/renderer/material_texture.h>
#include "resource_index/resource_root.h"
#include "util/data_format.h"

#include "cbin/cbin_asset_lookup.h"
#include "fnt/fnt_resource.h"
#include "util/texture_path_resolver.h"
#include "util/string_convert.h"

#include <base/gameprofile/gameprofile.h>
#include <base/gameprofile/required_resources.h>
#include <base/gameprofile/resource_missing.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace godot;
using namespace opennova::gameprofile;

// The bound loader enum is the engine's, value for value.
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_STAGE) ==
		static_cast<int>(opennova::renderer::TextureLoader::Stage));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_PLAIN) ==
		static_cast<int>(opennova::renderer::TextureLoader::Plain));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_ARCHIVE) ==
		static_cast<int>(opennova::renderer::TextureLoader::Archive));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_ARCHIVE_SELF_ALPHA) ==
		static_cast<int>(opennova::renderer::TextureLoader::ArchiveSelfAlpha));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_FILE) ==
		static_cast<int>(opennova::renderer::TextureLoader::File));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_TGA) ==
		static_cast<int>(opennova::renderer::TextureLoader::Tga));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_PCX) ==
		static_cast<int>(opennova::renderer::TextureLoader::Pcx));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_HUD_COLOR) ==
		static_cast<int>(opennova::renderer::TextureLoader::HudColor));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_HUD_ALPHA) ==
		static_cast<int>(opennova::renderer::TextureLoader::HudAlpha));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_MENU) ==
		static_cast<int>(opennova::renderer::TextureLoader::Menu));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_CINE_FADE) ==
		static_cast<int>(opennova::renderer::TextureLoader::CineFade));
static_assert(static_cast<int>(ResourceRoot::TEXTURE_LOADER_PARTICLE) ==
		static_cast<int>(opennova::renderer::TextureLoader::Particle));

namespace {

bool case_insensitive_less(const String &a, const String &b) {
	return a.to_lower() < b.to_lower();
}

bool is_flat_filename(const String &name) {
	return name.find("/") == -1 && name.find("\\") == -1;
}

} // namespace

void ResourceRoot::_bind_methods() {
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("is_valid_root", "path"), &ResourceRoot::is_valid_root);
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("cache_epoch"), &ResourceRoot::cache_epoch);
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("bump_cache_epoch"), &ResourceRoot::bump_cache_epoch);
	ClassDB::bind_method(D_METHOD("set_root_dir", "path"), &ResourceRoot::set_root_dir);
	ClassDB::bind_method(D_METHOD("mount_runtime", "path", "expansion", "allow_loose_override", "game_code"),
			&ResourceRoot::mount_runtime, DEFVAL(String()), DEFVAL(false), DEFVAL("jo"));
	ClassDB::bind_method(D_METHOD("list_expansions", "path"), &ResourceRoot::list_expansions);
	ClassDB::bind_method(D_METHOD("expansion_name", "path", "expansion"), &ResourceRoot::expansion_name);
	ClassDB::bind_method(D_METHOD("expansion_description", "path", "expansion"),
			&ResourceRoot::expansion_description);
	ClassDB::bind_method(D_METHOD("get_expansion"), &ResourceRoot::get_expansion);
	ClassDB::bind_method(D_METHOD("is_runtime_mount"), &ResourceRoot::is_runtime_mount);
	ClassDB::bind_method(D_METHOD("get_expansion_override_table"),
			&ResourceRoot::get_expansion_override_table);
	ClassDB::bind_method(D_METHOD("get_root_dir"), &ResourceRoot::get_root_dir);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ResourceRoot::get_last_error);
	ClassDB::bind_method(D_METHOD("clear"), &ResourceRoot::clear);
	ClassDB::bind_method(D_METHOD("resolve_file", "name"), &ResourceRoot::resolve_file);
	ClassDB::bind_method(D_METHOD("list_files", "suffix"), &ResourceRoot::list_files, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("list_file_entries", "suffix"), &ResourceRoot::list_file_entries, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("particle_extension"), &ResourceRoot::particle_extension);
	ClassDB::bind_method(D_METHOD("effect_files"), &ResourceRoot::effect_files);
	ClassDB::bind_method(D_METHOD("has_file", "name", "policy"), &ResourceRoot::has_file, DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("read_file", "name", "policy"), &ResourceRoot::read_file, DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("load_texture", "name", "loader", "policy"), &ResourceRoot::load_texture,
			DEFVAL(LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("load_material_texture", "name", "type"), &ResourceRoot::load_material_texture);
	ClassDB::bind_method(D_METHOD("load_font", "name"), &ResourceRoot::load_font);
	ClassDB::bind_method(D_METHOD("list_missing_boot_resources"), &ResourceRoot::list_missing_boot_resources);
	ClassDB::bind_method(D_METHOD("boot_resource_failure_text", "name"), &ResourceRoot::boot_resource_failure_text);
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("boot_resource_missing_marker"),
			&ResourceRoot::boot_resource_missing_marker);
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("launch_mission_failed_marker"),
			&ResourceRoot::launch_mission_failed_marker);
	ClassDB::bind_static_method("ResourceRoot", D_METHOD("report_missing", "kind", "name", "by", "words"),
			&ResourceRoot::report_missing, DEFVAL(String()), DEFVAL(String()));

	BIND_ENUM_CONSTANT(LOOKUP_FORCE_LOOSE_FIRST);
	BIND_ENUM_CONSTANT(LOOKUP_FORCE_ARCHIVE_ONLY);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_STAGE);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_PLAIN);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_ARCHIVE);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_ARCHIVE_SELF_ALPHA);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_FILE);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_TGA);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_PCX);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_HUD_COLOR);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_HUD_ALPHA);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_MENU);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_CINE_FADE);
	BIND_ENUM_CONSTANT(TEXTURE_LOADER_PARTICLE);

	// Every mount_runtime() / set_root_dir() is done: the mount, the expansion and
	// the expansion's override table may have changed.
	ADD_SIGNAL(MethodInfo("mounted"));
}

PackedStringArray ResourceRoot::list_missing_boot_resources() const {
	PackedStringArray missing;
	if (root_dir_.is_empty()) {
		return missing;
	}
	for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
		const RequiredResource *row = gameprofile_required_resource_at(i);
		if (row->severity != RES_FATAL) {
			continue;
		}
		// The archive-table trio's all-missing gate belongs to mount_runtime
		// itself [orig: fatal check @ 0x4a6f44, see docs/vfs/vfs-pff-mount-re.md]; pattern rows carry no
		// probeable literal name.
		if ((row->flags & (RES_F_PFF_TABLE_ANY | RES_F_PATTERN)) != 0) {
			continue;
		}
		const String name = String::utf8(row->name);
		if (!has_file(name)) {
			missing.push_back(name);
		}
	}
	return missing;
}

String ResourceRoot::boot_resource_missing_marker() {
	return String::utf8(kBootResourceMissingMarker);
}

String ResourceRoot::launch_mission_failed_marker() {
	return String::utf8(kLaunchMissionFailedMarker);
}

void ResourceRoot::report_missing(const String &kind, const String &name, const String &by, const String &words) {
	if (kind.is_empty() || name.is_empty()) {
		return;
	}
	// Once a process for each (kind, name, file naming it), as the game's lookups compare names: a model
	// every instance of an item draws, a set every footstep plays, is said once.
	static std::mutex mutex;
	static std::set<std::string> said;
	const std::string key = opennova::to_std(kind + String("|") + name.to_lower() + String("|") + by.to_lower());
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!said.insert(key).second) {
			return;
		}
	}
	ResourceMiss miss;
	miss.kind = opennova::to_std(kind);
	miss.name = opennova::to_std(name);
	miss.by = opennova::to_std(by);
	miss.words = opennova::to_std(words);
	UtilityFunctions::push_warning(String("ResourceRoot: ") + String::utf8(resource_missing_text(miss).c_str()));
}

String ResourceRoot::boot_resource_failure_text(const String &name) const {
	const RequiredResource *row =
			gameprofile_required_resource_find(name.utf8().get_data());
	return row ? String::utf8(row->failure) : String();
}

bool ResourceRoot::has_virtual_scheme(const String &path) {
	return path.begins_with("res://") || path.begins_with("user://");
}

String ResourceRoot::to_native_path(const String &path) {
	if (has_virtual_scheme(path)) {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (settings != nullptr) {
			return settings->globalize_path(path);
		}
	}
	return path;
}

String ResourceRoot::normalize_dir(const String &path) {
	return to_native_path(path.strip_edges()).replace("\\", "/").rstrip("/");
}

String ResourceRoot::lookup_name(const String &name) {
	return name.strip_edges().replace("\\", "/").get_file();
}

String ResourceRoot::get_expansion() const {
	return expansion_;
}

bool ResourceRoot::is_runtime_mount() const {
	return mount_kind_ == MountKind::Runtime;
}

PackedByteArray ResourceRoot::get_expansion_override_table() const {
	return expansion_override_table_;
}

namespace {

// One list_file_entries row (the resource-index enumeration edge the effect
// and foliage loaders walk by key).
Dictionary file_entry_to_dictionary(const opennova::ResourceFileEntry &entry) {
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

} // namespace

opennova::VfsLookupPolicy ResourceRoot::to_vfs_lookup_policy(LookupPolicy policy) {
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

bool ResourceRoot::is_valid_root(const String &path) {
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

Error ResourceRoot::set_root_dir(const String &path) {
	// Loose-source mount: never open PFF archives. Loose files aren't SCR-wrapped,
	// so the JO default (version-detect) is correct here.
	expansion_ = String();
	expansion_override_table_ = PackedByteArray();
	mount_kind_ = MountKind::None;
	const Error err = [&]() -> Error {
		String clean;
		const Error begun = begin_mount(path, clean);
		if (begun != OK) {
			return begun;
		}
		if (!index_.scan(clean.utf8().get_data(), std::string(), opennova::VfsMountMode::LooseOnly,
					opennova::VfsArchiveDiscovery::ScanAll)) {
			root_dir_ = String();
			last_error_ = String(index_.last_error().c_str());
			return ERR_CANT_OPEN;
		}
		index_.set_scr_policy(gameprofile_scr_policy_for_code("jo"));
		game_code_ = "jo";
		last_error_ = String();
		mount_kind_ = MountKind::Loose;
		return OK;
	}();
	emit_signal("mounted");
	return err;
}

Error ResourceRoot::mount_runtime(const String &path, const String &expansion, bool allow_loose_override,
		const String &game_code) {
	// A root already runtime-mounted switches in place with its old archives still open
	// (the menu's and the join's expansion switch); any other mount is the boot's, with
	// none open. The engine's vfs_expansion_override_table says what each one reaches.
	const opennova::ExpansionLoadPoint load_point = mount_kind_ == MountKind::Runtime
			? opennova::ExpansionLoadPoint::ArchivesOpen
			: opennova::ExpansionLoadPoint::ArchivesClosed;
	const Error err = mount_runtime_archives_(path, expansion, allow_loose_override, game_code);
	expansion_override_table_ = PackedByteArray();
	std::vector<uint8_t> table;
	if (err == OK &&
			opennova::vfs_expansion_override_table(opennova::to_std(root_dir_),
					opennova::to_std(expansion_), load_point, allow_loose_override, table)) {
		expansion_override_table_ = to_packed_bytes(table);
	}
	emit_signal("mounted");
	return err;
}

Error ResourceRoot::mount_runtime_archives_(const String &path, const String &expansion,
		bool allow_loose_override, const String &game_code) {
	// Runtime: the install mounted as a launch with these flags mounts it (mount_install): the
	// witnessed fixed boot table (extra .pff files in the root never mount in retail,
	// docs/vfs/vfs-pff-mount-re.md D-VFS-2), the packed PFFs the game data, loose files only
	// shadowing them under `/d`.
	mount_kind_ = MountKind::None;
	opennova::LaunchFlags flags;
	flags.loose_override = allow_loose_override;
	flags.expansion = opennova::to_std(expansion);
	flags.game = opennova::to_std(game_code.to_lower());
	String clean;
	const Error err = begin_mount(path, clean);
	if (err != OK) {
		expansion_ = String();
		return err;
	}
	const opennova::ResourceIndex::InstallScan scanned = index_.scan_install(clean.utf8().get_data(), flags);
	if (scanned == opennova::ResourceIndex::InstallScan::Unmounted) {
		expansion_ = String();
		root_dir_ = String();
		last_error_ = String(index_.last_error().c_str());
		return ERR_CANT_OPEN;
	}
	if (scanned == opennova::ResourceIndex::InstallScan::NoArchive) {
		// Retail aborts subsystem initialization when the fixed boot table opens no archives
		// (a corrupt sole archive included), and the partial loose mount goes with it.
		// [orig: PFF_OpenAllArchives @ 0x4a4310; Game_InitSubsystems @ 0x4a6f44, see docs/vfs/vfs-pff-mount-re.md]
		clear();
		last_error_ = "No game data archives could be opened";
		return ERR_FILE_NOT_FOUND;
	}
	game_code_ = game_code;
	last_error_ = String();
	// The expansion that actually mounted, which is NOT necessarily the requested one:
	// opennova::Vfs::mount_game falls back to base-game mounting for a missing/unknown
	// expansion and still succeeds. Reporting the request back would make every caller-side
	// "did my expansion take?" check tautological (D-NET-178).
	expansion_ = String(index_.mounted_expansion().c_str());
	mount_kind_ = MountKind::Runtime;
	return OK;
}

Error ResourceRoot::mount_files(std::shared_ptr<const opennova::FileSource> files) {
	expansion_ = String();
	expansion_override_table_ = PackedByteArray();
	mount_kind_ = MountKind::None;
	// This root's caches keyed to the old mount dropped; the global epoch stands, so another root's
	// holders keep theirs (the mounting device makes afresh whatever read a moved file).
	opennova::clear_texture_resolver_caches();
	texture_cache_.clear();
	resolve_memo_built_ = false;
	assets_.invalidate();
	if (!index_.mount_source(std::move(files))) {
		root_dir_ = String();
		last_error_ = String(index_.last_error().c_str());
		return ERR_INVALID_PARAMETER;
	}
	root_dir_ = String(opennova::ResourceIndex::kSourceRootDir);
	last_error_ = String();
	mount_kind_ = MountKind::Source;
	return OK;
}

Error ResourceRoot::begin_mount(const String &path, String &r_clean) {
	// The resolver's per-session caches are keyed to the previous root; drop them so a
	// a new or re-scanned resource directory is read fresh. The epoch bump tells
	// GDScript-side cache holders (placer, veg assets) the same thing.
	opennova::clear_texture_resolver_caches();
	opennova::bump_cache_epoch();
	// Discard the old native source before validation too: a rejected path
	// must not leave simulation or skeletal loaders reading the previous mount.
	index_.clear();
	assets_.invalidate();
	game_code_ = String();
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
	r_clean = clean;
	return OK;
}

PackedStringArray ResourceRoot::list_expansions(const String &path) const {
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

String ResourceRoot::expansion_name(const String &path, const String &expansion) const {
	const String clean = normalize_dir(path);
	if (clean.is_empty()) {
		return String();
	}
	return opennova::to_gd(opennova::vfs_expansion_info(
			opennova::to_std(clean), opennova::to_std(expansion)).name);
}

String ResourceRoot::expansion_description(const String &path, const String &expansion) const {
	const String clean = normalize_dir(path);
	if (clean.is_empty()) {
		return String();
	}
	return opennova::to_gd(opennova::vfs_expansion_info(
			opennova::to_std(clean), opennova::to_std(expansion)).description);
}

String ResourceRoot::get_root_dir() const {
	return root_dir_;
}

String ResourceRoot::get_last_error() const {
	return last_error_;
}

void ResourceRoot::clear() {
	root_dir_ = String();
	last_error_ = String();
	game_code_ = String();
	expansion_ = String();
	expansion_override_table_ = PackedByteArray();
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
	assets_.invalidate();
}

int64_t ResourceRoot::cache_epoch() {
	return static_cast<int64_t>(opennova::cache_epoch());
}

void ResourceRoot::bump_cache_epoch() {
	opennova::clear_texture_resolver_caches();
	opennova::bump_cache_epoch();
}

String ResourceRoot::resolve_file(const String &name) {
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
	if (mount_kind_ == MountKind::Source) {
		last_error_ = "A file source has no directory to resolve a file in: " + file;
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
				const std::string key = opennova::to_std(key_string);
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

	const auto found = resolve_memo_.find(opennova::to_std(wanted));
	if (found == resolve_memo_.end()) {
		return String();
	}
	if (found->second.is_empty()) {
		last_error_ = "Duplicate resource filename: " + file;
		return String();
	}
	return found->second;
}

PackedStringArray ResourceRoot::list_files(const String &suffix) const {
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

Array ResourceRoot::list_file_entries(const String &suffix) const {
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

PackedStringArray ResourceRoot::effect_files() const {
	PackedStringArray out;
	if (root_dir_.is_empty()) {
		return out;
	}
	for (const std::string &name : index_.effect_files()) {
		out.push_back(String::utf8(name.c_str()));
	}
	return out;
}

String ResourceRoot::particle_extension() const {
	return String(index_.particle_extension().c_str());
}

bool ResourceRoot::has_file(const String &name, LookupPolicy policy) const {
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
				opennova::to_std(name), to_vfs_lookup_policy(policy));
	}
	const String clean = name.strip_edges();
	if (clean.is_empty() || !is_flat_filename(clean)) {
		return false;
	}
	return index_.has_file(opennova::to_std(lookup_name(name)));
}

PackedByteArray ResourceRoot::read_file(const String &name, LookupPolicy policy) const {
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
				opennova::to_std(name), bytes, to_vfs_lookup_policy(policy));
	} else {
		const String clean = name.strip_edges();
		if (clean.is_empty() || !is_flat_filename(clean)) {
			return out;
		}
		found = index_.read_file(opennova::to_std(lookup_name(name)), bytes);
	}
	if (!found) {
		return out;
	}
	return to_packed_bytes(bytes);
}

std::vector<opennova::renderer::TextureLoad> ResourceRoot::texture_attempts_(const String &name,
		TextureLoader loader, LookupPolicy policy) const {
	// A runtime mount hands the loader the caller's whole query (its qualified lookup
	// is retail's); a loose mount resolves flat names, so a qualified query keeps its
	// file name.
	const String query = mount_kind_ == MountKind::Runtime ? name.strip_edges() : lookup_name(name);
	if (query.is_empty()) {
		return {};
	}
	opennova::renderer::TextureFileQuery files;
	files.exists = [this, policy](const std::string &file) {
		return has_file(opennova::to_gd(file), policy);
	};
	files.loose_first_hit = [this, policy](const std::string &file) {
		return index_.loose_first_hit(file, to_vfs_lookup_policy(policy));
	};
	return opennova::renderer::texture_load_attempts(
			static_cast<opennova::renderer::TextureLoader>(loader), opennova::to_std(query), files);
}

PackedByteArray ResourceRoot::read_texture_attempt_(const opennova::renderer::TextureLoad &load,
		LookupPolicy policy) const {
	if (load.source == opennova::renderer::TextureFileSource::ParticleTextureDir) {
		// The particle manager's own folder, "<game directory>\tga\", opened directly
		// (renderer::TextureFileSource::ParticleTextureDir); raw bytes, as fopen reads. A file
		// source (mount_files) has no game directory, so no such folder.
		if (mount_kind_ == MountKind::Source) {
			return PackedByteArray();
		}
		const String tga_dir = root_dir_.path_join("tga");
		const String file = opennova::to_gd(load.file).replace("\\", "/");
		const String path = file.contains("/")
				? (FileAccess::file_exists(tga_dir.path_join(file)) ? tga_dir.path_join(file) : String())
				: opennova::resolve_file_in_dir(tga_dir, file);
		return path.is_empty() ? PackedByteArray() : FileAccess::get_file_as_bytes(path);
	}
	return read_file(opennova::to_gd(load.file), policy);
}

Ref<Image> ResourceRoot::load_texture_image(const String &name, TextureLoader loader,
		LookupPolicy policy, bool *r_alpha_only) const {
	if (r_alpha_only != nullptr) {
		*r_alpha_only = false;
	}
	if (root_dir_.is_empty() || name.strip_edges().is_empty()) {
		return Ref<Image>();
	}
	const Ref<Image> image = opennova::load_texture_image(texture_attempts_(name, loader, policy),
			[this, policy](const opennova::renderer::TextureLoad &load) {
				return read_texture_attempt_(load, policy);
			},
			r_alpha_only);
	// An image consumer takes the top level alone.
	if (image.is_valid() && image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	return image;
}

Ref<Texture2D> ResourceRoot::load_texture(const String &name, TextureLoader loader,
		LookupPolicy policy) const {
	if (root_dir_.is_empty() || name.strip_edges().is_empty()) {
		return Ref<Texture2D>();
	}
	const uint64_t epoch = opennova::cache_epoch();
	if (texture_cache_epoch_ != epoch) {
		texture_cache_.clear();
		texture_cache_epoch_ = epoch;
	}
	// The files and readers the loader resolved, under the source policy, are the
	// texture's identity: one loader's decode never serves another's, a prior
	// archive/default decode never poisons a later forced-loose lookup, and two names
	// whose case the rules read differently (".MDT", ".PCX") stay apart.
	const std::vector<opennova::renderer::TextureLoad> attempts = texture_attempts_(name, loader, policy);
	const std::string cache_key = std::to_string(static_cast<int>(policy)) + ":" +
			opennova::texture_load_key(attempts);
	const auto cached = texture_cache_.find(cache_key);
	if (cached != texture_cache_.end()) {
		return cached->second;
	}
	const Ref<Texture2D> result = opennova::texture_with_mipmaps(opennova::load_texture_image(attempts,
			[this, policy](const opennova::renderer::TextureLoad &load) {
				return read_texture_attempt_(load, policy);
			}));
	texture_cache_.emplace(cache_key, result);
	return result;
}

Ref<Texture> ResourceRoot::load_material_texture(const String &name, uint8_t type) const {
	using opennova::renderer::MaterialTextureReader;
	// The one file the row's loader opens and the reader that decodes it, never an
	// alternate extension, suffix or reader (renderer::material_texture_source; a type-1
	// row's upper-case .PCX turned white with its blue as alpha, renderer::material_texture_load);
	// the session's loose-first policy decides a loose hit.
	opennova::renderer::MaterialTextureSource source;
	if (!root_dir_.is_empty() && !name.is_empty()) {
		source = opennova::renderer::material_texture_source(opennova::to_std(name), type,
				[this](const std::string &file) { return has_file(opennova::to_gd(file)); },
				[this](const std::string &file) { return index_.prefers_loose_file(file); });
	}
	if (source.reader == MaterialTextureReader::Chunk) {
		return opennova::prepare_material_chunk(read_file(opennova::to_gd(source.file)), type);
	}
	const opennova::renderer::TextureLoad load = opennova::renderer::material_texture_load(source, type);
	Ref<Texture2D> image;
	if (load.reader != opennova::renderer::TextureReader::None) {
		const uint64_t epoch = opennova::cache_epoch();
		if (texture_cache_epoch_ != epoch) {
			texture_cache_.clear();
			texture_cache_epoch_ = epoch;
		}
		const std::string key = "material-image:" + std::to_string(static_cast<int>(load.reader)) + ":" +
				std::to_string(static_cast<int>(load.transform)) + ":" +
				opennova::to_std(opennova::to_gd(load.file).to_lower());
		auto cached = texture_cache_.find(key);
		if (cached == texture_cache_.end()) {
			cached = texture_cache_.emplace(key, opennova::load_material_image_from_bytes(
					load, read_file(opennova::to_gd(load.file)))).first;
		}
		image = cached->second;
	}
	return opennova::prepare_material_texture(image, name, type);
}

bool ResourceRoot::material_texture_missing(const String &name, uint8_t type) const {
	if (root_dir_.is_empty() || name.is_empty()) {
		return false;
	}
	const opennova::renderer::MaterialTextureSource source = opennova::renderer::material_texture_source(
			opennova::to_std(name), type, [this](const std::string &file) { return has_file(opennova::to_gd(file)); },
			[this](const std::string &file) { return index_.prefers_loose_file(file); });
	return source.file.empty() ? !has_file(name) : !has_file(opennova::to_gd(source.file));
}

Ref<Resource> ResourceRoot::load_font(const String &name) const {
	if (root_dir_.is_empty()) {
		return Ref<Resource>();
	}
	const String file = lookup_name(name);
	if (file.is_empty()) {
		return Ref<Resource>();
	}
	const PackedByteArray bytes = read_file(file.get_extension().to_lower() == "fnt" ? file : file + String(".fnt"));
	if (!bytes.is_empty()) {
		Ref<FntResource> font;
		font.instantiate();
		if (font->load_from_bytes(bytes) == OK) {
			return font;
		}
	}
	// A CBIN font is found by walking the root's directory: a file source has none.
	if (mount_kind_ == MountKind::Source) {
		return Ref<Resource>();
	}
	return cbin_internal::find_font_by_name(file, root_dir_);
}
