#include <base/resource_index/resource_index.h>

#include <base/vfs/vfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>

#include <base/io/strutil.h>

namespace fs = std::filesystem;

namespace opennova {

namespace {
std::atomic<uint64_t> g_cache_epoch{1};

// Last-write time of `path` as Unix seconds (UTC), or 0 when it can't be read.
// fs::file_time_type has no portable epoch before C++20, so map it onto
// system_clock via the now()-offset trick (precise enough for display).
int64_t file_modified_unix_seconds(const fs::path &path) {
	std::error_code ec;
	const fs::file_time_type ftime = fs::last_write_time(path, ec);
	if (ec) {
		return 0;
	}
	const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
	        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
	return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

std::string to_lower_ascii(std::string value) { return opennova::strutil::to_lower(value); }

std::string trim_copy(const std::string &value) { return opennova::strutil::trim(value); }

bool has_magic(const std::vector<uint8_t> &bytes, const char (&want)[5]) {
	return bytes.size() >= 4 &&
	       bytes[0] == want[0] &&
	       bytes[1] == want[1] &&
	       bytes[2] == want[2] &&
	       bytes[3] == want[3];
}

bool has_rtxt_magic(const std::vector<uint8_t> &bytes) {
	return has_magic(bytes, "RTXT");
}

// MUS bytecode is stored in an .bin wrapper whose first four bytes are "SCR0"
// (the music script loader's SCR container). This disambiguates a music .bin
// from a localized-strings .bin (RTXT magic) and a raw .bin (neither).
bool has_scr_magic(const std::vector<uint8_t> &bytes) {
	return has_magic(bytes, "SCR0");
}

std::string extension_for_name(const std::string &name) {
	return to_lower_ascii(fs::path(name).extension().string());
}

std::string kind_for_name_and_magic(const std::string &name, bool is_rtxt_bin, bool is_scr_bin) {
	const std::string extension = extension_for_name(name);
	// Avatars.def is the singular player-character database. Matched by NAME, not
	// extension: the .def extension is
	// shared with weapon/items/ammo/hudpos.def, which the engine consumes by name at
	// runtime and which stay unbrowsable (like .dbf). [orig: CAvatarDefs_Init @ 0x57b180
	// opens "Avatars.def" by exact name]
	if (to_lower_ascii(fs::path(name).filename().string()) == "avatars.def") {
		return "avatar";
	}
	if (extension == ".bms" || extension == ".mis") {
		return "mission";
	}
	if (extension == ".trn") {
		return "terrain";
	}
	if (extension == ".env") {
		return "environment";
	}
	if (extension == ".3di") {
		return "object_model";
	}
	if (extension == ".kda") {
		return "credits";
	}
	if (extension == ".fnt") {
		return "font";
	}
	// The effect catalog is one grammar across three extensions. `.ptl` is the base
	// set; `.ptu` and `.ptg` are the US and German gore sets, parsed by the SAME
	// section callback and differing only in which one the runtime selects
	// [orig: CEffectSystem_Init @ 0x5f6070 matches an archive entry against ".ptl"
	// OR the selected extension @0x5f64f3 and parses both through
	// CEffectWorld_ParseSectionCallback @ 0x5ecb40; the loose `.ptu` leg's
	// CEffectWorld_LoadDefinitionFile @ 0x5ecf70 is a thin wrapper on the same
	// File_ParseASCIIFile + callback]. The index carries all three so the browser
	// and the loader can both see them; particle_extension() owns the SELECTION.
	if (extension == ".ptl" || extension == ".ptu" || extension == ".ptg") {
		return "particle";
	}
	if (extension == ".mnu") {
		return "menu";
	}
	if (extension == ".mns") {
		return "menu_style";
	}
	if (extension == ".sbf") {
		return "sbf";
	}
	if (extension == ".lwf") {
		return "sound";
	}
	// The .def family is name-keyed, not extension-keyed (items/weapon/ammo/avatars all share
	// .def and are consumed at runtime by name). Only hudpos.def is a browsable kind, for the
	// HUD layout catalog; the rest stay unclassified.
	if (extension == ".def" && to_lower_ascii(fs::path(name).filename().string()) == "hudpos.def") {
		return "hudpos";
	}
	// NOTE: .dbf (dialog bank) is intentionally NOT classified as a browsable kind.
	// It is consumed at runtime by name (DbfData); classifying it as an LWF sound
	// profile would conflate two unrelated formats.
	if (extension == ".bin" && is_scr_bin) {
		return "music_script";
	}
	if (extension == ".bin" && is_rtxt_bin) {
		return "strings";
	}
	return "";
}

std::string normalize_kind(const std::string &kind) {
	const std::string key = to_lower_ascii(trim_copy(kind));
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
		const std::string ext = extension_for_name(loc.logical_name);
		std::string kind;
		if (ext == ".bin") {
			// RTXT strings tables and SCR0 music scripts need a content peek; only
			// .bin candidates are read. Use decoded VFS bytes so explicit SCR-wrapped
			// loose/PFF payloads classify the same way as plaintext files.
			std::vector<uint8_t> bytes;
			const bool ok = impl_->vfs.read_file(loc.logical_name, bytes);
			const bool rtxt = ok && has_rtxt_magic(bytes);
			const bool scr = ok && has_scr_magic(bytes);
			kind = kind_for_name_and_magic(loc.logical_name, rtxt, scr);
		} else {
			kind = kind_for_name_and_magic(loc.logical_name, false, false);
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
			entry.modified_time = file_modified_unix_seconds(loose_path);
		} else {
			entry.source_type = "pff";
			entry.archive_path = loc.source_path;
		}
		impl_->records.push_back(std::move(entry));
	}

	std::sort(impl_->records.begin(), impl_->records.end(),
	          [](const ResourceFileEntry &a, const ResourceFileEntry &b) {
		          const std::string a_key = a.kind + ":" + to_lower_ascii(a.relative_path);
		          const std::string b_key = b.kind + ":" + to_lower_ascii(b.relative_path);
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
