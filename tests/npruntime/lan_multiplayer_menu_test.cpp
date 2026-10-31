// lan_multiplayer_menu: the LAN multiplayer screens' controller (runtime/inmatch/lan_multiplayer_menu.h,
// mp.mnu's LAN_MULTI_PLAYER browser and MULTI_PLAYER_HOST settings, the shell's "lan" delegate) driven
// headless over the menu runtime and a fake host, as the shell hands it the document: open after the
// open's wiring, every runtime event while it holds the document.
//
// Pinned: the LAN screen's activation (the list emptied, the records drained, the host screen's
// location and LAN-only controls hidden); LAN_SEARCH's searching state and its text, the window's end,
// a failed search's row; the list's rows and the keep-selection rule; a single click arming
// LAN_JOINGAME; the join's stop-then-row order and its refusal text; the host screen's populate from
// the cfg block, its pool (stock co-op host-invisible, the GAME_TYPE filter), ADD (the Type cell, the
// Switch default, the de-dupe) and REMOVE, the START gate, the Switch cell's toggle; START's HOSTMNX
// hide, its read into the block, the LAN arm's save ahead of the request and the NovaWorld arm's none;
// the NovaWorld setup's cancel on leaving the title's host screen (DFX2's NW_MULTI_PLAYER) and on the
// document's close; the LAN activation's network connect type 2; the restriction radios and the
// START gate's class words.
// [orig: UI_RegisterLANMultiplayerCallbacks @0x558d20; UI_InitLANMultiplayerScreen @0x556960;
//  UI_LANMultiPlayer_OnSearch @0x558c50; UI_HandleLANGameListEvent @0x556c70;
//  UI_LANMultiPlayer_OnJoinGame @0x556ce0; UI_HandleHostSessionStart @0x556d00]
#include <runtime/inmatch/lan_multiplayer_menu.h>

#include "common/test_expect.h"

#include <base/title/title.h>
#include <formats/mnu/mnu.h>
#include <runtime/inmatch/game_config_block.h>
#include <runtime/menu/menu_runtime.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

std::string wnd(const char *type, const char *name, int top, const std::string &inner = std::string()) {
	char position[160];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>10</LEFT><TOP>%d</TOP><RIGHT>250</RIGHT><BOTTOM>%d</BOTTOM></POSITION>", top, top + 20);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\">" + position + inner + "</WINDOW>";
}

std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN><NAME>") + name +
	       "</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>"
	       "<BOTTOM>600</BOTTOM></POSITION>" +
	       body + "</WINDOW></SCREEN>";
}

// A synthetic mp.mnu: the LAN browser and the host settings (the edits, the mission list, the
// 3-column rotation table, the buttons; with_spins adds GAME_TYPE and SERVERTYPE with authored values).
std::string mp_menu(bool with_spins) {
	std::string lan = wnd("list", "LAN_GAME_LIST", 10) + wnd("button", "LAN_SEARCH", 200) +
	                  wnd("button", "LAN_JOINGAME", 224);
	std::string host = wnd("edit", "GAME_NAME", 10) + wnd("edit", "MAX_PLAYERS", 34) +
	                   wnd("checkbox", "ALLOW_SPECTATORS", 330) + wnd("edit", "SPECTATOR_PW", 354) +
	                   wnd("list", "MISSION_LIST", 58) +
	                   "<WINDOW type=\"table\" name=\"SELECTED_MISSIONS\"><POSITION><LEFT>300</LEFT><TOP>58</TOP>"
	                   "<RIGHT>520</RIGHT><BOTTOM>200</BOTTOM></POSITION><COLUMN count=\"3\">"
	                   "<HEADER column=\"0\" width=\"80\" justify=\"LEFT\">MISSION</HEADER>"
	                   "<HEADER column=\"1\" width=\"60\" justify=\"LEFT\">TYPE</HEADER>"
	                   "<HEADER column=\"2\" width=\"40\" justify=\"LEFT\">SWITCH</HEADER></COLUMN></WINDOW>" +
	                   wnd("button", "ADD_MISSIONS", 210) + wnd("button", "REMOVE_MISSIONS", 234) +
	                   wnd("button", "START_GAME", 258) + wnd("window", "HOSTMNX", 400) +
	                   wnd("static", "STATIC_GAME_LOCATION", 420) +
	                   wnd("spinlist", "GAME_LOCATION", 440, "<ITEMS><ITEM value=\"32\">US</ITEM></ITEMS>") +
	                   wnd("static", "STATIC_SERVER_LANONLY", 460) + wnd("checkbox", "SERVER_LANONLY", 480);
	if (with_spins) {
		host += wnd("spinlist", "GAME_TYPE", 282,
				"<ITEMS><ITEM value=\"3\">HG_COOPERATIVE</ITEM><ITEM value=\"0\">HG_DEATHMATCH</ITEM></ITEMS>");
		host += wnd("spinlist", "SERVERTYPE", 306,
				"<ITEMS><ITEM value=\"0\">HG_SERVEPLAY</ITEM><ITEM value=\"1\">HG_SERVEONLY</ITEM></ITEMS>");
	}
	return screen("LAN_MULTI_PLAYER", lan) + screen("MULTI_PLAYER_HOST", host);
}

menu::MissionChoice pool_row(const char *file, uint32_t game_type = 0x30020) {
	return {file, file, std::string(), game_type};
}

// The host the shell's binding stands in for: every call recorded in order.
struct FakeHost : LanMultiplayerHost {
	GameConfigBlock block;
	std::vector<menu::MissionChoice> pool;
	std::string expansion_name;
	bool session = true;
	bool search_starts = true;
	std::string search_error; // reported through browse_error by a failing start
	LanMultiplayerMenu *menu = nullptr;
	bool joinable = true;
	std::vector<std::string> calls;
	std::vector<LanHostRequest> requests;
	std::string refused;
	int joined = -1;

	FakeHost() { block.set_title(*title_by_code("jo")); }
	std::string menu_text(const std::string &key) override { return "<" + key + ">"; }
	std::string error_text(const std::string &section, const std::string &key) override {
		return section + "/" + key;
	}
	hud::GameTextLookup gametext() override { return hud::GameTextLookup(); }
	hud::GameTextLookup menu_ui() override { return hud::GameTextLookup(); }
	const Title &session_title() override { return *block.title(); }
	GameConfigBlock *config_block() override { return &block; }
	std::vector<menu::MissionChoice> host_pool() override { return pool; }
	std::string expansion() override { return expansion_name; }
	bool has_session() override { return session; }
	bool start_search() override {
		calls.push_back("start_search");
		if (!search_error.empty() && menu) menu->browse_error(search_error);
		return search_starts;
	}
	void stop_enumerator() override { calls.push_back("stop_enumerator"); }
	void drain_records() override { calls.push_back("drain_records"); }
	bool join(int row) override {
		calls.push_back("join");
		joined = row;
		return joinable;
	}
	void join_refused(const std::string &text) override { refused = text; }
	void save_config() override { calls.push_back("save_config"); }
	void host_start(const LanHostRequest &request) override {
		calls.push_back("host_start");
		requests.push_back(request);
	}
	void novaworld_host_cancelled() override { calls.push_back("novaworld_host_cancelled"); }
	bool called(const char *name) const {
		for (const std::string &call : calls)
			if (call == name) return true;
		return false;
	}
};

// The shell's hand-over: the document opened on `screen_name`, the controller opened over it and every
// later runtime event forwarded while it holds the document.
struct Harness {
	mnu::Document doc;
	menu::MenuRuntime runtime;
	FakeHost host;
	LanMultiplayerMenu controller;
	bool ok = false;

	explicit Harness(const char *screen_name, bool with_spins = false,
			std::vector<menu::MissionChoice> pool = {}) {
		std::string error;
		ok = mnu::parse(mp_menu(with_spins), doc, error);
		host.pool = std::move(pool);
		host.menu = &controller;
		controller.set_host(&host);
		runtime.set_sink([this](const menu::MenuEvent &event) {
			if (controller.is_open()) controller.on_event(runtime, event);
		});
		ok = ok && runtime.open_document(&doc, "mp.mnu", screen_name);
		if (ok) controller.open(runtime, "mp.mnu", screen_name);
	}
	int id(const char *name) const { return runtime.widget_id(name); }
	void press(const char *name) { runtime.activate(id(name)); }
	void cell_click(const char *name, int row, int column) {
		menu::MenuEvent event;
		event.kind = menu::MenuEvent::Kind::TableCellClicked;
		event.id = id(name);
		event.text = name;
		event.value = row;
		event.column = column;
		controller.on_event(runtime, event);
	}
	void double_click(const char *name, int row) {
		menu::MenuEvent event;
		event.kind = menu::MenuEvent::Kind::ListActivated;
		event.id = id(name);
		event.value = row;
		controller.on_event(runtime, event);
	}
	void add_first_mission() {
		runtime.select_row(id("MISSION_LIST"), 0, true);
		press("ADD_MISSIONS");
	}
};

