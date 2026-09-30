#include "hud/map_view_window.h"

#include "hud/hud_overlay.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <base/io/tick_rate.h>

using namespace godot;

namespace {

using opennova::hud::MapViewEvent;

// The menu design space every widget rect is authored in (the witness lives
// at engine/runtime/menu/menu_frame.h kMenuDesignWidth/Height).
constexpr float kDesignW = 800.0f;
constexpr float kDesignH = 600.0f;

} // namespace

MapViewWindow::MapViewWindow() {
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

void MapViewWindow::zoom_button(int p_direction) {
	if (view_kind_ != VIEW_COMMAND) return;
	command_.zoom_button(p_direction);
	queue_redraw();
}

void MapViewWindow::command_toggle_changed(int p_toggle, bool p_checked) {
	if (view_kind_ != VIEW_COMMAND) return;
	command_.toggle_changed(p_toggle, p_checked);
	queue_redraw();
}

bool MapViewWindow::get_command_toggle(int p_toggle) const {
	return command_.toggle(p_toggle);
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
		command_.on_load();
		return;
	}
	model_.on_load();
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
	model_.fit(fit);
	tick_accum_ = 0.0;
	queue_redraw();
}

void MapViewWindow::screen_unload() {
	if (view_kind_ == VIEW_DEATH) model_.on_unload();
}

void MapViewWindow::push_map_event(int p_event, const Vector2i &p_position, int p_buttons,
		int p_wheel) {
	if (view_kind_ == VIEW_COMMAND) {
		command_.on_event(static_cast<MapViewEvent>(p_event), p_position.x, p_position.y,
				static_cast<uint32_t>(p_buttons), p_wheel);
		queue_redraw();
		return;
	}
	// The move leg's SPAWNPOINTS_TABLE deselect finds no such control on
	// death.mnu (its list is SPAWNPOINTS_LIST), so the return is unused.
	model_.on_event(static_cast<MapViewEvent>(p_event), p_position.x, p_position.y,
			static_cast<uint32_t>(p_buttons), p_wheel);
	queue_redraw();
}

void MapViewWindow::advance_frame() {
	if (view_kind_ == VIEW_DEATH) model_.ease_frame();
	queue_redraw();
}

void MapViewWindow::_process(double p_delta) {
	if (!is_visible_in_tree()) return;
	// The ease runs once per main frame, i.e. per 62.5 Hz logic tick.
	tick_accum_ += p_delta;
	const double tick = 1.0 / opennova::io::kTickHz;
	while (tick_accum_ >= tick) {
		tick_accum_ -= tick;
		if (view_kind_ == VIEW_DEATH) model_.ease_frame();
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
		draw = hud->compile_command_map(command_, rect, scaled_800, facts, surface.x,
				surface.y);
	} else {
		const opennova::hud::DeathMapFrame frame =
				model_.render(rect, scaled_800, facts.player_x, facts.player_y);
		draw = hud->compile_death_map(frame, facts, surface.x, surface.y);
	}
	if (draw == nullptr) return;
	renderer_.ensure(get_canvas_item(), 0, false, hud->map_additive_material(),
			hud->map_water_material());
	// The pass is in frame pixels; this window sits at its widget's origin.
	renderer_.set_transform(Transform2D(0.0, -get_position()));
	renderer_.render(draw->pass.map, draw->glyphs, hud->map_pass_textures(),
			&draw->pass.over_lines);
	pass_visible_ = draw->pass.map.visible;
	pass_terrain_tris_ = static_cast<int>(draw->pass.map.terrain.size());
	pass_sprites_ = static_cast<int>(draw->pass.map.sprites.size());
	pass_labels_ = static_cast<int>(draw->pass.map.labels.size());
	pass_over_lines_ = static_cast<int>(draw->pass.over_lines.size());
}

void MapViewWindow::_gui_input(const Ref<InputEvent> &p_event) {
	// Events arrive window-local; the handler's coordinates are frame pixels.
	const Vector2 origin = get_position();
	const Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		const Vector2 at = motion->get_position() + origin;
		int buttons = 0;
		if (motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT))
			buttons |= static_cast<int>(opennova::hud::kMapViewButtonLeft);
		if (motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_RIGHT))
			buttons |= static_cast<int>(opennova::hud::kMapViewButtonRight);
		push_map_event(static_cast<int>(MapViewEvent::kMove),
				Vector2i(static_cast<int>(at.x), static_cast<int>(at.y)), buttons, 0);
		return;
	}
	const Ref<InputEventMouseButton> button = p_event;
	if (button.is_null()) return;
	const Vector2 at = button->get_position() + origin;
	const Vector2i point(static_cast<int>(at.x), static_cast<int>(at.y));
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
