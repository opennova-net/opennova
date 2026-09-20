#pragma once
// Shared plumbing for the editor core tests: a throwaway project directory under the
// system temp directory (std::filesystem::temp_directory_path, so no env read of our
// own) that is wiped on construction and destruction, and a file writer.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace editor_test {

struct TempProjectDir {
	std::filesystem::path path;

	explicit TempProjectDir(const char *name) {
		path = std::filesystem::temp_directory_path() / name;
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
	}
	~TempProjectDir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	std::string root() const { return path.generic_string(); }
	std::string file(const char *relative) const {
		return (path / relative).generic_string();
	}
};

inline bool write_bytes(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream out(path, std::ios::binary);
	if (!out) return false;
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

inline bool write_text(const std::string &path, const std::string &text) {
	return write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

} // namespace editor_test