LanMenuServer server(const char *name, const char *ip, int port) {
	return {ip, port, std::string(name) + " (0/0)"};
}

// The LAN screen's activation and its list.
int lan_screen() {
	Harness h("LAN_MULTI_PLAYER");
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	TEST_EXPECT(h.host.called("drain_records"));
	// The host screen's location and LAN-only controls hide on the LAN screen's activation.
	for (const char *name : {"STATIC_GAME_LOCATION", "GAME_LOCATION", "STATIC_SERVER_LANONLY", "SERVER_LANONLY"})
		TEST_EXPECT(!h.runtime.is_widget_shown(h.id(name)));
	const int list = h.id("LAN_GAME_LIST");
	h.controller.servers_changed({server("alpha", "192.168.1.10", 32768), server("bravo", "192.168.1.11", 32768)});
	TEST_EXPECT(h.runtime.item_count(list) == 2 && h.runtime.item_text(list, 1) == "bravo (0/0)");
	// A single click selects the row and makes LAN_JOINGAME interactive.
	h.runtime.set_widget_disabled(h.id("LAN_JOINGAME"), true);
	h.runtime.select_row(list, 1, true);
	TEST_EXPECT(h.controller.selected_server() == 1);
	TEST_EXPECT(!h.runtime.is_widget_disabled(h.id("LAN_JOINGAME")));
	// A host appended below keeps the highlighted row selected; JOIN joins it.
	h.controller.servers_changed({server("alpha", "192.168.1.10", 32768), server("bravo", "192.168.1.11", 32768),
			server("charlie", "192.168.1.12", 32768)});
	TEST_EXPECT(h.runtime.item_count(list) == 3 && h.runtime.selected_row(list) == 1);
	TEST_EXPECT(h.controller.selected_server() == 1);
	h.press("LAN_JOINGAME");
	TEST_EXPECT(h.host.joined == 1);
	// The press stops the enumerator ahead of the join's row.
	TEST_EXPECT(h.host.calls.size() >= 2 && h.host.calls[h.host.calls.size() - 2] == "stop_enumerator" &&
	            h.host.calls.back() == "join");
	// A snapshot whose selected row is another session drops the selection: JOIN does nothing.
	h.host.joined = -1;
	h.controller.servers_changed({server("replacement", "192.168.1.20", 32769)});
	TEST_EXPECT(h.runtime.item_count(list) == 1 && h.runtime.item_text(list, 0) == "replacement (0/0)");
	TEST_EXPECT(h.controller.selected_server() == -1);
	h.press("LAN_JOINGAME");
	TEST_EXPECT(h.host.joined == -1);
	// The double click selects and joins at once.
	h.double_click("LAN_GAME_LIST", 0);
	TEST_EXPECT(h.host.joined == 0 && h.host.refused.empty());
	// A row whose index names no listed session is refused with gameerr's CVUNABLETOCREATE.
	h.host.joinable = false;
	h.double_click("LAN_GAME_LIST", 0);
	TEST_EXPECT(h.host.refused == "MP Errors/CVUNABLETOCREATE");
	// The LAN screen shown again empties the list and drains the records again.
	h.host.calls.clear();
	TEST_EXPECT(h.runtime.show_screen("MULTI_PLAYER_HOST") && h.runtime.show_screen("LAN_MULTI_PLAYER"));
	TEST_EXPECT(h.host.called("drain_records") && h.runtime.item_count(list) == 0);
	TEST_EXPECT(h.controller.servers().empty() && h.controller.selected_server() == -1);
	return 0;
}

