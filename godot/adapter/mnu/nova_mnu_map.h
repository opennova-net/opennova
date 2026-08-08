#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <vector>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A tactical map view (type="map"). There is no map data in the editor, so the
// builder shows the configured appearance/background (or a labeled placeholder in
// edit_mode). At runtime a shell supplies a map texture + markers and the view
// pans (drag) and zooms (wheel) over the static image.
//
// Honest scope: this is static-image pan/zoom, not a real projected tactical map;
// the marker coordinate space is shell-defined (pixels in the supplied texture).
class NovaMnuMap : public Control {
	GDCLASS(NovaMnuMap, Control)

private:
	struct Marker {
		int id = 0;
		Ref<Texture2D> icon;
		Vector2 map_pos;
		TextureRect *node = nullptr;
	};

	MnuWidgetBehavior behavior_;
	Ref<Texture2D> map_texture_;
	Vector2 pan_;
	float zoom_ = 1.0f;
	float min_zoom_ = 0.25f;
	float max_zoom_ = 4.0f;
	std::vector<Marker> markers_;
	int next_marker_id_ = 1;
	TextureRect *map_image_ = nullptr;
	Control *markers_layer_ = nullptr;
	bool dragging_ = false;

	void ensure_layers();
	void apply_view();

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _gui_input(const Ref<InputEvent> &p_event) override;

	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }

	// --- Runtime data binding ---
	void set_map_texture(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_map_texture() const { return map_texture_; }
	void set_pan(const Vector2 &p_pan);
	Vector2 get_pan() const { return pan_; }
	void set_zoom(float p_zoom);
	float get_zoom() const { return zoom_; }
	void center_on(const Vector2 &p_map_point);
	int add_marker(const Ref<Texture2D> &p_icon, const Vector2 &p_map_pos);
	void remove_marker(int p_id);
	void clear_markers();
	int get_marker_count() const { return static_cast<int>(markers_.size()); }
};

} // namespace godot
