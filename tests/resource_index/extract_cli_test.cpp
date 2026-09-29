// opennova-extract mounts an install as a launch with its flags mounts it (mount_install):
// a stock launch reads the archive's file where a loose one of the name sits beside it,
// /d the loose one; the tool's own options keep their values (`--out /d` names a
// directory, not the loose-override flag); a repeated /exp takes its last value, as the
// game's command line does [orig: Game_ParseCommandLineAndInit @ 0x4a7310, "/exp"
// @ 0x4a76a6 -> g_ExpansionName @ 0x4a76cf].
//
//   extract_cli_test <opennova-extract> <scratch dir>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <formats/pff/pff.h>

#include "common/run_command.h"
#include "common/test_expect.h"

namespace fs = std::filesystem;
using test_cmd::quoted;
using test_cmd::run;

namespace {

std::string text_of(const fs::path &path) {
	std::ifstream in(path.string(), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool archive(const fs::path &path, const char *text) {
	const opennova::pff::PffWriteEntry entries[] = {
	    {"note.txt", reinterpret_cast<const uint8_t *>(text), uint32_t(std::char_traits<char>::length(text)), 0, 0, 0}};
	return opennova::pff::pff_write_archive(path.string().c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	       opennova::pff::PFF_WRITE_OK;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: extract_cli_test <opennova-extract> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const fs::path root = fs::path(argv[2]) /
	                      ("opennova_extract_cli_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct TempRoot {
		fs::path path;
		~TempRoot() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	} const cleanup{root};
	const fs::path install = root / "install", out = root / "out", log = root / "log.txt";
	std::error_code ec;
	fs::create_directories(install / "expansion" / "xp1", ec);
	fs::create_directories(out, ec);
	TEST_EXPECT(archive(install / "resource.pff", "packed"));
	TEST_EXPECT(archive(install / "expansion" / "xp1" / "xp1.pff", "expansion"));
	std::ofstream((install / "note.txt").string(), std::ios::binary) << "loose";

	const auto extract = [&](const std::string &args) {
		fs::remove(out / "note.txt", ec);
		return run(cli + " --game " + quoted(install.string()) + " " + args + " > " + quoted(log.string()));
	};
	const std::string to_out = " --out " + quoted(out.string()) + " note.txt";

	// Stock: the archive's; /d: the loose file.
	TEST_EXPECT(extract(to_out) == 0 && text_of(out / "note.txt") == "packed");
	TEST_EXPECT(extract("/d" + to_out) == 0 && text_of(out / "note.txt") == "loose");
	// An --out value that spells a launch flag stays the directory: the mount is stock. The
	// entry is absent, so nothing is written there.
	TEST_EXPECT(extract("--out /d absent.txt") != 0 && text_of(log).find("archives only") != std::string::npos);
	// A repeated /exp takes its last value.
	TEST_EXPECT(extract("/exp xp1 /exp none" + to_out) == 0 && text_of(out / "note.txt") == "packed");
	TEST_EXPECT(extract("/exp none /exp xp1" + to_out) == 0 && text_of(out / "note.txt") == "expansion" &&
	            text_of(log).find("expansion: xp1") != std::string::npos);
	std::printf("extract_cli: OK\n");
	return 0;
}
