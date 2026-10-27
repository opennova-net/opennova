// Zip archives over the vendored miniz (apps/novaworld_common/core/zip_files.h): a folder packed
// and unpacked back file for file (a nested one too); and the archives refused before a byte is
// written, a file that is no zip and entries whose names would leave the folder (a ".." part, an
// absolute path, a backslash, a drive), each a stored empty file in a zip made here byte by byte.

#include "core/zip_files.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace io = opennova::io;
using opennova::novaworld_server::unzip_files;
using opennova::novaworld_server::zip_directory;

namespace {

void put16(std::string &out, uint16_t v) {
	out.push_back(static_cast<char>(v & 0xFF));
	out.push_back(static_cast<char>(v >> 8));
}

void put32(std::string &out, uint32_t v) {
	put16(out, static_cast<uint16_t>(v & 0xFFFF));
	put16(out, static_cast<uint16_t>(v >> 16));
}

// A zip of one stored, empty file named `name` (an empty file's CRC-32 is 0): its local header,
// its central directory entry and the end record.
std::string zip_of_one_empty_file(const std::string &name) {
	std::string out;
	put32(out, 0x04034b50); // local file header
	put16(out, 20); // version needed
	put16(out, 0); // flags
	put16(out, 0); // stored
	put16(out, 0); // time
	put16(out, 0); // date
	put32(out, 0); // crc-32
	put32(out, 0); // compressed size
	put32(out, 0); // size
	put16(out, static_cast<uint16_t>(name.size()));
	put16(out, 0); // extra
	out += name;
	const uint32_t directory = static_cast<uint32_t>(out.size());
	put32(out, 0x02014b50); // central directory entry
	put16(out, 20); // made by
	put16(out, 20); // version needed
	put16(out, 0);
	put16(out, 0);
	put16(out, 0);
	put16(out, 0);
	put32(out, 0);
	put32(out, 0);
	put32(out, 0);
	put16(out, static_cast<uint16_t>(name.size()));
	put16(out, 0); // extra
	put16(out, 0); // comment
	put16(out, 0); // disk
	put16(out, 0); // internal attributes
	put32(out, 0); // external attributes
	put32(out, 0); // the local header's offset
	out += name;
	const uint32_t directory_size = static_cast<uint32_t>(out.size()) - directory;
	put32(out, 0x06054b50); // end of central directory
	put16(out, 0);
	put16(out, 0);
	put16(out, 1);
	put16(out, 1);
	put32(out, directory_size);
	put32(out, directory);
	put16(out, 0); // comment
	return out;
}

std::string read(const std::string &path) {
	std::string text, error;
	return io::read_file_text(path, text, error) ? text : std::string("<unread>");
}

bool write(const std::string &path, const std::string &text) {
	std::string error;
	std::error_code ec;
	fs::create_directories(io::os_path(path).parent_path(), ec);
	return io::write_file_atomic(path, text, error);
}

int test_round_trip(test_temp::TempDir &temp) {
	const std::string from = temp.file("from"), zip = temp.file("packed.zip"),
					  to = temp.file("to");
	TEST_EXPECT(write(from + "/oneditor.exe", "the editor") &&
			write(from + "/README.txt", "read me") && write(from + "/data/deep/x.bin", "x"));
	std::string error;
	TEST_EXPECT(zip_directory(from, zip, error));
	TEST_EXPECT(unzip_files(zip, to, error));
	TEST_EXPECT(read(to + "/oneditor.exe") == "the editor" &&
			read(to + "/README.txt") == "read me" && read(to + "/data/deep/x.bin") == "x");
	// An empty file unpacks empty.
	TEST_EXPECT(write(temp.file("empty.zip"), zip_of_one_empty_file("empty.txt")) &&
			unzip_files(temp.file("empty.zip"), temp.file("empty"), error) &&
			fs::exists(io::os_path(temp.file("empty") + "/empty.txt")) &&
			read(temp.file("empty") + "/empty.txt").empty());
	return 0;
}

int test_refusals(test_temp::TempDir &temp) {
	std::string error;
	TEST_EXPECT(write(temp.file("junk.zip"), "no zip at all"));
	TEST_EXPECT(!unzip_files(temp.file("junk.zip"), temp.file("junk"), error) && !error.empty());
	int n = 0;
	for (const char *name : { "../evil.txt", "a/../../evil.txt", "/evil.txt", "a\\evil.txt",
				 "C:evil.txt", "a//evil.txt", "./evil.txt" }) {
		const std::string zip = temp.file(("bad" + std::to_string(++n) + ".zip").c_str());
		const std::string to = temp.file(("bad" + std::to_string(n)).c_str());
		error.clear();
		TEST_EXPECT(write(zip, zip_of_one_empty_file(name)));
		TEST_EXPECT(!unzip_files(zip, to, error) &&
				error.find("names no path under the folder") != std::string::npos);
		TEST_EXPECT(!fs::exists(io::os_path(temp.file("evil.txt"))));
	}
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir temp("zip_files");
	int failures = 0;
	failures += test_round_trip(temp);
	failures += test_refusals(temp);
	if (failures == 0)
		std::printf("zip_files: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
