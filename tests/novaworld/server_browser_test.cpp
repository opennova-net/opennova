// The server browser's table model (engine/net/novaworld/server_browser.h):
// the cells, the ping states, the quick filters and search, the numeric /
// text / ping sort orders (unmeasured last in either direction), the details
// lines and the hover text. Pins moved here from godot/tests/novaworld_panel_test.gd.
#include <cstdio>
#include <string>
#include <vector>

#include <net/novaworld/ping_sweep.h>
#include <net/novaworld/server_browser.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

GsbServerEntry row(uint32_t rid, const char *name, const char *mission, int players,
                   int max_players, const char *game_type, const char *password,
                   const char *mod = "") {
	GsbServerEntry e;
	e.rid = rid;
	e.server_name = name;
	e.mission_name = mission;
	e.players = players;
	e.max_players = max_players;
	e.game_type = game_type;
	e.password = password;
	e.locked = "N";
	e.mod = mod;
	return e;
}

std::vector<GsbServerEntry> browser_rows() {
	return {
		row(1, "Bravo", "G11", 16, 16, "COOP", "N"),
		row(2, "alpha", "Ash", 0, 32, "TDM", "Y"),
		row(3, "Charlie", "Delta", 4, 24, "COOP", "N", "escalation"),
	};
}

std::vector<std::size_t> all(std::size_t n) {
	std::vector<std::size_t> order;
	for (std::size_t i = 0; i < n; ++i) order.push_back(i);
	return order;
}

void test_row_cells_cover_the_table_columns() {
	GsbServerEntry r = row(7, "Alpha", "ASH_G11A", 3, 16, "COOP", "N");
	auto cells = browser_row_cells(r, 42);
	CHECK(cells[static_cast<int>(BrowserColumn::Name)] == "Alpha");
	CHECK(cells[static_cast<int>(BrowserColumn::Mission)] == "ASH_G11A");
	CHECK(cells[static_cast<int>(BrowserColumn::Type)] == "COOP");
	CHECK(cells[static_cast<int>(BrowserColumn::Players)] == "3/16");
	CHECK(cells[static_cast<int>(BrowserColumn::Ping)] == "42");
	CHECK(cells[static_cast<int>(BrowserColumn::Access)] == "Open");
	r.password = "Y";
	CHECK(browser_row_cells(r, kPingPending)[static_cast<int>(BrowserColumn::Access)] == "Password");
	r.password = "N";
	r.locked = "Y";
	CHECK(browser_row_is_locked(r));
}

// The ping cell's states: pending, the sweep's two negative codes, a round-trip.
void test_ping_text_states() {
	CHECK(browser_ping_text(kPingPending) == "...");
	CHECK(browser_ping_text(kPingFailed) == "N/A");
	CHECK(browser_ping_text(kPingNeverAttempted) == "N/A");
	CHECK(browser_ping_text(87) == "87");
	CHECK(browser_ping_text(0) == "0");
}

void test_filter_rows_quick_filters_and_search() {
	const auto rows = browser_rows();
	CHECK(browser_filter_rows(rows, {}).size() == 3);
	BrowserFilter f;
	f.hide_full = true;
	CHECK(browser_filter_rows(rows, f).size() == 2);
	f = {};
	f.hide_empty = true;
	CHECK(browser_filter_rows(rows, f).size() == 2);
	f = {};
	f.hide_locked = true;
	CHECK(browser_filter_rows(rows, f).size() == 2);
	f = {};
	f.game_type = "coop";
	CHECK(browser_filter_rows(rows, f).size() == 2);
	f = {};
	f.text = "  ESCAL ";
	auto by_text = browser_filter_rows(rows, f);
	CHECK(by_text.size() == 1 && rows[by_text[0]].server_name == "Charlie");
	f.text = "g11";
	by_text = browser_filter_rows(rows, f);
	CHECK(by_text.size() == 1 && rows[by_text[0]].rid == 1);
}

