// A scratch directory of this run's own under the system temp directory: made empty on
// construction, removed with everything in it on destruction. The name carries the process
// id (test_paths_unique), so two runs at once (parallel ctest legs, two worktrees on one
// machine) never wipe each other's files. Its name and every path it gives are UTF-8 strings,
// turned into the system's own through base/io/os_path.h, and it is cleared through
// remove_tree, so a tree past MAX_PATH under it (a leftover one at construction too) goes as
// any other. Infrastructure only.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/os_path.h>

#include "common/test_paths.h"

namespace test_temp {

// The file or directory tree at the UTF-8 `utf8` removed a level at a time, each entry reached
// through io::os_path, so one past MAX_PATH under a short root gets its own \\?\ prefix
// (std::filesystem::remove_all from a short root walks into the long paths without one, and
// fails there on Windows). A link is removed, never followed (as remove_all does); what cannot
// be removed stays.
inline void remove_tree(const std::string &utf8) {
	std::error_code ec;
	const std::filesystem::path path = opennova::io::os_path(utf8);
	if (std::filesystem::is_directory(std::filesystem::symlink_status(path, ec))) {
		std::vector<std::string> children;
		for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end;
				it.increment(ec))
			children.push_back(
					opennova::io::utf8_join(utf8, opennova::io::utf8_path(it->path().filename())));
		for (const std::string &child : children)
			remove_tree(child);
	}
	std::filesystem::remove(path, ec);
}

struct TempDir {
	std::filesystem::path path;

	explicit TempDir(const char *name) {
		path = std::filesystem::temp_directory_path() / opennova::io::os_path(test_paths_unique(name));
		remove_tree(root());
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
	}
	~TempDir() { remove_tree(root()); }
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	std::string root() const { return opennova::io::utf8_generic_path(path); }
	std::string file(const char *relative) const { return opennova::io::utf8_join(root(), relative); }
};

} // namespace test_temp
