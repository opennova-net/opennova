#include "particle/particle_graphic_layer.h"

#include <algorithm>
#include <cstdint>

using namespace godot;

ParticleGraphicLayer::ParticleGraphicLayer() {
	scale_func.instantiate();
	alpha_func.instantiate();
	red_func.instantiate();
	green_func.instantiate();
	blue_func.instantiate();
}

Color ParticleGraphicLayer::color3_to_godot(const opennova::particle::Color3 &c) {
	return Color(static_cast<float>(c.r) / 255.0f,
			static_cast<float>(c.g) / 255.0f,
			static_cast<float>(c.b) / 255.0f, 1.0f);
}

opennova::particle::Color3 ParticleGraphicLayer::godot_to_color3(const Color &c) {
	opennova::particle::Color3 out;
	out.r = static_cast<std::uint8_t>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
	out.g = static_cast<std::uint8_t>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
	out.b = static_cast<std::uint8_t>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
	return out;
}

void ParticleGraphicLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_index", "v"), &ParticleGraphicLayer::set_index);
	ClassDB::bind_method(D_METHOD("get_index"), &ParticleGraphicLayer::get_index);
	ClassDB::bind_method(D_METHOD("set_present", "v"), &ParticleGraphicLayer::set_present);
	ClassDB::bind_method(D_METHOD("get_present"), &ParticleGraphicLayer::get_present);
	ClassDB::bind_method(D_METHOD("set_texture", "v"), &ParticleGraphicLayer::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &ParticleGraphicLayer::get_texture);
	ClassDB::bind_method(D_METHOD("set_blend_mode_raw", "v"), &ParticleGraphicLayer::set_blend_mode_raw);
	ClassDB::bind_method(D_METHOD("get_blend_mode_raw"), &ParticleGraphicLayer::get_blend_mode_raw);
	ClassDB::bind_method(D_METHOD("set_blend_mode", "v"), &ParticleGraphicLayer::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &ParticleGraphicLayer::get_blend_mode);
	ClassDB::bind_method(D_METHOD("set_flip_frames", "v"), &ParticleGraphicLayer::set_flip_frames);
	ClassDB::bind_method(D_METHOD("get_flip_frames"), &ParticleGraphicLayer::get_flip_frames);
	ClassDB::bind_method(D_METHOD("set_flip_rate", "v"), &ParticleGraphicLayer::set_flip_rate);
	ClassDB::bind_method(D_METHOD("get_flip_rate"), &ParticleGraphicLayer::get_flip_rate);
	ClassDB::bind_method(D_METHOD("set_color1", "c"), &ParticleGraphicLayer::set_color1);
	ClassDB::bind_method(D_METHOD("get_color1"), &ParticleGraphicLayer::get_color1);
	ClassDB::bind_method(D_METHOD("set_color2", "c"), &ParticleGraphicLayer::set_color2);
	ClassDB::bind_method(D_METHOD("get_color2"), &ParticleGraphicLayer::get_color2);
	ClassDB::bind_method(D_METHOD("set_color3_prop", "c"), &ParticleGraphicLayer::set_color3_prop);
	ClassDB::bind_method(D_METHOD("get_color3_prop"), &ParticleGraphicLayer::get_color3_prop);
	ClassDB::bind_method(D_METHOD("set_color4", "c"), &ParticleGraphicLayer::set_color4);
	ClassDB::bind_method(D_METHOD("get_color4"), &ParticleGraphicLayer::get_color4);
	ClassDB::bind_method(D_METHOD("set_color_overrides_set", "v"), &ParticleGraphicLayer::set_color_overrides_set);
	ClassDB::bind_method(D_METHOD("get_color_overrides_set"), &ParticleGraphicLayer::get_color_overrides_set);
	ClassDB::bind_method(D_METHOD("set_alpha", "v"), &ParticleGraphicLayer::set_alpha);
	ClassDB::bind_method(D_METHOD("get_alpha"), &ParticleGraphicLayer::get_alpha);
	ClassDB::bind_method(D_METHOD("set_scale_value", "v"), &ParticleGraphicLayer::set_scale_value);
	ClassDB::bind_method(D_METHOD("get_scale_value"), &ParticleGraphicLayer::get_scale_value);
	ClassDB::bind_method(D_METHOD("set_scale_adj", "v"), &ParticleGraphicLayer::set_scale_adj);
	ClassDB::bind_method(D_METHOD("get_scale_adj"), &ParticleGraphicLayer::get_scale_adj);
	ClassDB::bind_method(D_METHOD("set_scale_func", "r"), &ParticleGraphicLayer::set_scale_func);
	ClassDB::bind_method(D_METHOD("get_scale_func"), &ParticleGraphicLayer::get_scale_func);
	ClassDB::bind_method(D_METHOD("set_alpha_func", "r"), &ParticleGraphicLayer::set_alpha_func);
	ClassDB::bind_method(D_METHOD("get_alpha_func"), &ParticleGraphicLayer::get_alpha_func);
	ClassDB::bind_method(D_METHOD("set_red_func", "r"), &ParticleGraphicLayer::set_red_func);
	ClassDB::bind_method(D_METHOD("get_red_func"), &ParticleGraphicLayer::get_red_func);
	ClassDB::bind_method(D_METHOD("set_green_func", "r"), &ParticleGraphicLayer::set_green_func);
	ClassDB::bind_method(D_METHOD("get_green_func"), &ParticleGraphicLayer::get_green_func);
	ClassDB::bind_method(D_METHOD("set_blue_func", "r"), &ParticleGraphicLayer::set_blue_func);
	ClassDB::bind_method(D_METHOD("get_blue_func"), &ParticleGraphicLayer::get_blue_func);

	ADD_GROUP("Identity", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "index"), "set_index", "get_index");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "present"), "set_present", "get_present");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "blend_mode_raw"), "set_blend_mode_raw", "get_blend_mode_raw");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM,
			"Blend,Additive,Premult,Bump,Mod,Mod2x,Bumpadd,Distort"),
			"set_blend_mode", "get_blend_mode");

	ADD_GROUP("Animation", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flip_frames"), "set_flip_frames", "get_flip_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flip_rate"), "set_flip_rate", "get_flip_rate");

	ADD_GROUP("Color", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color1"), "set_color1", "get_color1");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color2"), "set_color2", "get_color2");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color3"), "set_color3_prop", "get_color3_prop");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color4"), "set_color4", "get_color4");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "color_overrides_set"), "set_color_overrides_set", "get_color_overrides_set");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha"), "set_alpha", "get_alpha");

	ADD_GROUP("Scale", "scale_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_value"), "set_scale_value", "get_scale_value");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_adj"), "set_scale_adj", "get_scale_adj");

	ADD_GROUP("Curves", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scale_func", PROPERTY_HINT_RESOURCE_TYPE, "ParticleCurveRef"),
			"set_scale_func", "get_scale_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "alpha_func", PROPERTY_HINT_RESOURCE_TYPE, "ParticleCurveRef"),
			"set_alpha_func", "get_alpha_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "red_func", PROPERTY_HINT_RESOURCE_TYPE, "ParticleCurveRef"),
			"set_red_func", "get_red_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "green_func", PROPERTY_HINT_RESOURCE_TYPE, "ParticleCurveRef"),
			"set_green_func", "get_green_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blue_func", PROPERTY_HINT_RESOURCE_TYPE, "ParticleCurveRef"),
			"set_blue_func", "get_blue_func");
}

void ParticleGraphicLayer::set_index(int v) { index = v; emit_changed(); }
int ParticleGraphicLayer::get_index() const { return index; }
void ParticleGraphicLayer::set_present(bool v) { present = v; emit_changed(); }
bool ParticleGraphicLayer::get_present() const { return present; }
void ParticleGraphicLayer::set_texture(const String &v) { texture = v; emit_changed(); }
String ParticleGraphicLayer::get_texture() const { return texture; }
void ParticleGraphicLayer::set_blend_mode_raw(const String &v) { blend_mode_raw = v; emit_changed(); }
String ParticleGraphicLayer::get_blend_mode_raw() const { return blend_mode_raw; }
void ParticleGraphicLayer::set_blend_mode(int v) { blend_mode = std::clamp(v, 0, 7); emit_changed(); }
int ParticleGraphicLayer::get_blend_mode() const { return blend_mode; }
void ParticleGraphicLayer::set_flip_frames(int v) {
	flip_frames = std::clamp(v, 1, opennova::particle::kMaxParticleFlipFrames);
	emit_changed();
}
int ParticleGraphicLayer::get_flip_frames() const { return flip_frames; }
void ParticleGraphicLayer::set_flip_rate(int v) { flip_rate = v; emit_changed(); }
int ParticleGraphicLayer::get_flip_rate() const { return flip_rate; }
void ParticleGraphicLayer::set_color1(const Color &c) { color1 = c; emit_changed(); }
Color ParticleGraphicLayer::get_color1() const { return color1; }
void ParticleGraphicLayer::set_color2(const Color &c) { color2 = c; emit_changed(); }
Color ParticleGraphicLayer::get_color2() const { return color2; }
void ParticleGraphicLayer::set_color3_prop(const Color &c) { color3 = c; emit_changed(); }
Color ParticleGraphicLayer::get_color3_prop() const { return color3; }
void ParticleGraphicLayer::set_color4(const Color &c) { color4 = c; emit_changed(); }
Color ParticleGraphicLayer::get_color4() const { return color4; }
void ParticleGraphicLayer::set_color_overrides_set(bool v) { color_overrides_set = v; emit_changed(); }
bool ParticleGraphicLayer::get_color_overrides_set() const { return color_overrides_set; }
void ParticleGraphicLayer::set_alpha(float v) { alpha = v; emit_changed(); }
float ParticleGraphicLayer::get_alpha() const { return alpha; }
void ParticleGraphicLayer::set_scale_value(float v) { scale_value = v; emit_changed(); }
float ParticleGraphicLayer::get_scale_value() const { return scale_value; }
void ParticleGraphicLayer::set_scale_adj(float v) { scale_adj = v; emit_changed(); }
float ParticleGraphicLayer::get_scale_adj() const { return scale_adj; }
void ParticleGraphicLayer::set_scale_func(const Ref<ParticleCurveRef> &r) { scale_func = r; emit_changed(); }
Ref<ParticleCurveRef> ParticleGraphicLayer::get_scale_func() const { return scale_func; }
void ParticleGraphicLayer::set_alpha_func(const Ref<ParticleCurveRef> &r) { alpha_func = r; emit_changed(); }
Ref<ParticleCurveRef> ParticleGraphicLayer::get_alpha_func() const { return alpha_func; }
void ParticleGraphicLayer::set_red_func(const Ref<ParticleCurveRef> &r) { red_func = r; emit_changed(); }
Ref<ParticleCurveRef> ParticleGraphicLayer::get_red_func() const { return red_func; }
void ParticleGraphicLayer::set_green_func(const Ref<ParticleCurveRef> &r) { green_func = r; emit_changed(); }
Ref<ParticleCurveRef> ParticleGraphicLayer::get_green_func() const { return green_func; }
void ParticleGraphicLayer::set_blue_func(const Ref<ParticleCurveRef> &r) { blue_func = r; emit_changed(); }
Ref<ParticleCurveRef> ParticleGraphicLayer::get_blue_func() const { return blue_func; }

