// The stylesheet writer (formats/mns, mns::write): a stylesheet's model is the reader's list (what the game
// reads of the file, its directives decided, its comments, blanks and switched-off lines read as nothing), and
// the writer puts the list down alone, `name<TAB>value` with CR LF a line in the list's order (no original
// writer of a stylesheet is witnessed: the form is OpenNova's). A minted list is the writer's form byte for byte
// and reads back as itself; an authored sheet (comments, directives, a continuation, a repeat, odd spacing)
// read, written and read again is the same list; a name or value the reader would read otherwise is refused.
// The retail leg (--retail) reads the reference set's shipped menu_style.mns and every .mns the packed install
// serves: each read to its end, written, and read back as the same list.
#include <base/vfs/vfs.h>
#include <formats/mns/mns.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

#include <cctype>
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

int retail_sheets() {
	int checked = 0;
	const auto check = [&](const std::string &label, const std::string &bytes) {
		mns::ReadStatus status = mns::ReadStatus::Hangs;
		const mns::KeyValueList list = read(bytes, &status);
		std::string written;
		const bool ok = status == mns::ReadStatus::Read && !list.nodes.empty() && reads_back(list, &written);
		std::printf("  %s %s (%zu bytes, %zu variables, written %zu bytes)\n", ok ? "OK  " : "FAIL", label.c_str(),
		            bytes.size(), list.nodes.size(), written.size());
		++checked;
		return ok ? 0 : 1;
	};
	int failures = 0;
	const std::string reference = retail::reference_fixture("mns/menu_style.mns");
	std::string bytes;
	if (!reference.empty() && test_io::read_file_text(reference.c_str(), bytes))
		failures += check("reference/menu_style.mns", bytes);
	const std::string install = retail::install();
	if (!install.empty()) {
		std::vector<std::string> mounts = {std::string()};
		for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
		for (const std::string &expansion : mounts) {
			Vfs vfs;
			if (!vfs.mount_game(install, expansion, VfsMountMode::Packed)) {
				std::printf("  FAIL mount_game(%s, %s)\n", install.c_str(), expansion.c_str());
				++failures;
				continue;
			}
			for (const auto &loc : vfs.list_files()) {
				std::string name = loc.logical_name;
				std::string lower = name;
				for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".mns") != 0) continue;
				std::vector<uint8_t> raw;
				if (!vfs.read_file_raw(name, raw)) {
					std::printf("  FAIL %s (unreadable)\n", name.c_str());
					++failures;
					continue;
				}
				failures += check((expansion.empty() ? std::string("<install>/") : expansion + "/") + name,
				                  std::string(raw.begin(), raw.end()));
			}
		}
	}
	if (checked == 0) return retail::skip_leg("OPENNOVA_JO_DIR or OPENNOVA_JO_ASSETS (the shipped stylesheets)");
	std::printf("retail: %d stylesheet(s) read to the end, written and read back as the same list\n", checked);
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (minted() || refused() || authored()) return 1;
	if (!retail::selected()) return 0;
	return retail_sheets() ? 1 : 0;
}
