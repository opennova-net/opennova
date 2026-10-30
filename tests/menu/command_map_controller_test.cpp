// The CMAP screen's controller (menu/command_map_controller.h, hud-re D-HUD-19) headless: an
// authored cmap.mnu through the runtime's own driver over the state frame, the runtime's events
// handed to the controller as the shell hands a claimed controller its document's, over a fake host
// standing in for the map window, the tables' runner and the session.
//
// Pinned: the show [orig: CMap_OnScreenShow @0x54b320] checks RADIO_TAB_MAP and raises it (the MAP
// tab binds the delete button to MAP and clips it to MAP's screen rect), checks the toggle boxes
// from the view's bytes, populates TEAMLIST in full and applies its gates (RULES only from it),
// populates PLAYERLIST and writes RULELIST; every registered control runs its callback [orig:
// CCommandMap_RegisterAllControls @0x54ada0]: ZOOMIN / ZOOMOUT step, GRID stores its box and checks
// its ORDERS_ twin, CREATE_WAYPOINTS flips, the go codes and fireteams carry their user data, the
// chat send reads and clears CHAT_TEXT, the tabs rebind the delete button (ORDERS also populates
// its combos, TEAM runs the incremental update whose gates leave RULES alone, PLAYERS the player
// list), NEW_ORDER, CLEAR_WAYPOINTS, OK closes; the CREATE press's name dialog is centred on the
// click, pushed onto the bound map, shown with WPNAME focused, WPNAME's Enter presses WPNAME_OK,
// whose confirm clears the field and unchecks CREATE_WAYPOINTS; the bound delete button follows the
// render's corner and deletes the hovered waypoint; a table press reaches the tables' runner.
#include <formats/mnu/mnu.h>
#include <runtime/hud/hud_map_view.h>
#include <runtime/menu/command_map_controller.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/menu_state_frame.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

std::string wnd(const char *type, const char *name, int top,
		const std::string &inner = std::string(), const char *attrs = "", int left = 20,
		int width = 240, int height = 20) {
	char position[200];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>%d</LEFT><TOP>%d</TOP><RIGHT>%d</RIGHT><BOTTOM>%d</BOTTOM></POSITION>",
			left, top, left + width, top + height);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\"" + attrs + ">" +
			inner + position + "</WINDOW>\n";
}

std::string cmap() {
	std::string body = wnd("window", "MAP", 20,
			"<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>", "", 300, 480, 480);
	body += wnd("button", "ZOOMIN", 20) + wnd("button", "ZOOMOUT", 50);
	body += wnd("checkbox", "GRID", 80) + wnd("checkbox", "ORDERS_GRID", 110, "", " HIDDEN");
	body += wnd("checkbox", "TEXT", 140) + wnd("checkbox", "CREATE_WAYPOINTS", 170);
	body += wnd("button", "OK", 200) + wnd("radio", "RADIO_TAB_MAP", 230);
	for (const char *tab :
			{ "RADIO_TAB_ORDERS", "RADIO_TAB_PLAYERS", "RADIO_TAB_TEAM", "RADIO_TAB_RULES" })
		body += wnd("radio", tab, 260);
	body += wnd("button", "CLEAR_WAYPOINTS", 290) + wnd("button", "GOCODE_XRAY", 320);
	body += wnd("edit", "CHAT_TEXT", 350) + wnd("button", "CHAT_ALL", 380);
	body += wnd("button", "ADDTO_FIRETEAM_B", 410) + wnd("button", "NEW_ORDER", 440);
	body += wnd("multiline_edit", "RULELIST", 470);
	const std::string hide = "<ACTION type=\"window\" state=\"HIDE\">WAYPOINTNAME_DLG</ACTION>";
	const std::string dialog = wnd("edit", "WPNAME", 0) + wnd("button", "WPNAME_CANCEL", 0, hide) +
			wnd("button", "WPNAME_OK", 0, hide);
	body += wnd("window", "WAYPOINTNAME_DLG", 520, dialog, " HIDDEN");
	body += wnd("button", "USERWP_CLOSE", 550, "", " HIDDEN", 20, 16, 16);
	body += wnd("table", "TEAMLIST", 570,
			"<COLUMN count=\"10\"><HEADER column=\"0\" width=\"50\">R</HEADER></COLUMN>");
	return "<SCREEN><NAME>CMAP</NAME><WINDOW type=\"window\" "
		   "name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP>"
		   "<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\n" +
			body + "</WINDOW></SCREEN>\n";
}

