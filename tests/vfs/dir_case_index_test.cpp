// A loose directory's files by name without case (base/vfs/dir_case_index.h), the one index the
// game's resource root, its texture and sidecar resolves and a font's folder find a file by: a
// name in any case finds the folder's spelling, a folder is not a file, a missing directory or
// name finds nothing, and a name the folder holds in two cases (a case-sensitive file system's
// alone) finds nothing either. Infrastructure, minted files only.
#include <base/io/os_path.h>
#include <base/vfs/dir_case_index.h>

#include "common/temp_dir.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

void write_file(const std::string &utf8) {
	std::ofstream out(io::os_path(utf8), std::ios::binary);
	out << "x";
}

std::size_t entries(const std::string &utf8) {
	std::size_t n = 0;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(io::os_path(utf8), ec), end; !ec && it != end;
			it.increment(ec))
		++n;
	return n;
}

} // namespace

int main() {
	test_temp::TempDir dir("dir_case_index");
	write_file(dir.file("Alpha.TRN"));
	write_file(dir.file("bravo.til"));
	std::error_code ec;
	std::filesystem::create_directories(io::os_path(dir.file("Sub")), ec);

	const DirCaseIndex index = DirCaseIndex::scan(dir.root());
	CHECK(!index.empty());
	CHECK(index.find("alpha.trn") == "Alpha.TRN");
	CHECK(index.find("ALPHA.trn") == "Alpha.TRN");
	CHECK(index.find("Bravo.TIL") == "bravo.til");
	CHECK(!index.ambiguous("alpha.trn"));
	CHECK(index.find("sub").empty()); // a folder is not a file
	CHECK(index.find("charlie.trn").empty());
	CHECK(index.find("").empty());

	CHECK(DirCaseIndex::scan(dir.file("missing")).empty());
	CHECK(DirCaseIndex::scan(std::string()).empty());

	// A second spelling of one name: two files on a case-sensitive file system, which leave the
	// name ambiguous; one file on a case-insensitive one, found as before.
	test_temp::TempDir twice("dir_case_index_twice");
	write_file(twice.file("Dup.txt"));
	write_file(twice.file("DUP.TXT"));
	const DirCaseIndex dup = DirCaseIndex::scan(twice.root());
	if (entries(twice.root()) == 2) {
		CHECK(dup.ambiguous("dup.txt"));
		CHECK(dup.find("dup.txt").empty());
	} else {
		CHECK(!dup.ambiguous("dup.txt"));
		CHECK(!dup.find("dup.txt").empty());
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("dir_case_index: ok\n");
	return 0;
}
