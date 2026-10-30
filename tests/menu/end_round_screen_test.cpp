// The end of round's STAT screen controller (inmatch/end_round_screen.h) run headless over the
// runtime and the state frame, a stat.mnu-shaped document and a fake session.
//
// Pinned: the show fills the RESULTLIST from the session's columns (at the table's design width)
// and rows, sorted on the first stat column with the local slot's row selected; a team game checks
// OVERALL and leaves the tabs shown, any other hides the three; a tab radio filters the filled rows
// by their team; CONFIRM_YES closes, then exits; HIDDEN_BACK asks nothing of the session.
// [orig: StatScreen_ShowCallback @0x562840; StatScreen_StatFilterTabHandler @0x562140;
//  UI_StatConfirmExitCommand @0x562210]
#include <formats/mnu/mnu.h>
#include <runtime/inmatch/end_round_screen.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;
using menu::MenuRuntime;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

const char *kStat = R"(<SCREEN>
	<NAME>STAT</NAME>
	<WINDOW type="window" name="ROOT">
		<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
		<WINDOW type="table" name="RESULTLIST">
			<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>610</RIGHT><BOTTOM>410</BOTTOM></POSITION>
			<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
		</WINDOW>
		<WINDOW type="radio" name="RADIO_TAB_OVERALL">
			<POSITION><LEFT>10</LEFT><TOP>420</TOP><RIGHT>100</RIGHT><BOTTOM>440</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="radio" name="RADIO_TAB_REDTEAM">
			<POSITION><LEFT>110</LEFT><TOP>420</TOP><RIGHT>200</RIGHT><BOTTOM>440</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="radio" name="RADIO_TAB_BLUETEAM">
			<POSITION><LEFT>210</LEFT><TOP>420</TOP><RIGHT>300</RIGHT><BOTTOM>440</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="button" name="HIDDEN_BACK">
			<POSITION><LEFT>400</LEFT><TOP>500</TOP><RIGHT>500</RIGHT><BOTTOM>520</BOTTOM></POSITION>
		</WINDOW>
		<WINDOW type="button" name="CONFIRM_YES">
			<POSITION><LEFT>600</LEFT><TOP>500</TOP><RIGHT>700</RIGHT><BOTTOM>520</BOTTOM></POSITION>
		</WINDOW>
	</WINDOW>
</SCREEN>
)";

StatScreenRow make_row(uint8_t slot, uint8_t team) {
	StatScreenRow row;
	row.slot = slot;
	row.team = team;
	row.name = team == 1 ? "Ace" : "Bee";
	row.squad = "-";
	row.cells = { slot == 3 ? "7" : "2" };
	return row;
}

struct FakeSession final : EndRoundScreenHost {
	bool team = false;
	int width_asked = 0;
	int slot = 5;
	std::vector<std::string> requests;

	bool team_mode() override { return team; }
	std::vector<StatScreenColumn> columns(int table_width) override {
		width_asked = table_width;
		std::vector<StatScreenColumn> out(3);
		out[0].header = "Name";
		out[1].header = "Squad";
		out[2].header = "Kills";
		out[2].field_id = 1;
		out[2].field_index = 0;
		return out;
	}
	std::vector<StatScreenRow> rows() override { return { make_row(5, 2), make_row(3, 1) }; }
	int local_slot() override { return slot; }
	void close_screen() override { requests.push_back("close"); }
	void exit_to_menu() override { requests.push_back("exit"); }
};

struct Run {
	mnu::Document doc;
	menu::MenuStateFrame frame;
	MenuRuntime rt;
	FakeSession session;
	EndRoundScreen screen;

	bool open() {
		std::string error;
		if (!mnu::parse(kStat, doc, error)) return false;
		frame.set_configure([this](const std::string &name, menu::MenuFrameCompiler &compiler) {
			const mnu::Screen *found = name.empty() ? doc.first_screen() : doc.find_screen(name);
			compiler.configure(found);
			return found != nullptr;
		});
		rt.set_frame(&frame);
		rt.set_sink([this](const menu::MenuEvent &e) { screen.on_event(rt, e); });
		screen.set_host(&session);
		if (!rt.open_document(&doc, EndRoundScreen::kFile, EndRoundScreen::kScreen)) return false;
		screen.open(rt, EndRoundScreen::kFile, rt.current_screen());
		return true;
	}
	int id(const char *name) const { return rt.widget_id(name); }
};

void test_the_show_fills_the_table_and_hides_the_tabs_off_a_team_game() {
	Run run;
	CHECK(run.open());
	const int list = run.id("RESULTLIST");
	CHECK(run.session.width_asked == 600);
	CHECK(run.rt.table_row_count(list) == 2);
	CHECK(run.rt.table_sort_column(list) == 2);
	// The sort (kills descending) puts slot 3 first; the local slot 5's row is selected after it.
	CHECK(run.rt.table_cell_value(list, 0, 1) == 3 && run.rt.table_cell_value(list, 1, 1) == 5);
	CHECK(run.rt.table_selected_rows(list) == std::vector<int>{ 1 });
	for (const char *tab : { "RADIO_TAB_OVERALL", "RADIO_TAB_REDTEAM", "RADIO_TAB_BLUETEAM" })
		CHECK(!run.rt.is_widget_shown(run.id(tab)));
	CHECK(!run.rt.is_widget_checked(run.id("RADIO_TAB_OVERALL")));
}

void test_a_team_game_checks_overall_and_the_tabs_filter() {
	Run run;
	run.session.team = true;
	CHECK(run.open());
	const int list = run.id("RESULTLIST");
	CHECK(run.rt.is_widget_checked(run.id("RADIO_TAB_OVERALL")));
	for (const char *tab : { "RADIO_TAB_OVERALL", "RADIO_TAB_REDTEAM", "RADIO_TAB_BLUETEAM" })
		CHECK(run.rt.is_widget_shown(run.id(tab)));
	CHECK(run.screen.tab() == -1);
	// RED: the team-2 rows (slot 5 at row 1); BLUE: the team-1 rows; OVERALL: every row.
	run.rt.activate(run.id("RADIO_TAB_REDTEAM"));
	CHECK(run.screen.tab() == 1);
	CHECK(!run.rt.table_row_visible(list, 0) && run.rt.table_row_visible(list, 1));
	run.rt.activate(run.id("RADIO_TAB_BLUETEAM"));
	CHECK(run.screen.tab() == 2);
	CHECK(run.rt.table_row_visible(list, 0) && !run.rt.table_row_visible(list, 1));
	run.rt.activate(run.id("RADIO_TAB_OVERALL"));
	CHECK(run.screen.tab() == 0);
	CHECK(run.rt.table_row_visible(list, 0) && run.rt.table_row_visible(list, 1));
	// The filter keeps the rows and the selection.
	CHECK(run.rt.table_row_count(list) == 2 && run.rt.table_selected_rows(list) == std::vector<int>{ 1 });
}

void test_the_exit() {
	Run run;
	CHECK(run.open());
	run.rt.activate(run.id("HIDDEN_BACK"));
	CHECK(run.session.requests.empty());
	run.rt.activate(run.id("CONFIRM_YES"));
	CHECK((run.session.requests == std::vector<std::string>{ "close", "exit" }));
}

} // namespace

int main() {
	test_the_show_fills_the_table_and_hides_the_tabs_off_a_team_game();
	test_a_team_game_checks_overall_and_the_tabs_filter();
	test_the_exit();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("end_round_screen_test OK\n");
	return 0;
}
