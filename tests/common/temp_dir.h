// A scratch directory of this run's own under the system temp directory: made empty on
// construction, removed with everything in it on destruction. The name carries the process
// id (test_paths_unique), so two runs at once (parallel ctest legs, two worktrees on one
// machine) never wipe each other's files. Its name and every path it gives are UTF-8 strings,
// turned into the system's own through base/io/os_path.h. Infrastructure only.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include <base/io/os_path.h>

#include "common/test_paths.h"

namespace test_temp {

struct TempDir {
	std::filesystem::path path;

	explicit TempDir(const char *name) {
		path = std::filesystem::temp_directory_path() / opennova::io::os_path(test_paths_unique(name));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(opennova::io::os_path(path), ec);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	std::string root() const { return opennova::io::utf8_generic_path(path); }
	std::string file(const char *relative) const { return opennova::io::utf8_join(root(), relative); }
};

} // namespace test_temp
