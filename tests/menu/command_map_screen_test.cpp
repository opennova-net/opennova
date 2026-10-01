// The CMAP screen's tables and ORDERS composer (menu/command_map_screen.h)
// over the menu runtime: the TEAMLIST full and incremental populates and
// their tab gates, the recruit box's hold, the recruit / join / squad-colour
// clicks, the TEAMLIST and PLAYERLIST custom draws, the PLAYERLIST populate and
// its mute and punt clicks, the ADDTO_* members, and the ORDERS combos,
// NEW_ORDER and the CURRENT_ORDERS edit and delete clicks.
// [orig: CMap_PopulateTeamList @0x547a50; CMap_PopulateTeamList_0 @0x548d80;
//  CMap_EntityWidgetHandler @0x548380; CMap_UpdatePlayerListUI @0x547e00;
//  CMap_HandlePlayerListCallback @0x5486b0;
//  CCommandMap_SendWeaponActionToTeammates @0x548cb0; CMap_OnOpenPopulate
//  @0x5492a0; CMap_BuildAndSendOrderCommand @0x5472d0;
//  CCommandMap_HandleOrderAction @0x548990]
// Witness record: docs/interface/hud-re.md "The windowed map views".

#include <runtime/menu/command_map_screen.h>

#include <cstdio>
#include <map>
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

// A frame seam that keeps the runtime's pushes: the combos' rows and
// selection, the authored rows' counts and display texts.
struct Frame : MenuFrameSeam {
	std::map<int, std::vector<std::string>> items;
	std::map<int, std::vector<std::string>> authored;
	std::map<int, int> selection;
	bool is_configured() const override { return true; }
	void configure_screen(const std::string &) override {}
	void screen_configured() override {}
	void set_widget_shown_override(int, bool) override {}
	void set_widget_disabled(int, bool) override {}
	void set_widget_checked(int, bool) override {}
	void set_widget_text(int, const std::string &) override {}
	void set_widget_items(int i, const std::vector<std::string> &v) override { items[i] = v; }
	void set_widget_selection(int i, int sel, int, int) override { selection[i] = sel; }
	void set_widget_scroll_range(int, int, int, int, int) override {}
	void set_widget_selected_set(int, const std::vector<int> &) override {}
	void set_widget_table_rows(int, const std::vector<MenuTableRow> &) override {}
	void set_widget_clip_rect(int, bool, int, int, int, int) override {}
	void set_widget_hover_item(int, int) override {}
	void set_widget_popup_open(int, bool) override {}
	void set_widget_focused(int, bool) override {}
	void set_widget_rect(int, int, int, int, int) override {}
	void set_widget_caret(int, int) override {}
	int get_widget_caret(int) const override { return -1; }
	std::string get_widget_text(int) const override { return std::string(); }
	int item_count(int i) const override {
		const auto it = authored.find(i);
		return it != authored.end() ? static_cast<int>(it->second.size()) : 0;
	}
	std::string item_display_text(int i, int row) const override {
		const std::map<int, std::vector<std::string>> &source = items.count(i) != 0 ? items : authored;
		const auto it = source.find(i);
		if (it == source.end() || row < 0 || row >= static_cast<int>(it->second.size()))
			return std::string();
		return it->second[static_cast<size_t>(row)];
	}
	bool is_widget_disabled(int) const override { return false; }
	MenuRectF widget_rect(int) const override { return {}; }
	void design_scale(float &sx, float &sy) const override { sx = sy = 1.0f; }
	int process_mouse(float, float, bool) override { return -1; }
	bool process_popup_mouse(int, float, float, bool) override { return false; }
	bool process_mouse_wheel(float, float, int) override { return false; }
	void set_cursor_state(bool, float, float) override {}
	void apply_claim_cursor() override {}
	void reset_cursor() override {}
	int combo_popup_row_at(int, float, float) const override { return -1; }
	bool combo_popup_contains(int, float, float) const override { return false; }
	int list_row_at(int, float, float) const override { return -1; }
	int spin_arrow_at(int, float, float) const override { return 0; }
	bool table_hit(int, float, float, int *r, int *c) const override {
		*r = -1;
		*c = -1;
		return false;
	}
	int hotkey_widget(const std::string &, bool) const override { return -1; }
	bool edit_char(int, int) override { return false; }
	int edit_key(int, int, bool) override { return 0; }
};

mnu::Window widget(const char *name, mnu::WindowType type) {
	mnu::Window w;
	w.name = name;
	w.type = type;
	return w;
}

mnu::Window table(const char *name, int count, bool multiselect) {
	mnu::Window w = widget(name, mnu::WindowType::Table);
	w.table_data.column.has_count = true;
	w.table_data.column.count = count;
	w.items.multiselect = multiselect;
	return w;
}

mnu::Document cmap_document() {
	mnu::Document doc;
	mnu::Screen s;
	s.name = "CMAP";
	s.root_window = widget("MAIN", mnu::WindowType::Window);
	s.root_window.children = {table("TEAMLIST", 10, true), table("PLAYERLIST", 7, true),
		table("CURRENT_ORDERS", 2, false), widget("GROUP", mnu::WindowType::Combo),
		widget("COMMAND_ORDER", mnu::WindowType::Combo), widget("LOCATION", mnu::WindowType::Combo),
		widget("DIRECTIONAL", mnu::WindowType::Combo), widget("WAITFOR", mnu::WindowType::Combo)};
	doc.screens = {s};
	return doc;
}

CommandMapPlayer player(uint8_t slot, uint8_t team, const char *name, uint8_t leader = 0xFF,
		uint8_t fireteam = 0) {
	CommandMapPlayer p;
	p.slot = slot;
	p.team = team;
	p.name = name;
	p.has_entity = true;
	p.player_class = 5;
	p.leader = leader;
	p.fireteam = fireteam;
	return p;
}

CommandMapText text() {
	CommandMapText t;
	t.gametext = [](const char *section, const char *key, const char *fallback) -> std::string {
		const std::string k = key;
		if (k == "STR_CMAP_FIRETEAMB") return "FT-B";
		if (k == "STROVR_MEDIC") return "medic";
		if (k == "STROVER_SPECTATOR_SUFFIX") return "(spec)";
		if (k == "STROVER_OBJECTIVEPOINT_SHORT") return "Obj %s";
		if (k == "STRWPNAME002") return "Bravo";
		(void)section;
		return fallback;
	};
	t.menu_ui = [](const char *, const char *key, const char *fallback) -> std::string {
		return std::string(key) == "CHARCLASS_MEDIC" ? "Medic" : std::string(fallback);
	};
	return t;
}

// Local slot 1 on team 1; slot 0 a free teammate, slot 2 led by the local
// slot in fireteam B, slot 3 the other team, slot 4 a spectator, slot 5
// without an entity.
CommandMapRoster roster() {
	CommandMapRoster r;
	r.local_slot = 1;
	r.local_team = 1;
	r.in_session = true;
	r.players = {player(0, 1, "free"), player(1, 1, "me"), player(2, 1, "mine", 1, 2),
		player(3, 2, "enemy")};
	CommandMapPlayer spec = player(4, 1, "spec");
	spec.spectator = true;
	r.players.push_back(spec);
	CommandMapPlayer ghost = player(5, 1, "ghost");
	ghost.has_entity = false;
	r.players.push_back(ghost);
	return r;
}

struct Canvas : MenuTableCellCanvas {
	std::vector<std::string> calls;
	void draw_cell(int row, int column, int flags) override {
		calls.push_back("d" + std::to_string(row) + "," + std::to_string(column) + "," +
				std::to_string(flags));
	}
	void clear_rect(uint32_t argb, float l, float t, float r, float b) override {
		char buf[96];
		std::snprintf(buf, sizeof(buf), "c%08X %g %g %g %g", argb, l, t, r, b);
		calls.push_back(buf);
	}
	void line(uint32_t, float, float, float, float) override { calls.push_back("l"); }
};

void test_team_list() {
	mnu::Document doc = cmap_document();
	Frame frame;
	MenuRuntime menu;
	menu.set_frame(&frame);
	CHECK(menu.open_document(&doc, "cmap.mnu", "CMAP"));
	const int t = menu.widget_id("TEAMLIST");
	const CommandMapRoster r = roster();
	const CommandMapTabGates gates = command_map_populate_team_list(menu, t, r, text());
	CHECK(gates.orders && gates.players && gates.team && gates.rules && gates.sets_rules);
	CHECK(menu.table_row_count(t) == 3);
	// The free teammate: recruitable and joinable, the click mask 3.
	CHECK(menu.table_row_value(t, 0) == 0 && menu.table_cell_text(t, 0, 0) == "1" &&
			menu.table_cell_text(t, 0, 1) == "0" && menu.table_cell_value(t, 0, 1) == 3);
	CHECK(menu.table_cell_text(t, 0, 2) == "free" && menu.table_cell_text(t, 0, 3) == "Medic");
	// The local row: neither box (no leader to leave).
	CHECK(menu.table_row_value(t, 1) == 1 && menu.table_cell_text(t, 1, 0) == "-" &&
			menu.table_cell_text(t, 1, 1) == "-" && menu.table_cell_value(t, 1, 1) == 0);
	// The led member: the leader's name and the fireteam label.
	CHECK(menu.table_cell_text(t, 2, 5) == "me" && menu.table_cell_text(t, 2, 6) == "FT-B" &&
			menu.table_cell_text(t, 2, 0) == "-");
	// The death screen closes TEAM / RULES; out of a session PLAYERS too.
	CommandMapRoster dead = r;
	dead.death_screen = true;
	const CommandMapTabGates g2 = command_map_populate_team_list(menu, t, dead, text());
	CHECK(g2.orders && !g2.team && !g2.rules && g2.players);
	command_map_populate_team_list(menu, t, r, text());

	// The first draw puts the recruitable box back to "0".
	command_map_team_list_timers(menu, t, 5000);
	CHECK(menu.table_cell_text(t, 0, 0) == "0" && menu.table_cell_text(t, 1, 0) == "-");
	// Recruit: the send, the box held checked for a second, the selection
	// write undone.
	menu.table_set_row_selected(t, 0, true);
	CommandMapTeamClick click = command_map_team_list_click(menu, t, 0, 0, kTableRowSelected, r, 5000);
	CHECK(click.send_recruit && !click.send_join && click.slot == 0);
	CHECK(menu.table_cell_text(t, 0, 0) == "1" && menu.table_cell_value(t, 0, 2) == 6000 &&
			menu.table_row_state(t, 0) == kTableRowDefault);
	command_map_team_list_timers(menu, t, 5999);
	CHECK(menu.table_cell_text(t, 0, 0) == "1");
	command_map_team_list_timers(menu, t, 6001);
	CHECK(menu.table_cell_text(t, 0, 0) == "0");
	// Join; the squad colour steps (+1) % 14; a name press keeps the table's
	// selection write.
	click = command_map_team_list_click(menu, t, 0, 1, kTableRowDefault, r, 7000);
	CHECK(click.send_join && click.slot == 0 && menu.table_row_state(t, 0) == kTableRowSelected);
	CommandMapRoster colored = r;
	colored.players[0].squad_color = 13;
	click = command_map_team_list_click(menu, t, 0, 4, kTableRowSelected, colored, 7000);
	CHECK(click.set_squad_color && click.squad_color == 0);
	menu.table_set_row_selected(t, 2, true);
	click = command_map_team_list_click(menu, t, 2, 2, kTableRowSelected, r, 7000);
	CHECK(!click.send_join && !click.send_recruit && menu.table_row_state(t, 2) == kTableRowSelected);
	// A locked-free press on the recruit box of a non-recruitable row sends nothing.
	click = command_map_team_list_click(menu, t, 1, 0, kTableRowDefault, r, 7000);
	CHECK(!click.send_recruit);

	// ADDTO_*: the selected rows the local slot leads.
	menu.table_set_row_selected(t, -1, false);
	menu.table_set_row_selected(t, 0, true);
	menu.table_set_row_selected(t, 2, true);
	CHECK(command_map_fireteam_members(menu, t, r) == (std::vector<uint8_t>{2}));

	// The incremental update: an existing row keeps its name, a new slot is
	// appended, a slot gone from the roster is removed.
	CommandMapRoster next = r;
	next.players[0].name = "renamed";
	next.players[0].leader = 1;
	next.players.erase(next.players.begin() + 2);
	next.players.push_back(player(6, 1, "new"));
	const CommandMapTabGates g3 = command_map_update_team_list(menu, t, next, text());
	CHECK(g3.orders && g3.team && !g3.sets_rules);
	CHECK(menu.table_row_count(t) == 3 && menu.table_cell_text(t, 0, 2) == "free" &&
			menu.table_cell_text(t, 0, 5) == "me" && menu.table_row_value(t, 2) == 6 &&
			menu.table_cell_text(t, 2, 2) == "new");
}

