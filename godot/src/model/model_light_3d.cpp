#include "model/model_light_3d.h"

#include "model/model_frames.h"

using namespace godot;
using namespace opennova::threedi;

namespace {

uint8_t unit_byte(float value) {
	const double v = static_cast<double>(value) * 255.0;
	const int r = static_cast<int>(v >= 0.0 ? (v + 0.5) : (v - 0.5));
	return static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
}

// The file packs B, G, R, unused; a Color reads and writes R, G, B, A.
Color color_from_bgr(const uint8_t p_bgr[4]) {
	return Color(p_bgr[2] / 255.0f, p_bgr[1] / 255.0f, p_bgr[0] / 255.0f, p_bgr[3] / 255.0f);
}

void bgr_from_color(const Color &p_color, uint8_t r_bgr[4]) {
	r_bgr[0] = unit_byte(p_color.b);
	r_bgr[1] = unit_byte(p_color.g);
	r_bgr[2] = unit_byte(p_color.r);
	r_bgr[3] = unit_byte(p_color.a);
}

} // namespace

void ModelLight3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atten_start", "value"), &ModelLight3D::set_atten_start);
	ClassDB::bind_method(D_METHOD("get_atten_start"), &ModelLight3D::get_atten_start);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "atten_start"), "set_atten_start", "get_atten_start");
	ClassDB::bind_method(D_METHOD("set_atten_end", "value"), &ModelLight3D::set_atten_end);
	ClassDB::bind_method(D_METHOD("get_atten_end"), &ModelLight3D::get_atten_end);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "atten_end"), "set_atten_end", "get_atten_end");
	ClassDB::bind_method(D_METHOD("set_style", "value"), &ModelLight3D::set_style);
	ClassDB::bind_method(D_METHOD("get_style"), &ModelLight3D::get_style);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "style"), "set_style", "get_style");
	ClassDB::bind_method(D_METHOD("set_phase", "value"), &ModelLight3D::set_phase);
	ClassDB::bind_method(D_METHOD("get_phase"), &ModelLight3D::get_phase);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "phase"), "set_phase", "get_phase");
	ClassDB::bind_method(D_METHOD("set_rate", "value"), &ModelLight3D::set_rate);
	ClassDB::bind_method(D_METHOD("get_rate"), &ModelLight3D::get_rate);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rate"), "set_rate", "get_rate");
	ClassDB::bind_method(D_METHOD("set_color_start", "value"), &ModelLight3D::set_color_start);
	ClassDB::bind_method(D_METHOD("get_color_start"), &ModelLight3D::get_color_start);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_start"), "set_color_start", "get_color_start");
	ClassDB::bind_method(D_METHOD("set_color_end", "value"), &ModelLight3D::set_color_end);
	ClassDB::bind_method(D_METHOD("get_color_end"), &ModelLight3D::get_color_end);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_end"), "set_color_end", "get_color_end");
	ClassDB::bind_method(D_METHOD("set_subobject", "value"), &ModelLight3D::set_subobject);
	ClassDB::bind_method(D_METHOD("get_subobject"), &ModelLight3D::get_subobject);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "subobject"), "set_subobject", "get_subobject");
	ClassDB::bind_method(D_METHOD("set_flags", "value"), &ModelLight3D::set_flags);
	ClassDB::bind_method(D_METHOD("get_flags"), &ModelLight3D::get_flags);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags", PROPERTY_HINT_FLAGS,
						 "Disable corona:1,Disable terrain:2,Disable objects:4,Target type:8"),
			"set_flags", "get_flags");
	ClassDB::bind_method(D_METHOD("set_unknown1", "value"), &ModelLight3D::set_unknown1);
	ClassDB::bind_method(D_METHOD("get_unknown1"), &ModelLight3D::get_unknown1);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "unknown1"), "set_unknown1", "get_unknown1");
	ClassDB::bind_method(D_METHOD("set_falloff_byte", "value"), &ModelLight3D::set_falloff_byte);
	ClassDB::bind_method(D_METHOD("get_falloff_byte"), &ModelLight3D::get_falloff_byte);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "falloff_byte"), "set_falloff_byte", "get_falloff_byte");
	ClassDB::bind_method(D_METHOD("set_rotation_words", "value"), &ModelLight3D::set_rotation_words);
	ClassDB::bind_method(D_METHOD("get_rotation_words"), &ModelLight3D::get_rotation_words);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "rotation_words"), "set_rotation_words",
			"get_rotation_words");
	ClassDB::bind_method(D_METHOD("set_view_proj", "value"), &ModelLight3D::set_view_proj);
	ClassDB::bind_method(D_METHOD("get_view_proj"), &ModelLight3D::get_view_proj);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "view_proj"), "set_view_proj", "get_view_proj");
}

void ModelLight3D::set_color_start(const Color &p_value) { bgr_from_color(p_value, color_start_); }
Color ModelLight3D::get_color_start() const { return color_from_bgr(color_start_); }
void ModelLight3D::set_color_end(const Color &p_value) { bgr_from_color(p_value, color_end_); }
Color ModelLight3D::get_color_end() const { return color_from_bgr(color_end_); }

void ModelLight3D::assign(const ThreediLight &p_light) {
	set_position(presentation_from_model(p_light.offset[0], p_light.offset[1], p_light.offset[2]));
	atten_start_ = p_light.atten_start;
	atten_end_ = p_light.atten_end;
	style_ = p_light.style;
	phase_ = p_light.phase;
	rate_ = p_light.rate;
	for (int k = 0; k < 4; ++k) {
		color_start_[k] = p_light.color_start[k];
		color_end_[k] = p_light.color_end[k];
	}
	subobject_ = p_light.subobj_index;
	flags_ = p_light.flags;
	unknown1_ = p_light.unknown1;
	falloff_byte_ = p_light.falloff_byte;
	rotation_.resize(4);
	for (int k = 0; k < 4; ++k) {
		rotation_[k] = p_light.rotation[k];
	}
	view_proj_.resize(16);
	for (int k = 0; k < 16; ++k) {
		view_proj_[k] = p_light.view_proj[k];
	}
}

bool ModelLight3D::write(ThreediLight &r_light) const {
	r_light = ThreediLight{};
	const opennova::threedi::ThreediBuildVec3 offset = model_from_presentation(get_position());
	r_light.offset[0] = static_cast<float>(offset.x);
	r_light.offset[1] = static_cast<float>(offset.y);
	r_light.offset[2] = static_cast<float>(offset.z);
	r_light.atten_start = atten_start_;
	r_light.atten_end = atten_end_;
	r_light.style = static_cast<uint8_t>(style_);
	r_light.phase = static_cast<uint8_t>(phase_);
	r_light.rate = static_cast<uint16_t>(rate_);
	for (int k = 0; k < 4; ++k) {
		r_light.color_start[k] = color_start_[k];
		r_light.color_end[k] = color_end_[k];
	}
	r_light.subobj_index = static_cast<uint8_t>(subobject_);
	r_light.flags = static_cast<uint8_t>(flags_);
	r_light.unknown1 = static_cast<uint8_t>(unknown1_);
	r_light.falloff_byte = static_cast<uint8_t>(falloff_byte_);
	if (rotation_.size() != 0 && rotation_.size() != 4) {
		return false;
	}
	if (view_proj_.size() != 0 && view_proj_.size() != 16) {
		return false;
	}
	for (int k = 0; k < rotation_.size(); ++k) {
		r_light.rotation[k] = rotation_[k];
	}
	for (int k = 0; k < view_proj_.size(); ++k) {
		r_light.view_proj[k] = view_proj_[k];
	}
	return true;
}
