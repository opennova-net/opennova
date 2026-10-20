// The stylesheet writer (formats/mns, mns::write): a stylesheet's model is the reader's list (what the game
// reads of the file, its directives decided, its comments, blanks and switched-off lines read as nothing), and
// the writer puts the list down alone, `name<TAB>value` with CR LF a line in the list's order (no original
// writer of a stylesheet is witnessed: the form is OpenNova's). A minted list is the writer's form byte for byte
// and reads back as itself; an authored sheet (comments, directives, a continuation, a repeat, odd spacing)
// read, written and read again is the same list; a name or value the reader would read otherwise is refused.
// The tree's sheets (fixtures/mns, shipped_form.mns in the shipped sheets' form, and the base game's in
// assets/) are each read to its end, written, and read back as the same list; the writer's bytes for each
// authored one are a pinned FNV-1a64 vector.
#include <formats/mns/mns.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "mnu/repo_menus.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

mns::KeyValueList read(const std::string &text, mns::ReadStatus *status = nullptr) {
	mns::KeyValueList list;
	const mns::ReadResult result = mns::parse_key_value_buffer(text.data(), text.size(), list);
	if (status) *status = result.status;
	return list;
}

bool same(const mns::KeyValueList &a, const mns::KeyValueList &b) {
	if (a.nodes.size() != b.nodes.size()) return false;
	for (size_t i = 0; i < a.nodes.size(); ++i)
		if (a.nodes[i].name != b.nodes[i].name || a.nodes[i].value != b.nodes[i].value) return false;
	return true;
}

// Written and read again: the list it was.
bool reads_back(const mns::KeyValueList &list, std::string *written = nullptr) {
	std::string text, error;
	if (!mns::write(list, text, error)) {
		std::printf("  not written: %s\n", error.c_str());
		return false;
	}
	if (written) *written = text;
	mns::ReadStatus status = mns::ReadStatus::Read;
	return same(read(text, &status), list) && status == mns::ReadStatus::Read;
}

int minted() {
	mns::KeyValueList list;
	list.nodes = {{"DEF_FONTNAME", "Gunpl22b.fnt"}, {"Trim_Color", "FF808080"}, {"PATH", "menu\\\\art"},
	              {"TITLE", "Joint Ops: a/b"}};
	std::string text, error;
	TEST_EXPECT(mns::write(list, text, error));
	TEST_EXPECT(text == "DEF_FONTNAME\tGunpl22b.fnt\r\nTrim_Color\tFF808080\r\nPATH\tmenu\\\\art\r\nTITLE\tJoint Ops: a/b\r\n");
	TEST_EXPECT(reads_back(list));
	// An empty list is no text.
	TEST_EXPECT(mns::write(mns::KeyValueList{}, text, error) && text.empty());
	std::printf("minted: the writer's form byte for byte, read back as the list\n");
	return 0;
}

int refused() {
	const auto refuses = [](const char *name, const char *value) {
		mns::KeyValueList list;
		list.nodes = {{name, value}};
		std::string text, error;
		return !mns::write(list, text, error) && !error.empty() && text.empty();
	};
	TEST_EXPECT(refuses("BAD NAME", "1") && refuses("BAD%NAME", "1") && refuses("", "1") && refuses("A/B", "1"));
	TEST_EXPECT(refuses("A", "") && refuses("A", "#FF0000") && refuses("A", "a//b") && refuses("A", "a\\b") &&
	            refuses("A", " lead") && refuses("A", "trail ") && refuses("A", "two\r\nlines"));
	mns::KeyValueList twice;
	twice.nodes = {{"Same", "1"}, {"SAME", "2"}};
	std::string text, error;
	TEST_EXPECT(!mns::write(twice, text, error) && error.find("one name") != std::string::npos);
	TEST_EXPECT(mns::is_valid_name("DEF_TEXT_FG") && !mns::is_valid_name("A#B") && mns::is_valid_value("a\\\\b") &&
	            !mns::is_valid_value("a\\b"));
	std::printf("refused: a name or value the reader reads otherwise, two names it takes for one\n");
	return 0;
}

int authored() {
	const std::string sheet =
	        "\xEF\xBB\xBF// The style sheet, by hand\r\n"
	        "\r\n"
	        "DEF_FONTNAME\t\t\tGunpl22b.fnt   // the font\r\n"
	        "#if 0\r\n"
	        "OFF\t\tnever\r\n"
	        "#else\r\n"
	        "ON\t\tread\r\n"
	        "#endif\r\n"
	        "LONG first \\\r\n"
	        "   second\r\n"
	        "def_fontname\tGunpl27b.fnt\r\n";
	mns::ReadStatus status = mns::ReadStatus::Hangs;
	const mns::KeyValueList list = read(sheet, &status);
	TEST_EXPECT(status == mns::ReadStatus::Read && list.nodes.size() == 3);
	TEST_EXPECT(list.nodes[0].name == "DEF_FONTNAME" && list.nodes[0].value == "Gunpl27b.fnt");
	TEST_EXPECT(list.nodes[1].name == "ON" && list.nodes[1].value == "read");
	TEST_EXPECT(list.nodes[2].name == "LONG" && list.nodes[2].value == "first second");
	std::string written;
	TEST_EXPECT(reads_back(list, &written));
	TEST_EXPECT(written == "DEF_FONTNAME\tGunpl27b.fnt\r\nON\tread\r\nLONG\tfirst second\r\n");
	std::printf("authored: read, written in the writer's form, read back as the same list\n");
	return 0;
}

