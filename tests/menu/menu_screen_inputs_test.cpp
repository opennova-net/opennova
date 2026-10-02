// What a menu screen reads besides its windows, through a file source
// (engine/runtime/menu/menu_screen_inputs.h): the TEXT_RSRC a window's ids read (its own,
// else its root window's [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0]), the tables a
// screen names read once per (name, stamp) with a failed load looked for again [orig:
// CUIStringTable_LookupString @ 0x6527c0], the "menu" section only, and the shell's
// %VAR% list read again only when a sheet's stamp moves [orig: Menu_InitShellResources
// @ 0x552500].

#include <runtime/menu/menu_screen_inputs.h>

#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>

#include "common/test_expect.h"
#include "menu/fake_file_source.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using opennova::menu::MenuDependency;
using opennova::menu::MenuStyleSource;
using opennova::menu::MenuTextTableLoader;
using opennova::menu::MenuTextTables;

using FakeFiles = FakeFileSource;

std::vector<uint8_t> table_bytes(const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> &sections) {
	opennova::rtxt::File table;
	for (const auto &section : sections) {
		const uint32_t index = static_cast<uint32_t>(table.sections.size());
		table.sections.push_back({section.first, static_cast<uint32_t>(section.second.size())});
		for (const auto &row : section.second) {
			opennova::rtxt::Entry entry;
			entry.key = row.first;
			entry.text = row.second;
			entry.section_index = index;
			table.entries.push_back(entry);
		}
	}
	std::vector<uint8_t> bytes;
	std::string error;
	opennova::rtxt::write(table, bytes, error);
	return bytes;
}

// A root naming menutxt.bin, a child naming other.bin, and a grandchild naming none: the
// grandchild falls back to the ROOT's table, not its parent's.
const char *const kScreen =
        "<SCREEN><NAME>S</NAME>"
        "<WINDOW type=\"window\" name=\"ROOT\"><TEXT_RSRC>menutxt.bin</TEXT_RSRC><POSITION><LEFT>0</LEFT></POSITION>"
        "<WINDOW type=\"static\" name=\"CHILD\"><TEXT_RSRC>other.bin</TEXT_RSRC><POSITION><LEFT>0</LEFT></POSITION>"
        "<WINDOW type=\"static\" name=\"GRAND\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>"
        "</WINDOW></WINDOW></SCREEN>";

bool parse_screen(opennova::mnu::Document &doc) {
	std::string error;
	return opennova::mnu::parse(kScreen, doc, error) && !doc.screens.empty();
}

bool looked_up(const MenuTextTables &tables, const char *table, const char *key, const char *expected) {
	const std::string name = table;
	const std::string *text = tables.lookup(&name, key);
	return expected == nullptr ? text == nullptr : text != nullptr && *text == expected;
}

} // namespace

static int test_window_text_rsrc() {
	opennova::mnu::Document doc;
	TEST_EXPECT(parse_screen(doc));
	const opennova::mnu::Window &root = doc.screens[0].roots[0];
	const opennova::mnu::Window &child = root.children[0];
	const opennova::mnu::Window &grand = child.children[0];
	const std::string *own = opennova::menu::window_text_rsrc(child, &root);
	TEST_EXPECT(own != nullptr && *own == "other.bin");
	const std::string *inherited = opennova::menu::window_text_rsrc(grand, &root);
	TEST_EXPECT(inherited != nullptr && *inherited == "menutxt.bin"); // the root's, not the parent's
	TEST_EXPECT(opennova::menu::window_text_rsrc(grand, &grand) == nullptr); // a part reads its own only
	TEST_EXPECT(opennova::menu::screen_text_rsrc_names(doc.screens[0]) ==
	            (std::vector<std::string>{"menutxt.bin", "other.bin"}));
	std::printf("test_window_text_rsrc passed\n");
	return 0;
}

