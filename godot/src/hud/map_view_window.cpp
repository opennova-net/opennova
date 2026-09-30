#include "hud/map_view_window.h"

#include "hud/hud_overlay.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <base/io/tick_rate.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/world.h>

using namespace godot;

namespace {

using opennova::hud::MapViewEvent;

// The menu design space every widget rect is authored in (the witness lives
// at engine/runtime/menu/menu_frame.h kMenuDesignWidth/Height).
constexpr float kDesignW = 800.0f;
constexpr float kDesignH = 600.0f;

} // namespace

MapViewWindow::MapViewWindow() {
	state_.instantiate();
	// The retail pass draws inside a D3D viewport set to the widget rect;
	// clipping the window's items to its rect is that viewport here.
	set_clip_contents(true);
	// Motion and clicks still reach the frame (its cursor and hover pump).
	set_mouse_filter(MOUSE_FILTER_PASS);
}

MapViewWindow::~MapViewWindow() {
	renderer_.release();
}

void MapViewWindow::set_view_kind(int p_kind) {
	view_kind_ = p_kind == VIEW_COMMAND ? VIEW_COMMAND : VIEW_DEATH;
	queue_redraw();
}

void MapViewWindow::set_view_state(const Ref<MapViewState> &p_state) {
	if (p_state.is_valid()) state_ = p_state;
	queue_redraw();
}

void MapViewWindow::zoom_button(int p_direction) {
	if (view_kind_ != VIEW_COMMAND) return;
	state_->command.zoom_button(p_direction);
	queue_redraw();
}

void MapViewWindow::command_toggle_changed(int p_toggle, bool p_checked) {
	if (view_kind_ != VIEW_COMMAND) return;
	state_->command.toggle_changed(p_toggle, p_checked);
	queue_redraw();
}

void MapViewWindow::clear_create_waypoints() {
	state_->command.toggles.create_waypoints = false;
}

bool MapViewWindow::get_command_toggle(int p_toggle) const {
	return state_->command.toggle(p_toggle);
}

void MapViewWindow::set_hud_overlay(HudOverlay *p_hud) {
	hud_id_ = p_hud != nullptr ? ObjectID(p_hud->get_instance_id()) : ObjectID();
	queue_redraw();
}

void MapViewWindow::set_simulation(const Ref<Simulation> &p_sim) {
	sim_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
	queue_redraw();
}

