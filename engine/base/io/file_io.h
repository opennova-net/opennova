// Whole-file reads and writes of UTF-8 paths (header-only, C++17): the atomic write every
// save takes (write `<path>.tmp`, then rename over `path`, so a crash leaves either the old
// file or the new one), the rename that puts a file in place, and the small directory and
// link chores around them. `error` carries the OS reason on failure. Every one reaches the
// system through base/io/os_path.h, so a path outside the ANSI code page or past MAX_PATH
// reads and writes as any other.
//
// Platform file I/O, not a port: nothing here is witnessed engine behaviour.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <base/io/os_path.h>

namespace opennova::io {

namespace file_io_detail {

inline std::string os_error(const char *what, const std::string &path, const std::error_code &ec) {
	return std::string(what) + " " + path + ": " + ec.message();
}

// The whole of `data` written to `path` (created or truncated); a partial file is removed.
inline bool write_whole(const std::string &path, const void *data, size_t size, std::string &error) {
	std::FILE *f = fopen_utf8(path.c_str(), "wb");
	if (!f) {
		error = "cannot create " + path;
		return false;
	}
	const bool written = size == 0 || std::fwrite(data, 1, size, f) == size;
	const bool closed = std::fclose(f) == 0;
	if (!written || !closed) {
		std::error_code ignored;
		std::filesystem::remove(os_path(path), ignored);
		error = "cannot write " + path;
		return false;
	}
	return true;
}

} // namespace file_io_detail

inline bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	std::FILE *f = fopen_utf8(path.c_str(), "rb");
	if (!f) {
		error = "cannot open " + path;
		return false;
	}
	uint8_t buf[64 * 1024];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		out.insert(out.end(), buf, buf + n);
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	if (!ok) {
		error = "cannot read " + path;
		out.clear();
	}
	return ok;
}

inline bool read_file_text(const std::string &path, std::string &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!read_file_bytes(path, bytes, error)) return false;
	out.assign(bytes.begin(), bytes.end());
	return true;
}

// Whether a refused rename may go through when tried again a moment later: Windows' access denied
// and sharing violation, the refusals a scanner or an indexer holding one of the files open for a
// moment gives (S13 A3 measured 3 of 400 replace_file refusals clearing on a 2 ms retry).
inline bool rename_refusal_passes(const std::error_code &ec) {
#ifdef _WIN32
	// ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION.
	return ec.category() == std::system_category() && (ec.value() == 5 || ec.value() == 32 || ec.value() == 33);
#else
	return ec == std::errc::device_or_resource_busy || ec == std::errc::text_file_busy;
#endif
}

// std::filesystem::rename of the system paths, tried again a few times within some 30 ms while
// its refusal may pass (rename_refusal_passes); false with the last refusal in `ec`. Every
// rename that puts a file or a build in place takes it: replace_file, a build's publish.
inline bool rename_with_retry(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec) {
	for (int attempt = 0;; ++attempt) {
		ec.clear();
		std::filesystem::rename(from, to, ec);
		if (!ec) return true;
		if (attempt == 4 || !rename_refusal_passes(ec)) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2 << attempt)); // 2, 4, 8, 16 ms
	}
}

// The written file at `from` put in the place of `to`, replacing it (one rename, rename_with_retry);
// false with the OS reason when it is refused (a write-protected `to`), `from` then left where it
// is. The file replaced, its last write moves past the one it had, however soon after it the new
// one was written (the stamps that tell a file changed are its size and last write).
inline bool replace_file(const std::string &from, const std::string &to, std::string &error) {
	namespace fs = std::filesystem;
	// The last write of the file it replaces, which the new one's must pass (below).
	std::error_code ec;
	const fs::path target = os_path(to);
	const fs::file_time_type before = fs::last_write_time(target, ec);
	const bool replacing = !ec;
	// std::filesystem::rename replaces an existing target on every platform (unlike C
	// rename on Windows), so the swap is one call.
	if (!rename_with_retry(os_path(from), target, ec)) {
		error = file_io_detail::os_error("cannot replace", to, ec);
		return false;
	}
	// A cache tells a file changed by its size and its last write, and a file system stamps a write
	// with a clock that can stand still for milliseconds (Linux's file times step with the
	// scheduler's tick, some 4 ms), so a rewrite of the same size that soon after the last would
	// read as unchanged: its last write is set one tick past the one it replaced when the file
	// system left it there (S13 A3).
	if (replacing) {
		const fs::file_time_type after = fs::last_write_time(target, ec);
		if (!ec && after <= before) fs::last_write_time(target, before + fs::file_time_type::duration(1), ec);
	}
	return true;
}

