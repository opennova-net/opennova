// The CBIN form and its ConfigFile text form (formats/cbin/binary_config_text.h): a CBIN file laid out as
// the shipped credits are, through its text form and back byte for byte (its string table first in its
// order, under its key); an edit written in the form, read back as the edited text; one the text form
// cannot carry (the minted synth_nlist.kda, whose lines hold spaces the text form would read as
// separators) refused with why; the lines a text holds that the reader does not read whole (a comment, a
// line outside a section, a label not in capitals, an entry of no value, a CR or an LF alone), each
// refused at its line, and an entry of three values carried whole. Every word the game's binary reader
// takes (D-CBIN-3) decodes and encodes back byte for byte, and its sections are the ones the accessors'
// walk reads: cut at a null name, past an entry of no value, on through a named terminator into the next
// label's block; a signaling NaN read as a float comes out quiet; what the game faults on faults at the
// read that reaches it, never at the load.
#include <cstdint>
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
	TEST_EXPECT(refused_at("text = a\rb\r\n", 3, "CR alone"));
	TEST_EXPECT(refused_at(std::string("text = a\0b\r\n", 12), 3, "NUL"));
	cbin::ConfigTextRefusal refusal;
	TEST_EXPECT(!cbin::binary_config_text_readable("before = 1\r\n" + head, refusal) && refusal.line == 1 &&
	            refusal.why.find("outside any section") != std::string::npos);
	TEST_EXPECT(!cbin::binary_config_text_readable("[ENV]\r\nrate = 1\nmore = 2\r\n", refusal) && refusal.line == 2 &&
	            refusal.why.find("LF alone") != std::string::npos);
	// Blank lines, a trailing NUL and an entry of two values read whole.
	TEST_EXPECT(cbin::binary_config_text_readable(head + "\r\n   \r\nmore = a, 2\r\n" + std::string(1, '\0'), refusal));
	// An entry of three values goes in the form whole and comes back as the same text [orig: the reader takes
	// any value count, ConfigFile_ParseBinary @ 0x75eb21].
	const std::string three = "[TEXT]\r\ntext = a, b, c\r\n";
	TEST_EXPECT(cbin::binary_config_text_readable(three, refusal));
	const BinaryConfig from_three = cbin::binary_config_from_text(three, {}, 0x1234u);
	TEST_EXPECT(from_three.labels.size() == 1 && from_three.labels[0].entries.size() == 1 &&
	            from_three.labels[0].entries[0].values.size() == 3);
	const std::vector<uint8_t> three_bytes = encoded(from_three);
	BinaryConfig three_back;
	std::string error, three_text, why;
	TEST_EXPECT(!three_bytes.empty() && cbin::decode_binary_config(three_bytes.data(), three_bytes.size(), three_back, error) &&
	            cbin::binary_config_text(three_back, three_text, why) && three_text == three);
	std::printf("readable: each line the reader does not read whole refused at its line; three values carried\n");
	return 0;
}

// The words under the cipher, then the string table, as the form lays them out, under `key`.
std::vector<uint8_t> laid_out(const std::vector<uint32_t> &words, const std::vector<std::string> &strings, uint32_t key) {
	std::vector<uint8_t> plain;
	for (const uint32_t w : words)
		for (int i = 0; i < 4; ++i) plain.push_back(uint8_t(w >> (8 * i)));
	const size_t string_offset = 20 + plain.size();
	for (const std::string &text : strings) {
		plain.insert(plain.end(), text.begin(), text.end());
		plain.push_back(0);
	}
	cbin::apply_cipher(plain.data(), plain.size(), key);
	const uint32_t header[5] = {0x4E494243u, uint32_t(string_offset), uint32_t(20 + plain.size() - string_offset),
	                            uint32_t(strings.size()), key};
	std::vector<uint8_t> out;
	for (const uint32_t w : header)
		for (int i = 0; i < 4; ++i) out.push_back(uint8_t(w >> (8 * i)));
	out.insert(out.end(), plain.begin(), plain.end());
	return out;
}