struct Host : CommandMapHost {
	bool map = true;
	bool toggles[4] = { true, true, true, false };
	float zoom_steps = 0;
	int32_t click_x = -1, click_y = -1;
	std::vector<std::string> placed;
	int deleted = 0, cleared = 0;
	bool close_bound = false;
	int close_width = 0;
	bool close_shown = false;
	int32_t close_x = 0, close_y = 0;
	std::vector<std::string> calls;
	CommandMapTabGates full, update;
	bool refresh_due = false;
	std::vector<int> go_codes, fireteams;
	std::vector<std::pair<bool, std::string>> chats;
	std::string rules = "Rules";
	int closes = 0;
	std::string clicked_table;
	int clicked_row = -1, clicked_column = -1;

	bool has_map() const override { return map; }
	bool toggle(int t) const override { return toggles[t]; }
	void toggle_changed(int t, bool checked) override {
		toggles[t] = t == hud::kCommandToggleCreateWaypoints ? !toggles[t] : checked;
	}
	void clear_create_waypoints() override { toggles[hud::kCommandToggleCreateWaypoints] = false; }
	void zoom(int direction) override { zoom_steps += direction; }
	void store_waypoint_click(int32_t x, int32_t y) override {
		click_x = x;
		click_y = y;
	}
	bool confirm_waypoint(const std::string &name) override {
		placed.push_back(name);
		return true;
	}
	bool delete_hovered_waypoint() override {
		++deleted;
		return true;
	}
	void clear_waypoints() override { ++cleared; }
	void bind_close_button(bool bound, int width) override {
		close_bound = bound;
		close_width = width;
	}
	bool close_button_shown() const override { return close_shown; }
	void close_button_position(int32_t &x, int32_t &y) const override {
		x = close_x;
		y = close_y;
	}
	// The frame's own reading: the absolute rect less the parent's (every window here under MAIN at
	// the origin, the dialog's children under the dialog).
	MenuStateFrame *frame = nullptr;
	bool local_rect(const MenuRuntime &menu, int id, hud::MapViewRect &out) const override {
		mnu::RectEdges edges{};
		const int index = menu.frame_index(id);
		if (index < 0 || !frame->compiler().widget_local_rect(index, frame->state(), &edges))
			return false;
		out = { edges.left, edges.top, edges.right, edges.bottom };
		return true;
	}
	CommandMapTabGates populate_team_list(bool is_full) override {
		calls.push_back(is_full ? "team_full" : "team_update");
		return is_full ? full : update;
	}
	void populate_player_list() override { calls.push_back("players"); }
	void populate_orders() override { calls.push_back("orders"); }
	void new_order() override { calls.push_back("new_order"); }
	void assign_fireteam(int fireteam) override { fireteams.push_back(fireteam); }
	void table_cell_clicked(
			const std::string &table, int row, int column, int32_t, int32_t) override {
		clicked_table = table;
		clicked_row = row;
		clicked_column = column;
	}
	bool refresh(CommandMapTabGates &gates) override {
		if (!refresh_due)
			return false;
		gates = update;
		refresh_due = false;
		return true;
	}
	void send_go_code(int code) override { go_codes.push_back(code); }
	void send_chat(bool all, const std::string &text) override { chats.push_back({ all, text }); }
	std::string rules_text() override { return rules; }
	void close() override { ++closes; }
};

struct Run {
	mnu::Document doc;
	MenuStateFrame frame;
	MenuRuntime rt;
	CommandMapController controller;
	Host host;
	bool claimed = false;

	bool open() {
		std::string error;
		if (!mnu::parse(cmap(), doc, error))
			return false;
		frame.set_configure([this](const std::string &screen, MenuFrameCompiler &compiler) {
			const mnu::Screen *found =
					screen.empty() ? doc.first_screen() : doc.find_screen(screen);
			compiler.configure(found);
			return found != nullptr;
		});
		frame.set_clicked([this](int index, int part) { rt.on_widget_clicked(index, part); });
		rt.set_frame(&frame);
		// The shell's hand-off: the claimed controller hears the document's events.
		rt.set_sink([this](const MenuEvent &e) {
			if (claimed)
				controller.on_event(rt, e);
		});
		host.frame = &frame;
		controller.set_host(&host);
		if (!rt.open_document(&doc, "cmap.mnu", std::string()))
			return false;
		claimed = true;
		controller.open(rt, "cmap.mnu", rt.current_screen());
		return true;
	}
	int id(const char *name) const { return rt.widget_id(name); }
	void activate(const char *name) { rt.activate(id(name)); }
};