inline bool write_file_atomic(const std::string &path, const void *data, size_t size, std::string &error) {
	const std::string tmp = path + ".tmp";
	if (!file_io_detail::write_whole(tmp, data, size, error)) return false;
	if (!replace_file(tmp, path, error)) {
		std::error_code ignored;
		std::filesystem::remove(os_path(tmp), ignored);
		return false;
	}
	return true;
}

inline bool write_file_atomic(const std::string &path, const std::string &text, std::string &error) {
	return write_file_atomic(path, text.data(), text.size(), error);
}

// A file made at `path` and opened for writing, only when no file of that name is there: null,
// and nothing touched, when one is (or it cannot be made). A build's copies are made through it,
// so a copy never writes through a name another file may stand behind (a hard link to the last
// good build's archive: S13 A8).
inline std::FILE *create_new_file(const std::string &path) {
	return fopen_utf8(path.c_str(), "wbx");
}

// The file's last write set to now: a file put in place by a copy or a rename that kept the last
// write of the file it came from (a rename's copy, an import's publish) reads as written now to
// every cache that keys a file's content by its size and last write (S13 A8).
inline bool refresh_last_write(const std::string &path, std::string &error) {
	std::error_code ec;
	std::filesystem::last_write_time(os_path(path), std::filesystem::file_time_type::clock::now(), ec);
	if (ec) {
		error = file_io_detail::os_error("cannot date", path, ec);
		return false;
	}
	return true;
}

using FileReplace = std::function<bool(const std::string &from, const std::string &to, std::string &error)>;

// Several files written as one (a rename everywhere): every file's bytes read and every text
// written beside its file (`<path>.tmp`) first, nothing replaced when one of these fails; then
// each put in its file's place in turn (`replace`, replace_file but in a test). When one is not,
// its text and the ones after it are removed and the files already replaced get the bytes they
// held back: false, `problems` saying why (the refusal first, then a file whose bytes could
// not be put back, which holds the new text).
struct FileText {
	std::string path;
	std::string text;
};
inline bool write_files_together(const std::vector<FileText> &files, std::vector<std::string> &problems,
                                 const FileReplace &replace = replace_file) {
	std::vector<std::vector<uint8_t>> originals(files.size());
	std::vector<std::string> staged;
	const auto remove_staged = [&](size_t from) {
		for (size_t i = from; i < staged.size(); ++i) {
			std::error_code ignored;
			std::filesystem::remove(os_path(staged[i]), ignored);
		}
	};
	// What each file holds, to put back; then every text beside its file.
	for (size_t i = 0; i < files.size(); ++i) {
		std::string error;
		if (!read_file_bytes(files[i].path, originals[i], error)) {
			problems.push_back(error);
			return false;
		}
	}
	for (const FileText &file : files) {
		std::string error;
		const std::string tmp = file.path + ".tmp";
		if (!file_io_detail::write_whole(tmp, file.text.data(), file.text.size(), error)) {
			remove_staged(0);
			problems.push_back(error);
			return false;
		}
		staged.push_back(tmp);
	}
	for (size_t i = 0; i < files.size(); ++i) {
		std::string error;
		if (replace(staged[i], files[i].path, error)) continue;
		problems.push_back(error.empty() ? "cannot replace " + files[i].path : error);
		remove_staged(i);
		for (size_t j = 0; j < i; ++j) {
			std::string restore;
			if (!write_file_atomic(files[j].path, originals[j].data(), originals[j].size(), restore))
				problems.push_back(files[j].path + " keeps the new text: " + restore);
		}
		return false;
	}
	return true;
}

// mkdir -p; true when the directory exists afterwards.
inline bool ensure_directory(const std::string &path, std::string &error) {
	std::error_code ec;
	const std::filesystem::path directory = os_path(path);
	std::filesystem::create_directories(directory, ec);
	if (ec && !std::filesystem::is_directory(directory, ec)) {
		error = file_io_detail::os_error("cannot create directory", path, ec);
		return false;
	}
	return true;
}

// The file at `from` given a second name, `to`: a hard link, one file under two names with no
// byte copied (a build's archive its content left as the last build packed it, a run directory's
// archives: S13 A8). False, with the OS reason, where the file system will not (another volume, a
// file system without links, `to` taken): the caller copies the file instead.
inline bool link_file(const std::string &from, const std::string &to, std::string &error) {
	std::error_code ec;
	std::filesystem::create_hard_link(os_path(from), os_path(to), ec);
	if (ec) {
		error = file_io_detail::os_error("cannot link", to, ec);
		return false;
	}
	return true;
}

} // namespace opennova::io