// Every word the game's binary reader takes, decoded and encoded back byte for byte [orig:
// ConfigFile_ParseBinary @ 0x75e8a0]: a null label name (@ 0x75e9c4), a null entry name (@ 0x75ea7e), entries
// of no value and of three (@ 0x75eb21), a null string (@ 0x75eb4e), flags 0 and 3 (@ 0x75eb40), a signaling
// NaN's bits, a terminator that names an entry and takes a value after its label's (@ 0x75ea75,
// @ 0x75eb0b..0x75eb82), a label counted 0xFFFFFFFF that reads no block (@ 0x75ea73).
int test_every_word_round_trips() {
	const std::vector<std::string> strings = {"env", "rate", "text", "font", "tail"};
	const uint32_t key = 0x0BADF00Du;
	const std::vector<uint8_t> bytes = laid_out(
			{3,                                           // labels
			 1, 3, 0, 0xFFFFFFFFu, 3, 1,                  // (env, 3), (null, -1), (text, 1)
			 2, 1, 0, 1, 4, 0, 5, 1,                      // env: rate 1, null 1, font 0, its terminator (tail, 1)
			 4, 3, 0, 0,                                  // text: font 3, its terminator (0, 0)
			 0x7FA00000u, 2, 5, 1, 7, 1,                  // env's values: an SNaN, the null entry's, tail's
			 0, 4, 9, 0, 1, 3},                           // text's: a null string, flags 0, flags 3
			strings, key);
	BinaryConfig read;
	std::string error;
	TEST_EXPECT(cbin::decode_binary_config(bytes.data(), bytes.size(), read, error));
	TEST_EXPECT(read.labels.size() == 3 && read.labels[1].name == 0 && read.labels[1].count_minus_one &&
	            read.labels[1].entries.empty());
	const BinaryConfig::Label &env = read.labels[0];
	TEST_EXPECT(env.entries.size() == 3 && env.entries[1].name == 0 && env.entries[2].values.empty() &&
	            env.entries[0].values.size() == 1 && env.entries[0].values[0].raw == 0x7FA00000u);
	TEST_EXPECT(env.terminator.name == 5 && env.terminator.values.size() == 1 && env.terminator.values[0].raw == 7);
	const BinaryConfig::Label &text = read.labels[2];
	TEST_EXPECT(text.entries.size() == 1 && text.entries[0].values.size() == 3 && text.entries[0].values[0].raw == 0 &&
	            text.entries[0].values[0].flags == 4 && text.entries[0].values[1].flags == 0 &&
	            text.entries[0].values[2].flags == 3 && text.terminator.name == 0 && text.terminator.values.empty());
	TEST_EXPECT(encoded(read) == bytes);
	// A file that does not frame still fails: a count past what it holds, a value no entry reads.
	std::vector<uint8_t> cut = bytes;
	cut.resize(cut.size() - 1);
	TEST_EXPECT(!cbin::decode_binary_config(cut.data(), cut.size(), read, error));
	const std::vector<uint8_t> extra = laid_out({1, 1, 1, 2, 1, 0, 0, 5, 1, 6, 1}, strings, key);
	TEST_EXPECT(!cbin::decode_binary_config(extra.data(), extra.size(), read, error));
	// The text form carries none of these.
	std::string as_text, why;
	TEST_EXPECT(cbin::decode_binary_config(bytes.data(), bytes.size(), read, error) &&
	            !cbin::binary_config_text(read, as_text, why));
	std::printf("every word: %zu bytes decode and encode back the same\n", bytes.size());
	return 0;
}

uint32_t real_bits(float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof bits);
	return bits;
}

// The sections the accessors walk [orig: Effect_GetParamValue_0 @ 0x75fa00; Effect_GetParamValue @ 0x75f580]:
// env's walk goes past an entry of no value (its value pointer set, @ 0x75eb1e), takes its named terminator
// as an entry and goes on into text's block to text's null-named entry; a label counted 0xFFFFFFFF starts at
// the next block; a float read as a float through the x87 quiets a signaling NaN (fld @ 0x75fb7f, fstp
// @ 0x75fb83; fld @ 0x75f693, fstp @ 0x75f697); flags 8 read as a number is 0 (@ 0x75fb39..0x75fb3d) and as
// text none (D-CBIN-5); flags 5 is a string (@ 0x75eb42).
BinaryConfig walked() {
	BinaryConfig config;
	config.strings = {"env", "rate", "skip", "text", "line", "tail", "Credits"};
	config.xor_key = 0x13572468u;
	const auto value = [](uint32_t raw, uint32_t flags) { return BinaryConfig::Value{raw, flags}; };
	BinaryConfig::Label env{1, {{3, {}}, {2, {value(0x7FA00000u, BinaryConfig::kFloat)}}}};
	env.terminator = {6, {value(42, BinaryConfig::kInteger)}};
	BinaryConfig::Label open{3, {}};
	open.count_minus_one = true;
	BinaryConfig::Label text{4, {{5, {value(7, BinaryConfig::kString), value(9, 8), value(7, 4 | 1)}},
	                             {0, {value(0, BinaryConfig::kString)}},
	                             {5, {value(0, BinaryConfig::kString)}}}};
	config.labels = {env, open, text};
	return config;
}

int test_sections_walk() {
	const std::vector<uint8_t> bytes = encoded(walked());
	BinaryConfig read;
	std::string error;
	TEST_EXPECT(!bytes.empty() && cbin::decode_binary_config(bytes.data(), bytes.size(), read, error) &&
	            encoded(read) == bytes);
	std::vector<configfile::ConfigSection> sections;
	TEST_EXPECT(cbin::binary_config_sections(read, sections) && sections.size() == 3);
	if (sections.size() != 3) return 1;
	// env: skip, rate, its terminator tail, then text's line, to text's null name.
	TEST_EXPECT(configfile::config_entry_count(sections[0]) == 4 &&
	            configfile::config_entry_key(sections[0], configfile::config_entry(sections[0], 0)) == "skip" &&
	            configfile::config_entry_key(sections[0], configfile::config_entry(sections[0], 2)) == "tail" &&
	            configfile::config_entry_key(sections[0], configfile::config_entry(sections[0], 3)) == "line");
	TEST_EXPECT(configfile::config_section_label(sections[1]) == "skip" &&
	            configfile::config_entry_count(sections[1]) == 1 &&
	            configfile::config_entry_key(sections[1], configfile::config_entry(sections[1], 0)) == "line");
	TEST_EXPECT(configfile::config_entry_count(sections[2]) == 1 &&
	            configfile::config_entry(sections[2], 0).values.size() == 3);
	configfile::ConfigSection *env = configfile::find_config_section(sections, "ENV");
	float real = 0.0f;
	int32_t integer = 0;
	std::string word;
	TEST_EXPECT(env != nullptr && configfile::read_config_value(*env, "rate", 1, nullptr, &real, nullptr) &&
	            real_bits(real) == 0x7FE00000u);
	TEST_EXPECT(configfile::read_current_config_value(*env, "rate", 1, nullptr, &real, nullptr) &&
	            real_bits(real) == 0x7FE00000u);
	TEST_EXPECT(configfile::read_config_value(*env, "tail", 1, nullptr, nullptr, &integer) && integer == 42);
	TEST_EXPECT(configfile::read_config_value(*env, "line", 1, &word, nullptr, nullptr) && word == "Credits");
	configfile::ConfigSection *text = configfile::find_config_section(sections, "TEXT");
	TEST_EXPECT(text != nullptr && configfile::read_config_value(*text, "line", 2, &word, &real, &integer) &&
	            integer == 0 && real == 0.0f && word.empty());
	TEST_EXPECT(configfile::read_current_config_value(*text, "line", 3, &word, nullptr, &integer) &&
	            word == "Credits" && integer == 0);
	TEST_EXPECT(!configfile::read_config_value(*text, "missing", 1, &word, nullptr, nullptr));

	TEST_EXPECT(!configfile::config_faulted(sections));

	// What the game faults on faults at the read that reaches it, never at the load. A null label: a lookup
	// found before it reads on; one whose scan reaches it faults [orig: ConfigFile_FindLabelLinear @ 0x75ee50,
	// the stricmp call @ 0x75ee84].
	BinaryConfig null_label = walked();
	null_label.labels[1].name = 0;
	TEST_EXPECT(cbin::binary_config_sections(null_label, sections) && sections.size() == 3 && sections[1].null_label);
	TEST_EXPECT(configfile::find_config_section(sections, "env") != nullptr && !configfile::config_faulted(sections));
	TEST_EXPECT(configfile::find_config_section(sections, "text") == nullptr && configfile::config_faulted(sections));
	// A null word (a string of index 0, flags 8 of a word 0) or a string past the table: a numeric read is 0 and
	// touches nothing [orig: Effect_GetParamValue_0 @ 0x75fb39..0x75fb3d]; a text read faults [orig:
	// String_CopyN @ 0x75eca4].
	BinaryConfig null_text = walked();
	null_text.labels[2].entries[0].values[0].raw = 0;
	null_text.labels[2].entries[0].values[1].raw = 0;
	null_text.labels[2].entries[0].values[2].raw = 99;
	TEST_EXPECT(cbin::binary_config_sections(null_text, sections));
	text = configfile::find_config_section(sections, "text");
	for (int index = 1; index <= 3; ++index) {
		integer = 5;
		TEST_EXPECT(text != nullptr && configfile::read_config_value(*text, "line", index, nullptr, nullptr, &integer) &&
		            integer == 0 && !configfile::config_faulted(sections));
		configfile::find_config_section(sections, "text");
	}
	TEST_EXPECT(!configfile::read_config_value(*text, "line", 3, &word, nullptr, nullptr) &&
	            configfile::config_faulted(sections));
	TEST_EXPECT(cbin::binary_config_sections(null_text, sections));
	text = configfile::find_config_section(sections, "text");
	TEST_EXPECT(configfile::read_config_value(*text, "line", 1, nullptr, nullptr, &integer) &&
	            !configfile::read_current_config_value(*text, "line", 1, &word, nullptr, nullptr) &&
	            configfile::config_faulted(sections));
	// A name past the table: a walk that stops before it reads on; one that reaches it faults.
	BinaryConfig stray_name = walked();
	stray_name.labels[0].entries[1].name = 99;
	TEST_EXPECT(cbin::binary_config_sections(stray_name, sections) && configfile::config_entry(sections[0], 1).key_faults);
	configfile::ConfigSection *env_stray = configfile::find_config_section(sections, "env");
	TEST_EXPECT(env_stray != nullptr && !configfile::read_config_value(*env_stray, "skip", 1, nullptr, nullptr, &integer) &&
	            !configfile::config_faulted(sections));
	configfile::find_config_section(sections, "env");
	TEST_EXPECT(!configfile::read_config_value(*env_stray, "tail", 1, nullptr, nullptr, &integer) &&
	            configfile::config_faulted(sections));
	// A walk past the last block, into the allocation's slack the parse never wrote, ends there; a label
	// counted 0xFFFFFFFF last starts there, with no entry.
	BinaryConfig past = walked();
	past.labels[2].entries.resize(1);
	past.labels[2].terminator = {6, {}};
	past.labels.push_back(past.labels[1]);
	TEST_EXPECT(cbin::binary_config_sections(past, sections) && sections.size() == 4 &&
	            configfile::config_entry_count(sections[0]) == 5 && configfile::config_entry_count(sections[3]) == 0);
	env = configfile::find_config_section(sections, "env");
	TEST_EXPECT(env != nullptr && !configfile::read_config_value(*env, "missing", 1, nullptr, nullptr, &integer) &&
	            !configfile::config_faulted(sections));
	// A label past the table is a stray pointer the parse lowercases through: no load.
	BinaryConfig stray_label = walked();
	stray_label.labels[2].name = 99;
	TEST_EXPECT(!cbin::binary_config_sections(stray_label, sections) && sections.empty());
	std::printf("sections: the walk cut at a null name, past an empty entry, through a named terminator; "
	            "faults at the read\n");
	return 0;
}

