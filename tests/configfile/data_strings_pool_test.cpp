// The ConfigFile text reader's data-strings pool (formats/configfile DataStringsPool). The reader sizes its
// pool of text values by their bytes (each its length and one), FastMem_Alloc rounds that up to 64, and the
// parse clears it one byte per value [orig: ConfigFile_ParseText @ 0x7609e8]: a file of more values than the
// rounded pool writes past it into the game's heap. Covered: the allocator's rounding; the two fixtures on
// either side of the line (64 values and 65 over a 64-byte pool); the three witnessed files' numbers (the
// earlier base game's 72 values over 58 bytes, 8 past; the first try of classes 1 to 9, 105 over 60, 41 past,
// which crashed retail's mission start; JO:CA's 278 over 288, under); the reader's own count (a value past a
// line's first 255 bytes counted as the last one read again; a key written with leading spaces read from the
// next line of its key; a CBIN file, which the binary reader takes); the first value past the pool; the
// reader's comment put before lines, which takes their values away.
#include <cstdio>
#include <string>
#include <vector>

#include <formats/configfile/config_file.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;

namespace {

configfile::DataStringsPool pool_of(const std::string &text) {
	return configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::vector<configfile::ConfigSection> sections_of(const std::string &text) {
	return configfile::parse_config_text(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::string fixture_text(const char *name) {
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/configfile/" + name;
	std::vector<uint8_t> bytes;
	if (!test_io::read_file(path, bytes)) std::fprintf(stderr, "cannot read %s\n", path.c_str());
	return std::string(bytes.begin(), bytes.end());
}

// A charattr.def of `classes` classes in the form the base game's earlier files took: each class the eleven
// number keys (SCOPE_MUTE at 0.0 and RUN_MODIFIER at 0 where `zeros`, which the loader reads the same missing,
// else a value away from 0), then its ATTRIBUTES word where `words` names one ("" for none). CR LF line ends.
std::string charattr_text(const std::vector<std::string> &words, bool zeros) {
	std::string text = "; charattr.def in the base game's earlier form\r\n";
	for (size_t i = 0; i < words.size(); ++i) {
		text += "[CHARACTER" + std::to_string(i + 1) + "]\r\n";
		text += "STEALTH\t\t= 25\r\nHPBONUS\t\t= 2\r\nRECOIL_MUTE\t= 0.75\r\nXHAIR_MUTE\t= 2.5\r\nXHAIRDX_MUTE\t= 2.5\r\n";
		text += zeros ? "SCOPE_MUTE\t= 0.0\r\n" : "SCOPE_MUTE\t= 0.25\r\n";
		text += "RELOAD_MUTE\t= 0.5\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n";
		text += zeros ? "RUN_MODIFIER\t= 0\r\n" : "RUN_MODIFIER\t= 1\r\n";
		if (!words[i].empty()) text += "ATTRIBUTES\t= " + words[i] + "\r\n";
		text += "\r\n";
	}
	return text;
}

int test_pool_arithmetic() {
	// FastMem_Alloc: a size under 1 is 1, rounded up to 64 [orig: FastMem_Alloc @ 0x7697c4..0x7697d7].
	TEST_EXPECT(configfile::fastmem_block_bytes(0) == 64 && configfile::fastmem_block_bytes(1) == 64 &&
	            configfile::fastmem_block_bytes(64) == 64 && configfile::fastmem_block_bytes(65) == 128 &&
	            configfile::fastmem_block_bytes(288) == 320);
	// The three witnessed files' numbers: values (the clear's length) against the text values' bytes.
	const auto overrun = [](uint32_t values, uint32_t bytes) {
		configfile::DataStringsPool pool;
		pool.values = values;
		pool.string_bytes = bytes;
		pool.pool_bytes = configfile::fastmem_block_bytes(bytes);
		return pool.overrun();
	};
	TEST_EXPECT(overrun(72, 58) == 8);   // the base game's earlier charattr.def: it ran, its heap corrupt
	TEST_EXPECT(overrun(105, 60) == 41); // the first try of classes 1 to 9: retail's mission start crashed
	TEST_EXPECT(overrun(278, 288) == 0); // JO:CA's own charattr.def
	TEST_EXPECT(overrun(64, 60) == 0 && overrun(65, 60) == 1 && overrun(64, 0) == 0 && overrun(65, 0) == 1);
	std::printf("pool: the allocator's 64; 72/58 8 past, 105/60 41 past, 278/288 under\n");
	return 0;
}

int test_fixtures_and_examples() {
	// The fixtures, on either side of the line.
	const configfile::DataStringsPool at_line = pool_of(fixture_text("pool_at_line.def"));
	TEST_EXPECT(!at_line.binary && at_line.values == 64 && at_line.string_bytes == 60 && at_line.pool_bytes == 64 &&
	            at_line.overrun() == 0);
	const configfile::DataStringsPool past = pool_of(fixture_text("pool_past_line.def"));
	TEST_EXPECT(past.values == 65 && past.string_bytes == 60 && past.pool_bytes == 64 && past.overrun() == 1);
	// The examples' files as the reader counts them: the earlier base game's (six classes, AutoScope, Medic,
	// AutoScope, AutoScope, KnifeBonus, KnifeBonus: 72 values over 58 bytes) and the first try of classes 1 to 9
	// (2 to 4 with no word: 105 over 60).
	const configfile::DataStringsPool earlier =
			pool_of(charattr_text({ "AutoScope", "Medic", "AutoScope", "AutoScope", "KnifeBonus", "KnifeBonus" }, true));
	TEST_EXPECT(earlier.values == 72 && earlier.string_bytes == 58 && earlier.overrun() == 8);
	const configfile::DataStringsPool first_try = pool_of(charattr_text(
			{ "AutoScope", "", "", "", "Medic", "AutoScope", "SpreadBonus", "KnifeBonus", "KnifeBonus" }, false));
	TEST_EXPECT(first_try.values == 105 && first_try.string_bytes == 60 && first_try.overrun() == 41);
	std::printf("fixtures: 64/60 at the line, 65/60 one past; the examples' files 8 and 41 past\n");
	return 0;
}

int test_reader_count() {
	// No file, or the CBIN form (the binary reader's): no text pool.
	TEST_EXPECT(pool_of(std::string()).values == 0 && pool_of(std::string()).overrun() == 0);
	const configfile::DataStringsPool binary = pool_of(std::string("CBIN\x01\x00\x00\x00", 8));
	TEST_EXPECT(binary.binary && binary.overrun() == 0);
	// Words and numbers: a word its length and one, a number nothing; a value outside any section is none.
	const configfile::DataStringsPool plain = pool_of(std::string("K = 9\r\n[S]\r\nK = ab, 7 1.5 -2 cd\r\n"));
	TEST_EXPECT(plain.values == 5 && plain.string_bytes == 6);
	// A value past the line's first 255 bytes [orig: String_CopyN @ 0x75eca0]: the walk cannot read it, so the
	// last value read is counted again ("abc" twice: 8 bytes, not 4 + 3).
	const std::string far = "[S]\r\nK = abc" + std::string(260, ',') + "zz\r\n";
	const configfile::DataStringsPool past_window = pool_of(far);
	TEST_EXPECT(past_window.values == 2 && past_window.string_bytes == 8);
	// A token the 255 bytes cut is read cut: "K = " and 241 commas leave 10 of its letters.
	const std::string cut = "[S]\r\nK = " + std::string(241, ',') + "abcdefghijklmnop\r\n";
	TEST_EXPECT(pool_of(cut).values == 1 && pool_of(cut).string_bytes == 11);
	// A key written with leading spaces does not match its own line: the walk reads the next line of the key, a
	// number (no byte), where the entry holds a word [orig: ConfigFile_ReadKeyValue @ 0x75fdfd].
	const configfile::DataStringsPool spaced = pool_of(std::string("[S]\r\n  K = word\r\nK = 1 2\r\n"));
	TEST_EXPECT(spaced.values == 3 && spaced.string_bytes == 0);
	// ... and where a '[' line comes first, nothing is read: the buffer as it was (empty here), one byte.
	const configfile::DataStringsPool stopped = pool_of(std::string("[S]\r\n  K = word\r\n[T]\r\nK = 1\r\n"));
	TEST_EXPECT(stopped.values == 2 && stopped.string_bytes == 1);
	std::printf("reader: the CBIN form none; the 255-byte line, a cut token, a spaced key, the walk's stop\n");
	return 0;
}

// The first value past the pool, in the reader's order: value 65 of pool_past_line.def, its last value (the
// sixth class's ATTRIBUTES word); none for a file at the line.
int test_overrun_offset() {
	const std::string past = fixture_text("pool_past_line.def");
	const size_t offset = configfile::data_strings_overrun_offset(sections_of(past), pool_of(past));
	TEST_EXPECT(offset == past.rfind("KnifeBonus") && offset != 0);
	const std::string at_line = fixture_text("pool_at_line.def");
	TEST_EXPECT(configfile::data_strings_overrun_offset(sections_of(at_line), pool_of(at_line)) == 0);
	// Value 9 of a pool of 8 (a hand-sized pool): the second line's first value.
	const std::string text = "[S]\r\nK = 1 2 3 4 5 6 7 8\r\nL = 9 10\r\n";
	configfile::DataStringsPool small;
	small.pool_bytes = 8;
	TEST_EXPECT(configfile::data_strings_overrun_offset(sections_of(text), small) == text.find("9 10"));
	std::printf("overrun: value 65 at the last word; none at the line\n");
	return 0;
}

// The reader's comment put before lines: each listed line's values taken away, the rest kept; an offset past
// the text, or one given twice, puts none.
int test_commented() {
	const std::string text = "[S]\r\nA = 1\r\nB = word\r\nC = 2 3\r\n";
	const size_t a = text.find("A ="), c = text.find("C =");
	const std::string commented = configfile::config_commented(text, { c, a, c, text.size() + 4 });
	TEST_EXPECT(commented == "[S]\r\n;A = 1\r\nB = word\r\n;C = 2 3\r\n");
	const std::vector<configfile::ConfigSection> sections = sections_of(commented);
	TEST_EXPECT(sections.size() == 1 && sections[0].entries.size() == 1 && sections[0].entries[0].key == "B");
	TEST_EXPECT(pool_of(commented).values == 1 && pool_of(text).values == 4);
	TEST_EXPECT(configfile::config_commented(text, {}) == text);
	std::printf("commented: two lines taken away, the rest read\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_pool_arithmetic();
	failures += test_fixtures_and_examples();
	failures += test_reader_count();
	failures += test_overrun_offset();
	failures += test_commented();
	if (failures == 0) std::printf("configfile_data_strings_pool: all passed\n");
	return failures == 0 ? 0 : 1;
}