Simulation *MapViewWindow::sim_() const {
	if (!sim_id_.is_valid()) return nullptr;
	return Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

void MapViewWindow::set_widget_design_rect(const Rect2i &p_rect) {
	design_rect_ = p_rect;
	queue_redraw();
}

HudOverlay *MapViewWindow::hud_() const {
	if (!hud_id_.is_valid()) return nullptr;
	return Object::cast_to<HudOverlay>(ObjectDB::get_instance(hud_id_));
}

Vector2 MapViewWindow::surface_() const {
	const Control *frame = Object::cast_to<Control>(get_parent());
	return frame != nullptr ? frame->get_size() : Vector2();
}

opennova::hud::MapViewRect MapViewWindow::device_rect_(const Vector2 &p_surface) const {
	// The custom-draw payload's device rect: every authored edge x the frame
	// scale, truncated (opennova::hud::map_view_design_to_device).
	const float sx = p_surface.x / kDesignW;
	const float sy = p_surface.y / kDesignH;
	opennova::hud::MapViewRect rect;
	rect.left = opennova::hud::map_view_design_to_device(design_rect_.position.x, sx);
	rect.top = opennova::hud::map_view_design_to_device(design_rect_.position.y, sy);
	rect.right = opennova::hud::map_view_design_to_device(
			design_rect_.position.x + design_rect_.size.x, sx);
	rect.bottom = opennova::hud::map_view_design_to_device(
			design_rect_.position.y + design_rect_.size.y, sy);
	return rect;
}

void MapViewWindow::screen_load() {
	if (view_kind_ == VIEW_COMMAND) {
		state_->command.on_load();
		return;
	}
	state_->death.on_load();
}

void MapViewWindow::screen_show(bool p_shroud_present, const Vector2i &p_shroud_size) {
	// The zoom fit is the DEATH screen's show event alone.
	if (view_kind_ == VIEW_COMMAND) return;
	opennova::hud::DeathMapFacts facts;
	if (Simulation *sim = sim_()) sim->fill_death_map_facts(facts);
	opennova::hud::DeathMapFitInput fit;
	fit.shroud_present = p_shroud_present;
	fit.shroud_w = p_shroud_size.x;
	fit.shroud_h = p_shroud_size.y;
	fit.bounds_min_x = facts.bounds_min_x;
	fit.bounds_min_y = facts.bounds_min_y;
	fit.bounds_max_x = facts.bounds_max_x;
	fit.bounds_max_y = facts.bounds_max_y;
	fit.player_x = facts.player_x;
	fit.player_y = facts.player_y;
	state_->death.fit(fit);
	tick_accum_ = 0.0;
	queue_redraw();
}

void MapViewWindow::screen_unload() {
	if (view_kind_ == VIEW_DEATH) state_->death.on_unload();
}

void MapViewWindow::push_map_event(int p_event, const Vector2i &p_position, int p_buttons,
		int p_wheel) {
	if (view_kind_ == VIEW_COMMAND) {
		using Result = opennova::hud::CommandMapView::EventResult;
		const Result result = state_->command.on_event(static_cast<MapViewEvent>(p_event),
				p_position.x, p_position.y, static_cast<uint32_t>(p_buttons), p_wheel);
		// The press asks for the name dialog below the 16-waypoint cap; the
		// idle move runs the hover test while the delete button is bound
		// (hud-re "The windowed map views").
		if (result == Result::kPlaceWaypoint && get_waypoint_count() <
						opennova::world::UserWaypointTable::kCapacity)
			emit_signal("waypoint_dialog_requested", p_position);
		else if (result == Result::kHoverTest)
			hover_test_(p_position);
		queue_redraw();
		return;
	}
	// The move leg's SPAWNPOINTS_TABLE deselect finds no such control on
	// death.mnu (its list is SPAWNPOINTS_LIST), so the return is unused.
	state_->death.on_event(static_cast<MapViewEvent>(p_event), p_position.x, p_position.y,
			static_cast<uint32_t>(p_buttons), p_wheel);
	queue_redraw();
}

void MapViewWindow::store_waypoint_click(const Vector2i &p_design_point) {
	// The click becomes the map control's local point: less the control's
	// absolute origin (its rect plus every ancestor's).
	state_->command.view.last_x = p_design_point.x - design_rect_.position.x;
	state_->command.view.last_y = p_design_point.y - design_rect_.position.y;
}

bool MapViewWindow::confirm_waypoint(const String &p_name) {
	Simulation *sim = sim_();
	if (sim == nullptr || !sim->kernel_) return false;
	opennova::hud::DeathMapFacts facts;
	sim->fill_death_map_facts(facts);
	// The world point reads the local player (retail dereferences it
	// unconditionally).
	if (!facts.player_present) return false;
	int32_t x = 0, y = 0;
	opennova::hud::command_map_waypoint_world(state_->command, facts.player_x, facts.player_y,
			x, y);
	// A session sends each leg (ClientRuntime); the bare local role has no
	// wire, so only the world half runs.
	const std::string name(p_name.utf8().get_data());
	opennova::world::World &world = sim->kernel_->world;
	const bool placed = sim->runtime_ != nullptr
			? sim->runtime_->place_user_waypoint(world, x, y, name)
			: opennova::world::place_user_waypoint(world, x, y, name).valid();
	queue_redraw();
	return placed;
}

bool MapViewWindow::delete_hovered_waypoint() {
	Simulation *sim = sim_();
	if (sim == nullptr || !sim->kernel_) return false;
	opennova::world::World &world = sim->kernel_->world;
	const bool removed = sim->runtime_ != nullptr
			? sim->runtime_->delete_hovered_user_waypoint(world)
			: opennova::world::delete_hovered_user_waypoint(world).valid();
	// The button hides with the removal.
	close_shown_ = false;
	queue_redraw();
	return removed;
}

void MapViewWindow::clear_waypoints() {
	Simulation *sim = sim_();
	if (sim == nullptr || !sim->kernel_) return;
	opennova::world::World &world = sim->kernel_->world;
	if (sim->runtime_ != nullptr) {
		sim->runtime_->clear_user_waypoints(world);
	} else {
		std::vector<opennova::world::EntityHandle> removed;
		opennova::world::clear_user_waypoints(world, removed);
	}
	queue_redraw();
}

int MapViewWindow::get_waypoint_count() const {
	Simulation *sim = sim_();
	if (sim == nullptr || !sim->kernel_) return 0;
	return sim->kernel_->world.user_waypoints.count;
}

void MapViewWindow::set_close_button(bool p_bound, int p_width) {
	close_bound_ = p_bound;
	close_width_ = p_width;
	if (!p_bound) close_shown_ = false;
	queue_redraw();
}

Rect2i MapViewWindow::waypoint_dialog_rect(const Vector2i &p_click, const Rect2i &p_dialog,
		const Rect2i &p_map) {
	const opennova::hud::MapViewRect dialog{p_dialog.position.x, p_dialog.position.y,
			p_dialog.position.x + p_dialog.size.x, p_dialog.position.y + p_dialog.size.y};
	const opennova::hud::MapViewRect map{p_map.position.x, p_map.position.y,
			p_map.position.x + p_map.size.x, p_map.position.y + p_map.size.y};
	const opennova::hud::MapViewRect out =
			opennova::hud::command_map_waypoint_dialog_rect(p_click.x, p_click.y, dialog, map);
	return Rect2i(out.left, out.top, out.right - out.left, out.bottom - out.top);
}

void MapViewWindow::hover_test_(const Vector2i &p_design_point) {
	Simulation *sim = sim_();
	if (!close_bound_ || sim == nullptr || !sim->kernel_) return;
	opennova::world::UserWaypointTable &table = sim->kernel_->world.user_waypoints;
	// A slot counts while the table holds its row (the last render's anchor
	// otherwise).
	std::array<opennova::hud::CommandMapWaypointAnchor, opennova::hud::kCommandMapWaypointSlots>
			anchors = anchors_;
	bool hover[opennova::hud::kCommandMapWaypointSlots] = {};
	for (size_t i = 0; i < anchors.size(); ++i) {
		if (sim->kernel_->world.registry.get(table.entries[i].handle) == nullptr)
			anchors[i].live = false;
		hover[i] = table.entries[i].hover;
	}
	const Vector2 surface = surface_();
	const float sx = surface.x > 0.0f ? surface.x / kDesignW : 1.0f;
	const float sy = surface.y > 0.0f ? surface.y / kDesignH : 1.0f;
	opennova::hud::command_map_waypoint_hover(anchors.data(), hover,
			opennova::hud::kCommandMapWaypointSlots, p_design_point.x, p_design_point.y, sx, sy,
			close_width_);
	for (size_t i = 0; i < anchors.size(); ++i) table.entries[i].hover = hover[i];
}

void MapViewWindow::advance_frame() {
	if (view_kind_ == VIEW_DEATH) state_->death.ease_frame();
	queue_redraw();
}

void MapViewWindow::_process(double p_delta) {
	if (!is_visible_in_tree()) return;
	// The ease runs once per main frame, i.e. per 62.5 Hz logic tick.
	tick_accum_ += p_delta;
	const double tick = 1.0 / opennova::io::kTickHz;
	while (tick_accum_ >= tick) {
		tick_accum_ -= tick;
		if (view_kind_ == VIEW_DEATH) state_->death.ease_frame();
	}
	queue_redraw();
}

void MapViewWindow::_draw() {
	renderer_.clear();
	pass_visible_ = false;
	pass_terrain_tris_ = 0;
	pass_sprites_ = 0;
	pass_labels_ = 0;
	pass_over_lines_ = 0;
	HudOverlay *hud = hud_();
	Simulation *sim = sim_();
	const Vector2 surface = surface_();
	if (hud == nullptr || sim == nullptr || surface.x <= 1.0f || surface.y <= 1.0f) return;
	opennova::hud::DeathMapFacts facts;
	sim->fill_death_map_facts(facts);
	const opennova::hud::MapViewRect rect = device_rect_(surface);
	const int32_t scaled_800 = opennova::hud::map_view_design_to_device(
			static_cast<int32_t>(kDesignW), surface.x / kDesignW);
	const opennova::hud::HudFrameCompiler::MapWindowDraw *draw = nullptr;
	if (view_kind_ == VIEW_COMMAND) {
		draw = hud->compile_command_map(state_->command, rect, scaled_800, facts, surface.x,
				surface.y);
		// The delete button follows the first hovered placed waypoint, and
		// hides when none is.
		close_shown_ = false;
		if (draw != nullptr) {
			anchors_ = draw->waypoint_anchors;
			if (close_bound_ && sim->kernel_) {
				const opennova::world::UserWaypointTable &table = sim->kernel_->world.user_waypoints;
				bool hover[opennova::hud::kCommandMapWaypointSlots] = {};
				for (size_t i = 0; i < table.entries.size(); ++i) hover[i] = table.entries[i].hover;
				int32_t bx = 0, by = 0;
				close_shown_ = opennova::hud::command_map_close_button_position(anchors_.data(),
						hover, opennova::hud::kCommandMapWaypointSlots, surface.x / kDesignW,
						surface.y / kDesignH, close_width_, bx, by);
				close_position_ = Vector2i(bx, by);
			}
		}
	} else {
		const opennova::hud::DeathMapFrame frame =
				state_->death.render(rect, scaled_800, facts.player_x, facts.player_y);
		draw = hud->compile_death_map(frame, facts, surface.x, surface.y);
	}
	if (draw == nullptr) return;
	renderer_.ensure(get_canvas_item(), 0, false, hud->map_additive_material(),
			hud->map_water_material(), hud->map_modulate2x_material());
	// The pass is in frame pixels; this window sits at its widget's origin.
	renderer_.set_transform(Transform2D(0.0, -get_position()));
	HudMapSegmentsView segments;
	segments.pass = &draw->pass.zones;
	segments.segments = &draw->pass.segments;
	segments.glyphs = &draw->zone_glyphs;
	segments.glyph_ends = &draw->zone_glyph_ends;
	renderer_.render(draw->pass.map, draw->glyphs, hud->map_pass_textures(),
			&draw->pass.over_lines, &segments);
	pass_visible_ = draw->pass.map.visible;
	pass_terrain_tris_ = static_cast<int>(draw->pass.map.terrain.size());
	pass_sprites_ = static_cast<int>(draw->pass.map.sprites.size() +
			draw->pass.zones.sprites.size());
	pass_labels_ = static_cast<int>(draw->pass.map.labels.size() +
			draw->pass.zones.labels.size());
	pass_over_lines_ = static_cast<int>(draw->pass.over_lines.size());
}

Vector2i MapViewWindow::design_point_(const Vector2 &p_frame_px) const {
	// The menu mouse events carry the device point divided by the menu scale
	// (surface / 800, / 600), truncated (opennova::hud::map_view_device_to_design).
	const Vector2 surface = surface_();
	const float sx = surface.x > 0.0f ? surface.x / kDesignW : 1.0f;
	const float sy = surface.y > 0.0f ? surface.y / kDesignH : 1.0f;
	return Vector2i(
			opennova::hud::map_view_device_to_design(static_cast<int32_t>(p_frame_px.x), sx),
			opennova::hud::map_view_device_to_design(static_cast<int32_t>(p_frame_px.y), sy));
}

void MapViewWindow::_gui_input(const Ref<InputEvent> &p_event) {
	// Events arrive window-local; the frame pixel converts to the design
	// point the handler reads.
	const Vector2 origin = get_position();
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		const Vector2i at = design_point_(motion->get_position() + origin);
		int buttons = 0;
		if (motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT))
			buttons |= static_cast<int>(opennova::hud::kMapViewButtonLeft);
		if (motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_RIGHT))
			buttons |= static_cast<int>(opennova::hud::kMapViewButtonRight);
		push_map_event(static_cast<int>(MapViewEvent::kMove), at, buttons, 0);
		return;
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_null()) return;
	const Vector2i point = design_point_(button->get_position() + origin);
	switch (button->get_button_index()) {
	case MOUSE_BUTTON_LEFT:
		push_map_event(static_cast<int>(button->is_pressed() ? MapViewEvent::kLeftDown
				: MapViewEvent::kLeftUp), point, 0, 0);
		break;
	case MOUSE_BUTTON_RIGHT:
		push_map_event(static_cast<int>(button->is_pressed() ? MapViewEvent::kRightDown
				: MapViewEvent::kRightUp), point, 0, 0);
		break;
	case MOUSE_BUTTON_WHEEL_UP:
		if (button->is_pressed())
			push_map_event(static_cast<int>(MapViewEvent::kWheel), point, 0, 1);
		break;
	case MOUSE_BUTTON_WHEEL_DOWN:
		if (button->is_pressed())
			push_map_event(static_cast<int>(MapViewEvent::kWheel), point, 0, -1);
		break;
	default:
		break;
	}
}

