#include <runtime/menu/command_map_controller.h>

#include <base/io/strutil.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::menu {

namespace {

// The two custom-draw map controls (the MAP tab's and the ORDERS tab's), each registered with
// CMapWindow_HandleEvent [orig: CCommandMap_RegisterAllControls @0x54ae52 / @0x54b0b0].
const char *const kMap = "MAP";
const char *const kOrdersMap = "ORDERS_MAP";
const char *const kDelete = "USERWP_CLOSE";
const char *const kDialog = "WAYPOINTNAME_DLG";
const char *const kName = "WPNAME";
const char *const kNameOk = "WPNAME_OK";
const char *const kCreate = "CREATE_WAYPOINTS";
const char *const kChatText = "CHAT_TEXT";

// The draw toggles and their twins: each handler writes its byte from the box and checks the twin
// from the byte; user data 0 the MAP tab's box, 1 the ORDERS tab's [orig: CMap_OnGridCheckbox
// @0x547ff0 (registered @0x54aeac / @0x54b10a), CMap_OnWaypointsCheckbox @0x548080 (@0x54aeca /
// @0x54b128), CMap_OnTextCheckbox @0x5481a0 (@0x54aee8 / @0x54b146)].
struct ToggleRow {
	const char *box;
	const char *twin;
	int toggle;
};
constexpr ToggleRow kToggles[] = {
	{ "GRID", "ORDERS_GRID", hud::kCommandToggleGrid },
	{ "ORDERS_GRID", "GRID", hud::kCommandToggleGrid },
	{ "WAYPOINTS", "ORDERS_WAYPOINTS", hud::kCommandToggleWaypoints },
	{ "ORDERS_WAYPOINTS", "WAYPOINTS", hud::kCommandToggleWaypoints },
	{ "TEXT", "ORDERS_TEXT", hud::kCommandToggleText },
	{ "ORDERS_TEXT", "TEXT", hud::kCommandToggleText },
};

// The go-code buttons, codes 0..5 [orig: CMap_OnGoCodeButton @0x548290, registered
// @0x54b185..0x54b21b with user data 0..5].
const char *const kGoCodes[] = { "GOCODE_UNIFORM", "GOCODE_VICTOR", "GOCODE_WHISKEY", "GOCODE_XRAY",
	"GOCODE_YANKEE", "GOCODE_ZULU" };
// The fireteam buttons, fireteams 0 (none) .. 3 (C) [orig: CMap_SendFireteamAssign @0x548cb0,
// registered @0x54af63..0x54afbd with user data 0..3].
const char *const kFireteams[] = { "ADDTO_NO_FIRETEAM", "ADDTO_FIRETEAM_A", "ADDTO_FIRETEAM_B",
	"ADDTO_FIRETEAM_C" };

// The tab radios whose interactive state a team-list populate decides.
struct GateRow {
	const char *radio;
	bool CommandMapTabGates::*gate;
};
constexpr GateRow kGates[] = {
	{ "RADIO_TAB_ORDERS", &CommandMapTabGates::orders },
	{ "RADIO_TAB_PLAYERS", &CommandMapTabGates::players },
	{ "RADIO_TAB_TEAM", &CommandMapTabGates::team },
	{ "RADIO_TAB_RULES", &CommandMapTabGates::rules },
};

template <size_t N> int index_of(const char *const (&names)[N], const std::string &name) {
	for (size_t i = 0; i < N; ++i)
		if (strutil::iequals(name, names[i]))
			return static_cast<int>(i);
	return -1;
}

// A widget's absolute design rect, truncated per member as the frame's Rect2i takes it; false off
// the screen.
bool absolute_rect(const MenuRuntime &menu, int id, int &left, int &top, int &right, int &bottom) {
	const int index = id >= 0 ? menu.frame_index(id) : -1;
	if (index < 0 || !menu.frame())
		return false;
	const MenuRectF rect = menu.frame()->widget_rect(index);
	left = static_cast<int>(rect.x);
	top = static_cast<int>(rect.y);
	right = left + static_cast<int>(rect.w);
	bottom = top + static_cast<int>(rect.h);
	return true;
}

} // namespace