// LAN_SEARCH: the window's state and text; a search that does not start shows why.
int lan_search() {
	Harness h("LAN_MULTI_PLAYER");
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	const int search = h.id("LAN_SEARCH");
	h.press("LAN_SEARCH");
	TEST_EXPECT(h.host.called("start_search"));
	TEST_EXPECT(h.runtime.is_widget_disabled(search) && h.runtime.get_widget_text(search) == "<MP_STATUS_SEARCHING>");
	h.controller.browse_finished();
	TEST_EXPECT(!h.runtime.is_widget_disabled(search) && h.runtime.get_widget_text(search) == "<MP_SEARCH>");
	// A start the device refuses with its reason: the reason in the list, the button live.
	const int list = h.id("LAN_GAME_LIST");
	h.host.search_starts = false;
	h.host.search_error = "the socket did not bind";
	h.press("LAN_SEARCH");
	TEST_EXPECT(!h.runtime.is_widget_disabled(search));
	TEST_EXPECT(h.runtime.item_count(list) == 1 && h.runtime.item_text(list, 0) == "Search failed - the socket did not bind");
	// A silent refusal says the search did not start.
	h.host.search_error.clear();
	h.press("LAN_SEARCH");
	TEST_EXPECT(h.runtime.item_text(list, 0) == "Search failed - could not start the search");
	// A listed row clears the failure; the failure row is inert (nothing to join).
	h.host.joined = -1;
	h.double_click("LAN_GAME_LIST", 0);
	TEST_EXPECT(h.host.joined == -1);
	h.controller.servers_changed({server("alpha", "192.168.1.10", 32768)});
	TEST_EXPECT(h.controller.browse_error_text().empty() && h.runtime.item_text(list, 0) == "alpha (0/0)");
	// No session device: the search does nothing but show the list.
	Harness bare("LAN_MULTI_PLAYER");
	bare.host.session = false;
	bare.press("LAN_SEARCH");
	TEST_EXPECT(!bare.host.called("start_search") && !bare.runtime.is_widget_disabled(bare.id("LAN_SEARCH")));
	return 0;
}

// The host screen: the populate, the pool, ADD and REMOVE, the gate, the Switch cell.
int host_rotation() {
	Harness h("MULTI_PLAYER_HOST", false, {pool_row("alpha.bms"), pool_row("bravo.bms"), pool_row("training.bms", 0x10020)});
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	// The populate shows the cfg block.
	TEST_EXPECT(h.runtime.get_widget_text(h.id("GAME_NAME")) == h.host.block.cfg().game_name);
	TEST_EXPECT(h.runtime.get_widget_text(h.id("MAX_PLAYERS")) == std::to_string(h.host.block.cfg().mp_max_players));
	// Stock co-op is host-invisible; an empty rotation keeps START non-interactive and inert.
	const int list = h.id("MISSION_LIST");
	const int table = h.id("SELECTED_MISSIONS");
	TEST_EXPECT(h.runtime.item_count(list) == 2);
	TEST_EXPECT(h.runtime.is_widget_disabled(h.id("START_GAME")));
	h.press("START_GAME");
	TEST_EXPECT(h.host.requests.empty());
	// ADD moves the highlighted mission into the rotation: its Type cell the GateTypeAbbrev key (no
	// gametext), its Switch off for objective co-op; it leaves the list.
	h.add_first_mission();
	TEST_EXPECT(h.runtime.table_row_count(table) == 1);
	TEST_EXPECT(h.runtime.table_cell_text(table, 0, 0) == "alpha.bms" && h.runtime.table_cell_text(table, 0, 1) == "COOP" &&
	            h.runtime.table_cell_text(table, 0, 2) == "0");
	TEST_EXPECT(h.runtime.item_count(list) == 1 && !h.runtime.is_widget_disabled(h.id("START_GAME")));
	// The reseed clears the list's selection: a second ADD picks nothing.
	h.press("ADD_MISSIONS");
	TEST_EXPECT(h.runtime.table_row_count(table) == 1);
	// REMOVE drops the selected row and restores the mission to the list.
	h.runtime.table_select_row(table, 0, false);
	h.press("REMOVE_MISSIONS");
	TEST_EXPECT(h.runtime.table_row_count(table) == 0 && h.runtime.item_count(list) == 2);
	TEST_EXPECT(h.runtime.is_widget_disabled(h.id("START_GAME")));
	// The Switch cell: a team non-objective mission (team deathmatch) defaults on; a click toggles it.
	h.controller.seed_host_pool({pool_row("tdm.bms", 0x10000)});
	h.add_first_mission();
	TEST_EXPECT(h.runtime.table_cell_text(table, 0, 2) == "1");
	h.cell_click("SELECTED_MISSIONS", 0, 2);
	TEST_EXPECT(h.runtime.table_cell_text(table, 0, 2) == "0");
	h.cell_click("SELECTED_MISSIONS", 0, 1); // another column toggles nothing
	TEST_EXPECT(h.runtime.table_cell_text(table, 0, 2) == "0");
	return 0;
}

