#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A campaign-select globe (type="globe"). There is no globe data in the editor, so
// the builder shows the configured appearance (or a labeled placeholder). At runtime
// a shell supplies a globe image; the view can auto-rotate.
//
// Honest scope: this is a 2D fake (the supplied image is spun in-plane), NOT a real
// textured 3D sphere. A faithful sphere (SubViewport + MeshInstance3D) is future
// work; the data API is shaped so that upgrade is transparent to shells.
class NovaMnuGlobe : public Control {
	GDCLASS(NovaMnuGlobe, Control)

private:
	MnuWidgetBehavior behavior_;
	Ref<Texture2D> globe_texture_;
	float rotation_speed_ = 0.0f; // degrees per second
	float angle_ = 0.0f;
	bool auto_rotate_ = false;
	TextureRect *globe_image_ = nullptr;

	void ensure_image();
	void apply_angle();
	void update_processing();

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _process(double p_delta) override;

	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }

	// --- Runtime data binding ---
	void set_globe_texture(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_globe_texture() const { return globe_texture_; }
	void set_rotation_speed(float p_deg_per_sec);
	float get_rotation_speed() const { return rotation_speed_; }
	void set_auto_rotate(bool p_value);
	bool get_auto_rotate() const { return auto_rotate_; }
	void set_angle(float p_deg);
	float get_angle() const { return angle_; }
};

} // namespace godot