// The sections share the reader's one element block, each a run of it, so storage stays linear however far
// the walks run [orig: ConfigFile_ParseBinary @ 0x75e8a0 -- one "config elements" allocation @ 0x75ea2f,
// each label's pointer into it @ 0x75ea6f]: 10000 labels, each of one named entry of no value and a named
// terminator but the last, whose walks each run on to the file's end, keep 20000 entries in all, and the
// first section's walk reads the last entry.
int test_sections_storage_is_linear() {
	constexpr uint32_t kLabels = 10000;
	BinaryConfig config;
	config.strings = {"label", "a", "t", "z"};
	config.xor_key = 0x600DF00Du;
	config.labels.resize(kLabels);
	for (uint32_t i = 0; i < kLabels; ++i) {
		BinaryConfig::Label &label = config.labels[i];
		label.name = 1;
		label.entries.push_back(BinaryConfig::Entry{2, {}});
		label.terminator = BinaryConfig::Entry{3, {}};
	}
	config.labels.back().entries[0] = BinaryConfig::Entry{4, {BinaryConfig::Value{7, BinaryConfig::kInteger}}};
	config.labels.back().terminator = BinaryConfig::Entry{};
	const std::vector<uint8_t> bytes = encoded(config);
	BinaryConfig read;
	std::string error;
	TEST_EXPECT(!bytes.empty() && cbin::decode_binary_config(bytes.data(), bytes.size(), read, error));
	std::vector<configfile::ConfigSection> sections;
	TEST_EXPECT(cbin::binary_config_sections(read, sections) && sections.size() == kLabels);
	if (sections.size() != kLabels) return 1;
	const auto *shared = sections[0].block.get();
	bool one_block = shared != nullptr && shared->entries.size() == 2 * kLabels;
	for (const configfile::ConfigSection &section : sections) one_block = one_block && section.block.get() == shared;
	TEST_EXPECT(one_block);
	TEST_EXPECT(configfile::config_entry_count(sections[0]) == 2 * kLabels - 1 &&
	            configfile::config_entry_count(sections[kLabels - 1]) == 1);
	int32_t integer = 0;
	TEST_EXPECT(configfile::read_config_value(sections[0], "z", 1, nullptr, nullptr, &integer) && integer == 7 &&
	            !configfile::config_faulted(sections));
	std::printf("storage: %u labels walking to the end share one block of %zu entries\n", kLabels,
	            shared != nullptr ? shared->entries.size() : size_t(0));
	return 0;
}

// The sections hold their names and string values as indices into the block's one string table, as the reader
// holds pointers into its table [orig: ConfigFile_ParseBinary @ 0x75e8a0 -- the label @ 0x75e9d2, the name
// @ 0x75ea8e, the string @ 0x75eb5a]: one 64 KiB string that 2000 labels, 2000 entries and 2000 string values
// name stays one string, so storage stays linear in the file's bytes, and every read still sees it.
int test_sections_strings_are_shared() {
	constexpr uint32_t kNames = 2000;
	const std::string big(65536, 'q');
	BinaryConfig config;
	config.strings = {big, "z"};
	config.xor_key = 0x5EED5EEDu;
	config.labels.resize(kNames);
	for (uint32_t i = 0; i < kNames; ++i) {
		BinaryConfig::Label &label = config.labels[i];
		label.name = 1;
		label.entries.push_back(BinaryConfig::Entry{1, {BinaryConfig::Value{1, BinaryConfig::kString}}});
	}
	config.labels.back().entries.push_back(BinaryConfig::Entry{2, {BinaryConfig::Value{1, BinaryConfig::kString}}});
	const std::vector<uint8_t> bytes = encoded(config);
	BinaryConfig read;
	std::string error;
	TEST_EXPECT(!bytes.empty() && cbin::decode_binary_config(bytes.data(), bytes.size(), read, error));
	std::vector<configfile::ConfigSection> sections;
	TEST_EXPECT(cbin::binary_config_sections(read, sections) && sections.size() == kNames);
	if (sections.size() != kNames) return 1;
	const configfile::ConfigBlock *shared = sections[0].block.get();
	TEST_EXPECT(shared != nullptr && shared->strings.size() == 2);
	if (shared == nullptr) return 1;
	// The bytes every string the sections hold takes: the table once, nothing per name or value.
	size_t held = 0;
	for (const std::string &text : shared->strings) held += text.size();
	for (const configfile::ConfigEntry &entry : shared->entries) {
		held += entry.key.size();
		for (const configfile::ConfigValue &value : entry.values) held += value.text.size();
	}
	for (const configfile::ConfigSection &section : sections) held += section.label.size();
	TEST_EXPECT(held <= bytes.size());
	TEST_EXPECT(configfile::config_section_label(sections[0]) == big &&
	            configfile::config_entry_key(sections[5], configfile::config_entry(sections[5], 0)) == big);
	std::string word;
	configfile::ConfigSection *last = &sections[kNames - 1];
	TEST_EXPECT(configfile::read_config_value(*last, "z", 1, &word, nullptr, nullptr) && word == big);
	std::printf("strings: %u labels, names and values of one %zu-byte string hold %zu bytes of text\n", kNames,
	            big.size(), held);
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_round_trip();
	failures += test_no_text_form();
	failures += test_readable();
	failures += test_every_word_round_trips();
	failures += test_sections_walk();
	failures += test_sections_storage_is_linear();
	failures += test_sections_strings_are_shared();
	if (failures == 0) std::printf("cbin_binary_config_text: all passed\n");
	return failures == 0 ? 0 : 1;
}
