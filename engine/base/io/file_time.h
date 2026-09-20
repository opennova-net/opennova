#pragma once
// Last-write time of a path as Unix seconds (UTC), shared by the runtime catalog and
// the editor's asset registry. std::filesystem::file_time_type has no portable epoch
// before C++20, so the value is mapped onto system_clock via the now()-offset trick
// (precise enough for display and change detection). 0 when the time can't be read.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace opennova {
namespace io {

inline int64_t file_modified_unix_seconds(const std::filesystem::path &path) {
	std::error_code ec;
	const std::filesystem::file_time_type ftime = std::filesystem::last_write_time(path, ec);
	if (ec) {
		return 0;
	}
	const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
	        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
	return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

} // namespace io
} // namespace opennova
