// The .env reader's report of the lines it reads otherwise than the record holds (formats/env env_source_issues):
// a keyword read over, a line skipped, a colour line short of its values, a time that reads as another, a last line
// no CR LF ends, a 17th tod_begin, an envscale after a colour it scales otherwise (blocking), a terrain key kept and
// one no line can write back (blocking) [orig: TimeOfDay_ParseProperty @ 0x57c590; File_ParseASCIIFile
// @ 0x53D8C7..0x53D8F5; Terrain_ParseConfigCallback @ 0x60F330 for the terrain keys, D-TERRAIN-18].
#include <formats/env/env_source_issue.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

namespace {

using opennova::env::EnvSourceIssue;
using opennova::env::env_source_issues;

bool has_issue(const std::vector<EnvSourceIssue> &issues, size_t line, bool blocks, const std::string &words) {
	for (const EnvSourceIssue &issue : issues)
		if (issue.line == line && issue.blocks == blocks && issue.message.find(words) != std::string::npos) return true;
	return false;
}

int test_ignored() {
	const std::string text = "enviro_name \"x\"\r\n" // 1
	                         "fog_level 500\r\n"     // 2
	                         "fog_level 600\r\n"     // 3: read over line 2
	                         "speling 3\r\n"         // 4: skipped
	                         "water_rgb 10,20\r\n"   // 5: two of three values
	                         "curtime 2400\r\n"      // 6: reads 23:00
	                         "sky_height 175\r\n";   // 7
	const std::vector<EnvSourceIssue> issues = env_source_issues(text);
	TEST_EXPECT(issues.size() == 4);
	TEST_EXPECT(has_issue(issues, 2, false, "written again on line 3") && issues[0].field == "fog_level");
	TEST_EXPECT(has_issue(issues, 4, false, "skips 'speling'"));
	TEST_EXPECT(has_issue(issues, 5, false, "2 of its three values"));
	TEST_EXPECT(has_issue(issues, 6, false, "23:00") && has_issue(issues, 6, false, "a save writes 2300"));
	// In line order.
	for (size_t i = 1; i < issues.size(); ++i) TEST_EXPECT(issues[i - 1].line <= issues[i].line);
	// A last line no CR LF ends loses its last byte (retail's FULL_03.ENV ends on "tod_en").
	TEST_EXPECT(has_issue(env_source_issues("sky_height 175\r\ncurtime 1200"), 2, false, "no line end"));
	// A 17th tod_begin takes no slot: its colours land where the slot pointer is.
	{
		std::string many;
		for (int i = 0; i < 17; ++i) many += "tod_begin " + std::to_string(100 * i) + "\r\n    sun_rgb 1,1,1\r\ntod_end\r\n";
		TEST_EXPECT(has_issue(env_source_issues("sky_height 175\r\n" + many), 50, false, "the colours with no keyframe"));
	}
	std::printf("ignored: a line read over, skipped, short, a time read as another, a cut last line, a 17th block\n");
	return 0;
}

int test_blocking() {
	// An envscale after a colour it would scale otherwise: the record holds one envscale for every colour.
	{
		const std::vector<EnvSourceIssue> issues = env_source_issues("water_rgb 100,100,100\r\nenvscale 2\r\nsky_height 175\r\n");
		TEST_EXPECT(issues.size() == 1 && has_issue(issues, 1, true, "envscale") && issues[0].field == "water_rgb");
		TEST_EXPECT(env_source_issues("envscale 2\r\nwater_rgb 100,100,100\r\nsky_height 175\r\n").empty());
	}
	// A terrain keyword is the terrain's (D-TERRAIN-18): no issue. horizon and terrain_name are read by no arm of either
	// reader (formats/trn trn_parser_key): skipped lines.
	TEST_EXPECT(env_source_issues("polytrn_colormap map.tga\r\nsky_height 175\r\n").empty());
	TEST_EXPECT(has_issue(env_source_issues("horizon 0\r\nsky_height 175\r\n"), 1, false, "skips"));
	TEST_EXPECT(has_issue(env_source_issues("sky_height 175\r\nterrain_name \"x\"\r\n"), 2, false, "skips 'terrain_name'"));
	// A terrain key's line no line writes back as the game reads it (its 30th token runs to the line's end, a
	// separator in it) blocks.
	{
		std::string line = "polytrn_sectors";
		for (int i = 0; i < 28; ++i) line += " 1";
		TEST_EXPECT(has_issue(env_source_issues("sky_height 175\r\n" + line + " x y\r\n"), 2, true, "cannot be written again"));
	}
	std::printf("blocking: an envscale after a colour, a terrain key no line writes back; terrain keys kept\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_ignored();
	failures += test_blocking();
	if (failures == 0) std::printf("env_source_issue: all passed\n");
	return failures == 0 ? 0 : 1;
}
