#include "nova_mnu_globe.h"

using namespace godot;

static const double MNU_DEG2RAD = 0.017453292519943295;

void NovaMnuGlobe::ensure_image() {
	if (globe_image_ != nullptr) {
		return;
	}
	globe_image_ = memnew(TextureRect);
	globe_image_->set_name("GlobeImage");
	globe_image_->set_anchors_preset(Control::PRESET_FULL_RECT);
	globe_image_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	globe_image_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	globe_image_->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	if (globe_texture_.is_valid()) {
		globe_image_->set_texture(globe_texture_);
	}
	add_child(globe_image_);
}

void NovaMnuGlobe::apply_angle() {
	if (globe_image_ == nullptr) {
		return;
	}
	globe_image_->set_pivot_offset(globe_image_->get_size() * 0.5f);
	globe_image_->set_rotation((float)(angle_ * MNU_DEG2RAD));
}

void NovaMnuGlobe::update_processing() {
	const bool run = !behavior_.edit_mode && auto_rotate_ && rotation_speed_ != 0.0f;
	set_process(run);
}

void NovaMnuGlobe::_ready() {
	ensure_image();
	apply_angle();
	update_processing();
}

void NovaMnuGlobe::_process(double p_delta) {
	angle_ += rotation_speed_ * (float)p_delta;
	apply_angle();
}

void NovaMnuGlobe::set_globe_texture(const Ref<Texture2D> &p_tex) {
	globe_texture_ = p_tex;
	ensure_image();
	if (globe_image_ != nullptr) {
		globe_image_->set_texture(p_tex);
	}
}

void NovaMnuGlobe::set_rotation_speed(float p_deg_per_sec) {
	rotation_speed_ = p_deg_per_sec;
	update_processing();
}

void NovaMnuGlobe::set_auto_rotate(bool p_value) {
	auto_rotate_ = p_value;
	update_processing();
}

void NovaMnuGlobe::set_angle(float p_deg) {
	angle_ = p_deg;
	apply_angle();
}

void NovaMnuGlobe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_globe_texture", "tex"), &NovaMnuGlobe::set_globe_texture);
	ClassDB::bind_method(D_METHOD("get_globe_texture"), &NovaMnuGlobe::get_globe_texture);
	ClassDB::bind_method(D_METHOD("set_rotation_speed", "deg_per_sec"), &NovaMnuGlobe::set_rotation_speed);
	ClassDB::bind_method(D_METHOD("get_rotation_speed"), &NovaMnuGlobe::get_rotation_speed);
	ClassDB::bind_method(D_METHOD("set_auto_rotate", "value"), &NovaMnuGlobe::set_auto_rotate);
	ClassDB::bind_method(D_METHOD("get_auto_rotate"), &NovaMnuGlobe::get_auto_rotate);
	ClassDB::bind_method(D_METHOD("set_angle", "deg"), &NovaMnuGlobe::set_angle);
	ClassDB::bind_method(D_METHOD("get_angle"), &NovaMnuGlobe::get_angle);

	ADD_SIGNAL(MethodInfo("region_selected", PropertyInfo(Variant::INT, "marker_id")));
}
