#pragma once
// Last-write time of a path. As Unix seconds (UTC) for the runtime catalog:
// std::filesystem::file_time_type has no portable epoch before C++20, so the value is
// mapped onto system_clock via the now()-offset trick (precise enough for display, but
// it can shift by a second between calls). As the file system's own clock ticks for
// the editor's change detection (the scan's caches, the import cache): exact, compared
// for equality, never shown. 0 when the time can't be read.
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

inline int64_t file_modified_ticks(const std::filesystem::path &path) {
	std::error_code ec;
	const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, ec);
	return ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}

// The file system's clock now, in file_modified_ticks's units.
inline int64_t file_clock_now_ticks() {
	return static_cast<int64_t>(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
}

// How long before a pass of reads a file's last write must lie for a content hash read in that
// pass to be trusted again by the file's size and last write alone: a file system stamps writes
// with a clock that can stand still (FAT and exFAT keep two seconds, NTFS's and Linux's tick is
// milliseconds), so a rewrite of the same size inside the same tick keeps the stamp it had.
inline constexpr std::chrono::seconds kFileStampSettle{2};

// Whether a hash cache may keep a content hash read in a pass that began at `pass_began`
// (file_clock_now_ticks) of a file whose last write was `modified` (ADR 0046 S13 A8, git's racy
// rule measured from the read): only when that last write lies kFileStampSettle or more before
// the pass, so no later rewrite can share its stamp. A file it may not keep is read again by the
// next pass. The import cache and the build cache both keep their entries by it.
inline bool file_stamp_settled(int64_t modified, int64_t pass_began) {
	using Ticks = std::filesystem::file_time_type::duration;
	const int64_t settle = static_cast<int64_t>(std::chrono::duration_cast<Ticks>(kFileStampSettle).count());
	return modified != 0 && modified <= pass_began - settle;
}

} // namespace io
} // namespace opennova