void ParticleGraphicLayer::copy_from_native(const opennova::particle::GraphicLayer &layer) {
	index = layer.index;
	present = layer.present;
	texture = String::utf8(layer.texture.c_str());
	blend_mode_raw = String::utf8(layer.blend_mode_raw.c_str());
	blend_mode = static_cast<int>(layer.blend_mode);
	flip_frames = std::clamp(
			layer.flip_frames, 1, opennova::particle::kMaxParticleFlipFrames);
	flip_rate = layer.flip_rate;
	color1 = color3_to_godot(layer.color1);
	color2 = color3_to_godot(layer.color2);
	color3 = color3_to_godot(layer.color3);
	color4 = color3_to_godot(layer.color4);
	color_overrides_set = layer.color_overrides_set;
	alpha = layer.alpha;
	scale_value = layer.scale;
	scale_adj = layer.scale_adj;
	if (scale_func.is_null()) scale_func.instantiate();
	if (alpha_func.is_null()) alpha_func.instantiate();
	if (red_func.is_null()) red_func.instantiate();
	if (green_func.is_null()) green_func.instantiate();
	if (blue_func.is_null()) blue_func.instantiate();
	scale_func->copy_from_native(layer.scale_func);
	alpha_func->copy_from_native(layer.alpha_func);
	red_func->copy_from_native(layer.red_func);
	green_func->copy_from_native(layer.green_func);
	blue_func->copy_from_native(layer.blue_func);
}

opennova::particle::GraphicLayer ParticleGraphicLayer::to_native() const {
	opennova::particle::GraphicLayer out;
	out.index = index;
	out.present = present;
	out.texture = texture.utf8().get_data();
	out.blend_mode_raw = blend_mode_raw.utf8().get_data();
	out.blend_mode = static_cast<opennova::particle::BlendMode>(std::clamp(blend_mode, 0, 7));
	out.flip_frames = std::clamp(
			flip_frames, 1, opennova::particle::kMaxParticleFlipFrames);
	out.flip_rate = flip_rate;
	out.color1 = godot_to_color3(color1);
	out.color2 = godot_to_color3(color2);
	out.color3 = godot_to_color3(color3);
	out.color4 = godot_to_color3(color4);
	out.color_overrides_set = color_overrides_set;
	out.alpha = alpha;
	out.scale = scale_value;
	out.scale_adj = scale_adj;
	if (scale_func.is_valid()) out.scale_func = scale_func->to_native();
	if (alpha_func.is_valid()) out.alpha_func = alpha_func->to_native();
	if (red_func.is_valid()) out.red_func = red_func->to_native();
	if (green_func.is_valid()) out.green_func = green_func->to_native();
	if (blue_func.is_valid()) out.blue_func = blue_func->to_native();
	return out;
}
