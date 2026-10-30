#pragma once

// cmap.mnu's CMAP screen as the code registers it: the commander map's controls by NAME, each with
// what its callback does, the show's order and the USERWP_CLOSE delete button's binding to the map
// control on screen [orig: CCommandMap_RegisterAllControls @0x54ada0 (the 41 registrations, from
// the screen's INIT); CMap_OnScreenShow @0x54b320 (the show, UI_DispatchScreenEvent event 5
// @0x54ef84)]. The views, the waypoint table, the tables, the ORDERS composer and the sends are the
// engine's elsewhere (hud/hud_map_view.h, world/user_waypoints.h, menu/command_map_screen.h,
// inmatch/client_squad.cpp); this controller runs the screen over them through its host, which the
// embedder backs with the map window over MAP / ORDERS_MAP, the tables' runner and the session.
// Witness record: docs/interface/hud-re.md "The windowed map views" (D-HUD-19).

#include <cstdint>
#include <string>

#include <runtime/hud/hud_map_view.h>
#include <runtime/menu/command_map_screen.h>
#include <runtime/menu/menu_screen_controller.h>

namespace opennova::menu {

class CommandMapHost {
public:
	virtual ~CommandMapHost() = default;

	// --- the map window over MAP / ORDERS_MAP (the view's globals, the placed-waypoint table) ---
	// Whether the screen hosts a map window (it authors MAP or ORDERS_MAP).
	virtual bool has_map() const = 0;
	// The view's toggle bytes (hud::kCommandToggle*), a toggle box's handler on them
	// (hud::CommandMapView::toggle_changed: GRID / TEXT / WAYPOINTS take the box's state,
	// CREATE_WAYPOINTS flips), and CREATE_WAYPOINTS' clear.
	virtual bool toggle(int toggle) const = 0;
	virtual void toggle_changed(int toggle, bool checked) = 0;
	virtual void clear_create_waypoints() = 0;
	// ZOOMIN (1) / ZOOMOUT (-1) [orig: CMap_ZoomStep @0x548230].
	virtual void zoom(int direction) = 0;
	// The click the name dialog opened on, as the map control's local design point.
	virtual void store_waypoint_click(int32_t x, int32_t y) = 0;
	// WPNAME_OK's place from the typed name; true when one was placed.
	virtual bool confirm_waypoint(const std::string &name) = 0;
	// USERWP_CLOSE's delete of the hovered placed waypoint; CLEAR_WAYPOINTS' of all of them.
	virtual bool delete_hovered_waypoint() = 0;
	virtual void clear_waypoints() = 0;
	// The delete button's binding to the map window (its square width), and where the last render
	// put it (shown beside the hovered waypoint, else hidden).
	virtual void bind_close_button(bool bound, int width) = 0;
	virtual bool close_button_shown() const = 0;
	virtual void close_button_position(int32_t &x, int32_t &y) const = 0;
	// A widget's own rect relative to its parent's origin (CWnd_GetRect), design units; false off
	// the screen.
	virtual bool local_rect(const MenuRuntime &menu, int id, hud::MapViewRect &out) const = 0;

	// --- the tables and the ORDERS composer (command_map_screen.h over the role's roster) ---
	virtual CommandMapTabGates populate_team_list(bool full) = 0;
	virtual void populate_player_list() = 0;
	virtual void populate_orders() = 0;
	virtual void new_order() = 0;
	virtual void assign_fireteam(int fireteam) = 0;
	virtual void table_cell_clicked(
			const std::string &table, int row, int column, int32_t state, int32_t cell_value) = 0;
	// Once a frame while the screen is up: the recruit holds and a squad fold's incremental
	// TEAMLIST update; true with the gates that update left.
	virtual bool refresh(CommandMapTabGates &gates) = 0;

	// --- the session ---
	virtual void send_go_code(int code) = 0;
	virtual void send_chat(bool all, const std::string &text) = 0;
	// The RULELIST text (session_rules_text over the stored S2C 0x58); "" builds nothing.
	virtual std::string rules_text() = 0;
	// OK: the screen closes back to the mission [orig: CMap_OnOk @0x547980].
	virtual void close() = 0;
};

class CommandMapController : public MenuScreenController {
public:
	void set_host(CommandMapHost *host) { host_ = host; }

	// The show [orig: CMap_OnScreenShow @0x54b320]: RADIO_TAB_MAP checked and raised (its callback
	// binds the delete button to MAP), the toggle controls checked from the view's bytes, the
	// delete button bound to MAP, the full TEAMLIST populate and its gates, PLAYERLIST, then
	// RULELIST.
	void open(MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(MenuRuntime &menu, const MenuEvent &event) override;
	void close(MenuRuntime &menu) override;
	// Once a frame while the screen is up: the delete button where the last render put it, and the
	// tables' refresh.
	void frame(MenuRuntime &menu);
	// A CREATE_WAYPOINTS press on the map (`x, y` the design point): WAYPOINTNAME_DLG placed on the
	// bound map control, shown, WPNAME focused, the click stored.
	// [orig: CMapWindow_HandleEvent @0x549e9c..0x549fd4]
	void waypoint_dialog_requested(MenuRuntime &menu, int32_t x, int32_t y);

	// The map control the delete button follows ("" none since a TEAM / PLAYERS / RULES radio) and
	// its state.
	const std::string &bound_map() const { return bound_map_; }
	bool delete_bound() const { return delete_bound_; }
	bool delete_shown() const { return delete_shown_; }

private:
	void apply_gates_(MenuRuntime &menu, const CommandMapTabGates &gates) const;
	void sync_toggle_checks_(MenuRuntime &menu) const;
	void bind_delete_(MenuRuntime &menu, const std::string &map);
	void clip_delete_(MenuRuntime &menu, const std::string &map) const;
	void set_delete_shown_(MenuRuntime &menu, bool shown);
	void place_delete_(MenuRuntime &menu);
	int delete_width_(const MenuRuntime &menu) const;
	void on_activated_(MenuRuntime &menu, const std::string &name);
	void toggle_changed_(MenuRuntime &menu, const std::string &name, int id);
	void confirm_waypoint_(MenuRuntime &menu);
	void uncheck_create_waypoints_(MenuRuntime &menu);
	void submit_chat_(MenuRuntime &menu, bool all);

	CommandMapHost *host_ = nullptr;
	std::string bound_map_; // dword_252DCC8's control
	bool delete_bound_ = false; // dword_252DCCC set
	bool delete_shown_ = false;
	int32_t delete_x_ = -1, delete_y_ = -1;
};

} // namespace opennova::menu
