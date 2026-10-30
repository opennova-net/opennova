#include "hud/command_map_controller.h"

#include "hud/command_map_screen.h"
#include "hud/hud_overlay.h"
#include "hud/map_view_window.h"
#include "mnu/menu_text.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"
#include "world/world_view.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/rect2.hpp>

namespace godot {

namespace {

opennova::menu::CommandMapTabGates gates_of(int p_bits) {
	opennova::menu::CommandMapTabGates gates;
	gates.orders = (p_bits & CommandMapScreen::GATE_ORDERS) != 0;
	gates.players = (p_bits & CommandMapScreen::GATE_PLAYERS) != 0;
	gates.team = (p_bits & CommandMapScreen::GATE_TEAM) != 0;
	gates.rules = (p_bits & CommandMapScreen::GATE_RULES) != 0;
	gates.sets_rules = (p_bits & CommandMapScreen::GATE_SETS_RULES) != 0;
	return gates;
}

} // namespace

class CommandMapController::Host : public opennova::menu::CommandMapHost {
	CommandMapController *owner_;

	MapViewWindow *map() const { return owner_->map_window_(); }
	Ref<Simulation> sim() const {
		return owner_->view_.is_valid() ? owner_->view_->sim() : Ref<Simulation>();
	}
	int64_t now_ms() const {
		return owner_->view_.is_valid() ? owner_->view_->get_frame_clock_ms() : 0;
	}

public:
	explicit Host(CommandMapController *p_owner) : owner_(p_owner) {}

	bool has_map() const override { return map() != nullptr; }
	bool toggle(int p_toggle) const override {
		MapViewWindow *window = map();
		return window != nullptr && window->get_command_toggle(p_toggle);
	}
	void toggle_changed(int p_toggle, bool p_checked) override {
		if (MapViewWindow *window = map())
			window->command_toggle_changed(p_toggle, p_checked);
	}
	void clear_create_waypoints() override {
		if (MapViewWindow *window = map())
			window->clear_create_waypoints();
	}
	void zoom(int p_direction) override {
		if (MapViewWindow *window = map())
			window->zoom_button(p_direction);
	}
	void store_waypoint_click(int32_t p_x, int32_t p_y) override {
		if (MapViewWindow *window = map())
			window->store_waypoint_click(Vector2i(p_x, p_y));
	}
	bool confirm_waypoint(const std::string &p_name) override {
		MapViewWindow *window = map();
		return window != nullptr &&
				window->confirm_waypoint(menu_text_to_gd(owner_->driver_(), p_name));
	}
	bool delete_hovered_waypoint() override {
		MapViewWindow *window = map();
		return window != nullptr && window->delete_hovered_waypoint();
	}
	void clear_waypoints() override {
		if (MapViewWindow *window = map())
			window->clear_waypoints();
	}
	void bind_close_button(bool p_bound, int p_width) override {
		if (MapViewWindow *window = map())
			window->set_close_button(p_bound, p_width);
	}
	bool close_button_shown() const override {
		MapViewWindow *window = map();
		return window != nullptr && window->is_close_button_shown();
	}
	void close_button_position(int32_t &p_x, int32_t &p_y) const override {
		MapViewWindow *window = map();
		const Vector2i at = window != nullptr ? window->get_close_button_position() : Vector2i();
		p_x = at.x;
		p_y = at.y;
	}
	bool local_rect(const opennova::menu::MenuRuntime &, int p_id,
			opennova::hud::MapViewRect &p_out) const override {
		MenuDriver *driver = owner_->driver_();
		if (driver == nullptr || driver->frame_index(p_id) < 0)
			return false;
		// The frame's design rect, truncated per member as Rect2i takes it.
		const Rect2i rect(driver->widget_local_rect(p_id));
		p_out = { rect.position.x, rect.position.y, rect.position.x + rect.size.x,
			rect.position.y + rect.size.y };
		return true;
	}

	opennova::menu::CommandMapTabGates populate_team_list(bool p_full) override {
		return owner_->screen_.is_valid() ? gates_of(owner_->screen_->populate_team_list(p_full))
										  : opennova::menu::CommandMapTabGates();
	}
	void populate_player_list() override {
		if (owner_->screen_.is_valid())
			owner_->screen_->populate_player_list();
	}
	void populate_orders() override {
		if (owner_->screen_.is_valid())
			owner_->screen_->populate_orders();
	}
	void new_order() override {
		if (owner_->screen_.is_valid())
			owner_->screen_->new_order();
	}
	void assign_fireteam(int p_fireteam) override {
		if (owner_->screen_.is_valid())
			owner_->screen_->assign_fireteam(p_fireteam);
	}
	void table_cell_clicked(const std::string &p_table, int p_row, int p_column, int32_t p_state,
			int32_t p_cell_value) override {
		if (owner_->screen_.is_valid())
			owner_->screen_->table_cell_clicked(
					opennova::to_gd(p_table), p_row, p_column, p_state, p_cell_value, now_ms());
	}
	bool refresh(opennova::menu::CommandMapTabGates &p_gates) override {
		if (owner_->screen_.is_null())
			return false;
		const int bits = owner_->screen_->refresh(now_ms());
		if (bits < 0)
			return false;
		p_gates = gates_of(bits);
		return true;
	}

	void send_go_code(int p_code) override {
		const Ref<Simulation> session = sim();
		if (session.is_valid())
			session->send_go_code(p_code);
	}
	void send_chat(bool p_all, const std::string &p_text) override {
		const Ref<Simulation> session = sim();
		HudOverlay *hud = owner_->hud_source_.is_valid()
				? Object::cast_to<HudOverlay>(static_cast<Object *>(owner_->hud_source_.call()))
				: nullptr;
		// The flood check reads the main-frame counter the HUD's frame set (engine
		// inmatch/chat_flood.h).
		if (hud != nullptr && session.is_valid())
			hud->send_command_map_chat(session, p_all, menu_text_to_gd(owner_->driver_(), p_text));
	}
	std::string rules_text() override {
		const Ref<Simulation> session = sim();
		return session.is_valid() ? opennova::gd_to_retail_text(
											session->get_command_map_rules_text(owner_->gametext_))
								  : std::string();
	}
	void close() override { owner_->emit_signal("close_requested"); }
};

CommandMapController::CommandMapController() : host_(std::make_unique<Host>(this)) {
	controller_.set_host(host_.get());
}

CommandMapController::~CommandMapController() {
	controller_.set_host(nullptr);
}

MenuDriver *CommandMapController::driver_() const {
	return driver_id_.is_null() ? nullptr
								: Object::cast_to<MenuDriver>(ObjectDB::get_instance(driver_id_));
}

MapViewWindow *CommandMapController::map_window_() const {
	return map_window_id_.is_null()
			? nullptr
			: Object::cast_to<MapViewWindow>(ObjectDB::get_instance(map_window_id_));
}

void CommandMapController::setup(const Ref<MenuDriver> &p_driver, const Ref<WorldView> &p_view,
		const Ref<CommandMapScreen> &p_screen, MapViewWindow *p_map_window,
		const Ref<RtxtStringFile> &p_gametext, const Callable &p_hud_source) {
	driver_id_ = p_driver.is_valid() ? ObjectID(p_driver->get_instance_id()) : ObjectID();
	view_ = p_view;
	screen_ = p_screen;
	map_window_id_ =
			p_map_window != nullptr ? ObjectID(p_map_window->get_instance_id()) : ObjectID();
	gametext_ = p_gametext;
	hud_source_ = p_hud_source;
}

void CommandMapController::frame() {
	if (MenuDriver *driver = driver_())
		controller_.frame(driver->runtime());
}

void CommandMapController::waypoint_dialog_requested(const Vector2i &p_point) {
	if (MenuDriver *driver = driver_())
		controller_.waypoint_dialog_requested(driver->runtime(), p_point.x, p_point.y);
}

void CommandMapController::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("setup", "driver", "view", "screen", "map_window", "gametext", "hud_source"),
			&CommandMapController::setup);
	ClassDB::bind_method(D_METHOD("frame"), &CommandMapController::frame);
	ClassDB::bind_method(D_METHOD("waypoint_dialog_requested", "point"),
			&CommandMapController::waypoint_dialog_requested);
	ClassDB::bind_method(D_METHOD("get_bound_map"), &CommandMapController::get_bound_map);
	ClassDB::bind_method(D_METHOD("is_delete_bound"), &CommandMapController::is_delete_bound);
	// OK clicked (or its Escape hotkey): the screen's frame closes back to the mission.
	ADD_SIGNAL(MethodInfo("close_requested"));
}

} // namespace godot
