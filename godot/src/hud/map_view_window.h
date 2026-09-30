#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/hud/hud_map_view.h>

#include <array>

#include "hud/hud_map_pass_renderer.h"
#include "hud/map_view_state.h"

namespace godot {

class HudOverlay;
class Simulation;

// A menu map window host: a Control a presenter mounts over a custom-draw map
// widget (a frame child, so the frame's own cursor/popup overlay stays above
// it) — death.mnu's MAP (the DEATH kind) or cmap.mnu's MAP / ORDERS_MAP (the
// COMMAND kind). The engine owns the witnessed views (opennova::hud::
// DeathMapView / CommandMapView: pan/zoom, the fit, the ease, the event maps,
// the toggles) and the passes (HudFrameCompiler::compile_death_map /
// compile_command_map over the HUD overlay's frame state); this node is the
// device leg — it samples the mouse, keeps the logic-tick clock the ease
// rides, clips to the widget rect like the retail viewport, and submits the
// pass through the shared map renderer.
// Witness record: docs/interface/hud-re.md (D-HUD-19).
class MapViewWindow : public Control {
	GDCLASS(MapViewWindow, Control)

public:
	enum ViewKind {
		VIEW_DEATH = 0,
		VIEW_COMMAND = 1,
	};
	enum CommandToggle {
		COMMAND_TOGGLE_GRID = opennova::hud::kCommandToggleGrid,
		COMMAND_TOGGLE_TEXT = opennova::hud::kCommandToggleText,
		COMMAND_TOGGLE_WAYPOINTS = opennova::hud::kCommandToggleWaypoints,
		COMMAND_TOGGLE_CREATE_WAYPOINTS = opennova::hud::kCommandToggleCreateWaypoints,
	};
	// The CMAP tab radios the screen's show gates (hud_map_view.h
	// command_map_tab_gates).
	enum CommandTab {
		COMMAND_TAB_ORDERS = 0,
		COMMAND_TAB_PLAYERS = 1,
		COMMAND_TAB_TEAM = 2,
		COMMAND_TAB_RULES = 3,
	};

	MapViewWindow();
	~MapViewWindow();

	// Which view this window hosts (set before the first load).
	void set_view_kind(int p_kind);
	int get_view_kind() const { return view_kind_; }
	// The CMAP zoom buttons (1 in, -1 out) and toggle controls.
	void zoom_button(int p_direction);
	void command_toggle_changed(int p_toggle, bool p_checked);
	bool get_command_toggle(int p_toggle) const;
	// The confirm's and the cancel's CREATE_WAYPOINTS byte = 0 (the presenter
	// unchecks the control beside it).
	void clear_create_waypoints();

	// The view state this window drives: a presenter hands its process-
	// lifetime MapViewState so the views survive the per-mission rebuild
	// (a window without one keeps its own).
	void set_view_state(const Ref<MapViewState> &p_state);
	Ref<MapViewState> get_view_state() const { return state_; }

	// The pass sources: the HUD overlay (terrain, markers, fonts, textures)
	// and the Simulation the world facts come from (either null: no pass).
	void set_hud_overlay(HudOverlay *p_hud);
	void set_simulation(const Ref<Simulation> &p_sim);
	// The MAP widget's absolute authored rect in the 800x600 menu design
	// space; the device rect is its edges x the frame scale, truncated.
	void set_widget_design_rect(const Rect2i &p_rect);
	Rect2i get_widget_design_rect() const { return design_rect_; }

	// The screen lifecycle the retail menu dispatches: load (event 3), show
	// (event 5, the zoom fit against the DEATH_SHROUD window's authored size
	// when the screen has one), unload (event 4).
	void screen_load();
	void screen_show(bool p_shroud_present, const Vector2i &p_shroud_size);
	void screen_unload();

	// One mouse-class event at a menu DESIGN point (the handler's space: the
	// input leg divides the frame pixel by the menu scale like
	// UI_DispatchMouseEvent; the tests' seam): the MapViewEvent code, the
	// button mask (bit 0 left, bit 1 right) and the wheel remainder (> 0
	// forward).
	void push_map_event(int p_event, const Vector2i &p_position, int p_buttons, int p_wheel);
	// One logic tick of the pan ease (the per-main-frame menu handler).
	void advance_frame();

	// THE CMAP USER-WAYPOINT LEGS (the COMMAND kind; hud_map_view.h and
	// inmatch/client_squad.cpp). A CREATE_WAYPOINTS left press with fewer than
	// 16 placed emits waypoint_dialog_requested(design point): the presenter
	// places and shows WAYPOINTNAME_DLG, focuses WPNAME, then stores the click
	// here (the map control's local point, the view's last point). The
	// confirm turns that point into the world point and places the waypoint;
	// the delete button removes the hovered one; CLEAR_WAYPOINTS all of them.
	// The delete button (USERWP_CLOSE) takes part while the presenter has it
	// bound to this map (its authored width is the hover box's left margin);
	// every render then reports where it goes, or that it hides. Witness:
	// hud-re "The windowed map views" (CMapWindow_HandleEvent).
	void store_waypoint_click(const Vector2i &p_design_point);
	bool confirm_waypoint(const String &p_name);
	bool delete_hovered_waypoint();
	void clear_waypoints();
	int get_waypoint_count() const;
	void set_close_button(bool p_bound, int p_width);
	bool is_close_button_shown() const { return close_shown_; }
	Vector2i get_close_button_position() const { return close_position_; }
	static Rect2i waypoint_dialog_rect(const Vector2i &p_click, const Rect2i &p_dialog,
			const Rect2i &p_map);
	// The tab radios' interactive states from this frame's facts.
	bool is_command_tab_enabled(int p_tab) const;

	// Read seams (ADR 0018): the view state and the last compiled pass.
	float get_zoom() const { return pan_view_().zoom; }
	Vector2i get_pan() const { return Vector2i(pan_view_().pan_x, pan_view_().pan_y); }
	Vector2i get_pan_target() const {
		return Vector2i(state_->death.target_x, state_->death.target_y);
	}
	bool is_pass_visible() const { return pass_visible_; }
	int get_pass_terrain_tris() const { return pass_terrain_tris_; }
	int get_pass_sprites() const { return pass_sprites_; }
	int get_pass_labels() const { return pass_labels_; }
	int get_pass_over_lines() const { return pass_over_lines_; }

	void _process(double p_delta) override;
	void _draw() override;
	void _gui_input(const Ref<InputEvent> &p_event) override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	HudOverlay *hud_() const;
	// Weak: the window never extends the session's lifetime.
	Simulation *sim_() const;
	// The frame the window sits in (its parent Control), whose size is the
	// device surface the menu scales to.
	Vector2 surface_() const;
	opennova::hud::MapViewRect device_rect_(const Vector2 &p_surface) const;
	Vector2i design_point_(const Vector2 &p_frame_px) const;
	const opennova::hud::MapViewPan &pan_view_() const {
		return view_kind_ == VIEW_COMMAND ? state_->command.view : state_->death.view;
	}

	// The world's placed-waypoint hover bytes against the last render's
	// anchors (the move leg).
	void hover_test_(const Vector2i &p_design_point);

	int view_kind_ = VIEW_DEATH;
	Ref<MapViewState> state_;
	// The last CMAP render's waypoint anchors and the delete button's state.
	std::array<opennova::hud::CommandMapWaypointAnchor, opennova::hud::kCommandMapWaypointSlots>
			anchors_{};
	bool close_bound_ = false;
	int close_width_ = 0;
	bool close_shown_ = false;
	Vector2i close_position_;
	ObjectID hud_id_;
	ObjectID sim_id_;
	Rect2i design_rect_;
	HudMapPassRenderer renderer_;
	double tick_accum_ = 0.0;
	bool pass_visible_ = false;
	int pass_terrain_tris_ = 0;
	int pass_sprites_ = 0;
	int pass_labels_ = 0;
	int pass_over_lines_ = 0;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MapViewWindow::ViewKind);
VARIANT_ENUM_CAST(godot::MapViewWindow::CommandToggle);
VARIANT_ENUM_CAST(godot::MapViewWindow::CommandTab);