void CommandMapController::open(MenuRuntime &menu, const std::string &, const std::string &) {
	if (!host_)
		return;
	// [orig: CMap_OnScreenShow @0x54b320 — RADIO_TAB_MAP's vtable+0x64(1) @0x54b342: checked, its
	//  event raised (CMap_OnRadioTabMap binds and clips the delete button to MAP)]
	const int tab = menu.widget_id("RADIO_TAB_MAP");
	if (tab >= 0) {
		menu.set_widget_checked(tab, true);
		menu.activate(tab);
	}
	// [orig: the toggle boxes from the view's bytes @0x54b344..0x54b44c (CREATE_WAYPOINTS keeps its
	//  own)]
	sync_toggle_checks_(menu);
	// [orig: USERWP_CLOSE and MAP into the binding globals @0x54b44c..0x54b47b]
	bind_delete_(menu, kMap);
	// [orig: CMap_PopulateTeamList @0x54b480, CMap_UpdatePlayerListUI @0x54b485]
	apply_gates_(menu, host_->populate_team_list(true));
	host_->populate_player_list();
	// [orig: Overlay_BuildEndGameStatsText(RULELIST) @0x54b4a0 — nothing built leaves the control's
	//  text]
	const int rules = menu.widget_id("RULELIST");
	if (rules >= 0) {
		const std::string text = host_->rules_text();
		if (!text.empty())
			menu.set_widget_text(rules, text);
	}
}

void CommandMapController::close(MenuRuntime &) {}

void CommandMapController::on_event(MenuRuntime &menu, const MenuEvent &event) {
	if (!host_)
		return;
	switch (event.kind) {
		case MenuEvent::Kind::WidgetActivated:
			on_activated_(menu, event.text);
			return;
		case MenuEvent::Kind::EditCommitted:
			// WPNAME's Enter presses WPNAME_OK [orig: CMap_OnWaypointNameCommit @0x547950 — event
			// 0x7000002 @0x547958, CWnd_SetHotkeyPressed(WPNAME_OK) @0x547975; registered
			// @0x54adf8].
			if (strutil::iequals(event.text, kName)) {
				const int ok = menu.widget_id(kNameOk);
				if (ok >= 0)
					menu.activate(ok);
			}
			return;
		case MenuEvent::Kind::TableCellClicked:
			// TEAMLIST, PLAYERLIST and CURRENT_ORDERS after the table's own selection write
			// [orig: CMap_EntityWidgetHandler @0x548380 (@0x54af45), CMap_HandlePlayerListCallback
			//  @0x5486b0 (@0x54afde), CCommandMap_HandleOrderAction @0x548990 (@0x54b167)].
			host_->table_cell_clicked(
					event.text, event.value, event.column, event.state, event.cell_value);
			return;
		default:
			return;
	}
}

