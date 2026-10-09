// base/gameprofile/graphics_log.h: the missions the game's ghw.txt says it began loading, each
// finished by the "Mission loading complete" after it.
#include <cstdio>
#include <string>
#include <vector>

#include <base/gameprofile/graphics_log.h>

#include "common/test_expect.h"

using namespace opennova::gameprofile;

// The graphics log's missions: each `Mission:"<file>"` line, finished by the "Mission loading complete"
// after it; CR LF and LF lines alike, a completion before any mission read as none.
static int test_graphics_log() {
	const std::vector<GraphicsLogMission> missions = graphics_log_missions(
	        "GHW.TXT - LOG FILE - 10/6/2026\r\nMission:\"A.BMS\" - \"x\" - \"A.BMS\" - \"\"\r\nSniper_Start()\r\n"
	        "Mission loading complete\r\nMission:\"B.BMS\" - \"y\" - \"B.BMS\" - \"\"\nTaking Snapshot\n");
	TEST_EXPECT(missions.size() == 2 && missions[0].file == "A.BMS" && missions[0].complete && missions[1].file == "B.BMS" &&
	            !missions[1].complete);
	TEST_EXPECT(graphics_log_missions("Mission loading complete\n").empty());
	TEST_EXPECT(graphics_log_missions("Mission:\"unclosed\n").empty());
	TEST_EXPECT(graphics_log_missions("").empty());
	TEST_EXPECT(std::string(kGraphicsLogName) == "ghw.txt");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_graphics_log();
	if (failures == 0) std::printf("graphics_log: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
