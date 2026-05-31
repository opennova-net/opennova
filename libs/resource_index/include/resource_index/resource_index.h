#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova {

struct ResourceFileEntry {
	std::string kind;
	std::string path;
	std::string logical_name;
	std::string display_name;
	std::string relative_path;
	std::string source_type;
	std::string archive_path;
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
	bool scan(const std::string &root_dir, const std::string &expansion = std::string());
	void clear();

	std::vector<ResourceFileEntry> resource_files(const std::string &kind) const;
	bool read_file(const std::string &name, std::vector<uint8_t> &out) const;
	const std::string &root_dir() const;
	const std::string &last_error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova
