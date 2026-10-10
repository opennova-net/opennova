// A marquee's credits over both of the ConfigFile's forms (runtime/menu/menu_credits.h): a CBIN file reads
// through the binary reader's sections (formats/cbin/binary_config.h binary_config_sections) as its text form
// reads through the text reader, value for value and node for node; a label's name lowercased in the string
// table itself, so a string value of that very string reads lowercased (one of the same text elsewhere in
// the table does not); a value of flags none of an integer, a float or a string, and a file laid out
// otherwise, not loaded and the credits left as they were. The minted synth_nlist trio loads with its
// [ENV] values and its lines and fonts as the credits view reads them. The retail leg (OPENNOVA_JO_ASSETS):
// the shipped JO, JOX01 and BHD lists each load as their text form does, their counts pinned.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <formats/cbin/binary_config.h>
#include <formats/cbin/binary_config_text.h>
#include <formats/cbin/cbin.h>
#include <runtime/menu/menu_credits.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;
using cbin::BinaryConfig;
using menu::MarqueeCreditNode;
using menu::MarqueeCredits;

namespace {

BinaryConfig::Value value(uint32_t raw, uint32_t flags) {
	return BinaryConfig::Value{raw, flags};
}

uint32_t bits_of(float f) {
	uint32_t bits = 0;
	std::memcpy(&bits, &f, sizeof bits);
	return bits;
}

std::vector<uint8_t> encoded(const BinaryConfig &config) {
	std::vector<uint8_t> out;
	std::string error;
	if (!cbin::encode_binary_config(config, out, error)) out.clear();
	return out;
}

bool load(const std::vector<uint8_t> &bytes, MarqueeCredits &credits) {
	return menu::marquee_load_credits(bytes.data(), bytes.size(), credits, [](const std::string &) { return true; });
}

bool load_text(const std::string &text, MarqueeCredits &credits) {
	return menu::marquee_load_credits(reinterpret_cast<const uint8_t *>(text.data()), text.size(), credits,
			[](const std::string &) { return true; });
}

bool same_node(const MarqueeCreditNode &a, const MarqueeCreditNode &b) {
	return a.text == b.text && a.line == b.line && a.font == b.font && a.color == b.color && a.justify == b.justify &&
			a.image == b.image && a.fades == b.fades && a.fixed_x == b.fixed_x && a.fixed_y == b.fixed_y &&
			a.offset == b.offset;
}

bool same_credits(const MarqueeCredits &a, const MarqueeCredits &b) {
	if (a.scroll_rate != b.scroll_rate || a.center_x != b.center_x || a.vertical_space != b.vertical_space ||
			a.space_mark != b.space_mark || a.comma_mark != b.comma_mark || a.nodes.size() != b.nodes.size())
		return false;
	for (size_t i = 0; i < a.nodes.size(); ++i)
		if (!same_node(a.nodes[i], b.nodes[i])) return false;
	return true;
}

// A CBIN file's credits against its text form's: both readers build the same sections.
int check_forms_agree(const char *label, const std::vector<uint8_t> &bytes, MarqueeCredits &out) {
	BinaryConfig config;
	std::string error, text, why;
	TEST_EXPECT(cbin::decode_binary_config(bytes.data(), bytes.size(), config, error));
	TEST_EXPECT(cbin::binary_config_text(config, text, why));
	MarqueeCredits binary, read;
	TEST_EXPECT(load(bytes, binary));
	TEST_EXPECT(load_text(text, read));
	if (!same_credits(binary, read)) {
		std::fprintf(stderr, "%s: the CBIN form's credits differ from its text form's\n", label);
		return 1;
	}
	out = binary;
	return 0;
}

// A list laid out as the shipped ones are (lowercase labels and keys, a space written '_'): a justify, a
// colour, a line with its font, a line end, a spacing image, a fading image, a line keeping the last font.
BinaryConfig laid_out_list() {
	BinaryConfig config;
	config.strings = {"env", "text", "scroll_rate", "vertical_space", "center_x", "~JL", "~CFF0000",
			"Open_Nova", "Serpen24", "<CR>", "~Icr_logo.png", "~F10|20|cr1.png", "Credits"};
	config.xor_key = 0x5EEDF00Du;
	BinaryConfig::Label env{1, {{3, {value(bits_of(0.5f), BinaryConfig::kFloat)}},
			{4, {value(14, BinaryConfig::kInteger)}}, {5, {value(300, BinaryConfig::kInteger)}}}};
	BinaryConfig::Label text{2, {{2, {value(6, BinaryConfig::kString)}}, {2, {value(7, BinaryConfig::kString)}},
			{2, {value(8, BinaryConfig::kString), value(9, BinaryConfig::kString)}},
			{2, {value(10, BinaryConfig::kString)}}, {2, {value(11, BinaryConfig::kString)}},
			{2, {value(12, BinaryConfig::kString)}}, {2, {value(13, BinaryConfig::kString)}}}};
	config.labels = {env, text};
	return config;
}

int test_forms_agree() {
	const std::vector<uint8_t> bytes = encoded(laid_out_list());
	TEST_EXPECT(!bytes.empty());
	MarqueeCredits credits;
	if (check_forms_agree("laid-out list", bytes, credits) != 0) return 1;
	TEST_EXPECT(credits.scroll_rate == 0.5f && credits.vertical_space == 14 && credits.center_x == 300);
	TEST_EXPECT(credits.space_mark == '_' && credits.comma_mark == '@');
	TEST_EXPECT(credits.nodes.size() == 4);
	const MarqueeCreditNode &line = credits.nodes[0];
	TEST_EXPECT(line.text && line.line == "Open_Nova" && line.font == "Serpen24" && line.color == 0xFF0000u &&
			line.justify == 0 && line.offset == 0);
	TEST_EXPECT(menu::marquee_node_text(credits, line) == "Open Nova");
	const MarqueeCreditNode &spacing = credits.nodes[1];
	TEST_EXPECT(!spacing.text && spacing.image == "cr_logo.png" && !spacing.fades && spacing.offset == 28);
	const MarqueeCreditNode &fading = credits.nodes[2];
	TEST_EXPECT(!fading.text && fading.image == "cr1.png" && fading.fades && fading.fixed_x == 10 &&
			fading.fixed_y == 20 && fading.offset == 42);
	const MarqueeCreditNode &kept = credits.nodes[3];
	TEST_EXPECT(kept.text && kept.line == "Credits" && kept.font == "Serpen24" && kept.offset == 42);
	return 0;
}

// A label's name lowercased in the string table: the [TEXT] section found, a line of that very string read
// lowercased, a line of the same text at another place in the table read as written.
int test_label_lowercased_in_place() {
	BinaryConfig config;
	config.strings = {"TEXT", "TEXT", "Serpen24"};
	config.xor_key = 0x1234u;
	config.labels = {{1, {{1, {value(1, BinaryConfig::kString), value(3, BinaryConfig::kString)}},
			{1, {value(2, BinaryConfig::kString)}}}}};
	MarqueeCredits credits;
	TEST_EXPECT(load(encoded(config), credits));
	TEST_EXPECT(credits.nodes.size() == 2 && credits.nodes[0].line == "text" && credits.nodes[1].line == "TEXT");
	return 0;
}

// What the binary reader does not build: a value of flags none of an integer, a float or a string (the
// accessor would read its word as a string's address), and a file laid out otherwise. Neither loads, the
// credits left as they were.
int test_not_loaded() {
	MarqueeCredits credits;
	TEST_EXPECT(load(encoded(laid_out_list()), credits) && credits.nodes.size() == 4);
	BinaryConfig flagged = laid_out_list();
	flagged.labels[1].entries[2].values[1].flags = 8;
	TEST_EXPECT(!load(encoded(flagged), credits));
	std::vector<uint8_t> cut = encoded(laid_out_list());
	cut.resize(cut.size() - 3);
	TEST_EXPECT(!load(cut, credits));
	TEST_EXPECT(credits.nodes.size() == 4 && credits.center_x == 300);
	// A value of flags with 4 set among others is a string, as the reader takes it.
	BinaryConfig marked = laid_out_list();
	marked.labels[1].entries[6].values[0].flags = 4 | 1;
	MarqueeCredits read;
	TEST_EXPECT(load(encoded(marked), read) && read.nodes.size() == 4 && read.nodes[3].line == "Credits");
	return 0;
}

// The minted lists (fixtures/cbin, tests/fixtures/minimal_cbin_gen.cpp): their [ENV] values, and each text
// line with its font as the credits view reads the same bytes.
int check_minted(const std::string &path, int center_x) {
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	TEST_EXPECT(!bytes.empty());
	MarqueeCredits credits;
	TEST_EXPECT(load(bytes, credits));
	TEST_EXPECT(credits.scroll_rate == 0.5f && credits.vertical_space == 14 && credits.center_x == center_x);
	cbin::Credits view;
	std::string error;
	TEST_EXPECT(cbin::decode_credits(bytes.data(), bytes.size(), view, error));
	std::vector<const MarqueeCreditNode *> lines;
	for (const MarqueeCreditNode &node : credits.nodes)
		if (node.text) lines.push_back(&node);
	std::vector<const cbin::Entry *> texts;
	for (const cbin::Entry &entry : view.entries)
		if (entry.type == cbin::EntryType::Text) texts.push_back(&entry);
	TEST_EXPECT(!lines.empty() && lines.size() == texts.size());
	std::string font;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (!texts[i]->font.empty()) font = texts[i]->font;
		TEST_EXPECT(lines[i]->line == texts[i]->text && lines[i]->font == font);
	}
	return 0;
}

