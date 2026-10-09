// The original game's /FRISK file log (formats/filelog/file_access_log.h): its two line forms [orig:
// File_LogFileAccess @ 0x75a510], the archives, the files they served and the files read from disk, each
// name once (the first spelling, compared without case), in the order first opened; a line of neither form
// counted and nothing else; "\n" and "\r\n" line ends alike.
#include <formats/filelog/file_access_log.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::filelog;

namespace {

int test_file_access_log() {
	const FileAccessLog log = parse_file_access_log("LOADED FILE: language.pff\n"
	                                                "LOADED FILE: localres.pff\r\n"
	                                                "LOADED FILE: RESOURCE.PFF\n"
	                                                "PFF LOADED FILE: gameerr.bin\n"
	                                                "PFF LOADED FILE: weapon.def\n"
	                                                "PFF LOADED FILE: WEAPON.DEF\n"
	                                                "LOADED FILE: player.sav\n"
	                                                "LOADED FILE: expansion\\jxm\\jxm.bin\n"
	                                                "something else\n"
	                                                "PFF LOADED FILE: main.mnu");
	TEST_EXPECT(log.lines == 10);
	TEST_EXPECT(log.archives == std::vector<std::string>({"language.pff", "localres.pff", "RESOURCE.PFF"}));
	TEST_EXPECT(log.from_archives == std::vector<std::string>({"gameerr.bin", "weapon.def", "main.mnu"}));
	TEST_EXPECT(log.from_disk == std::vector<std::string>({"player.sav", "expansion\\jxm\\jxm.bin"}));
	TEST_EXPECT(parse_file_access_log("") == FileAccessLog());
	FileAccessLog one;
	add_file_access_line(one, "PFF LOADED FILE: keyhelp.bin");
	TEST_EXPECT(one.lines == 1 && one.from_archives == std::vector<std::string>({"keyhelp.bin"}) && one.archives.empty());
	TEST_EXPECT(std::string(kInstallFileLogName) == "_filelog.txt");
	std::printf("file access log: three archives, three files served, two from disk\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_file_access_log();
	if (failures == 0) std::printf("filelog_file_access_log: all passed\n");
	return failures == 0 ? 0 : 1;
}
