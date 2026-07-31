#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;
class Range;

// A themed scrollbar / slider (type="scroll"). Orientation is fixed at build time
// from ORIENTATION (godot-cpp exposes only the concrete H/VScrollBar leaves, which
// also can't take the discrete SHUTTLE/SCROLLUP/SCROLLDOWN sprite art + per-button
// sounds), so this is a custom-draw Control owning a track TextureRect, two arrow
// TextureButtons, and a draggable shuttle. Exposes Range-like value semantics; a
// shell binds the real min/max/page (the .mnu is a template with no data). This is
// the reusable scroll embedded by NovaMnuCombo's popup and NovaMnuTable.
//
// Inert in edit_mode: the art lays out but the arrows are disabled and no drag is
// wired.
class NovaMnuScroll : public Control {
	GDCLASS(NovaMnuScroll, Control)

private:
	MnuWidgetBehavior behavior_;
	bool vertical_ = true;
	double min_ = 0.0;
	double max_ = 100.0;
	double page_ = 0.0;
	double value_ = 0.0;
	double step_ = 1.0;

	Ref<Texture2D> track_tex_;
	Ref<Texture2D> shuttle_tex_;
	Ref<Texture2D> shuttle_hover_tex_;
	Ref<Texture2D> shuttle_pressed_tex_;
	Ref<Texture2D> shuttle_disabled_tex_;
	Ref<Texture2D> up_tex_;
	Ref<Texture2D> up_hover_tex_;
	Ref<Texture2D> up_pressed_tex_;
	Ref<Texture2D> up_disabled_tex_;
	Ref<Texture2D> down_tex_;
	Ref<Texture2D> down_hover_tex_;
	Ref<Texture2D> down_pressed_tex_;
	Ref<Texture2D> down_disabled_tex_;
	int arrow_extent_ = 16;

	TextureRect *track_ = nullptr;
	TextureButton *arrow_up_ = nullptr;
	TextureButton *arrow_down_ = nullptr;
	TextureRect *shuttle_ = nullptr;

	NodePath scroll_target_path_;
	NodePath range_target_path_;
	bool syncing_range_ = false;
	bool dragging_ = false;
	double travel_ = 0.0; // pixels the shuttle can move (track length - shuttle length)

	double usable_max() const;
	void build_parts();
	void layout();
	void on_arrow_up();
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_arrow_down();
	void apply_scroll_target();
	void update_shuttle_texture(bool p_hovered, bool p_pressed);
	Range *range_target() const;
	void sync_range_target();
	void on_range_value_changed(double p_value);

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _gui_input(const Ref<InputEvent> &p_event) override;

	// --- Build-time configuration ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }
	void set_orientation_vertical(bool p_vertical) { vertical_ = p_vertical; }
	bool is_vertical() const { return vertical_; }
	void set_track_texture(const Ref<Texture2D> &p_tex) { track_tex_ = p_tex; }
	void set_shuttle_textures(const Ref<Texture2D> &p_normal, const Ref<Texture2D> &p_hover) {
		shuttle_tex_ = p_normal;
		shuttle_hover_tex_ = p_hover;
	}
	void set_shuttle_state_textures(const Ref<Texture2D> &p_normal,
			const Ref<Texture2D> &p_hover, const Ref<Texture2D> &p_pressed,
			const Ref<Texture2D> &p_disabled) {
		shuttle_tex_ = p_normal;
		shuttle_hover_tex_ = p_hover;
		shuttle_pressed_tex_ = p_pressed;
		shuttle_disabled_tex_ = p_disabled;
	}
	void set_arrow_textures(const Ref<Texture2D> &p_up, const Ref<Texture2D> &p_up_hover,
			const Ref<Texture2D> &p_down, const Ref<Texture2D> &p_down_hover) {
		up_tex_ = p_up;
		up_hover_tex_ = p_up_hover;
		down_tex_ = p_down;
		down_hover_tex_ = p_down_hover;
	}
	void set_arrow_state_textures(const Ref<Texture2D> &p_up,
			const Ref<Texture2D> &p_up_hover, const Ref<Texture2D> &p_up_pressed,
			const Ref<Texture2D> &p_up_disabled, const Ref<Texture2D> &p_down,
			const Ref<Texture2D> &p_down_hover, const Ref<Texture2D> &p_down_pressed,
			const Ref<Texture2D> &p_down_disabled) {
		up_tex_ = p_up;
		up_hover_tex_ = p_up_hover;
		up_pressed_tex_ = p_up_pressed;
		up_disabled_tex_ = p_up_disabled;
		down_tex_ = p_down;
		down_hover_tex_ = p_down_hover;
		down_pressed_tex_ = p_down_pressed;
		down_disabled_tex_ = p_down_disabled;
	}
	void set_arrow_extent(int p_extent) {
		if (p_extent > 0) {
			arrow_extent_ = p_extent;
		}
	}

	// --- Runtime range API (Range-like; value clamps to [min, max - page]) ---
	void set_min(double p_v);
	double get_min() const { return min_; }
	void set_max(double p_v);
	double get_max() const { return max_; }
	void set_page(double p_v);
	double get_page() const { return page_; }
	void set_step(double p_v) { step_ = p_v; }
	double get_step() const { return step_; }
	void set_value(double p_v);
	double get_value() const { return value_; }
	void set_range(double p_min, double p_max, double p_page);
	void link_scroll_target(const NodePath &p_path);
	// Bind to a Godot Range (ItemList/TextEdit/ScrollContainer native scrollbar)
	// while keeping authored art and pointer handling on this Control.
	void link_range_target(const NodePath &p_path);
	double get_ratio() const;
	void set_ratio(double p_ratio);
};

} // namespace godot
