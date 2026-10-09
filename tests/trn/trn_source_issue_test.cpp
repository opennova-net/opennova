// The .trn readers' report of the lines they read otherwise than the record holds (formats/trn trn_source_issues):
// a grid row before the width, short of it or past it, more than 16 rows; a key skipped, written again; a water
// colour short or past a byte, a murk past 0.99; a block's codes past four or past a byte, an attrib word no arm
// reads, a colour mode past 0..2, a line no block arm reads; a fifth block swallowing the rest; a cut last line (a
// block's "end" read "en"); an environment keyword the record does not hold, polytrn_scale and polytrn_depthmap
// (blocking) [orig: Terrain_ParseConfigCallback @ 0x60F330; Environment_LoadTimeOfDayConfig @ 0x57DB30's .trn pass;
// File_ParseASCIIFile @ 0x53D8C7..0x53D8F5].
#include <formats/trn/trn_source_issue.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

namespace {

using opennova::TrnSourceIssue;
using opennova::TrnSourceRule;
using opennova::trn_source_issues;

const std::string kHead = "polytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\npolytrn_polydata h.cpt\r\n";

// The issue of `rule` on `line`, null for none (the issues printed).
const TrnSourceIssue *find(const std::vector<TrnSourceIssue> &issues, TrnSourceRule rule, size_t line) {
	for (const TrnSourceIssue &issue : issues)
		if (issue.rule == rule && issue.line == line) return &issue;
	std::fprintf(stderr, "no issue of rule %d on line %zu; the issues:\n", int(rule), line);
	for (const TrnSourceIssue &issue : issues)
		std::fprintf(stderr, "  %zu rule %d %s '%s'\n", issue.line, int(issue.rule), issue.blocks ? "blocks" : "ignored",
		             issue.field.c_str());
	return nullptr;
}

int test_read_otherwise() {
	const std::vector<TrnSourceIssue> issues = trn_source_issues(kHead +
	                                                             "polytrn_sectors 1 2\r\n"      // 4: before the width
	                                                             "polytrn_sectorcount 2\r\n"    // 5
	                                                             "polytrn_sectors 1\r\n"        // 6: short
	                                                             "polytrn_sectors 1 2 3\r\n"    // 7: wide
	                                                             "polytrn_scaled 3\r\n"         // 8: skipped
	                                                             "water_murk 1.0\r\n"           // 9: past 0.99
	                                                             "polytrn_detaildensity 64\r\n" // 10: written again on 11
	                                                             "polytrn_detaildensity 96\r\n" // 11
	                                                             "foliage\r\n"                  // 12
	                                                             "  graphic a.3di\r\n"          // 13
	                                                             "  match 1 2 3 4 5\r\n"        // 14: past four
	                                                             "  colour 3\r\n"               // 15: no such arm in a block
	                                                             "end\r\n");
	TEST_EXPECT(issues.size() == 8);
	const TrnSourceIssue *issue = find(issues, TrnSourceRule::RowBeforeWidth, 4);
	TEST_EXPECT(issue && !issue->blocks && issue->field == "polytrn_sectors" && issue->read == 2 && issue->kept == 0);
	issue = find(issues, TrnSourceRule::RowShort, 6);
	TEST_EXPECT(issue && issue->read == 1 && issue->kept == 2);
	issue = find(issues, TrnSourceRule::RowWide, 7);
	TEST_EXPECT(issue && issue->read == 3 && issue->kept == 2);
	issue = find(issues, TrnSourceRule::Skipped, 8);
	TEST_EXPECT(issue && issue->token == "polytrn_scaled" && !issue->blocks);
	TEST_EXPECT(find(issues, TrnSourceRule::MurkClamp, 9) != nullptr);
	issue = find(issues, TrnSourceRule::ReadAgain, 10);
	TEST_EXPECT(issue && issue->field == "polytrn_detaildensity" && issue->again == 11);
	TEST_EXPECT(find(issues, TrnSourceRule::MatchPastFour, 14) != nullptr);
	issue = find(issues, TrnSourceRule::BlockKeySkipped, 15);
	TEST_EXPECT(issue && issue->token == "colour");
	// In line order.
	for (size_t i = 1; i < issues.size(); ++i) TEST_EXPECT(issues[i - 1].line <= issues[i].line);
	std::printf("read otherwise: a row before the width, short, wide; a key skipped, written again; a murk past 0.99; "
	            "a block's codes past four, a line no block arm reads\n");
	return 0;
}

int test_values() {
	const std::vector<TrnSourceIssue> issues = trn_source_issues(kHead +
	                                                             "water_rgb 10,20\r\n"      // 4: two of three
	                                                             "water_rgb 300,0,0\r\n"    // 5: past a byte (and again)
	                                                             "foliage\r\n"              // 6
	                                                             "  graphic a.3di\r\n"      // 7
	                                                             "  match 300\r\n"          // 8: past a byte
	                                                             "  attrib shadow glow\r\n" // 9: glow read by no arm
	                                                             "  color_upper 3\r\n"      // 10: past 0..2
	                                                             "  fog_level 600\r\n"      // 11: the environment's, in a block
	                                                             "  water_height 4\r\n"     // 12: the record's, in a block
	                                                             "end\r\n");
	const TrnSourceIssue *issue = find(issues, TrnSourceRule::ShortColour, 4);
	TEST_EXPECT(issue && issue->read == 2 && issue->field == "water_rgb");
	issue = find(issues, TrnSourceRule::ColourByte, 5);
	TEST_EXPECT(issue && issue->read == 300 && issue->kept == 255);
	issue = find(issues, TrnSourceRule::ReadAgain, 4);
	TEST_EXPECT(issue && issue->again == 5);
	issue = find(issues, TrnSourceRule::MatchByte, 8);
	TEST_EXPECT(issue && issue->read == 300 && issue->kept == 44);
	issue = find(issues, TrnSourceRule::AttribWord, 9);
	TEST_EXPECT(issue && issue->token == "glow");
	issue = find(issues, TrnSourceRule::ColourMode, 10);
	TEST_EXPECT(issue && issue->field == "color_upper" && issue->read == 3 && issue->kept == 2);
	issue = find(issues, TrnSourceRule::EnvironmentKey, 11);
	TEST_EXPECT(issue && issue->blocks && issue->token == "fog_level");
	TEST_EXPECT(issues.size() == 7);
	std::printf("values: a colour short and past a byte, a code past a byte, an attrib word, a colour mode; an "
	            "environment keyword inside a block\n");
	return 0;
}

int test_structure() {
	// A fifth foliage block: from it on the file is read by no arm.
	{
		std::string blocks;
		for (int i = 0; i < 5; ++i) blocks += "foliage\r\n  graphic g" + std::to_string(i) + ".3di\r\n  match 9\r\nend\r\n";
		const std::vector<TrnSourceIssue> issues = trn_source_issues(kHead + blocks + "polytrn_charmap m.pcx\r\nspeling 1\r\n");
		TEST_EXPECT(issues.size() == 1 && find(issues, TrnSourceRule::FifthBlock, 20) != nullptr);
	}
	// A last line no CR LF ends: a block's "end" read "en", the block kept open; another line read short.
	{
		const std::vector<TrnSourceIssue> cut = trn_source_issues(kHead + "foliage\r\n  graphic a.3di\r\n  match 9\r\nend");
		const TrnSourceIssue *issue = cut.size() == 1 ? find(cut, TrnSourceRule::CutBlockEnd, 7) : nullptr;
		TEST_EXPECT(issue && issue->field == "end" && !issue->blocks);
		const std::vector<TrnSourceIssue> short_line = trn_source_issues(kHead + "polytrn_detaildensity 64");
		issue = find(short_line, TrnSourceRule::CutLastLine, 4);
		TEST_EXPECT(issue && issue->token == "4" && issue->field == "polytrn_detaildensity");
	}
	// More than 16 grid rows: refused by the gate, said on the 17th row's line.
	{
		std::string rows = "polytrn_sectorcount 1\r\n";
		for (int i = 0; i < 17; ++i) rows += "polytrn_sectors 1\r\n";
		const std::vector<TrnSourceIssue> issues = trn_source_issues(kHead + rows);
		const TrnSourceIssue *issue = find(issues, TrnSourceRule::TooManyRows, 21);
		TEST_EXPECT(issues.size() == 1 && issue && issue->read == 17);
	}
	std::printf("structure: a fifth block swallowing the rest, a cut last line, 17 grid rows\n");
	return 0;
}

int test_blocking() {
	// An environment keyword the record does not hold, polytrn_scale, polytrn_depthmap: each blocks.
	const auto one = [](const std::string &line, TrnSourceRule rule, bool blocks) {
		const std::vector<TrnSourceIssue> issues = trn_source_issues(kHead + line + "\r\n");
		const TrnSourceIssue *issue = issues.size() == 1 ? find(issues, rule, 4) : nullptr;
		return issue != nullptr && issue->blocks == blocks;
	};
	TEST_EXPECT(one("fog_level 600", TrnSourceRule::EnvironmentKey, true));
	TEST_EXPECT(one("tod_begin 0600", TrnSourceRule::EnvironmentKey, true));
	TEST_EXPECT(one("polytrn_scale 128", TrnSourceRule::ScaleKey, true));
	TEST_EXPECT(one("polytrn_depthmap d.raw", TrnSourceRule::DepthmapKey, true));
	// horizon: no arm reads it and the record does not keep it (D-TERRAIN-20): skipped, as any unknown key.
	TEST_EXPECT(one("horizon 0", TrnSourceRule::Skipped, false));
	// The record's own: the water's keywords and the two names, read once, nothing said.
	TEST_EXPECT(trn_source_issues(kHead + "terrain_name \"x\"\r\nterrain_creator \"y\"\r\nwater_height 21\r\n"
	                                      "water_rgb 108,81,48\r\nwater_murk .3\r\n")
	                    .empty());
	std::printf("blocking: an environment keyword, polytrn_scale, polytrn_depthmap; horizon skipped; the record's own "
	            "keys read\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_read_otherwise();
	failures += test_values();
	failures += test_structure();
	failures += test_blocking();
	if (failures == 0) std::printf("trn_source_issue: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