// The GAME_TYPE spin's filter and value.
int host_filter() {
	Harness h("MULTI_PLAYER_HOST", true, {pool_row("coop.bms", 0x30020), pool_row("tkoth.bms", 0x10001)});
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	// Stock co-op never lists; the spin's selected row (this menu authors no ALL row, so its first,
	// the TKOTH category 3, stays) filters the pool, and a pick of it filters again.
	const int list = h.id("MISSION_LIST");
	TEST_EXPECT(h.runtime.item_count(list) == 1 && h.runtime.item_text(list, 0) == "tkoth.bms");
	h.runtime.select_row(h.id("GAME_TYPE"), 0, true);
	TEST_EXPECT(h.runtime.item_count(list) == 1 && h.runtime.item_text(list, 0) == "tkoth.bms");
	// START relays the spin's value and reads SERVERTYPE's value into the block (dedicated).
	h.runtime.select_row(h.id("SERVERTYPE"), 1, true);
	h.add_first_mission();
	h.press("START_GAME");
	TEST_EXPECT(h.host.requests.size() == 1);
	if (h.host.requests.size() != 1) return 1;
	TEST_EXPECT(h.host.requests[0].game_type_attr == "3" && h.host.requests[0].mission == "tkoth.bms");
	TEST_EXPECT(h.host.block.cfg().dedicated == 1);
	return 0;
}

// START: HOSTMNX hidden, the read into the block, the LAN arm's save, the request.
int host_start() {
	Harness h("MULTI_PLAYER_HOST", false, {pool_row("alpha.bms"), pool_row("bravo.bms")});
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	h.host.expansion_name = "jox01";
	h.runtime.set_widget_text(h.id("GAME_NAME"), "CoopNight");
	h.runtime.set_widget_text(h.id("MAX_PLAYERS"), "6");
	h.runtime.set_widget_text(h.id("SPECTATOR_PW"), "watch");
	h.add_first_mission();
	h.add_first_mission();
	h.host.calls.clear();
	h.press("START_GAME");
	TEST_EXPECT(!h.runtime.is_widget_shown(h.id("HOSTMNX")));
	TEST_EXPECT(h.host.block.cfg().game_name == "CoopNight" && h.host.block.cfg().mp_max_players == 6);
	TEST_EXPECT(h.host.block.cfg().mp_host_spectator_password == "watch");
	TEST_EXPECT(h.host.calls.size() == 2 && h.host.calls[0] == "save_config" && h.host.calls[1] == "host_start");
	TEST_EXPECT(h.host.requests.size() == 1);
	if (h.host.requests.size() != 1) return 1;
	const LanHostRequest &request = h.host.requests[0];
	TEST_EXPECT(request.missions == std::vector<std::string>({"alpha.bms", "bravo.bms"}));
	TEST_EXPECT(request.launch_options == std::vector<int32_t>({0, 0}) && request.mission == "alpha.bms");
	TEST_EXPECT(request.game_type_attr.empty() && request.expansion == "jox01" && !request.novaworld);
	// A cleared edit is read verbatim.
	h.runtime.set_widget_text(h.id("GAME_NAME"), "");
	h.runtime.set_widget_text(h.id("MAX_PLAYERS"), "");
	h.press("START_GAME");
	TEST_EXPECT(h.host.block.cfg().game_name.empty() && h.host.block.cfg().mp_max_players == 0);
	return 0;
}

// The NovaWorld host: no save, the request on its channel; leaving the host screen, or the document,
// ends its setup.
int novaworld_host() {
	Harness h("MULTI_PLAYER_HOST", false, {pool_row("alpha.bms")});
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	h.controller.set_novaworld_host(true);
	h.add_first_mission();
	h.host.calls.clear();
	h.press("START_GAME");
	TEST_EXPECT(h.host.calls.size() == 1 && h.host.calls[0] == "host_start");
	TEST_EXPECT(h.host.requests.size() == 1 && h.host.requests[0].novaworld &&
	            h.host.requests[0].missions == std::vector<std::string>({"alpha.bms"}));
	TEST_EXPECT(h.runtime.show_screen("LAN_MULTI_PLAYER"));
	TEST_EXPECT(h.host.called("novaworld_host_cancelled") && !h.controller.novaworld_host());
	// The document's close ends a setup still running.
	h.host.calls.clear();
	h.controller.set_novaworld_host(true);
	h.controller.close(h.runtime);
	TEST_EXPECT(h.host.called("novaworld_host_cancelled") && !h.controller.novaworld_host() && !h.controller.is_open());
	// A LAN-channel close cancels nothing.
	Harness lan("MULTI_PLAYER_HOST");
	lan.controller.close(lan.runtime);
	TEST_EXPECT(!lan.host.called("novaworld_host_cancelled"));
	return 0;
}

// The title's own host screen and the screens' block writes: the LAN activation writes the network
// connect type 2 [orig: UI_InitLANMultiplayerScreen @0x5569E1]; a NovaWorld setup ends on leaving the
// title's host screen (dfx2.exe's NW_MULTI_PLAYER, where Jointops.exe's MULTI_PLAYER_HOST is just
// another screen); a set-every-word radio sets the class words and the START gate reads them
// [orig: UI_RegisterHostSettingsCallbacks @0x5580f0; HostDialog_AddRemoveSelectedMissions
// @0x557F24..0x557F43].
int host_title_and_words() {
	Harness h("MULTI_PLAYER_HOST", false, {pool_row("alpha.bms")});
	TEST_EXPECT(h.ok);
	if (!h.ok) return 1;
	h.host.block.cfg().networkconnecttype = 1;
	TEST_EXPECT(h.runtime.show_screen("LAN_MULTI_PLAYER"));
	TEST_EXPECT(h.host.block.cfg().networkconnecttype == 2);
	TEST_EXPECT(h.runtime.show_screen("MULTI_PLAYER_HOST"));
	// The class words gate START: CLASS_NEVER clears all ten, CLASS_ALWAYS sets them again.
	h.add_first_mission();
	TEST_EXPECT(!h.runtime.is_widget_disabled(h.id("START_GAME")));
	menu::MenuEvent radio;
	radio.kind = menu::MenuEvent::Kind::WidgetActivated;
	radio.text = "CLASS_NEVER";
	h.controller.on_event(h.runtime, radio);
	TEST_EXPECT(h.host.block.cfg().class_availability[5] == 0 && h.host.block.cfg().class_availability[9] == 0);
	TEST_EXPECT(h.runtime.is_widget_disabled(h.id("START_GAME")) && !h.controller.host_dialog().can_start());
	radio.text = "CLASS_ALWAYS";
	h.controller.on_event(h.runtime, radio);
	TEST_EXPECT(h.host.block.cfg().class_availability[5] == 1);
	TEST_EXPECT(!h.runtime.is_widget_disabled(h.id("START_GAME")));
	// DFX2: the host screen is NW_MULTI_PLAYER, so MULTI_PLAYER_HOST ends a NovaWorld setup.
	h.host.block.set_title(*title_by_code("dfx2"));
	TEST_EXPECT(h.runtime.show_screen("LAN_MULTI_PLAYER"));
	h.controller.set_novaworld_host(true);
	h.host.calls.clear();
	TEST_EXPECT(h.runtime.show_screen("MULTI_PLAYER_HOST"));
	TEST_EXPECT(h.host.called("novaworld_host_cancelled") && !h.controller.novaworld_host());
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(lan_screen() == 0);
	TEST_EXPECT(lan_search() == 0);
	TEST_EXPECT(host_rotation() == 0);
	TEST_EXPECT(host_filter() == 0);
	TEST_EXPECT(host_start() == 0);
	TEST_EXPECT(novaworld_host() == 0);
	TEST_EXPECT(host_title_and_words() == 0);
	std::printf("lan_multiplayer_menu OK\n");
	return 0;
}