bool clipped_to(const MenuRuntime &rt, const MenuStateFrame &frame, int id, int left, int top,
		int right, int bottom) {
	const MenuWidgetState *row = find_frame_widget(frame.state(), rt.frame_index(id));
	return row && row->has_clip && row->clip.left == left && row->clip.top == top &&
			row->clip.right == right && row->clip.bottom == bottom;
}

void test_the_show() {
	Run run;
	run.host.toggles[hud::kCommandToggleText] = false;
	run.host.full.orders = true;
	run.host.full.players = false;
	run.host.full.team = true;
	run.host.full.rules = false;
	run.host.full.sets_rules = true;
	CHECK(run.open());
	CHECK(run.rt.is_widget_checked(run.id("RADIO_TAB_MAP")));
	// The MAP tab's callback: the delete button bound to MAP, hidden, clipped to MAP's screen rect.
	CHECK(run.controller.bound_map() == "MAP" && run.controller.delete_bound() &&
			!run.controller.delete_shown());
	CHECK(run.host.close_bound && run.host.close_width == 16);
	CHECK(clipped_to(run.rt, run.frame, run.id("USERWP_CLOSE"), 300, 20, 780, 500));
	// The toggle boxes from the view's bytes.
	CHECK(run.rt.is_widget_checked(run.id("GRID")) &&
			run.rt.is_widget_checked(run.id("ORDERS_GRID")));
	CHECK(!run.rt.is_widget_checked(run.id("TEXT")));
	// The full populate's gates, RULES among them; then the player list; RULELIST written.
	CHECK(run.host.calls.size() == 2 && run.host.calls[0] == "team_full" &&
			run.host.calls[1] == "players");
	CHECK(!run.rt.is_widget_disabled(run.id("RADIO_TAB_ORDERS")));
	CHECK(run.rt.is_widget_disabled(run.id("RADIO_TAB_PLAYERS")));
	CHECK(!run.rt.is_widget_disabled(run.id("RADIO_TAB_TEAM")));
	CHECK(run.rt.is_widget_disabled(run.id("RADIO_TAB_RULES")));
	CHECK(run.rt.get_widget_text(run.id("RULELIST")) == "Rules");
	// Nothing built leaves the control's text.
	Run empty;
	empty.host.rules.clear();
	CHECK(empty.open() && empty.rt.get_widget_text(empty.id("RULELIST")).empty());
}

void test_the_controls() {
	Run run;
	CHECK(run.open());
	run.activate("ZOOMIN");
	run.activate("ZOOMOUT");
	run.activate("ZOOMIN");
	CHECK(run.host.zoom_steps == 1);
	// GRID's click clears the box that opened checked: the byte takes it, the twin follows.
	run.activate("GRID");
	CHECK(!run.rt.is_widget_checked(run.id("GRID")) && !run.host.toggles[hud::kCommandToggleGrid]);
	CHECK(!run.rt.is_widget_checked(run.id("ORDERS_GRID")));
	run.activate("CREATE_WAYPOINTS");
	CHECK(run.host.toggles[hud::kCommandToggleCreateWaypoints]);
	run.activate("GOCODE_XRAY");
	CHECK(run.host.go_codes.size() == 1 && run.host.go_codes[0] == 3);
	run.activate("ADDTO_FIRETEAM_B");
	CHECK(run.host.fireteams.size() == 1 && run.host.fireteams[0] == 2);
	run.rt.set_widget_text(run.id("CHAT_TEXT"), "on me");
	run.activate("CHAT_ALL");
	CHECK(run.host.chats.size() == 1 && run.host.chats[0].first &&
			run.host.chats[0].second == "on me");
	CHECK(run.rt.get_widget_text(run.id("CHAT_TEXT")).empty());
	run.activate("NEW_ORDER");
	run.activate("CLEAR_WAYPOINTS");
	CHECK(run.host.calls.back() == "new_order" && run.host.cleared == 1);
	// A table press after its selection write reaches the tables' runner.
	MenuEvent press;
	press.kind = MenuEvent::Kind::TableCellClicked;
	press.id = run.id("TEAMLIST");
	press.text = "TEAMLIST";
	press.value = 2;
	press.column = 1;
	run.controller.on_event(run.rt, press);
	CHECK(run.host.clicked_table == "TEAMLIST" && run.host.clicked_row == 2 &&
			run.host.clicked_column == 1);
	run.activate("OK");
	CHECK(run.host.closes == 1);
	// Without a map window the map's own controls do nothing.
	Run bare;
	bare.host.map = false;
	CHECK(bare.open());
	bare.activate("ZOOMIN");
	CHECK(bare.host.zoom_steps == 0);
}