void test_paint() {
	CommandMapRoster r = roster();
	r.players[0].squad_color = 3;
	Canvas canvas;
	MenuTableCellEvent header;
	header.row = -1;
	header.column = 2;
	header.left = 10;
	header.top = 20;
	header.right = 60;
	header.bottom = 40;
	command_map_team_list_paint(header, canvas, 0, r);
	CHECK(canvas.calls.size() == 2 && canvas.calls[0] == "c00002040 10 20 60 40" &&
			canvas.calls[1] == "d-1,2,2");
	canvas.calls.clear();
	MenuTableCellEvent body = header;
	body.row = 0;
	body.column = 4;
	body.state = 2;
	command_map_team_list_paint(body, canvas, 0, r);
	CHECK(canvas.calls.size() == 7 && canvas.calls[0] == "d0,4,1" &&
			canvas.calls[1] == "cFFFFCC99 12 22 58 38" && canvas.calls[2] == "d0,4,2" &&
			canvas.calls[3] == "l");
	canvas.calls.clear();
	// No squad colour: the team colour (team 1 -> 0xFF304080; no slot -> 0xFF208020).
	command_map_team_list_paint(body, canvas, 1, roster());
	CHECK(canvas.calls[1] == "cFF304080 12 22 58 38");
	canvas.calls.clear();
	command_map_team_list_paint(body, canvas, 99, roster());
	CHECK(canvas.calls[1] == "cFF208020 12 22 58 38");
	canvas.calls.clear();
	body.column = 0;
	body.state = 0;
	command_map_player_list_paint(body, canvas);
	CHECK(canvas.calls.size() == 2 && canvas.calls[0] == "d0,0,1" && canvas.calls[1] == "d0,0,2");
}

void test_player_list() {
	mnu::Document doc = cmap_document();
	Frame frame;
	MenuRuntime menu;
	menu.set_frame(&frame);
	CHECK(menu.open_document(&doc, "cmap.mnu", "CMAP"));
	const int t = menu.widget_id("PLAYERLIST");
	CommandMapRoster r = roster();
	r.players[0].mute = 2;
	r.players[3].punt_mark = 3;
	command_map_populate_player_list(menu, t, r, text());
	// Every slot holding an entity, spectators included.
	CHECK(menu.table_row_count(t) == 5);
	CHECK(menu.table_cell_text(t, 0, 1) == "medic" && menu.table_cell_text(t, 0, 2) == "0" &&
			menu.table_cell_text(t, 0, 3) == "1" && menu.table_cell_text(t, 0, 4) == "0");
	CHECK(menu.table_cell_text(t, 3, 1) == "---" && menu.table_cell_text(t, 3, 4) == "1");
	CHECK(menu.table_cell_text(t, 4, 1) == "(spec)" && menu.table_cell_text(t, 4, 2) == "1");
	// Chat unmuted on another player, the box rewritten, the selection write undone.
	CommandMapPlayerClick click = command_map_player_list_click(menu, t, 0, 2, kTableRowSelected, r);
	CHECK(click.set_mute && click.mute == 0 && menu.table_cell_text(t, 0, 2) == "1");
	// The local player's own mute does not toggle.
	click = command_map_player_list_click(menu, t, 1, 3, kTableRowSelected, r);
	CHECK(!click.set_mute && menu.table_cell_text(t, 1, 3) == "1");
	// Punt: every box cleared, this one marked and sent.
	click = command_map_player_list_click(menu, t, 0, 4, kTableRowSelected, r);
	CHECK(click.clear_punt_marks && click.set_punt_mark && click.punt_mark == 0 && click.send_punt);
	CHECK(menu.table_cell_text(t, 0, 4) == "1" && menu.table_cell_text(t, 3, 4) == "0");
	// A second press on the voted player un-votes without a send.
	r.players[0].punt_mark = 0;
	click = command_map_player_list_click(menu, t, 0, 4, kTableRowSelected, r);
	CHECK(click.clear_punt_marks && !click.set_punt_mark && !click.send_punt &&
			menu.table_cell_text(t, 0, 4) == "0");
	// On the death screen the punt press does nothing but undo the selection.
	r.death_screen = true;
	click = command_map_player_list_click(menu, t, 3, 4, kTableRowSelected, r);
	CHECK(!click.clear_punt_marks && !click.send_punt);
}

