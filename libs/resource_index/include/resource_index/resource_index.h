#pragma once

#include <memory>
#include <string>
#include <vector>

namespace opennova {

struct ResourceFileEntry {
	std::string kind;
	std::string path;
	std::string display_name;
	std::string relative_path;
};

class ResourceIndex {
public:
	ResourceIndex();
	~ResourceIndex();

	ResourceIndex(ResourceIndex &&) noexcept;
	ResourceIndex &operator=(ResourceIndex &&) noexcept;

	ResourceIndex(const ResourceIndex &) = delete;
	ResourceIndex &operator=(const ResourceIndex &) = delete;

	bool scan(const std::string &root_dir);
	void clear();

	std::vector<ResourceFileEntry> resource_files(const std::string &kind) const;
	const std::string &root_dir() const;
	const std::string &last_error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova
