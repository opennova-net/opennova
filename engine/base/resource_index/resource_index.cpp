#include <base/resource_index/resource_index.h>

#include <base/resource_index/boot_policy.h>
#include <base/resource_index/resource_kind.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <system_error>

#include <base/io/file_time.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

namespace fs = std::filesystem;

namespace opennova {

namespace {
std::atomic<uint64_t> g_cache_epoch{1};

// The kind classifier (extension + the `.bin` magic peeks) lives in resource_kind.cpp so
// the editor's asset registry shares it (ADR 0046 d9).

std::string normalize_kind(const std::string &kind) {
	const std::string key = strutil::to_lower(strutil::trim(kind));
	if (key.empty() || key == "*" || key == "all") {
		return "";
	}
	if (key == "bms") {
		return "mission";
	}
	if (key == "trn") {
		return "terrain";
	}
	if (key == "env") {
		return "environment";
	}
	if (key == "3di") {
		return "object_model";
	}
	if (key == "kda") {
		return "credits";
	}
	if (key == "fnt" || key == "fonts") {
		return "font";
	}
	if (key == "ptl" || key == "ptu" || key == "ptg" || key == "particles") {
		return "particle";
	}
	if (key == "mnu" || key == "menus") {
		return "menu";
	}
	if (key == "mns") {
		return "menu_style";
	}
	if (key == "bin" || key == "rtxt") {
		return "strings";
	}
	if (key == "mus") {
		return "music_script";
	}
	if (key == "lwf" || key == "sounds" || key == "sound_profile") {
		return "sound";
	}
	// "sbf", "music_script", "sound", and the "music" umbrella pass through unchanged.
	return key;
}

bool is_object_kind(const std::string &kind) {
	return kind == "object_model";
}

// The music umbrella groups .sbf banks and .bin (SCR0) scripts.
bool is_music_kind(const std::string &kind) {
	return kind == "sbf" || kind == "music_script";
}

std::string display_name_from_name(const std::string &name) {
	const std::string file = io::utf8_file_name(name);
	const size_t dot = file.rfind('.');
	const std::string stem = (dot == std::string::npos || dot == 0) ? file : file.substr(0, dot);
	return stem.empty() ? file : stem;
}

} // namespace

// ResourceIndex is the runtime-facing kind catalog on top of the engine-faithful Vfs. The
// Vfs owns mount precedence, decryption/decoding, and byte reads; ResourceIndex enumerates it
// and tags each top-level/archived file with a format kind. scan(root) mounts the root the way
// the game would (loose files shadow archives; root *.pff are mounted as secondaries in sorted
// order). read_file delegates to the Vfs, so any file resolves (not only recognized kinds).
struct ResourceIndex::Impl {
	Vfs vfs;
	std::string root_dir;
	std::string last_error;
	std::vector<ResourceFileEntry> records; // recognized-kind entries
	// An embedder's own file set mounted in place of an install (mount_source); null otherwise.
	std::shared_ptr<const FileSource> source;
};

uint64_t cache_epoch() {
	return g_cache_epoch.load(std::memory_order_acquire);
}

void bump_cache_epoch() {
	g_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
}

ResourceIndex::ResourceIndex() : impl_(std::make_unique<Impl>()) {}
ResourceIndex::~ResourceIndex() = default;
ResourceIndex::ResourceIndex(ResourceIndex &&) noexcept = default;
ResourceIndex &ResourceIndex::operator=(ResourceIndex &&other) noexcept {
	if (this != &other) {
		impl_ = std::move(other.impl_);
		++revision_;
	}
	return *this;
}

bool ResourceIndex::scan(const std::string &root_dir, const std::string &expansion, VfsMountMode mode,
                         VfsArchiveDiscovery discovery) {
	clear();

	if (!impl_->vfs.mount_game(root_dir, expansion, mode, discovery)) {
		impl_->last_error = impl_->vfs.last_error();
		return false;
	}
	index_mounted();
	return true;
}

ResourceIndex::InstallScan ResourceIndex::scan_install(const std::string &root_dir, const LaunchFlags &flags) {
	clear();
	if (!mount_install(impl_->vfs, root_dir, flags)) {
		impl_->last_error = impl_->vfs.last_error();
		// The Vfs keeps the root it mounted: none means the root itself did not mount.
		return impl_->vfs.game_root().empty() ? InstallScan::Unmounted : InstallScan::NoArchive;
	}
	index_mounted();
	return InstallScan::Mounted;
}

bool ResourceIndex::mount_source(std::shared_ptr<const FileSource> files) {
	clear();
	if (!files) {
		impl_->last_error = "No file source to mount";
		return false;
	}
	impl_->source = std::move(files);
	impl_->root_dir = kSourceRootDir;
	return true;
}

void ResourceIndex::index_mounted() {
	impl_->root_dir = impl_->vfs.game_root();

	for (const VfsFileLocation &loc : impl_->vfs.list_files()) {
		const std::string ext = resource_extension_for_name(loc.logical_name);
		std::string kind;
		if (ext == ".bin") {
			// RTXT strings tables and SCR0 music scripts need a content peek; only
			// .bin candidates are read. Use decoded VFS bytes so explicit SCR-wrapped
			// loose/PFF payloads classify the same way as plaintext files.
			std::vector<uint8_t> bytes;
			const bool ok = impl_->vfs.read_file(loc.logical_name, bytes);
			const bool rtxt = ok && resource_bin_has_rtxt_magic(bytes);
			const bool scr = ok && resource_bin_has_scr_magic(bytes);
			kind = resource_kind_for_name_and_magic(loc.logical_name, rtxt, scr);
		} else {
			kind = resource_kind_for_name_and_magic(loc.logical_name, false, false);
		}
		if (kind.empty()) {
			continue;
		}

		ResourceFileEntry entry;
		entry.kind = kind;
		entry.logical_name = loc.logical_name;
		entry.display_name = display_name_from_name(loc.logical_name);
		entry.relative_path = loc.logical_name;
		if (loc.source == VfsSource::LooseDir) {
			entry.source_type = "file";
			// '/'-separated (utf8_generic_path) on every platform. Godot paths are always
			// '/'-separated and consumers compare these against String.path_join() output
			// (also '/'); native '\' on Windows breaks those equality checks and yields
			// non-portable object paths.
			const fs::path loose_path = io::os_path(io::utf8_join(loc.source_path, loc.logical_name));
			entry.path = io::utf8_generic_path(loose_path);
			std::error_code size_ec;
			const auto size = fs::file_size(loose_path, size_ec);
			if (!size_ec) {
				entry.size_bytes = static_cast<uint64_t>(size);
			}
			entry.modified_time = io::file_modified_unix_seconds(loose_path);
		} else {
			entry.source_type = "pff";
			entry.archive_path = loc.source_path;
		}
		impl_->records.push_back(std::move(entry));
	}

	std::sort(impl_->records.begin(), impl_->records.end(),
	          [](const ResourceFileEntry &a, const ResourceFileEntry &b) {
		          const std::string a_key = a.kind + ":" + strutil::to_lower(a.relative_path);
		          const std::string b_key = b.kind + ":" + strutil::to_lower(b.relative_path);
		          return a_key < b_key;
	          });
}

void ResourceIndex::clear() {
	++revision_;
	impl_->vfs.clear();
	impl_->root_dir.clear();
	impl_->last_error.clear();
	impl_->records.clear();
	impl_->source.reset();
}

bool ResourceIndex::has_mounted_archive() const {
	return impl_->vfs.has_mounted_archive();
}

std::vector<ResourceFileEntry> ResourceIndex::resource_files(const std::string &kind) const {
	const std::string filter = normalize_kind(kind);
	std::vector<ResourceFileEntry> out;
	for (const ResourceFileEntry &entry : impl_->records) {
		if (filter.empty() || entry.kind == filter ||
		    (filter == "object" && is_object_kind(entry.kind)) ||
		    (filter == "music" && is_music_kind(entry.kind))) {
			out.push_back(entry);
		}
	}
	return out;
}

std::string ResourceIndex::particle_extension() const {
	// The gore set this mount picks (gore_particle_extension carries the witness).
	return gore_particle_extension(has_file(kGoreContentMarker));
}

std::vector<std::string> ResourceIndex::effect_files() const {
	// Retail parses an archived `.ptl` or gore-set file once per entry its walk admits, by
	// name through the front door [orig: CEffectSystem_Init @ 0x5f6070 — slots 0..5
	// ascending @ 0x5f6485, `cmp dword ptr [ebp-4], 0` @ 0x5f64c0 skipping a zero-stamped
	// entry, strrchr('.') + stricmp(".ptl" / the gore extension) @ 0x5f64cd..0x5f64f3,
	// File_ParseASCIIFile by name @ 0x5f6545]: a name every mounted entry of which is
	// stamped 0 is never read, and one stamped entry is enough.
	std::vector<std::string> names;
	for (const ResourceFileEntry &entry : impl_->records) {
		if (entry.kind != "particle") continue;
		if (impl_->vfs.archive_stamp(entry.logical_name) == VfsArchiveStamp::Unstamped)
			continue;
		names.push_back(entry.logical_name);
	}
	return effect_file_order(names, particle_extension());
}

std::vector<std::string> effect_file_order(const std::vector<std::string> &names, const std::string &gore_extension) {
	std::vector<std::string> out;
	for (const std::string &extension : {std::string(".ptl"), gore_extension}) {
		std::vector<std::string> listed;
		for (const std::string &name : names)
			if (strutil::ends_with_icase(name, extension)) listed.push_back(name);
		std::sort(listed.begin(), listed.end(), [](const std::string &a, const std::string &b) {
			return strutil::to_lower(a) < strutil::to_lower(b);
		});
		out.insert(out.end(), listed.begin(), listed.end());
	}
	return out;
}

std::vector<VfsArchiveEntry> ResourceIndex::archive_slot_entries(int slot) const {
	return impl_->vfs.archive_slot_entries(slot);
}

bool ResourceIndex::archive_slot_has_file(int slot, const std::string &name) const {
	return impl_->vfs.archive_slot_has_file(slot, name);
}

void ResourceIndex::set_scr_policy(int scr_policy) {
	++revision_;
	// Vfs::clear() (called by scan) does not reset the policy, so this persists across re-scans.
	impl_->vfs.set_scr_policy(scr_policy);
}

bool ResourceIndex::prefers_loose_file(const std::string &name) const {
	// A file source has no archive a loose file could be preferred over.
	if (impl_->source) return false;
	return impl_->vfs.prefers_loose_file(name);
}

bool ResourceIndex::loose_first_hit(const std::string &name, VfsLookupPolicy policy) const {
	// A file source has no archive a loose file could win over, whatever the policy.
	if (impl_->source) return false;
	return impl_->vfs.loose_first_hit(name, policy);
}

bool ResourceIndex::has_file(const std::string &name) const {
	if (impl_->source) return impl_->source->stamp(name) != 0;
	return impl_->vfs.has_file(name);
}

bool ResourceIndex::has_file(const std::string &name, VfsLookupPolicy policy) const {
	if (impl_->source) return impl_->source->stamp(name) != 0;
	return impl_->vfs.has_file(name, policy);
}

bool ResourceIndex::read_file(const std::string &name, std::vector<uint8_t> &out) const {
	if (impl_->source) return impl_->source->read(name, out);
	return impl_->vfs.read_file(name, out);
}

bool ResourceIndex::read_file(const std::string &name, std::vector<uint8_t> &out,
                              VfsLookupPolicy policy) const {
	if (impl_->source) return impl_->source->read(name, out);
	return impl_->vfs.read_file(name, out, policy);
}

const std::string &ResourceIndex::root_dir() const {
	return impl_->root_dir;
}

const std::string &ResourceIndex::mounted_expansion() const {
	return impl_->vfs.mounted_expansion();
}

const std::string &ResourceIndex::last_error() const {
	return impl_->last_error;
}

} // namespace opennova
