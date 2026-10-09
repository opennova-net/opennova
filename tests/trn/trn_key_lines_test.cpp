// A later file's terrain key lines (formats/trn, D-TERRAIN-18): what the terrain's parser makes of each from the
// file alone (trn_key_readings) [orig: Terrain_ParseConfigCallback @ 0x60F330] -- a grid row adds a row after the
// terrain's, a block's key with no block open, a line inside a block the file opens, and from a fifth block on no
// arm reads a line -- and the values a line writes quoted (trn_value_needs_quotes) [orig:
// Terrain_TokenizeConfigLine @0x53CC16..0x53CC4C].
#include <formats/trn/trn_io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

namespace {

int test_readings() {
	using opennova::TrnKeyLine;
	// One block the file opens and closes, its own keys inside: every line reads as written.
	{
		const std::vector<TrnKeyLine> keys = opennova::read_trn_key_lines(
				"sky_height 175\r\nPOLYTRN_COLORMAP \"red map.tga\" ; tinted\r\nlock_topleft 3,4\r\nfoliage\r\n"
				"graphic tree.3di\r\nfog_type 2\r\ncolor_lower 2\r\nend\r\npolytrn_detaildensity 64\r\n"
				"terrain_name \"x\"\r\n");
		const std::vector<TrnKeyLine> expected = {{"polytrn_colormap", {"red map.tga"}},
		                                          {"lock_topleft", {"3", "4"}},
		                                          {"foliage", {}},
		                                          {"graphic", {"tree.3di"}},
		                                          {"color_lower", {"2"}},
		                                          {"end", {}},
		                                          {"polytrn_detaildensity", {"64"}}};
		TEST_EXPECT(keys == expected);
		const std::vector<std::string> readings = opennova::trn_key_readings(keys);
		TEST_EXPECT(readings.size() == keys.size());
		for (const std::string &reading : readings) TEST_EXPECT(reading.empty());
	}
	// A grid row, a lone block key, a line inside the file's block, then a fifth block's lines.
	{
		std::string blocks;
		for (int i = 0; i < 5; ++i) blocks += "foliage\r\ngraphic g" + std::to_string(i) + ".3di\r\nend\r\n";
		const std::vector<TrnKeyLine> keys = opennova::read_trn_key_lines(
				"sky_height 175\r\npolytrn_sectors 1 1 1 1\r\ngraphic lone.3di\r\nfoliage\r\npolytrn_wrapx 1\r\nend\r\n" +
				blocks);
		TEST_EXPECT(keys.size() == 20);
		const std::vector<std::string> readings = opennova::trn_key_readings(keys);
		TEST_EXPECT(readings.size() == 20);
		// The grid row (0), the lone graphic (1), the wrap inside the block (3), and from the file's fifth block on
		// (14: the blocks at 2..4, 5..7, 8..10 and 11..13 close four) every line.
		std::vector<size_t> at;
		for (size_t i = 0; i < readings.size(); ++i)
			if (!readings[i].empty()) {
				TEST_EXPECT(readings[i].find("[orig: ") != std::string::npos);
				at.push_back(i);
			}
		TEST_EXPECT((at == std::vector<size_t>{0, 1, 3, 14, 15, 16, 17, 18, 19}));
		TEST_EXPECT(readings[0].find("row after the terrain's") != std::string::npos &&
		            readings[1].find("only inside a block") != std::string::npos &&
		            readings[3].find("inside the foliage block") != std::string::npos &&
		            readings[14].find("A fifth foliage block") != std::string::npos &&
		            readings[15].find("inside a fifth foliage block") != std::string::npos);
	}
	TEST_EXPECT(opennova::trn_key_readings({}).empty());
	std::printf("readings: a grid row, a lone block key, a line inside a block, a fifth block\n");
	return 0;
}

int test_needs_quotes() {
	for (const char *plain : {"red_c.tga", "a/b.tga", "x-y", "128"}) TEST_EXPECT(!opennova::trn_value_needs_quotes(plain));
	for (const char *cut : {"red map.tga", "a,b", "a\tb", "a;b", "a//b"}) TEST_EXPECT(opennova::trn_value_needs_quotes(cut));
	// What trn_values_text quotes.
	TEST_EXPECT(opennova::trn_values_text({"plain", "two words"}) == "plain \"two words\"");
	std::printf("needs quotes: the tokenizer's separators and comment starts\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_readings();
	failures += test_needs_quotes();
	if (failures == 0) std::printf("trn_key_lines: all passed\n");
	return failures == 0 ? 0 : 1;
}