int test_minted() {
	const std::string root = test_paths_repo_root(__FILE__);
	int failures = 0;
	failures += check_minted(root + "/fixtures/cbin/synth_nlist.kda", 400);
	failures += check_minted(root + "/fixtures/cbin/synth_nlist_jox01.kda", 400);
	failures += check_minted(root + "/fixtures/cbin/synth_nlist_bhd.kda", 400);
	return failures;
}

struct Counts {
	size_t nodes = 0, lines = 0, spacing = 0, fading = 0;
};

Counts counts_of(const MarqueeCredits &credits) {
	Counts c;
	c.nodes = credits.nodes.size();
	for (const MarqueeCreditNode &node : credits.nodes) {
		if (node.text) ++c.lines;
		else if (node.fades) ++c.fading;
		else ++c.spacing;
	}
	return c;
}

// The shipped lists, each as its text form reads, their [ENV] values and node counts pinned (every texture
// taken as loading).
struct ShippedList {
	const char *path;
	int vertical_space;
	Counts counts;
};

int test_retail() {
	const ShippedList lists[] = {
			{"cbin/nlist.reference.kda", 14, {250, 231, 2, 17}},
			{"cbin/nlist.jox01.reference.kda", 14, {250, 231, 2, 17}},
			{"cbin/nlist.bhd.reference.kda", 20, {259, 259, 0, 0}},
	};
	for (const ShippedList &list : lists)
		if (retail::reference_fixture(list.path).empty())
			return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/cbin/nlist*.reference.kda (the shipped credits lists)");
	int failures = 0;
	for (const ShippedList &list : lists) {
		MarqueeCredits credits;
		if (check_forms_agree(list.path, test_io::read_file(retail::reference_fixture(list.path)), credits) != 0) {
			++failures;
			continue;
		}
		const Counts c = counts_of(credits);
		std::printf("%s: scroll %g, space %d, centre %d; %zu nodes (%zu lines, %zu spacing images, %zu fading)\n",
				list.path, double(credits.scroll_rate), credits.vertical_space, credits.center_x, c.nodes, c.lines,
				c.spacing, c.fading);
		if (credits.scroll_rate != 0.5f || credits.vertical_space != list.vertical_space || credits.center_x != 400 ||
				c.nodes != list.counts.nodes || c.lines != list.counts.lines || c.spacing != list.counts.spacing ||
				c.fading != list.counts.fading) {
			std::fprintf(stderr, "%s: its values or counts differ from the pins\n", list.path);
			++failures;
		}
	}
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_forms_agree();
	failures += test_label_lowercased_in_place();
	failures += test_not_loaded();
	failures += test_minted();
	failures += test_retail();
	if (failures == 0) std::printf("menu_credits: all passed\n");
	return failures == 0 ? 0 : 1;
}
