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
	bool scan(const std::string &root_dir, const std::string &expansion = std::string(),
	          VfsMountMode mode = VfsMountMode::PackedWithLooseOverride);
	void clear();

	// Choose how read_file keys SCR payloads (forwards to the underlying Vfs). Pass a
	// gameprofile ScrPolicy / VfsScrPolicy value; defaults to version-detect and persists
	// across scans. Set by the game-aware caller so demo-vs-retail keying is correct.
	void set_scr_policy(int scr_policy);

	std::vector<ResourceFileEntry> resource_files(const std::string &kind) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out) const;
	const std::string &root_dir() const;
	const std::string &last_error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova
