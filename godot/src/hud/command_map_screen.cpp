#include "hud/command_map_screen.h"

#include <godot_cpp/core/object.hpp>

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/mission/mission_kernel.h>

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h"
#include "mnu/menu_frame.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"

namespace godot {

namespace {

constexpr const char *kScreen = "CMAP";
constexpr const char *kTeamList = "TEAMLIST";
constexpr const char *kPlayerList = "PLAYERLIST";
constexpr const char *kCurrentOrders = "CURRENT_ORDERS";

int gate_bits(const opennova::menu::CommandMapTabGates &g) {
	return (g.orders ? CommandMapScreen::GATE_ORDERS : 0) |
			(g.players ? CommandMapScreen::GATE_PLAYERS : 0) |
			(g.team ? CommandMapScreen::GATE_TEAM : 0) |
			(g.rules ? CommandMapScreen::GATE_RULES : 0) |
			(g.sets_rules ? CommandMapScreen::GATE_SETS_RULES : 0);
}

} // namespace

void CommandMapScreen::setup(const Ref<MenuDriver> &p_driver, const Ref<MapViewState> &p_state,
		const Ref<RtxtStringFile> &p_gametext, const Ref<RtxtStringFile> &p_menu_ui) {
	driver_id_ = p_driver.is_valid() ? ObjectID(p_driver->get_instance_id()) : ObjectID();
	state_ = p_state;
	text_.gametext = game_text_lookup(p_gametext);
	text_.menu_ui = game_text_lookup(p_menu_ui);
	paint_->driver = driver_id_;
	MenuDriver *driver = driver_();
	paint_->teamlist = driver != nullptr ? driver->widget_id(kTeamList) : -1;
}

void CommandMapScreen::set_simulation(const Ref<Simulation> &p_sim) {
	sim_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
	revision_known_ = false;
}

MenuDriver *CommandMapScreen::driver_() const {
	if (!driver_id_.is_valid()) return nullptr;
	return Object::cast_to<MenuDriver>(ObjectDB::get_instance(driver_id_));
}

Simulation *CommandMapScreen::sim_() const {
	if (!sim_id_.is_valid()) return nullptr;
	return Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

// The roster the walks and painters read, refreshed from the role's replica.
bool CommandMapScreen::snapshot_() {
	Simulation *sim = sim_();
	if (sim == nullptr || !sim->kernel_) {
		paint_->roster = opennova::menu::CommandMapRoster{};
		return false;
	}
	return opennova::inmatch::command_map_roster(sim->role_view(), paint_->roster);
}

// The TEAMLIST and PLAYERLIST custom-draw handlers (engine
// command_map_team_list_paint / command_map_player_list_paint), bound on the
// frame by widget index; each reads the latest snapshot and the TEAMLIST
// row's value through the driver's runtime.
void CommandMapScreen::install_painters() {
	MenuDriver *driver = driver_();
	if (driver == nullptr) return;
	MenuFrame *frame = driver->get_frame();
	if (frame == nullptr) return;
	const std::shared_ptr<PaintState> paint = paint_;
	const int team_index = driver->frame_index(driver->widget_id(kTeamList));
	if (team_index >= 0) {
		frame->set_table_cell_painter(team_index,
				[paint](const opennova::menu::MenuTableCellEvent &event,
						opennova::menu::MenuTableCellCanvas &canvas) {
					int32_t row_value = 0;
					if (event.row >= 0 && paint->driver.is_valid()) {
						if (MenuDriver *d = Object::cast_to<MenuDriver>(
									ObjectDB::get_instance(paint->driver)))
							row_value = d->runtime().table_row_value(paint->teamlist, event.row);
					}
					opennova::menu::command_map_team_list_paint(event, canvas, row_value,
							paint->roster);
				});
	}
	const int player_index = driver->frame_index(driver->widget_id(kPlayerList));
	if (player_index >= 0) {
		frame->set_table_cell_painter(player_index,
				[](const opennova::menu::MenuTableCellEvent &event,
						opennova::menu::MenuTableCellCanvas &canvas) {
					opennova::menu::command_map_player_list_paint(event, canvas);
				});
	}
}

int CommandMapScreen::populate_team_list(bool p_full) {
	MenuDriver *driver = driver_();
	if (driver == nullptr) return 0;
	snapshot_();
	opennova::menu::MenuRuntime &menu = driver->runtime();
	const int table = driver->widget_id(kTeamList);
	const opennova::menu::CommandMapTabGates gates = p_full
			? opennova::menu::command_map_populate_team_list(menu, table, paint_->roster, text_)
			: opennova::menu::command_map_update_team_list(menu, table, paint_->roster, text_);
	return gate_bits(gates);
}

void CommandMapScreen::populate_player_list() {
	MenuDriver *driver = driver_();
	if (driver == nullptr) return;
	snapshot_();
	opennova::menu::command_map_populate_player_list(driver->runtime(),
			driver->widget_id(kPlayerList), paint_->roster, text_);
}

int CommandMapScreen::refresh(int64_t p_now_ms) {
	MenuDriver *driver = driver_();
	if (driver == nullptr) return -1;
	snapshot_();
	opennova::menu::MenuRuntime &menu = driver->runtime();
	opennova::menu::command_map_team_list_timers(menu, driver->widget_id(kTeamList),
			static_cast<uint32_t>(p_now_ms));
	// Every S2C 0x71 / 0x73 fold re-runs the incremental populate
	// (ClientState::squad_revision; engine command_map_update_team_list).
	Simulation *sim = sim_();
	if (sim == nullptr || sim->runtime_ == nullptr) return -1;
	const uint32_t revision = sim->runtime_->state().squad_revision;
	const bool changed = revision_known_ && revision != squad_revision_;
	squad_revision_ = revision;
	revision_known_ = true;
	if (!changed) return -1;
	return gate_bits(opennova::menu::command_map_update_team_list(menu,
			driver->widget_id(kTeamList), paint_->roster, text_));
}

void CommandMapScreen::table_cell_clicked(const String &p_widget_name, int p_row, int p_column,
		int p_state, int p_cell_value, int64_t p_now_ms) {
	MenuDriver *driver = driver_();
	Simulation *sim = sim_();
	if (driver == nullptr || sim == nullptr) return;
	snapshot_();
	opennova::menu::MenuRuntime &menu = driver->runtime();
	opennova::inmatch::ClientRuntime *runtime = sim->runtime_;
	const String name = p_widget_name.to_upper();
	if (name == kTeamList) {
		const opennova::menu::CommandMapTeamClick click =
				opennova::menu::command_map_team_list_click(menu, driver->widget_id(kTeamList),
						p_row, p_column, p_state, paint_->roster, static_cast<uint32_t>(p_now_ms));
		if (runtime == nullptr) return;
		if (click.send_recruit) runtime->send_squad_recruit(click.slot);
		if (click.send_join) runtime->send_squad_join(click.slot);
		if (click.set_squad_color) runtime->state().roster[click.slot].squad_color = click.squad_color;
	} else if (name == kPlayerList) {
		const opennova::menu::CommandMapPlayerClick click =
				opennova::menu::command_map_player_list_click(menu,
						driver->widget_id(kPlayerList), p_row, p_column, p_state, paint_->roster);
		if (runtime == nullptr) return;
		opennova::replication::ClientState &cs = runtime->state();
		if (click.set_mute) cs.roster[click.slot].radio_mute_flags = click.mute;
		if (click.clear_punt_marks) {
			// Every active slot's mark (CommandMapPlayerClick::clear_punt_marks).
			for (opennova::replication::ClientRosterSlot &slot : cs.roster)
				if (slot.bound) slot.punt_mark = 0xFF;
		}
		if (click.set_punt_mark) cs.roster[click.slot].punt_mark = click.punt_mark;
		if (click.send_punt) runtime->send_punt_vote(click.punt_mark);
	} else if (name == kCurrentOrders) {
		if (state_.is_null()) return;
		send_order_(opennova::menu::command_map_current_orders_click(menu, order_widgets_(),
				state_->orders, p_row, p_column, p_state, p_cell_value, paint_->roster));
	}
}

void CommandMapScreen::assign_fireteam(int p_fireteam) {
	MenuDriver *driver = driver_();
	Simulation *sim = sim_();
	if (driver == nullptr || sim == nullptr) return;
	snapshot_();
	const std::vector<uint8_t> members = opennova::menu::command_map_fireteam_members(
			driver->runtime(), driver->widget_id(kTeamList), paint_->roster);
	// Nothing selected sends nothing (engine command_map_fireteam_members).
	if (members.empty() || sim->runtime_ == nullptr) return;
	sim->runtime_->send_fireteam_assign(static_cast<uint8_t>(p_fireteam), members);
}

opennova::menu::CommandMapOrderWidgets CommandMapScreen::order_widgets_() const {
	opennova::menu::CommandMapOrderWidgets w;
	MenuDriver *driver = driver_();
	if (driver == nullptr) return w;
	w.group = driver->widget_id("GROUP");
	w.command_order = driver->widget_id("COMMAND_ORDER");
	w.location = driver->widget_id("LOCATION");
	w.directional = driver->widget_id("DIRECTIONAL");
	w.waitfor = driver->widget_id("WAITFOR");
	w.current_orders = driver->widget_id(kCurrentOrders);
	return w;
}

void CommandMapScreen::populate_orders() {
	MenuDriver *driver = driver_();
	Simulation *sim = sim_();
	if (driver == nullptr || sim == nullptr || !sim->kernel_ || state_.is_null()) return;
	snapshot_();
	opennova::menu::CommandMapLocations locations;
	opennova::inmatch::command_map_locations(sim->role_view(), sim->deploy_zone_registry(),
			locations);
	opennova::menu::command_map_populate_orders(driver->runtime(), order_widgets_(),
			state_->orders, paint_->roster, locations, text_);
}

void CommandMapScreen::new_order() {
	MenuDriver *driver = driver_();
	if (driver == nullptr || state_.is_null()) return;
	snapshot_();
	send_order_(opennova::menu::command_map_new_order(driver->runtime(), order_widgets_(),
			state_->orders, paint_->roster));
}

void CommandMapScreen::send_order_(const opennova::menu::CommandMapOrderSend &p_send) {
	Simulation *sim = sim_();
	if (!p_send.send || sim == nullptr || sim->runtime_ == nullptr) return;
	sim->runtime_->send_squad_order(p_send.kind, p_send.text, p_send.targets);
}

void CommandMapScreen::seed_current_orders() {
	MenuDriver *driver = driver_();
	if (driver == nullptr || state_.is_null()) return;
	opennova::menu::command_map_seed_current_orders(driver->runtime(),
			driver->widget_id(kCurrentOrders), state_->orders);
}

void CommandMapScreen::clear_orders() {
	if (state_.is_valid()) state_->orders.clear();
}

void CommandMapScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "driver", "state", "gametext", "menu_ui"),
			&CommandMapScreen::setup);
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &CommandMapScreen::set_simulation);
	ClassDB::bind_method(D_METHOD("install_painters"), &CommandMapScreen::install_painters);
	ClassDB::bind_method(D_METHOD("populate_team_list", "full"),
			&CommandMapScreen::populate_team_list);
	ClassDB::bind_method(D_METHOD("populate_player_list"), &CommandMapScreen::populate_player_list);
	ClassDB::bind_method(D_METHOD("refresh", "now_ms"), &CommandMapScreen::refresh);
	ClassDB::bind_method(D_METHOD("table_cell_clicked", "widget_name", "row", "column", "state",
								 "cell_value", "now_ms"),
			&CommandMapScreen::table_cell_clicked);
	ClassDB::bind_method(D_METHOD("assign_fireteam", "fireteam"), &CommandMapScreen::assign_fireteam);
	ClassDB::bind_method(D_METHOD("populate_orders"), &CommandMapScreen::populate_orders);
	ClassDB::bind_method(D_METHOD("new_order"), &CommandMapScreen::new_order);
	ClassDB::bind_method(D_METHOD("seed_current_orders"), &CommandMapScreen::seed_current_orders);
	ClassDB::bind_method(D_METHOD("clear_orders"), &CommandMapScreen::clear_orders);
	BIND_ENUM_CONSTANT(GATE_ORDERS);
	BIND_ENUM_CONSTANT(GATE_PLAYERS);
	BIND_ENUM_CONSTANT(GATE_TEAM);
	BIND_ENUM_CONSTANT(GATE_RULES);
	BIND_ENUM_CONSTANT(GATE_SETS_RULES);
}

} // namespace godot