void test_orders() {
	mnu::Document doc = cmap_document();
	Frame frame;
	MenuRuntime menu;
	menu.set_frame(&frame);
	CHECK(menu.open_document(&doc, "cmap.mnu", "CMAP"));
	CommandMapOrderWidgets w;
	w.group = menu.widget_id("GROUP");
	w.command_order = menu.widget_id("COMMAND_ORDER");
	w.location = menu.widget_id("LOCATION");
	w.directional = menu.widget_id("DIRECTIONAL");
	w.waitfor = menu.widget_id("WAITFOR");
	w.current_orders = menu.widget_id("CURRENT_ORDERS");
	frame.authored[menu.frame_index(w.command_order)] = {"Halt", "Attack", "Defend"};
	frame.authored[menu.frame_index(w.directional)] = {"None", "North"};
	frame.authored[menu.frame_index(w.waitfor)] = {"NONE", "UNIFORM"};
	CommandMapOrders store;
	CommandMapLocations locations;
	locations.user_waypoints = {"", "Rally"};
	locations.zone_indices = {1, -1};
	locations.location_names = {"Village"};
	const CommandMapRoster r = roster();
	command_map_populate_orders(menu, w, store, r, locations, text());
	// GROUP: fireteam B (first met), then the led member.
	CHECK(menu.get_widget_items(w.group) == (std::vector<std::string>{"FT-B", "mine"}));
	CHECK(store.group_values == (std::vector<int32_t>{0x10000002, 2}));
	CHECK(menu.get_widget_items(w.location) ==
			(std::vector<std::string>{"My Position", "Rally", "Obj Bravo", "Obj ", "Village", "mine"}));
	CHECK(store.location_values ==
			(std::vector<int32_t>{0x200000FF, 0x20000001, 0x10000001, -1, 0, 0x40000002}));
	CHECK(menu.selected_row(w.group) == 0 && menu.selected_row(w.command_order) == 0);

	// NEW_ORDER to fireteam B: attack, the rally point, north.
	menu.select_row(w.command_order, 1, false);
	menu.select_row(w.location, 1, false);
	menu.select_row(w.directional, 1, false);
	CommandMapOrderSend send = command_map_new_order(menu, w, store, r);
	CHECK(send.send && send.kind == 1 && send.text == "FT-B-Attack-Rally-North" &&
			send.targets == (std::vector<uint8_t>{2}));
	CHECK(store.orders.size() == 1 && store.orders[0].group == 0x10000002 &&
			store.orders[0].code == ((1u << 24) | (1u << 16) | (1u << 12)));
	const int co = w.current_orders;
	CHECK(menu.table_row_count(co) == 1 && menu.table_row_value(co, 0) == 0 &&
			menu.table_cell_text(co, 0, 0) == "0" && menu.table_cell_text(co, 0, 1) == send.text);
	// The same group again rewrites entry 0 in place.
	menu.select_row(w.directional, 0, false);
	menu.select_row(w.waitfor, 1, false);
	send = command_map_new_order(menu, w, store, r);
	CHECK(store.orders.size() == 1 && send.text == "FT-B-Attack-Rally-UNIFORM" &&
			store.orders[0].code == ((1u << 24) | (1u << 16) | (1u << 9)));
	CHECK(menu.table_cell_text(co, 0, 1) == "FT-B-Attack-Rally-UNIFORM");
	// A member order: kind 0 to that slot.
	menu.select_row(w.group, 1, false);
	send = command_map_new_order(menu, w, store, r);
	CHECK(send.kind == 0 && send.targets == (std::vector<uint8_t>{2}) && store.orders.size() == 2 &&
			(store.orders[1].code & 0x1FFu) == 1u);
	// Editing row 1 re-selects the combos from its code (GROUP from the
	// entry's own index).
	menu.select_row(w.group, 0, false);
	menu.select_row(w.command_order, 2, false);
	CommandMapOrderSend none =
			command_map_current_orders_click(menu, w, store, 1, 1, kTableRowSelected, 0, r);
	CHECK(!none.send && menu.selected_row(w.group) == 1 && menu.selected_row(w.command_order) == 1 &&
			menu.selected_row(w.waitfor) == 1);
	// Deleting row 0 cancels it (an empty order to its targets) and renumbers.
	send = command_map_current_orders_click(menu, w, store, 0, 0, kTableRowSelected, 0, r);
	CHECK(send.send && send.kind == 1 && send.text.empty() &&
			send.targets == (std::vector<uint8_t>{2}));
	CHECK(store.orders.size() == 1 && menu.table_row_count(co) == 1 &&
			menu.table_row_value(co, 0) == 0 && store.orders[0].group == 2);
	// The rebuild's re-seed lists the store.
	command_map_seed_current_orders(menu, co, store);
	CHECK(menu.table_row_count(co) == 1 && menu.table_cell_text(co, 0, 1) == store.orders[0].text);
}

} // namespace

int main() {
	test_team_list();
	test_paint();
	test_player_list();
	test_orders();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("command_map_screen_test: all checks passed\n");
	return 0;
}
