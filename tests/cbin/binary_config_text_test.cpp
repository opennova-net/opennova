// The CBIN form and its ConfigFile text form (formats/cbin/binary_config_text.h): a CBIN file laid out as
// the shipped credits are, through its text form and back byte for byte (its string table first in its
// order, under its key); an edit written in the form, read back as the edited text; one the text form
// cannot carry (the minted synth_nlist.kda, whose lines hold spaces the text form would read as
// separators) refused with why; the lines a text holds that the reader does not read whole (a comment, a
// line outside a section, a label not in capitals, an entry of no value or of three, a CR or an LF alone),
// each refused at its line.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <formats/cbin/binary_config.h>
#include <formats/cbin/binary_config_text.h>
#include <formats/configfile/config_file.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;
using cbin::BinaryConfig;

namespace {

// A CBIN credits file laid out as the shipped one is (its [ENV] values, then its [TEXT] lines: a
// justify, a colour, a line with its font, a line end, an image; a space written '_', which the
// marquee draws as one), its form made by the writer.
BinaryConfig minted_credits() {
	BinaryConfig config;
	config.strings = {"env", "text", "scroll_rate", "vertical_space", "center_x", "~JR", "~CFF0000",
	                  "Joint_Operations:", "Serpen24", "<CR>", "~F0|0|cr1.png"};
	config.xor_key = 0x5EEDF00Du;
	const auto value = [](uint32_t raw, uint32_t flags) { return BinaryConfig::Value{raw, flags}; };
	float half = 0.5f;
	uint32_t half_bits = 0;
	std::memcpy(&half_bits, &half, sizeof half_bits);
	BinaryConfig::Label env{1, {{3, {value(half_bits, BinaryConfig::kFloat)}}, {4, {value(14, BinaryConfig::kInteger)}},
	                            {5, {value(400, BinaryConfig::kInteger)}}}};
	BinaryConfig::Label text{2, {{2, {value(6, BinaryConfig::kString)}}, {2, {value(7, BinaryConfig::kString)}},
	                             {2, {value(8, BinaryConfig::kString), value(9, BinaryConfig::kString)}},
	                             {2, {value(10, BinaryConfig::kString)}}, {2, {value(11, BinaryConfig::kString)}}}};
	config.labels = {env, text};
	return config;
}

std::vector<uint8_t> encoded(const BinaryConfig &config) {
	std::vector<uint8_t> out;
	std::string error;
	if (!cbin::encode_binary_config(config, out, error)) std::fprintf(stderr, "encode: %s\n", error.c_str());
	return out;
}

// The CBIN form through its text form and back: the text the reader reads as the form holds it, written
// back in the form the same bytes; an edited line written in the form and read back as the edited text.
int test_round_trip() {
	const std::vector<uint8_t> stored = encoded(minted_credits());
	TEST_EXPECT(!stored.empty());
	BinaryConfig read;
	std::string error;
	TEST_EXPECT(cbin::decode_binary_config(stored.data(), stored.size(), read, error));
	std::string text, why;
	TEST_EXPECT(cbin::binary_config_text(read, text, why));
	TEST_EXPECT(text == "[ENV]\r\nscroll_rate = 0.5\r\nvertical_space = 14\r\ncenter_x = 400\r\n[TEXT]\r\n"
	                    "text = ~JR\r\ntext = ~CFF0000\r\ntext = Joint_Operations:, Serpen24\r\ntext = <CR>\r\n"
	                    "text = ~F0|0|cr1.png\r\n");
	cbin::ConfigTextRefusal refusal;
	TEST_EXPECT(cbin::binary_config_text_readable(text, refusal));
	TEST_EXPECT(encoded(cbin::binary_config_from_text(text, read.strings, read.xor_key)) == stored);
	// A line edited: written in the form, which reads back as the edited text, the new string after the kept table.
	std::string edited = text;
	const std::string line = "text = ~JR\r\n";
	edited.replace(edited.find(line), line.size(), "text = Edited_by_D9, font\r\n");
	TEST_EXPECT(cbin::binary_config_text_readable(edited, refusal));
	const BinaryConfig from_edit = cbin::binary_config_from_text(edited, read.strings, read.xor_key);
	TEST_EXPECT(from_edit.strings.size() == read.strings.size() + 2 && from_edit.strings.back() == "font");
	const std::vector<uint8_t> written = encoded(from_edit);
	TEST_EXPECT(!written.empty() && written != stored);
	BinaryConfig again;
	std::string again_text;
	TEST_EXPECT(cbin::decode_binary_config(written.data(), written.size(), again, error) &&
	            cbin::binary_config_text(again, again_text, why) && again_text == edited);
	std::printf("round trip: %zu bytes through %zu characters of text and back the same; an edit written\n",
	            stored.size(), text.size());
	return 0;
}

// What the text form cannot carry: a value of a space (the minted synth_nlist.kda), a float that is no
// number, a string that reads as a number.
int test_no_text_form() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<uint8_t> spaced = test_io::read_file(repo + "/fixtures/cbin/synth_nlist.kda");
	BinaryConfig read;
	std::string error, text, why;
	TEST_EXPECT(!spaced.empty() && cbin::decode_binary_config(spaced.data(), spaced.size(), read, error));
	TEST_EXPECT(!cbin::binary_config_text(read, text, why) && why.find("which its text form would read otherwise") !=
	                                                                    std::string::npos);
	BinaryConfig nan = minted_credits();
	nan.labels[0].entries[0].values[0].raw = 0x7FC00000u;
	TEST_EXPECT(!cbin::binary_config_text(nan, text, why) && why.find("no number") != std::string::npos);
	BinaryConfig number = minted_credits();
	number.strings[5] = "12";
	TEST_EXPECT(!cbin::binary_config_text(number, text, why) && why.find("\"12\"") != std::string::npos);
	BinaryConfig past = minted_credits();
	past.labels[1].entries[0].values[0].raw = 99;
	TEST_EXPECT(!cbin::binary_config_text(past, text, why) && why.find("past its string table") != std::string::npos);
	std::printf("no text form: a spaced value, a NaN, a numeric string, an index past the table\n");
	return 0;
}

// The CBIN form keeps what the reader reads and nothing else: a line it reads none of or only part of is
// refused at its line [orig: ConfigFile_ParseText @ 0x7608a0].
int test_readable() {
	const std::string head = "[ENV]\r\nrate = 1\r\n";
	const auto refused_at = [&](const std::string &line, size_t at, const char *says) {
		cbin::ConfigTextRefusal refusal;
		const bool read = cbin::binary_config_text_readable(head + line, refusal);
		if (!read && refusal.line == at && refusal.why.find(says) != std::string::npos) return true;
		std::fprintf(stderr, "readable: \"%s\" -> line %zu: %s\n", line.c_str(), refusal.line, refusal.why.c_str());
		return false;
	};
	TEST_EXPECT(refused_at("; a comment\r\n", 3, "comment"));
	TEST_EXPECT(refused_at("text = kept ; and a comment\r\n", 3, "';' comment"));
	TEST_EXPECT(refused_at("[text]\r\n", 3, "opens no section"));
	TEST_EXPECT(refused_at(" [TEXT]\r\n", 3, "after a blank"));
	TEST_EXPECT(refused_at("[TEXT] more\r\n", 3, "more than its section's label"));
	TEST_EXPECT(refused_at("text =\r\n", 3, "no value"));
	TEST_EXPECT(refused_at("no equals here\r\n", 3, "no '='"));
	TEST_EXPECT(refused_at("text = a, b, c\r\n", 3, "3 values"));
	TEST_EXPECT(refused_at("text = a\rb\r\n", 3, "CR alone"));
	TEST_EXPECT(refused_at(std::string("text = a\0b\r\n", 12), 3, "NUL"));
	cbin::ConfigTextRefusal refusal;
	TEST_EXPECT(!cbin::binary_config_text_readable("before = 1\r\n" + head, refusal) && refusal.line == 1 &&
	            refusal.why.find("outside any section") != std::string::npos);
	TEST_EXPECT(!cbin::binary_config_text_readable("[ENV]\r\nrate = 1\nmore = 2\r\n", refusal) && refusal.line == 2 &&
	            refusal.why.find("LF alone") != std::string::npos);
	// Blank lines, a trailing NUL and an entry of two values read whole.
	TEST_EXPECT(cbin::binary_config_text_readable(head + "\r\n   \r\nmore = a, 2\r\n" + std::string(1, '\0'), refusal));
	std::printf("readable: each line the reader does not read whole refused at its line\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_round_trip();
	failures += test_no_text_form();
	failures += test_readable();
	if (failures == 0) std::printf("cbin_binary_config_text: all passed\n");
	return failures == 0 ? 0 : 1;
}
