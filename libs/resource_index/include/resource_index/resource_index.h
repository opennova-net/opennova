#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vfs/vfs.h>

namespace opennova {

struct ResourceFileEntry {
	std::string kind;
	std::string path;
	std::string logical_name;
	std::string display_name;
	std::string relative_path;
	std::string source_type;
	std::string archive_path;
	// File metadata, filled at scan time for loose entries (the editor mounts
	// LooseOnly, so these are always populated there). Archive (.pff) entries
	// leave these zero because the location carries no size/time. size_bytes is
	// the on-disk byte count; modified_time is the last-write time in Unix
	// seconds (UTC), or 0 when unknown.
	uint64_t size_bytes = 0;
	int64_t modified_time = 0;
};

class ResourceIndex {
public:
	ResourceIndex();
	~ResourceIndex();

	ResourceIndex(ResourceIndex &&) noexcept;
	ResourceIndex &operator=(ResourceIndex &&) noexcept;

	ResourceIndex(const ResourceIndex &) = delete;
	ResourceIndex &operator=(const ResourceIndex &) = delete;

	// Mount and index a game install. When `expansion` is non-empty and
	// <root>/expansion/<name>/<name>.pff exists, the expansion's loose files + archives
	// override the base game (see opennova::Vfs::mount_game). Empty expansion = base game.
	// `mode` selects which layers are mounted (loose, archives, or both) — see VfsMountMode.
	// `discovery` selects base-archive discovery: the index defaults to ScanAll (the
	// editor's browse index must see arbitrary modder archives — a deliberate divergence,
	// docs/vfs/vfs-pff-mount-re.md D-VFS-2); the game runtime passes RetailTable (the
	// witnessed fixed boot table [orig: PFF_OpenAllArchives @ 0x4a4310]).
	// Scanning an already-mounted index REPLACES the mount outright (clear() first: archives
	// closed, search paths dropped, entries rebuilt) — it never layers onto the previous one,
	// so a remount in place is how a live root moves to another expansion.
	bool scan(const std::string &root_dir, const std::string &expansion = std::string(),
	          VfsMountMode mode = VfsMountMode::PackedWithLooseOverride,
	          VfsArchiveDiscovery discovery = VfsArchiveDiscovery::ScanAll);
	void clear();
	bool has_mounted_archive() const;

	// Choose how read_file keys SCR payloads (forwards to the underlying Vfs). Pass a
	// gameprofile ScrPolicy / VfsScrPolicy value; defaults to version-detect and persists
	// across scans. Set by the game-aware caller so demo-vs-retail keying is correct.
	void set_scr_policy(int scr_policy);

	std::vector<ResourceFileEntry> resource_files(const std::string &kind) const;
	// Default overloads use the session policy selected by scan(); policy overloads
	// let retail consumers force one lookup without mutating that session default.
	bool has_file(const std::string &name) const;
	bool has_file(const std::string &name, VfsLookupPolicy policy) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out, VfsLookupPolicy policy) const;
	const std::string &root_dir() const;
	// The expansion whose layers ACTUALLY mounted, empty for base game — including after
	// scan()'s silent fallback for a missing/unknown expansion (opennova::Vfs::mount_game).
	// Read this, never the requested name, when running on the wrong data set is a bug
	// (the LAN joiner's host-expansion reconcile, D-NET-178).
	const std::string &mounted_expansion() const;
	const std::string &last_error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova
