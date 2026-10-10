// The modeled layout of a line-oriented text file (formats/textlayout; ADR 0003 holding, the maintainer's ruling
// of 2026-10-04, "model it, generate it"): a line cut into its parts and made again of them; a file's records
// generated over its notes, every byte from the writer's lines and the layout data: an entry's words in the
// file's spelling while they are the writer's as read, a line the writer puts down none of kept as its tokens,
// a writer's line the file left out left out while it is as read, a set's words wherever they stand, the
// writer's own separators never over a file, a nested record's place of its kind in the writer's order with the
// lines before it, a line or a record put down anew after the one before it in the writer's order, a record gone
// with its lines and the lines before it kept. With no notes, the writer's form.
#include <formats/textlayout/text_layout.h>

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::textlayout;

namespace {

// A tiny reader for the test's own format: `key value...` lines, `[name]` opening a block of the root that
// runs to the next one, every other line read for nothing; the entries of a block are its keys.
struct Block {
	std::string name;
	std::vector<std::pair<std::string, std::string>> keys;
	uint64_t note = 0;
};
struct File {
	std::vector<std::pair<std::string, std::string>> keys; // the root's
	std::vector<Block> blocks;
	uint64_t note = 0;
};

OutRecord records_of(const File &file) {
	OutRecord root;
	root.note = file.note;
	root.lines.push_back({"", "// the writer's own header"});
	for (const auto &[key, value] : file.keys) root.lines.push_back({key, key + " " + value});
	for (const Block &block : file.blocks) {
		OutRecord record;
		record.note = block.note;
		record.kind = "block";
		record.lines.push_back({"[", "[" + block.name + "]"});
		for (const auto &[key, value] : block.keys) {
			OutLine line{key, key + " " + value};
			if (key == "flags") line.set_from = 1;
			record.lines.push_back(line);
		}
		record.lines.push_back({"", ""});
		root.lines.push_back({"", "", int(root.children.size())});
		root.children.push_back(record);
	}
	return root;
}

File read(const std::string &text, Notes &notes) {
	File file;
	Noter noter(text.data(), text.size(), &notes, cut_ascii_walk);
	file.note = noter.root();
	uint64_t open = 0;
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find("\r\n", at);
		const size_t next = end == std::string::npos ? text.size() : end + 2;
		if (end == std::string::npos) end = text.size();
		noter.line(at, next);
		const Line line = cut_ascii_walk(text.data() + at, next - at);
		if (!line.words.empty() && line.words[0].front() == '[') {
			if (open) noter.end_before(open);
			open = noter.open(noter.root());
			noter.entry(open, "[");
			Block block;
			block.name = line.words[0].substr(1, line.words[0].size() - 2);
			block.note = open;
			file.blocks.push_back(block);
		} else if (line.words.size() >= 2) {
			std::string value = line.words[1];
			for (size_t i = 2; i < line.words.size(); ++i) value += " " + line.words[i];
			if (open) {
				file.blocks.back().keys.emplace_back(line.words[0], value);
				noter.entry(open, line.words[0]);
			} else {
				file.keys.emplace_back(line.words[0], value);
				noter.entry(noter.root(), line.words[0]);
			}
		}
		at = next;
	}
	noter.finish();
	model(notes, records_of(file), cut_ascii_walk);
	return file;
}

std::string write(const File &file, const Notes *notes) { return compose(notes, records_of(file), cut_ascii_walk, "\r\n"); }

int cut() {
	for (const char *text : {"  key\t\"a b\", c ; comment\r\n", "\t// only a comment", "", "word\n\r\n", ",lead  \r\n"}) {
		const Line line = cut_ascii_walk(text, std::string(text).size());
		TEST_EXPECT(line.text() == text);
	}
	const Line line = cut_ascii_walk("  key\t\"a b\", c ; x\r\n", 20);
	TEST_EXPECT(line.indent == "  " && line.words.size() == 3 && line.words[1] == "\"a b\"" && line.gaps[1] == ", ");
	TEST_EXPECT(line.tail == " ; x" && line.eol == "\r\n");
	std::printf("cut: a line's parts make it again; a quoted run one word, a comma a separator, `;` the comment\n");
	return 0;
}

int composed() {
	const std::string text =
			"// a hand-written file\r\n"
			"version\t\t4.50   // spelled\r\n"
			"\r\n"
			"// block a\r\n"
			"[a]\r\n"
			"x   1\r\n"
			"flags  C A\r\n"
			"\r\n"
			"// block b\r\n"
			"[b]\r\n"
			"y 2\r\n";
	Notes notes;
	File file = read(text, notes);
	TEST_EXPECT(write(file, &notes) == text);
	// With no notes: the writer's form.
	TEST_EXPECT(write(file, nullptr) == "// the writer's own header\r\nversion 4.50\r\n[a]\r\nx 1\r\nflags C A\r\n\r\n[b]\r\ny 2\r\n\r\n");
	// A value changed: its one line, its spacing kept; the spelling of an unchanged number stays.
	File changed = file;
	changed.blocks[0].keys[0].second = "7";
	TEST_EXPECT(write(changed, &notes) == std::string(text).replace(text.find("x   1"), 5, "x   7"));
	// A set's member removed and one added: the file's where they stood, the new after them.
	changed = file;
	changed.blocks[0].keys[1].second = "A B";
	TEST_EXPECT(write(changed, &notes).find("flags  A B\r\n") != std::string::npos);
	// A key put down anew: after the key before it in the writer's order, in its form.
	changed = file;
	changed.blocks[0].keys.insert(changed.blocks[0].keys.begin() + 1, {"z", "9"});
	TEST_EXPECT(write(changed, &notes).find("x   1\r\nz 9\r\nflags  C A\r\n") != std::string::npos);
	// Blocks swapped: each with the lines before it (a block ending at the next one's line leaves its blank and the
	// next one's banner to the file, before the next).
	changed = file;
	std::swap(changed.blocks[0], changed.blocks[1]);
	TEST_EXPECT(write(changed, &notes) ==
	            "// a hand-written file\r\nversion\t\t4.50   // spelled\r\n\r\n// block b\r\n[b]\r\ny 2\r\n\r\n// block a\r\n"
	            "[a]\r\nx   1\r\nflags  C A\r\n");
	// A block gone: its lines with it, the lines before it kept.
	changed = file;
	changed.blocks.erase(changed.blocks.begin());
	TEST_EXPECT(write(changed, &notes) ==
	            "// a hand-written file\r\nversion\t\t4.50   // spelled\r\n\r\n// block a\r\n\r\n// block b\r\n[b]\r\ny 2\r\n");
	// A block added: after the last of its kind, in the writer's form with its separator.
	changed = file;
	changed.blocks.push_back({"c", {{"w", "1"}}, 0});
	TEST_EXPECT(write(changed, &notes) == text + "[c]\r\nw 1\r\n\r\n");
	// A key the writer leaves out (version gone) goes; a key the file left out stays out while it is as read.
	changed = file;
	changed.keys.clear();
	TEST_EXPECT(write(changed, &notes).find("version") == std::string::npos);
	std::printf("compose: the file again as it was; a value, a set, a key anew, a swap, a block gone and one added\n");
	return 0;
}

} // namespace

int main() {
	if (cut() || composed()) return 1;
	return 0;
}