// The writer's bytes for each authored sheet under fixtures/mns, pinned as FNV-1a64 vectors (the
// tests/novaworld nw_codec_identity form). The form is OpenNova's own (no original writer of a
// stylesheet is witnessed), so a vector changes only with a change of that form and
// `mns_write_test --dump`'s table in the same commit.
struct WriterVector {
	const char *file;
	size_t len;
	uint64_t hash;
};

const WriterVector kWriterVectors[] = {
    {"shipped_form.mns", 334u, 0x0e0375f00a2efcd2ull},
    {"test_style.mns", 87u, 0xaacfef68eeb8e1f1ull},
};

uint64_t fnv1a64(const std::string &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (const char c : bytes) {
		h ^= static_cast<uint8_t>(c);
		h *= 1099511628211ull;
	}
	return h;
}

// The tree's stylesheets (tests/mnu/repo_menus.h) in place of an installed game's: the authored
// ones, shipped_form.mns among them (the shipped sheets' form: CR LF lines, no byte-order mark,
// whole-line comments and blank lines between the groups, each name set off by two to five tabs,
// font, colour and image values), and the base game's: each read to its end, written, and read back
// as the same list; the writer's bytes for each authored one are the pinned vector.
int tree_sheets(bool dump) {
	const std::vector<std::string> paths = repo_menus::stylesheets();
	if (!repo_menus::holds(paths, "shipped_form.mns")) {
		std::printf("fixtures/mns/shipped_form.mns (the shipped stylesheets' form) is missing\n");
		return 1;
	}
	int failures = 0;
	std::vector<std::string> names;
	std::vector<WriterVector> got;
	for (const std::string &path : paths) {
		std::string bytes;
		if (!test_io::read_file_text(path.c_str(), bytes)) {
			std::printf("  FAIL %s (unreadable)\n", path.c_str());
			++failures;
			continue;
		}
		const std::string name = repo_menus::name(path);
		// An authored sheet sets variables; the base game's brand.mns is comments alone (no brand yet).
		const bool authored = path.find("/fixtures/mns/") != std::string::npos;
		mns::ReadStatus status = mns::ReadStatus::Hangs;
		const mns::KeyValueList list = read(bytes, &status);
		std::string written;
		const bool ok = status == mns::ReadStatus::Read && (!authored || !list.nodes.empty()) && reads_back(list, &written);
		if (!dump)
			std::printf("  %s %s (%zu bytes, %zu variables, written %zu bytes)\n", ok ? "OK  " : "FAIL", name.c_str(),
			            bytes.size(), list.nodes.size(), written.size());
		if (!ok) ++failures;
		// The authored sheets are pinned; the base game's change as the game does.
		if (!authored) continue;
		names.push_back(name);
		got.push_back({nullptr, written.size(), fnv1a64(written)});
	}
	if (dump) {
		std::printf("const WriterVector kWriterVectors[] = {\n");
		for (size_t i = 0; i < got.size(); ++i)
			std::printf("    {\"%s\", %zuu, 0x%016llxull},\n", names[i].c_str(), got[i].len,
			            static_cast<unsigned long long>(got[i].hash));
		std::printf("};\n");
		return 0;
	}
	const size_t expected = sizeof(kWriterVectors) / sizeof(kWriterVectors[0]);
	if (expected != got.size()) {
		std::printf("  FAIL %zu sheets written but %zu vectors pinned (mns_write_test --dump)\n", got.size(), expected);
		++failures;
	}
	for (size_t i = 0; i < got.size() && i < expected; ++i) {
		const WriterVector &e = kWriterVectors[i];
		if (names[i] != e.file || got[i].len != e.len || got[i].hash != e.hash) {
			std::printf("  FAIL %s written %zu bytes hash 0x%016llx, pinned %s %zu bytes hash 0x%016llx\n",
			            names[i].c_str(), got[i].len, static_cast<unsigned long long>(got[i].hash), e.file, e.len,
			            static_cast<unsigned long long>(e.hash));
			++failures;
		}
	}
	std::printf("tree: %zu stylesheet(s) read to the end, written and read back as the same list; %zu writer vectors\n",
	            paths.size(), got.size());
	return failures;
}

} // namespace

// `mns_write_test --dump` prints the writer vector table; the ctest registration passes nothing.
int main(int argc, char **argv) {
	for (int i = 1; i < argc; ++i)
		if (std::string(argv[i]) == "--dump") return tree_sheets(true);
	if (minted() || refused() || authored()) return 1;
	return tree_sheets(false) ? 1 : 0;
}
