// Whole-file byte I/O for the test and fixture-generator sources.
//
// The one home for the read_file/write_file helpers the fixture generators and
// roundtrip tests used to carry per file: a fixture generator reads the
// committed bytes back to byte-compare its writer's output, a roundtrip test
// reads the fixture it feeds the parser. Header-only, infrastructure only (no
// retail counterpart to cite).
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

namespace test_io {

// Read the whole file into `out`. False when the file cannot be opened (an
// empty file reads as true with an empty vector).
inline bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	out.resize(static_cast<size_t>(sz));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return static_cast<bool>(f);
}

// The value-returning form: the bytes, or an empty vector when the file is
// missing or short (an unreadable fixture then fails the caller's size or
// parse assertion instead of crashing it).
inline std::vector<uint8_t> read_file(const std::string &path) {
	std::vector<uint8_t> data;
	if (!read_file(path, data)) return {};
	return data;
}

// The text forms: the whole file as a std::string, read in binary mode (no
// newline translation). False when the file cannot be opened.
inline bool read_file_text(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// The value-returning form: the text, or an empty string when the file is
// missing.
inline std::string read_file_text(const std::string &path) {
	std::string text;
	if (!read_file_text(path, text)) return {};
	return text;
}

inline bool write_file(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) return false;
	if (!bytes.empty()) f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(f);
}

// True when `bytes` is an unpulled Git LFS pointer rather than the fixture
// itself (a checkout without `git lfs pull`). The byte-compare guards report
// such a file and skip it instead of failing the comparison.
inline bool is_lfs_pointer(const std::vector<uint8_t> &bytes) {
	static const char kLfsSentinel[] = "version https://git-lfs";
	return bytes.size() >= sizeof(kLfsSentinel) - 1 &&
	       std::memcmp(bytes.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0;
}

} // namespace test_io