void CommandMapController::on_activated_(MenuRuntime &menu, const std::string &name) {
	// [orig: CMap_OnOk @0x547980 (registered @0x54adb9): Game_CloseInGameScreens]
	if (strutil::iequals(name, "OK")) {
		host_->close();
		return;
	}
	// [orig: CMap_HandleChatSubmit @0x5482e0, user data 0 CHAT_TEAM (@0x54b257) / 1 CHAT_ALL
	//  (@0x54b27a)]
	if (strutil::iequals(name, "CHAT_TEAM") || strutil::iequals(name, "CHAT_ALL")) {
		submit_chat_(menu, strutil::iequals(name, "CHAT_ALL"));
		return;
	}
	const int code = index_of(kGoCodes, name);
	if (code >= 0) {
		host_->send_go_code(code);
		return;
	}
	// The tab radios rebind the delete button [orig: CMap_OnRadioTabMap @0x547850 (@0x54affc) and
	// CMap_OnOpenPopulate @0x5492a0 (@0x54b038) to their map control with CWnd_SetClipRect to its
	// screen rect @0x5478c6 / @0x54933b; CMap_OnRadioTabTeam @0x549240 (@0x54b01a),
	// CMap_OnRadioTabPlayers @0x549270 (@0x54b056) and CMap_OnRadioTabRules @0x5478e0 (@0x54b074)
	// unbind it].
	if (strutil::iequals(name, "RADIO_TAB_MAP")) {
		bind_delete_(menu, kMap);
		clip_delete_(menu, kMap);
		return;
	}
	if (strutil::iequals(name, "RADIO_TAB_ORDERS")) {
		bind_delete_(menu, kOrdersMap);
		clip_delete_(menu, kOrdersMap);
		host_->populate_orders();
		return;
	}
	if (strutil::iequals(name, "RADIO_TAB_TEAM")) {
		bind_delete_(menu, std::string());
		// [orig: CMap_PopulateTeamList_0 @0x549265]
		apply_gates_(menu, host_->populate_team_list(false));
		return;
	}
	if (strutil::iequals(name, "RADIO_TAB_PLAYERS")) {
		bind_delete_(menu, std::string());
		// [orig: CMap_UpdatePlayerListUI @0x549295]
		host_->populate_player_list();
		return;
	}
	if (strutil::iequals(name, "RADIO_TAB_RULES")) {
		bind_delete_(menu, std::string());
		return;
	}
	const int fireteam = index_of(kFireteams, name);
	if (fireteam >= 0) {
		host_->assign_fireteam(fireteam);
		return;
	}
	// [orig: CMap_BuildAndSendOrderCommand @0x5472d0 (registered @0x54b092)]
	if (strutil::iequals(name, "NEW_ORDER")) {
		host_->new_order();
		return;
	}
	if (!host_->has_map())
		return;
	// [orig: CMap_ZoomStep @0x548230, user data 1 ZOOMIN / ORDERS_ZOOMIN (@0x54ae70 / @0x54b0ce),
	// -1
	//  ZOOMOUT / ORDERS_ZOOMOUT (@0x54ae8e / @0x54b0ec)]
	if (strutil::iequals(name, "ZOOMIN") || strutil::iequals(name, "ORDERS_ZOOMIN")) {
		host_->zoom(1);
		return;
	}
	if (strutil::iequals(name, "ZOOMOUT") || strutil::iequals(name, "ORDERS_ZOOMOUT")) {
		host_->zoom(-1);
		return;
	}
	if (strutil::iequals(name, kCreate)) {
		// The CREATE_WAYPOINTS byte flips [orig: CMap_OnCreateWaypointsCheckbox @0x548180
		//  (registered @0x54af24) — byte_252DD63 = !byte_252DD63 @0x548194].
		const int id = menu.widget_id(kCreate);
		host_->toggle_changed(
				hud::kCommandToggleCreateWaypoints, id >= 0 && menu.is_widget_checked(id));
		return;
	}
	for (const ToggleRow &row : kToggles)
		if (strutil::iequals(name, row.box)) {
			toggle_changed_(menu, row.box, menu.widget_id(row.box));
			return;
		}
	// [orig: CMap_HandleWaypointCreateConfirm @0x54a0d0 (registered @0x54ae16)]
	if (strutil::iequals(name, kNameOk)) {
		confirm_waypoint_(menu);
		return;
	}
	// [orig: CMap_HandleWaypointCreateCancel @0x547910 (registered @0x54ae34): the box
	//  unchecked @0x547937 and its byte cleared @0x54793c]
	if (strutil::iequals(name, "WPNAME_CANCEL")) {
		uncheck_create_waypoints_(menu);
		return;
	}
	// The hovered waypoint deleted while the button is bound, the button hidden [orig:
	// CMap_OnUserWaypointClose @0x547990 (registered @0x54add7) — the bound test @0x5479c9, the
	// hide
	// @0x547a3d].
	if (strutil::iequals(name, kDelete)) {
		if (delete_bound_) {
			host_->delete_hovered_waypoint();
			set_delete_shown_(menu, false);
		}
		return;
	}
	// [orig: CMap_OnClearWaypoints @0x548110 (registered @0x54af06)]
	if (strutil::iequals(name, "CLEAR_WAYPOINTS"))
		host_->clear_waypoints();
}

void CommandMapController::toggle_changed_(MenuRuntime &menu, const std::string &name, int id) {
	for (const ToggleRow &row : kToggles) {
		if (!strutil::iequals(name, row.box))
			continue;
		host_->toggle_changed(row.toggle, id >= 0 && menu.is_widget_checked(id));
		const int twin = menu.widget_id(row.twin);
		if (twin >= 0)
			menu.set_widget_checked(twin, host_->toggle(row.toggle));
		return;
	}
}

void CommandMapController::sync_toggle_checks_(MenuRuntime &menu) const {
	if (!host_->has_map())
		return;
	for (const ToggleRow &row : kToggles) {
		const int id = menu.widget_id(row.box);
		if (id >= 0)
			menu.set_widget_checked(id, host_->toggle(row.toggle));
	}
}

void CommandMapController::apply_gates_(MenuRuntime &menu, const CommandMapTabGates &gates) const {
	// RULES only from the show's full populate (CMap_PopulateTeamList @0x547a50; its incremental
	// update leaves it alone).
	for (const GateRow &row : kGates) {
		if (row.gate == &CommandMapTabGates::rules && !gates.sets_rules)
			continue;
		const int id = menu.widget_id(row.radio);
		if (id >= 0)
			menu.set_widget_disabled(id, !(gates.*row.gate));
	}
}

int CommandMapController::delete_width_(const MenuRuntime &menu) const {
	hud::MapViewRect rect;
	const int id = menu.widget_id(kDelete);
	if (id < 0 || !host_->local_rect(menu, id, rect))
		return 0;
	return rect.right - rect.left;
}