void test_sort_rows_text_numeric_and_ping() {
	const auto rows = browser_rows();
	const std::vector<int> no_pings(rows.size(), kPingPending);
	auto order = all(rows.size());
	browser_sort_rows(order, rows, no_pings, BrowserColumn::Name, true);
	CHECK(rows[order[0]].server_name == "alpha");
	CHECK(rows[order[2]].server_name == "Charlie");
	browser_sort_rows(order, rows, no_pings, BrowserColumn::Name, false);
	CHECK(rows[order[0]].server_name == "Charlie");
	browser_sort_rows(order, rows, no_pings, BrowserColumn::Players, false);
	CHECK(rows[order[0]].players == 16);
	browser_sort_rows(order, rows, no_pings, BrowserColumn::Access, true);
	CHECK(rows[order[2]].rid == 2);  // the passworded row sorts after the open ones
	// Ping: rid 3 fastest, rid 1 slower, rid 2 unmeasured -> always last.
	const std::vector<int> pings = { 95, kPingPending, 20 };
	browser_sort_rows(order, rows, pings, BrowserColumn::Ping, true);
	CHECK(rows[order[0]].rid == 3 && rows[order[1]].rid == 1 && rows[order[2]].rid == 2);
	browser_sort_rows(order, rows, pings, BrowserColumn::Ping, false);
	CHECK(rows[order[0]].rid == 1 && rows[order[1]].rid == 3 && rows[order[2]].rid == 2);
	// A failed sweep sorts with the unmeasured, after every measured row.
	const std::vector<int> failed = { kPingFailed, 12, kPingNeverAttempted };
	browser_sort_rows(order, rows, failed, BrowserColumn::Ping, true);
	CHECK(rows[order[0]].rid == 2);
	CHECK(rows[order[1]].server_name == "Bravo" && rows[order[2]].server_name == "Charlie");  // name tiebreak
}

void test_details_lines_and_tooltip() {
	GsbServerEntry r = row(1, "Bravo", "G11", 4, 24, "COOP", "Y", "escalation");
	r.msg = "Friday night co-op";
	r.ver1 = "1.7.5.7";
	r.dedicated = "Y";
	r.pb_server = "0";
	r.region = "  ";
	std::string text;
	for (const std::string &line : browser_details_lines(r)) text += line + "\n";
	CHECK(text.find("Server: Bravo\n") != std::string::npos);
	CHECK(text.find("Message: Friday night co-op\n") != std::string::npos);
	CHECK(text.find("Players: 4/24\n") != std::string::npos);
	CHECK(text.find("Mod: escalation\n") != std::string::npos);
	CHECK(text.find("Version: 1.7.5.7\n") != std::string::npos);
	CHECK(text.find("Dedicated server\n") != std::string::npos);
	CHECK(text.find("Password protected\n") != std::string::npos);
	CHECK(text.find("PunkBuster") == std::string::npos);
	CHECK(text.find("Region:") == std::string::npos);  // blank fields are skipped

	GsbServerEntry t;
	t.mission_name = "ASH_G11A";
	t.region = "Jungle";
	t.country = "US";
	t.ip = "203.0.113.7";
	CHECK(browser_row_tooltip(t) == "Mission: ASH_G11A\nRegion: Jungle\nCountry: US\nAddress: 203.0.113.7");
	t.exp = "JOE";
	t.ip = "0.0.0.0";
	CHECK(browser_row_tooltip(t) == "Mission: ASH_G11A\nRegion: Jungle\nCountry: US\nExpansion: JOE");
}

} // namespace

int main() {
	test_row_cells_cover_the_table_columns();
	test_ping_text_states();
	test_filter_rows_quick_filters_and_search();
	test_sort_rows_text_numeric_and_ping();
	test_details_lines_and_tooltip();
	if (failures == 0) std::printf("server_browser_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
