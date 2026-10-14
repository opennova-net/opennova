// Whole-file byte I/O for the test and fixture-generator sources.
//
// The one home for the read_file/write_file helpers the fixture generators and
// roundtrip tests used to carry per file: a fixture generator reads the
// committed bytes back to byte-compare its writer's output, a roundtrip test
// reads the fixture it feeds the parser. Every path is a UTF-8 string, turned
// into the system's own through base/io/os_path.h (a folder named outside the
// ANSI code page, a path past MAX_PATH). And the CR LF text a test stages a
// retail text file with. Header-only, infrastructure only.
//
// The UTF-8 contract holds here and in temp_dir.h, not yet in the two roots
// tests/common reads from the environment: test_paths_temp_dir() (test_paths.h)
// and the retail roots with the files found under them (retail_paths.h) are the
// narrow strings getenv and std::filesystem::path::string() give, in the ANSI
// code page on Windows. The tests that take them hand them to fopen, _mkdir and
// std::filesystem::path(std::string), which read that code page, as well as to
// the UTF-8 calls here and in the engine, so turning those roots to UTF-8 turns
// their callers too (to io::os_path, io::fopen_utf8 or test_temp::TempDir), a
// change beyond tests/common. Where a root is ASCII (the runners' temp
// directories, a retail tree under an ASCII path) both spellings are the same
// bytes and either passes here; a root outside ASCII waits on that change.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/os_path.h>

namespace test_io {

// Read the whole file into `out`. False when the file cannot be opened (an
// empty file reads as true with an empty vector).
inline bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(opennova::io::os_path(path), std::ios::binary | std::ios::ate);
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
	std::ifstream f(opennova::io::os_path(path), std::ios::binary);
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

// The whole of `bytes` written to `path` (created, or replaced: io::write_file_atomic), its
// folders made (io::ensure_directory).
inline bool write_file(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::string error;
	const std::filesystem::path folder = opennova::io::os_path(path).parent_path();
	if (!folder.empty() && !opennova::io::ensure_directory(opennova::io::utf8_path(folder), error))
		return false;
	return opennova::io::write_file_atomic(path, bytes.data(), bytes.size(), error);
}

// True when `bytes` is an unpulled Git LFS pointer rather than the fixture
// itself (a checkout without `git lfs pull`). The byte-compare guards report
// such a file and skip it instead of failing the comparison.
inline bool is_lfs_pointer(const std::vector<uint8_t> &bytes) {
	static const char kLfsSentinel[] = "version https://git-lfs";
	return bytes.size() >= sizeof(kLfsSentinel) - 1 &&
	       std::memcmp(bytes.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0;
}

// `text` with every line ending in CR LF, the last one included: the one break the retail walk of a
// .def, .trn or .env file splits at, which drops the last byte of a line left without it
// [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5; engine/base/io/ascii_config.h], and the one the
// ConfigFile reader ends a line at (charattr.def). A test writes such a file's text with LF and
// stages it through this (the GUT suite's TestFs.crlf); a CR LF already there stays one.
inline std::string crlf(std::string_view text) {
	std::string lf;
	for (size_t i = 0; i < text.size(); ++i)
		if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
			lf += text[i];
	if (!lf.empty() && lf.back() != '\n')
		lf += '\n';
	std::string out;
	for (const char c : lf) {
		if (c == '\n')
			out += '\r';
		out += c;
	}
	return out;
}

// A text as a file named `name` holds it: a .def, .trn or .env with CR LF line ends (crlf above),
// any other as it is.
inline std::string staged(const std::string &name, const std::string &text) {
	std::string extension = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
	for (char &c : extension)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return extension == ".def" || extension == ".trn" || extension == ".env" ? crlf(text) : text;
}

// A text file written at `path` as staged above, its folders made (write_file).
inline bool write_text(const std::string &path, const std::string &text) {
	const std::string bytes = staged(path, text);
	return write_file(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

} // namespace test_io
