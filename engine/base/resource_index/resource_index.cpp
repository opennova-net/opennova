#include <base/resource_index/resource_index.h>

#include <base/resource_index/resource_kind.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <system_error>

#include <base/io/file_time.h>
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
	if (key == "bms" || key == "mis") {
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
	const fs::path path(name);
	const std::string stem = path.stem().string();
	return stem.empty() ? path.filename().string() : stem;
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
			// generic_string() (not string()) so the joined path uses forward slashes on
			// every platform. Godot paths are always '/'-separated and consumers compare
			// these against String.path_join() output (also '/'); native '\' on Windows
			// breaks those equality checks and yields non-portable object paths.
			const fs::path loose_path = fs::path(loc.source_path) / loc.logical_name;
			entry.path = loose_path.generic_string();
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
	return true;
}

void ResourceIndex::clear() {
	++revision_;
	impl_->vfs.clear();
	impl_->root_dir.clear();
	impl_->last_error.clear();
	impl_->records.clear();
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
	// The gore set the effect catalog loads alongside every `.ptl`. Retail picks it
	// once at config time from the mere PRESENCE of `fgn2.bin` in the mount stack —
	// the German content marker — and never re-reads it
	// [orig: Game_LoadConfig @ 0x551480 sets byte_24D4DF9 = FileSystem_FileExists(
	// "fgn2.bin") != 0 @0x5514e8..0x5514fa; CEffectSystem_Init @ 0x5f6070 reads it to
	// pick ".ptg" over the ".ptu" default @0x5f608b..0x5f6095].
	return impl_->vfs.has_file("fgn2.bin") ? std::string(".ptg") : std::string(".ptu");
}

void ResourceIndex::set_scr_policy(int scr_policy) {
	++revision_;
	// Vfs::clear() (called by scan) does not reset the policy, so this persists across re-scans.
	impl_->vfs.set_scr_policy(scr_policy);
}

bool ResourceIndex::prefers_loose_file(const std::string &name) const {
    return impl_->vfs.prefers_loose_file(name);
}

bool ResourceIndex::has_file(const std::string &name) const {
	return impl_->vfs.has_file(name);
}

bool ResourceIndex::has_file(const std::string &name, VfsLookupPolicy policy) const {
	return impl_->vfs.has_file(name, policy);
}

bool ResourceIndex::read_file(const std::string &name, std::vector<uint8_t> &out) const {
	return impl_->vfs.read_file(name, out);
}

bool ResourceIndex::read_file(const std::string &name, std::vector<uint8_t> &out,
                              VfsLookupPolicy policy) const {
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
