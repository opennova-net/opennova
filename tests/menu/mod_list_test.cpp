// The Mods list (menu/mod_list.h, D-MNU-31) run through the runtime's own driver over a headless frame
// (MenuStateFrame): the rows [orig: Options_PopulateModList @ 0x559fb0], a pick's description
// [orig: Options_OnModListSelect @ 0x55a530], ACCEPT's pick and its compare with the game running
// [orig: Options_HandleAcceptOrBack @ 0x55ad05..0x55ad5b].
//
// Pinned: the base game's row first, its text the list's string table's for the key (the key itself
// where the table names none, and where the window reads no table); a row per record in order, its
// EXP_NAME; the base row selected, else the record whose directory names the game running without
// case, its description shown (the descriptions untouched with the base game running); a pick shows
// its record's description alone, the base row's nothing; ACCEPT takes the selected row's directory,
// "" for the base row, the first record with nothing selected; the pick of the game running takes
// nothing and drops a request raised before, the base game's "" is a pick like another.
#include <formats/mnu/mnu.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/menu/menu_flow.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>
#include <runtime/menu/menu_text_tables.h>
#include <runtime/menu/mod_list.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// The base game's Mods screen as the original game's code finds it: OPTIONS, AVAIL_LIST, MOD_DESC.
const char *kOptions = R"(<SCREEN>
	<NAME>OPTIONS</NAME>
	<WINDOW type="window" name="MAIN">
		<TEXT_RSRC>menutxt.bin</TEXT_RSRC>
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="list" name="AVAIL_LIST">
			<POSITION><LEFT>100</LEFT><TOP>160</TOP><RIGHT>390</RIGHT><BOTTOM>420</BOTTOM></POSITION>
			<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
		</WINDOW>
		<WINDOW type="multiline_edit" name="MOD_DESC" READONLY>
			<POSITION><LEFT>410</LEFT><TOP>160</TOP><RIGHT>700</RIGHT><BOTTOM>420</BOTTOM></POSITION>
			<STRING justify="LEFT" vjustify="TOP">AUTHORED</STRING>
		</WINDOW>
		<WINDOW type="button" name="ACCEPT">
			<POSITION><LEFT>510</LEFT><TOP>440</TOP><RIGHT>700</RIGHT><BOTTOM>468</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

std::shared_ptr<const rtxt::File> menu_table(const std::string &key, const std::string &text) {
	auto file = std::make_shared<rtxt::File>();
	file->sections.push_back({ "Menu", 1 });
	file->entries.push_back({ key, text, {}, 0 });
	return file;
}

struct Run {
	mnu::Document doc;
	MenuStateFrame frame;
	MenuRuntime rt;
	MenuTextTables tables;

	bool open(const char *text) {
		std::string error;
		if (!mnu::parse(text, doc, error)) return false;
		frame.set_configure([this](const std::string &screen, MenuFrameCompiler &compiler) {
			const mnu::Screen *found = screen.empty() ? doc.first_screen() : doc.find_screen(screen);
			compiler.set_text_tables(&tables);
			compiler.configure(found);
			return found != nullptr;
		});
		rt.set_frame(&frame);
		return rt.open_document(&doc, "options.mnu", std::string());
	}
	int id(const char *name) const { return rt.widget_id(name); }
};

std::vector<ExpansionRecord> records() {
	return {
		{ "jox01", { "Joint Operations: Escalation", "Kendari island." } },
		{ "onjo1", { "OpenNova: Indigo Storm", "A storm." } },
	};
}

ModList mods() {
	ModList list;
	list.set_descriptions({ "MOD_DESC", "MOD_DESCRIPTION" });
	list.set_records(records());
	return list;
}