static int test_tables_load_once_per_stamp() {
	opennova::mnu::Document doc;
	TEST_EXPECT(parse_screen(doc));
	FakeFiles files;
	files.put("MENUTXT.BIN", table_bytes({{"Stats", {{"ONLY_STATS", "stats"}}},
	                                      {"Menu", {{"TITLE", "Title"}}},
	                                      {"menu", {{"LATER", "second menu section"}}}}),
	          7);
	MenuTextTableLoader loader;
	MenuTextTables tables;
	std::vector<MenuDependency> dependencies;
	loader.load(doc.screens[0], files, nullptr, tables, &dependencies);
	TEST_EXPECT(dependencies.size() == 2 && dependencies[0].stamp == 7 && dependencies[1].stamp == 0);
	TEST_EXPECT(looked_up(tables, "menutxt.bin", "title", "Title")); // key case-blind, section "menu"
	TEST_EXPECT(looked_up(tables, "menutxt.bin", "ONLY_STATS", nullptr)); // another section is not read
	TEST_EXPECT(looked_up(tables, "menutxt.bin", "LATER", nullptr));      // the first "menu" section only
	TEST_EXPECT(looked_up(tables, "other.bin", "TITLE", nullptr));        // not loaded: the key shows
	TEST_EXPECT(files.reads["menutxt.bin"] == 1 && files.reads["other.bin"] == 0);

	// Unchanged stamps: nothing is read again.
	files.reads.clear();
	loader.load(doc.screens[0], files, nullptr, tables);
	TEST_EXPECT(files.total_reads() == 0);
	TEST_EXPECT(looked_up(tables, "menutxt.bin", "TITLE", "Title"));

	// A table that was missing is looked for again and read once it resolves; a moved stamp
	// reads that table alone.
	files.put("other.bin", table_bytes({{"MENU", {{"TITLE", "Other"}}}}), 3);
	loader.load(doc.screens[0], files, nullptr, tables);
	TEST_EXPECT(files.reads["other.bin"] == 1 && files.reads["menutxt.bin"] == 0);
	TEST_EXPECT(looked_up(tables, "OTHER.BIN", "title", "Other"));
	files.reads.clear();
	files.put("other.bin", table_bytes({{"Menu", {{"TITLE", "Edited"}}}}), 4);
	loader.load(doc.screens[0], files, nullptr, tables);
	TEST_EXPECT(files.reads["other.bin"] == 1 && files.reads["menutxt.bin"] == 0);
	TEST_EXPECT(looked_up(tables, "other.bin", "TITLE", "Edited"));

	// A table that does not parse is not read again until its stamp moves.
	files.put_text("other.bin", "not a table", 5);
	files.reads.clear();
	loader.load(doc.screens[0], files, nullptr, tables);
	loader.load(doc.screens[0], files, nullptr, tables);
	TEST_EXPECT(files.reads["other.bin"] == 1);
	TEST_EXPECT(looked_up(tables, "other.bin", "TITLE", nullptr));

	// The expansion's override table is searched first.
	opennova::rtxt::File override_table;
	const std::vector<uint8_t> override_bytes = table_bytes({{"Menu", {{"TITLE", "Overridden"}}}});
	std::string error;
	TEST_EXPECT(opennova::rtxt::parse(override_bytes.data(), override_bytes.size(), override_table, error));
	loader.load(doc.screens[0], files, &override_table, tables);
	TEST_EXPECT(looked_up(tables, "menutxt.bin", "TITLE", "Overridden"));
	std::printf("test_tables_load_once_per_stamp passed\n");
	return 0;
}

static int test_style_source() {
	FakeFiles files;
	files.put_text("menu_style.mns", "DEF_TEXT_FG FFFFFFFF\r\nTRIM_COLOR FF808080\r\n", 1);
	files.put_text("brand.mns", "TRIM_COLOR FF102030\r\n", 1);
	MenuStyleSource style;
	const std::map<std::string, std::string> &vars = style.vars(files);
	TEST_EXPECT(vars.size() == 2 && vars.at("TRIM_COLOR") == "FF102030" && vars.at("DEF_TEXT_FG") == "FFFFFFFF");
	TEST_EXPECT(files.reads["menu_style.mns"] == 1 && files.reads["brand.mns"] == 1);
	std::vector<MenuDependency> dependencies;
	style.dependencies(dependencies);
	TEST_EXPECT(dependencies.size() == 2 && dependencies[0].name == "menu_style.mns" && dependencies[1].name == "brand.mns");

	files.reads.clear();
	style.vars(files);
	TEST_EXPECT(files.total_reads() == 0); // unchanged stamps: the list is kept

	files.put_text("brand.mns", "TRIM_COLOR FF000001\r\n", 2);
	TEST_EXPECT(style.vars(files).at("TRIM_COLOR") == "FF000001");
	files.remove("brand.mns"); // a missing sheet changes nothing
	TEST_EXPECT(style.vars(files).at("TRIM_COLOR") == "FF808080");
	std::printf("test_style_source passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_window_text_rsrc();
	failures += test_tables_load_once_per_stamp();
	failures += test_style_source();
	return failures == 0 ? 0 : 1;
}
