// base/io/file_io.h: whole-file reads and the atomic write (a .tmp beside the file, renamed
// over it), a file rewritten at its size moving its stamp however soon after its last write, a
// rename the system refuses while another holds the file tried again a bounded few times, several
// files written as one and put back when one is refused, a file made only where none stands, a
// last write set to now, mkdir -p and a hard link; every one through a UTF-8 path outside the
// ANSI code page and deeper than MAX_PATH. And base/io/os_path.h's path_within.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/os_path.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

namespace fs = std::filesystem;
namespace io = opennova::io;

namespace {

bool exists(const std::string &utf8) {
	std::error_code ec;
	return fs::exists(io::os_path(utf8), ec);
}

// Removes a tree level by level through io::os_path, so each long descendant gets its own
// \\?\ prefix (std::filesystem::remove_all from a short root walks into the long paths
// without one, and fails there on Windows).
void remove_tree(const std::string &utf8) {
	std::error_code ec;
	const fs::path path = io::os_path(utf8);
	if (fs::is_directory(path, ec)) {
		std::vector<std::string> children;
		for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
			children.push_back(utf8 + "/" + io::utf8_path(it->path().filename()));
		for (const std::string &child : children) remove_tree(child);
	}
	fs::remove(path, ec);
}

uint64_t size_of(const std::string &utf8) {
	std::error_code ec;
	const uintmax_t size = fs::file_size(io::os_path(utf8), ec);
	return ec ? 0 : uint64_t(size);
}

} // namespace

// A write leaves the whole text and no .tmp; a read gives it back, bytes and text alike; a
// rewrite replaces it; a read of no file and a write into no folder fail, saying which path.
static int test_write_and_read_back() {
	test_temp::TempDir dir("opennova_file_io_round_trip");
	const std::string path = dir.file("a.def");
	std::string error;
	TEST_EXPECT(io::write_file_atomic(path, std::string("weapon \"GUN\"\r\nend\r\n"), error));
	TEST_EXPECT(!exists(path + ".tmp"));
	std::string text;
	TEST_EXPECT(io::read_file_text(path, text, error) && text == "weapon \"GUN\"\r\nend\r\n");
	const uint8_t bytes[] = {0, 1, 2, 0xFF};
	TEST_EXPECT(io::write_file_atomic(path, bytes, sizeof(bytes), error));
	std::vector<uint8_t> read;
	TEST_EXPECT(io::read_file_bytes(path, read, error) && read == std::vector<uint8_t>(bytes, bytes + sizeof(bytes)));
	TEST_EXPECT(io::write_file_atomic(path, nullptr, 0, error) && size_of(path) == 0);
	TEST_EXPECT(io::read_file_bytes(path, read, error) && read.empty());

	const std::string missing = dir.file("missing.def");
	error.clear();
	TEST_EXPECT(!io::read_file_bytes(missing, read, error) && error == "cannot open " + missing);
	const std::string nowhere = dir.file("no/such/folder/b.def");
	error.clear();
	TEST_EXPECT(!io::write_file_atomic(nowhere, std::string("x"), error) && error == "cannot create " + nowhere + ".tmp");
	return 0;
}

// A file rewritten at its size moves its stamp however soon after its last write (S13 A3: a cache
// tells a change by the size and the last write, and a file system's clock can stand still for
// milliseconds, Linux's above all): ten back-to-back rewrites of one size through
// write_file_atomic, and ten through write_files_together, each leave a later last write.
static int test_rewrite_moves_the_stamp() {
	test_temp::TempDir dir("opennova_file_io_stamp");
	const std::string path = dir.file("same.def");
	std::string error;
	TEST_EXPECT(io::write_file_atomic(path, std::string("weapon \"GUN_A\"\nend\n"), error));
	int64_t last = io::file_modified_ticks(io::os_path(path));
	for (int i = 0; i < 20; ++i) {
		const std::string text = std::string("weapon \"GUN_") + char('B' + i) + "\"\nend\n";
		std::vector<std::string> problems;
		TEST_EXPECT(i < 10 ? io::write_file_atomic(path, text, error) : io::write_files_together({{path, text}}, problems));
		const int64_t now = io::file_modified_ticks(io::os_path(path));
		TEST_EXPECT(now > last && size_of(path) == text.size());
		last = now;
	}
	return 0;
}