void test_the_tabs_rebind_the_delete_button() {
	Run run;
	run.host.update.orders = false;
	run.host.update.players = true;
	run.host.update.team = true;
	run.host.update.rules = true;
	run.host.update.sets_rules = false;
	CHECK(run.open());
	const bool rules_disabled = run.rt.is_widget_disabled(run.id("RADIO_TAB_RULES"));
	run.activate("RADIO_TAB_TEAM");
	CHECK(!run.controller.delete_bound() && run.controller.bound_map() == "MAP");
	CHECK(run.host.calls.back() == "team_update" && !run.host.close_bound);
	CHECK(run.rt.is_widget_disabled(run.id("RADIO_TAB_ORDERS")));
	CHECK(run.rt.is_widget_disabled(run.id("RADIO_TAB_RULES")) == rules_disabled);
	run.activate("RADIO_TAB_PLAYERS");
	CHECK(run.host.calls.back() == "players" && !run.controller.delete_bound());
	run.activate("RADIO_TAB_ORDERS");
	CHECK(run.host.calls.back() == "orders" && run.controller.delete_bound() &&
			run.controller.bound_map() == "ORDERS_MAP");
	run.activate("RADIO_TAB_RULES");
	CHECK(!run.controller.delete_bound());
	run.activate("RADIO_TAB_MAP");
	CHECK(run.controller.delete_bound() && run.controller.bound_map() == "MAP");
	// A squad fold's refresh applies its gates.
	run.host.update.orders = true;
	run.host.refresh_due = true;
	run.controller.frame(run.rt);
	CHECK(!run.rt.is_widget_disabled(run.id("RADIO_TAB_ORDERS")));
}

void test_the_waypoint_legs() {
	Run run;
	CHECK(run.open());
	run.activate("CREATE_WAYPOINTS");
	// (400 - 120, 200 - 10) pushed right onto MAP's left edge 300.
	run.controller.waypoint_dialog_requested(run.rt, 400, 200);
	const int dialog = run.id("WAYPOINTNAME_DLG");
	hud::MapViewRect placed;
	CHECK(run.rt.is_widget_shown(dialog) && run.host.local_rect(run.rt, dialog, placed));
	CHECK(placed.left == 300 && placed.top == 190 && placed.right == 540 && placed.bottom == 210);
	CHECK(run.rt.focused_widget() == run.id("WPNAME"));
	CHECK(run.host.click_x == 400 && run.host.click_y == 200);
	run.rt.set_widget_text(run.id("WPNAME"), "RALLY");
	// WPNAME's Enter commits the edit, which presses WPNAME_OK.
	MenuKeyInput enter;
	enter.vk = 0x0D;
	CHECK(run.rt.handle_key(enter));
	CHECK(run.host.placed.size() == 1 && run.host.placed[0] == "RALLY");
	CHECK(run.rt.get_widget_text(run.id("WPNAME")).empty());
	CHECK(!run.rt.is_widget_checked(run.id("CREATE_WAYPOINTS")) &&
			!run.host.toggles[hud::kCommandToggleCreateWaypoints]);
	CHECK(!run.rt.is_widget_shown(dialog));
	// The bound delete button takes the render's corner at its square size, then deletes and hides.
	run.host.close_shown = true;
	run.host.close_x = 410;
	run.host.close_y = 130;
	run.controller.frame(run.rt);
	const int del = run.id("USERWP_CLOSE");
	CHECK(run.controller.delete_shown() && run.rt.is_widget_shown(del));
	hud::MapViewRect at;
	CHECK(run.host.local_rect(run.rt, del, at) && at.left == 410 && at.top == 130 &&
			at.right == 426 && at.bottom == 146);
	run.activate("USERWP_CLOSE");
	CHECK(run.host.deleted == 1 && !run.controller.delete_shown() && !run.rt.is_widget_shown(del));
	// Unbound, the press deletes nothing.
	run.activate("RADIO_TAB_RULES");
	run.activate("USERWP_CLOSE");
	CHECK(run.host.deleted == 1);
	// WPNAME_CANCEL unchecks CREATE_WAYPOINTS and clears its byte.
	run.activate("CREATE_WAYPOINTS");
	CHECK(run.host.toggles[hud::kCommandToggleCreateWaypoints]);
	run.activate("WPNAME_CANCEL");
	CHECK(!run.rt.is_widget_checked(run.id("CREATE_WAYPOINTS")) &&
			!run.host.toggles[hud::kCommandToggleCreateWaypoints]);
}

} // namespace

int main() {
	test_the_show();
	test_the_controls();
	test_the_tabs_rebind_the_delete_button();
	test_the_waypoint_legs();
	if (failures)
		return 1;
	std::printf("command_map_controller OK\n");
	return 0;
}
