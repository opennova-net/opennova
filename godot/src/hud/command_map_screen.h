#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/menu/command_map_screen.h>

#include <memory>

#include "hud/map_view_state.h"

namespace godot {

class MenuDriver;
class RtxtStringFile;
class Simulation;

// The CMAP screen's tables and ORDERS composer as the presenter drives them:
// the device half of engine/runtime/menu/command_map_screen.h. It snapshots
// the role's roster (inmatch::command_map_roster) for the table walks, binds
// the TEAMLIST / PLAYERLIST custom-draw handlers on the frame, runs the
// populates and the click handlers against the driver's menu runtime, and
// carries out what a click asks for — the C2S sends through the role's
// client runtime and the local slot writes (squad colour, mute, punt mark) on
// its replica roster. The order store lives on the process-lifetime
// MapViewState. Witness record: docs/interface/hud-re.md (D-HUD-19).
class CommandMapScreen : public RefCounted {
	GDCLASS(CommandMapScreen, RefCounted)

public:
	// The tab radios' interactive states a team-list populate leaves (the
	// bits of populate_team_list / refresh).
	enum TabGate {
		GATE_ORDERS = 1,
		GATE_PLAYERS = 2,
		GATE_TEAM = 4,
		GATE_RULES = 8,
		GATE_SETS_RULES = 16,
	};

	void setup(const Ref<MenuDriver> &p_driver, const Ref<MapViewState> &p_state,
			const Ref<RtxtStringFile> &p_gametext, const Ref<RtxtStringFile> &p_menu_ui);
	void set_simulation(const Ref<Simulation> &p_sim);
	// Bind the TEAMLIST / PLAYERLIST custom-draw handlers on the frame.
	void install_painters();

	// The show's full TEAMLIST populate (`p_full`) or the TEAM radio's
	// incremental update; the gate bits.
	int populate_team_list(bool p_full);
	void populate_player_list();
	// Per frame while the screen is up: the roster snapshot the handlers and
	// painters read, the recruit boxes' hold, and the incremental TEAMLIST
	// update when a squad fold landed since the last frame (its gate bits,
	// else -1).
	int refresh(int64_t p_now_ms);
	// The driver's table_cell_clicked for TEAMLIST / PLAYERLIST /
	// CURRENT_ORDERS.
	void table_cell_clicked(const String &p_widget_name, int p_row, int p_column, int p_state,
			int p_cell_value, int64_t p_now_ms);
	// ADDTO_NO_FIRETEAM / A / B / C (fireteam 0..3).
	void assign_fireteam(int p_fireteam);
	// The ORDERS radio's combos, NEW_ORDER, and the menu rebuild's CURRENT_ORDERS.
	void populate_orders();
	void new_order();
	void seed_current_orders();
	// The session's end: the order store emptied (the next session entry
	// starts clean).
	void clear_orders();

protected:
	static void _bind_methods();

private:
	struct PaintState {
		opennova::menu::CommandMapRoster roster;
		ObjectID driver;
		int teamlist = -1;
	};

	MenuDriver *driver_() const;
	Simulation *sim_() const;
	bool snapshot_();
	opennova::menu::CommandMapOrderWidgets order_widgets_() const;
	void send_order_(const opennova::menu::CommandMapOrderSend &p_send);

	ObjectID driver_id_;
	ObjectID sim_id_;
	Ref<MapViewState> state_;
	opennova::menu::CommandMapText text_;
	std::shared_ptr<PaintState> paint_ = std::make_shared<PaintState>();
	uint32_t squad_revision_ = 0;
	bool revision_known_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::CommandMapScreen::TabGate);