// A rename the system refuses while another holds the file (on Windows a reader that does not share
// delete: an indexer, a scanner) is tried again a bounded few times and then refused with the refusal
// that may pass, never waited on; with the file let go it renames (rename_with_retry, which a save's
// replace, a build's publish and a case-only rename take).
static int test_rename_retry_is_bounded() {
	test_temp::TempDir dir("opennova_file_io_rename_retry");
	const std::string from = dir.file("held.txt"), to = dir.file("HELD2.txt");
	std::string error;
	TEST_EXPECT(io::write_file_atomic(from, std::string("held"), error));
	std::error_code ec;
#ifdef _WIN32
	{
		std::ifstream holder(io::os_path(from), std::ios::binary);
		TEST_EXPECT(holder.is_open());
		const auto began = std::chrono::steady_clock::now();
		TEST_EXPECT(!io::rename_with_retry(io::os_path(from), io::os_path(to), ec) && io::rename_refusal_passes(ec));
		TEST_EXPECT(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
	}
#endif
	TEST_EXPECT(io::rename_with_retry(io::os_path(from), io::os_path(to), ec) && exists(to) && !exists(from));
	return 0;
}

// Several files as one: all written when every replace goes through; when the second is refused,
// the first gets its bytes back, no .tmp is left, and the refusal is said first.
static int test_write_files_together() {
	test_temp::TempDir dir("opennova_file_io_together");
	const std::string a = dir.file("a.txt"), b = dir.file("b.txt");
	std::string error, text;
	TEST_EXPECT(io::write_file_atomic(a, std::string("old a"), error) && io::write_file_atomic(b, std::string("old b"), error));
	std::vector<std::string> problems;
	TEST_EXPECT(io::write_files_together({{a, "new a"}, {b, "new b"}}, problems) && problems.empty());
	TEST_EXPECT(io::read_file_text(a, text, error) && text == "new a");
	TEST_EXPECT(io::read_file_text(b, text, error) && text == "new b");

	int replaced = 0;
	const io::FileReplace refuse_second = [&](const std::string &from, const std::string &to, std::string &why) {
		if (replaced++ == 1) {
			why = "refused " + to;
			return false;
		}
		return io::replace_file(from, to, why);
	};
	TEST_EXPECT(!io::write_files_together({{a, "newer a"}, {b, "newer b"}}, problems, refuse_second));
	TEST_EXPECT(problems.size() == 1 && problems[0] == "refused " + b);
	TEST_EXPECT(io::read_file_text(a, text, error) && text == "new a");
	TEST_EXPECT(io::read_file_text(b, text, error) && text == "new b");
	TEST_EXPECT(!exists(a + ".tmp") && !exists(b + ".tmp"));

	// A file that cannot be read refuses the whole set before anything is written.
	problems.clear();
	TEST_EXPECT(!io::write_files_together({{a, "x"}, {dir.file("gone.txt"), "y"}}, problems));
	TEST_EXPECT(problems.size() == 1 && io::read_file_text(a, text, error) && text == "new a" && !exists(a + ".tmp"));
	return 0;
}

// A file made only where none stands; a last write set to now; mkdir -p; a hard link, one file
// under two names, refused where the second name is taken.
static int test_create_date_mkdir_link() {
	test_temp::TempDir dir("opennova_file_io_chores");
	const std::string made = dir.file("made.bin");
	std::FILE *f = io::create_new_file(made);
	TEST_EXPECT(f != nullptr);
	TEST_EXPECT(std::fputs("one", f) >= 0 && std::fclose(f) == 0);
	TEST_EXPECT(io::create_new_file(made) == nullptr && size_of(made) == 3);

	std::error_code ec;
	fs::last_write_time(io::os_path(made), fs::file_time_type::clock::now() - std::chrono::hours(1), ec);
	TEST_EXPECT(!ec);
	const int64_t old = io::file_modified_ticks(io::os_path(made));
	std::string error;
	TEST_EXPECT(io::refresh_last_write(made, error) && io::file_modified_ticks(io::os_path(made)) > old);
	TEST_EXPECT(!io::refresh_last_write(dir.file("gone.bin"), error) && error.rfind("cannot date ", 0) == 0);

	const std::string deep = dir.file("one/two/three");
	TEST_EXPECT(io::ensure_directory(deep, error) && fs::is_directory(io::os_path(deep)));
	TEST_EXPECT(io::ensure_directory(deep, error));

	const std::string linked = dir.file("one/linked.bin");
	TEST_EXPECT(io::link_file(made, linked, error));
	std::string text;
	TEST_EXPECT(io::read_file_text(linked, text, error) && text == "one" && fs::hard_link_count(io::os_path(made)) == 2);
	TEST_EXPECT(!io::link_file(made, linked, error) && error.rfind("cannot link " + linked, 0) == 0);
	return 0;
}

// Every helper through a UTF-8 path outside the ANSI code page and deeper than MAX_PATH: on
// Windows a narrow path names another file and one of 260 characters or more fails without the
// \\?\ prefix (this test executable is not long-path aware); elsewhere it passes trivially.
static int test_utf8_path_past_max_path() {
	test_temp::TempDir dir("opennova_file_io_long");
	std::string deep = dir.file("Jos\xC3\xA9");
	while (deep.size() < 400) deep += "/\xE3\x83\xA2\xE3\x83\x87\xE3\x83\xAB_folder_of_some_length";
	std::string error, text;
	TEST_EXPECT(io::ensure_directory(deep, error));
	const std::string path = deep + "/\xE3\x83\xA2.txt";
	TEST_EXPECT(io::os_path(path).native().size() > 260);
	TEST_EXPECT(io::write_file_atomic(path, std::string("deep"), error) && io::write_file_atomic(path, std::string("deeper"), error));
	TEST_EXPECT(io::read_file_text(path, text, error) && text == "deeper" && !exists(path + ".tmp"));
	std::vector<std::string> problems;
	TEST_EXPECT(io::write_files_together({{path, "deepest"}}, problems) && io::read_file_text(path, text, error) && text == "deepest");
	TEST_EXPECT(io::refresh_last_write(path, error));
	const std::string copy = deep + "/copy.txt";
	std::FILE *f = io::create_new_file(copy);
	TEST_EXPECT(f != nullptr && std::fclose(f) == 0 && exists(copy));
	const std::string linked = deep + "/linked.txt";
	TEST_EXPECT(io::link_file(path, linked, error) && io::read_file_text(linked, text, error) && text == "deepest");
	remove_tree(dir.file("Jos\xC3\xA9"));
	TEST_EXPECT(!exists(path) && !exists(dir.file("Jos\xC3\xA9")));
	return 0;
}

// path_within: the folder itself and what lies under it, symbolic links resolved and "." / ".."
// read; never a sibling that shares its name's start, the parent, or another root.
static int test_path_within() {
	test_temp::TempDir dir("opennova_file_io_within");
	std::string error;
	TEST_EXPECT(io::ensure_directory(dir.file("abc/sub"), error) && io::ensure_directory(dir.file("abcd"), error));
	const fs::path abc = io::os_path(dir.file("abc"));
	TEST_EXPECT(io::path_within(abc, abc));
	TEST_EXPECT(io::path_within(io::os_path(dir.file("abc/sub")), abc));
	TEST_EXPECT(io::path_within(io::os_path(dir.file("abc/not/made/yet.txt")), abc));
	TEST_EXPECT(io::path_within(io::os_path(dir.file("abcd/../abc/sub")), abc));
	TEST_EXPECT(!io::path_within(io::os_path(dir.file("abcd")), abc));
	TEST_EXPECT(!io::path_within(io::os_path(dir.file("abc/sub/../..")), abc));
	TEST_EXPECT(!io::path_within(io::os_path(dir.root()), abc));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_write_and_read_back();
	failures += test_rewrite_moves_the_stamp();
	failures += test_rename_retry_is_bounded();
	failures += test_write_files_together();
	failures += test_create_date_mkdir_link();
	failures += test_utf8_path_past_max_path();
	failures += test_path_within();
	if (failures == 0) std::printf("file_io: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