void MapViewWindow::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) set_process(true);
}

void MapViewWindow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_view_kind", "kind"), &MapViewWindow::set_view_kind);
	ClassDB::bind_method(D_METHOD("get_view_kind"), &MapViewWindow::get_view_kind);
	ClassDB::bind_method(D_METHOD("zoom_button", "direction"), &MapViewWindow::zoom_button);
	ClassDB::bind_method(D_METHOD("command_toggle_changed", "toggle", "checked"),
			&MapViewWindow::command_toggle_changed);
	ClassDB::bind_method(D_METHOD("get_command_toggle", "toggle"),
			&MapViewWindow::get_command_toggle);
	ClassDB::bind_method(D_METHOD("clear_create_waypoints"),
			&MapViewWindow::clear_create_waypoints);
	ClassDB::bind_method(D_METHOD("set_view_state", "state"), &MapViewWindow::set_view_state);
	ClassDB::bind_method(D_METHOD("get_view_state"), &MapViewWindow::get_view_state);
	ClassDB::bind_method(D_METHOD("set_hud_overlay", "hud"), &MapViewWindow::set_hud_overlay);
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &MapViewWindow::set_simulation);
	ClassDB::bind_method(D_METHOD("set_widget_design_rect", "rect"),
			&MapViewWindow::set_widget_design_rect);
	ClassDB::bind_method(D_METHOD("get_widget_design_rect"),
			&MapViewWindow::get_widget_design_rect);
	ClassDB::bind_method(D_METHOD("screen_load"), &MapViewWindow::screen_load);
	ClassDB::bind_method(D_METHOD("screen_show", "shroud_present", "shroud_size"),
			&MapViewWindow::screen_show);
	ClassDB::bind_method(D_METHOD("screen_unload"), &MapViewWindow::screen_unload);
	ClassDB::bind_method(D_METHOD("push_map_event", "event", "position", "buttons", "wheel"),
			&MapViewWindow::push_map_event);
	ClassDB::bind_method(D_METHOD("advance_frame"), &MapViewWindow::advance_frame);
	ClassDB::bind_method(D_METHOD("store_waypoint_click", "design_point"),
			&MapViewWindow::store_waypoint_click);
	ClassDB::bind_method(D_METHOD("confirm_waypoint", "name"), &MapViewWindow::confirm_waypoint);
	ClassDB::bind_method(D_METHOD("delete_hovered_waypoint"),
			&MapViewWindow::delete_hovered_waypoint);
	ClassDB::bind_method(D_METHOD("clear_waypoints"), &MapViewWindow::clear_waypoints);
	ClassDB::bind_method(D_METHOD("get_waypoint_count"), &MapViewWindow::get_waypoint_count);
	ClassDB::bind_method(D_METHOD("set_close_button", "bound", "width"),
			&MapViewWindow::set_close_button);
	ClassDB::bind_method(D_METHOD("is_close_button_shown"), &MapViewWindow::is_close_button_shown);
	ClassDB::bind_method(D_METHOD("get_close_button_position"),
			&MapViewWindow::get_close_button_position);
	ClassDB::bind_static_method("MapViewWindow",
			D_METHOD("waypoint_dialog_rect", "click", "dialog", "map"),
			&MapViewWindow::waypoint_dialog_rect);
	ADD_SIGNAL(MethodInfo("waypoint_dialog_requested",
			PropertyInfo(Variant::VECTOR2I, "design_point")));
	ClassDB::bind_method(D_METHOD("get_zoom"), &MapViewWindow::get_zoom);
	ClassDB::bind_method(D_METHOD("get_pan"), &MapViewWindow::get_pan);
	ClassDB::bind_method(D_METHOD("get_pan_target"), &MapViewWindow::get_pan_target);
	ClassDB::bind_method(D_METHOD("is_pass_visible"), &MapViewWindow::is_pass_visible);
	ClassDB::bind_method(D_METHOD("get_pass_terrain_tris"),
			&MapViewWindow::get_pass_terrain_tris);
	ClassDB::bind_method(D_METHOD("get_pass_sprites"), &MapViewWindow::get_pass_sprites);
	ClassDB::bind_method(D_METHOD("get_pass_labels"), &MapViewWindow::get_pass_labels);
	ClassDB::bind_method(D_METHOD("get_pass_over_lines"), &MapViewWindow::get_pass_over_lines);
	BIND_ENUM_CONSTANT(VIEW_DEATH);
	BIND_ENUM_CONSTANT(VIEW_COMMAND);
	BIND_ENUM_CONSTANT(COMMAND_TOGGLE_GRID);
	BIND_ENUM_CONSTANT(COMMAND_TOGGLE_TEXT);
	BIND_ENUM_CONSTANT(COMMAND_TOGGLE_WAYPOINTS);
	BIND_ENUM_CONSTANT(COMMAND_TOGGLE_CREATE_WAYPOINTS);
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_MOVE",
			static_cast<int>(MapViewEvent::kMove));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_LEFT_DOWN",
			static_cast<int>(MapViewEvent::kLeftDown));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_LEFT_UP",
			static_cast<int>(MapViewEvent::kLeftUp));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_RIGHT_DOWN",
			static_cast<int>(MapViewEvent::kRightDown));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_RIGHT_UP",
			static_cast<int>(MapViewEvent::kRightUp));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAP_EVENT_WHEEL",
			static_cast<int>(MapViewEvent::kWheel));
}