// A tab radio's (or the show's) binding: the delete button hidden and bound to `map`, or unbound
// for
// "" (the map control kept for the name dialog).
void CommandMapController::bind_delete_(MenuRuntime &menu, const std::string &map) {
	delete_shown_ = true;
	set_delete_shown_(menu, false);
	delete_bound_ = !map.empty() && menu.widget_id(kDelete) >= 0;
	if (!map.empty())
		bound_map_ = map;
	if (host_->has_map())
		host_->bind_close_button(delete_bound_, delete_width_(menu));
}

// USERWP_CLOSE clipped to the map control's screen rect (CWnd_GetRect + the ancestors' offsets).
void CommandMapController::clip_delete_(MenuRuntime &menu, const std::string &map) const {
	const int id = menu.widget_id(kDelete);
	int left = 0, top = 0, right = 0, bottom = 0;
	if (id < 0 || !absolute_rect(menu, menu.widget_id(map), left, top, right, bottom))
		return;
	menu.set_widget_clip_rect(id, true, left, top, right, bottom);
}

void CommandMapController::set_delete_shown_(MenuRuntime &menu, bool shown) {
	const int id = menu.widget_id(kDelete);
	if (id < 0 || shown == delete_shown_)
		return;
	delete_shown_ = shown;
	menu.set_widget_shown(id, shown);
}

// The render leg's delete button: while bound it takes the corner the last render reported beside
// the first hovered placed waypoint, at its own square size, and shows, or hides with none hovered
// [orig: CMapWindow_HandleEvent @0x549b68..0x549c23 — CWnd_MoveToSquare, CWnd_SetShown].
void CommandMapController::place_delete_(MenuRuntime &menu) {
	const int id = menu.widget_id(kDelete);
	if (!host_->has_map() || id < 0)
		return;
	const int width = delete_width_(menu);
	host_->bind_close_button(delete_bound_, width);
	if (!delete_bound_)
		return;
	const bool shown = host_->close_button_shown();
	if (shown) {
		int32_t x = 0, y = 0;
		host_->close_button_position(x, y);
		if (x != delete_x_ || y != delete_y_ || !delete_shown_) {
			menu.set_widget_rect(id, x, y, x + width, y + width);
			delete_x_ = x;
			delete_y_ = y;
		}
	}
	set_delete_shown_(menu, shown);
}

void CommandMapController::frame(MenuRuntime &menu) {
	if (!host_)
		return;
	place_delete_(menu);
	CommandMapTabGates gates;
	if (host_->refresh(gates))
		apply_gates_(menu, gates);
}

void CommandMapController::waypoint_dialog_requested(MenuRuntime &menu, int32_t x, int32_t y) {
	if (!host_ || !host_->has_map() || bound_map_.empty())
		return;
	const int dialog = menu.widget_id(kDialog);
	const int map = menu.widget_id(bound_map_);
	hud::MapViewRect dialog_rect, map_rect;
	if (dialog < 0 || map < 0 || !host_->local_rect(menu, dialog, dialog_rect) ||
			!host_->local_rect(menu, map, map_rect))
		return;
	const hud::MapViewRect placed =
			hud::command_map_waypoint_dialog_rect(x, y, dialog_rect, map_rect);
	menu.set_widget_rect(dialog, placed.left, placed.top, placed.right, placed.bottom);
	menu.set_widget_shown(dialog, true);
	const int name = menu.widget_id(kName);
	if (name >= 0)
		menu.focus_edit(name);
	host_->store_waypoint_click(x, y);
}

// WPNAME_OK: the waypoint placed from the typed name (the dialog hides by its authored action), the
// delete button hidden when one was placed, the field cleared and CREATE_WAYPOINTS unchecked.
void CommandMapController::confirm_waypoint_(MenuRuntime &menu) {
	const int name = menu.widget_id(kName);
	if (name < 0)
		return;
	if (host_->confirm_waypoint(menu.get_widget_text(name)))
		set_delete_shown_(menu, false);
	menu.set_widget_text(name, std::string());
	uncheck_create_waypoints_(menu);
}

void CommandMapController::uncheck_create_waypoints_(MenuRuntime &menu) {
	const int id = menu.widget_id(kCreate);
	if (id < 0)
		return;
	menu.set_widget_checked(id, false);
	host_->clear_create_waypoints();
}

// CHAT_TEAM / CHAT_ALL: the CHAT_TEXT line read and cleared, then sent through the team or the
// global sender.
void CommandMapController::submit_chat_(MenuRuntime &menu, bool all) {
	const int id = menu.widget_id(kChatText);
	if (id < 0)
		return;
	const std::string text = menu.get_widget_text(id);
	menu.set_widget_text(id, std::string());
	host_->send_chat(all, text);
}

} // namespace opennova::menu