void test_rows() {
	Run run;
	CHECK(run.open(kOptions));
	const int list = run.id("AVAIL_LIST");
	const int desc = run.id("MOD_DESC");
	CHECK(list >= 0 && desc >= 0);
	const ModList modlist = mods();

	// No table loaded for the screen's TEXT_RSRC: the key shows. The base game runs: its row selected,
	// the description as authored.
	modlist.populate(run.rt, list, "");
	const std::vector<std::string> rows = run.rt.get_widget_items(list);
	CHECK(rows.size() == 3);
	if (rows.size() == 3) {
		CHECK(rows[0] == kModListBaseKey);
		CHECK(rows[1] == "Joint Operations: Escalation" && rows[2] == "OpenNova: Indigo Storm");
	}
	CHECK(run.rt.selected_row(list) == 0);
	CHECK(run.rt.get_widget_text(desc) == "AUTHORED");

	// The running expansion, named without case: its row selected, its description shown.
	modlist.populate(run.rt, list, "ONJO1");
	CHECK(run.rt.selected_row(list) == 2);
	CHECK(run.rt.get_widget_text(desc) == "A storm.");

	// A name no record has: the base row stays selected.
	modlist.populate(run.rt, list, "absent");
	CHECK(run.rt.selected_row(list) == 0);
}

void test_base_row_from_the_string_table() {
	Run run;
	run.tables.set_table("menutxt.bin", menu_table(kModListBaseKey, "OpenNova"));
	CHECK(run.open(kOptions));
	const int list = run.id("AVAIL_LIST");
	mods().populate(run.rt, list, "");
	CHECK(run.rt.item_text(list, 0) == "OpenNova");
	CHECK(run.rt.widget_string(list, "NOT_A_KEY") == "NOT_A_KEY");
	// The override table is tried first, through the window's own table.
	auto override_table = menu_table(kModListBaseKey, "Override");
	run.tables.set_override(override_table.get());
	CHECK(run.rt.widget_string(list, kModListBaseKey) == "Override");
	run.tables.set_override(nullptr);
}

void test_select_and_pick() {
	Run run;
	CHECK(run.open(kOptions));
	const int list = run.id("AVAIL_LIST");
	const int desc = run.id("MOD_DESC");
	const ModList modlist = mods();
	modlist.populate(run.rt, list, "");

	modlist.select(run.rt, list, 1);
	CHECK(run.rt.get_widget_text(desc) == "Kendari island.");
	modlist.select(run.rt, list, 0);
	CHECK(run.rt.get_widget_text(desc).empty());
	modlist.select(run.rt, list, 7); // no such row: nothing
	CHECK(run.rt.get_widget_text(desc).empty());

	run.rt.select_row(list, 2, false);
	CHECK(modlist.pick(run.rt, list) == "onjo1");
	run.rt.select_row(list, 0, false);
	CHECK(modlist.pick(run.rt, list).empty());
	// Nothing selected: the game reads the value 0, the first record.
	run.rt.select_row(list, -1, false);
	CHECK(run.rt.selected_row(list) == -1);
	CHECK(modlist.pick(run.rt, list) == "jox01");
	ModList empty;
	CHECK(empty.pick(run.rt, list).empty());
}

void test_accept_compare() {
	using Pick = MenuFlow::ExpansionPick;
	MenuFlow flow;
	CHECK(flow.request_expansion("JOX01", "jox01", true) == Pick::Ignored);
	CHECK(!flow.has_pending_expansion());
	CHECK(flow.request_expansion("", "jox01", false) == Pick::NeedsPackedRoot);
	CHECK(flow.request_expansion("", "jox01", true) == Pick::Queued);
	CHECK(flow.has_pending_expansion());
	CHECK(flow.take_expansion_reload().empty() && !flow.has_pending_expansion());
	CHECK(flow.request_expansion("onjo1", "", true) == Pick::Queued);
	CHECK(flow.request_expansion("", "", true) == Pick::Ignored);
	CHECK(!flow.has_pending_expansion());
}

} // namespace

int main() {
	test_rows();
	test_base_row_from_the_string_table();
	test_select_and_pick();
	test_accept_compare();
	if (failures == 0) std::printf("menu_mod_list: all passed\n");
	return failures == 0 ? 0 : 1;
}
