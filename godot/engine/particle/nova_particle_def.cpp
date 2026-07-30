#include "nova_particle_def.h"

#include <algorithm>
#include <cstdint>

using namespace godot;

namespace {

Color color3_to_godot(const opennova::particle::Color3 &c) {
	return Color(static_cast<float>(c.r) / 255.0f,
			static_cast<float>(c.g) / 255.0f,
			static_cast<float>(c.b) / 255.0f, 1.0f);
}

opennova::particle::Color3 godot_to_color3(const Color &c) {
	opennova::particle::Color3 out;
	out.r = static_cast<std::uint8_t>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
	out.g = static_cast<std::uint8_t>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
	out.b = static_cast<std::uint8_t>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
	return out;
}

Vector3 vec3_to_godot(const opennova::particle::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

opennova::particle::Vec3 godot_to_vec3(const Vector3 &v) {
	opennova::particle::Vec3 out;
	out.x = v.x;
	out.y = v.y;
	out.z = v.z;
	return out;
}

void ensure_curve(Ref<NovaParticleCurveRef> &r) {
	if (r.is_null()) {
		r.instantiate();
	}
}

} // namespace

NovaParticleDef::NovaParticleDef() {
	emit_rate_func.instantiate();
	scale_func.instantiate();
	alpha_func.instantiate();
	red_func.instantiate();
	green_func.instantiate();
	blue_func.instantiate();
	graphics.resize(4);
	for (int i = 0; i < 4; ++i) {
		Ref<NovaParticleGraphicLayer> g;
		g.instantiate();
		graphics[i] = g;
	}
}

static_assert(NovaParticleDef::FLAG_YAW_AND_PITCH == opennova::particle::particle_flag::YawAndPitch);
static_assert(NovaParticleDef::FLAG_FOREVER_EMIT == opennova::particle::particle_flag::ForeverEmit);
static_assert(NovaParticleDef::FLAG_POSITION_RELATIVE == opennova::particle::particle_flag::PositionRelative);

void NovaParticleDef::_bind_methods() {
	#define BIND_GETSET(prop, setter, getter) \
		ClassDB::bind_method(D_METHOD(setter, "v"), &NovaParticleDef::set_##prop); \
		ClassDB::bind_method(D_METHOD(getter), &NovaParticleDef::get_##prop)

	BIND_GETSET(id, "set_id", "get_id");
	BIND_GETSET(child_id, "set_child_id", "get_child_id");
	BIND_GETSET(flags_raw, "set_flags_raw", "get_flags_raw");
	BIND_GETSET(flags, "set_flags", "get_flags");
	BIND_GETSET(move_raw, "set_move_raw", "get_move_raw");
	BIND_GETSET(move, "set_move", "get_move");
	BIND_GETSET(lod, "set_lod", "get_lod");

	BIND_GETSET(emit_dur, "set_emit_dur", "get_emit_dur");
	BIND_GETSET(emit_dur_adj, "set_emit_dur_adj", "get_emit_dur_adj");
	BIND_GETSET(emit_rate, "set_emit_rate", "get_emit_rate");
	BIND_GETSET(emit_rate_adj, "set_emit_rate_adj", "get_emit_rate_adj");
	BIND_GETSET(emit_rate_func, "set_emit_rate_func", "get_emit_rate_func");
	BIND_GETSET(emit_delay, "set_emit_delay", "get_emit_delay");
	BIND_GETSET(emit_burst, "set_emit_burst", "get_emit_burst");
	BIND_GETSET(emit_maxoverride, "set_emit_maxoverride", "get_emit_maxoverride");
	BIND_GETSET(emit_shape, "set_emit_shape", "get_emit_shape");
	BIND_GETSET(emit_shape_size, "set_emit_shape_size", "get_emit_shape_size");
	BIND_GETSET(emit_shape_size_skip, "set_emit_shape_size_skip", "get_emit_shape_size_skip");

	BIND_GETSET(y_offset, "set_y_offset", "get_y_offset");
	BIND_GETSET(z_offset, "set_z_offset", "get_z_offset");
	BIND_GETSET(age, "set_age", "get_age");
	BIND_GETSET(age_adj, "set_age_adj", "get_age_adj");
	BIND_GETSET(scale_value, "set_scale_value", "get_scale_value");
	BIND_GETSET(scale_adj, "set_scale_adj", "get_scale_adj");
	BIND_GETSET(scale_func, "set_scale_func", "get_scale_func");

	BIND_GETSET(alpha, "set_alpha", "get_alpha");
	BIND_GETSET(alpha_func, "set_alpha_func", "get_alpha_func");
	BIND_GETSET(red_func, "set_red_func", "get_red_func");
	BIND_GETSET(green_func, "set_green_func", "get_green_func");
	BIND_GETSET(blue_func, "set_blue_func", "get_blue_func");
	BIND_GETSET(color1, "set_color1", "get_color1");
	BIND_GETSET(color2, "set_color2", "get_color2");
	BIND_GETSET(color3_prop, "set_color3_prop", "get_color3_prop");
	BIND_GETSET(color4, "set_color4", "get_color4");
	BIND_GETSET(bump_scale, "set_bump_scale", "get_bump_scale");

	BIND_GETSET(orientation, "set_orientation", "get_orientation");
	BIND_GETSET(orientationadj, "set_orientationadj", "get_orientationadj");
	BIND_GETSET(yaw_rot, "set_yaw_rot", "get_yaw_rot");
	BIND_GETSET(yaw_rot_adj, "set_yaw_rot_adj", "get_yaw_rot_adj");
	BIND_GETSET(pitch_rot, "set_pitch_rot", "get_pitch_rot");
	BIND_GETSET(pitch_rot_adj, "set_pitch_rot_adj", "get_pitch_rot_adj");
	BIND_GETSET(roll_rot, "set_roll_rot", "get_roll_rot");
	BIND_GETSET(roll_rot_adj, "set_roll_rot_adj", "get_roll_rot_adj");
	BIND_GETSET(speed, "set_speed", "get_speed");
	BIND_GETSET(speed_adj, "set_speed_adj", "get_speed_adj");
	BIND_GETSET(elastic, "set_elastic", "get_elastic");
	BIND_GETSET(gravity, "set_gravity", "get_gravity");
	BIND_GETSET(gravity_mask, "set_gravity_mask", "get_gravity_mask");
	BIND_GETSET(drag, "set_drag", "get_drag");
	BIND_GETSET(spread, "set_spread", "get_spread");
	BIND_GETSET(spread_skip, "set_spread_skip", "get_spread_skip");
	BIND_GETSET(orbitalspeed, "set_orbitalspeed", "get_orbitalspeed");
	BIND_GETSET(orbitalspeed_adj, "set_orbitalspeed_adj", "get_orbitalspeed_adj");
	BIND_GETSET(orbital_axis, "set_orbital_axis", "get_orbital_axis");

	BIND_GETSET(collide_sounds, "set_collide_sounds", "get_collide_sounds");
	BIND_GETSET(graphics, "set_graphics", "get_graphics");
	BIND_GETSET(unknown_keys, "set_unknown_keys", "get_unknown_keys");

	#undef BIND_GETSET

	// Canonical-table introspection + deep clone (editor helpers).
	ClassDB::bind_static_method("NovaParticleDef", D_METHOD("get_particle_flag_table"), &NovaParticleDef::get_particle_flag_table);
	BIND_CONSTANT(FLAG_YAW_AND_PITCH);
	BIND_CONSTANT(FLAG_FOREVER_EMIT);
	BIND_CONSTANT(FLAG_POSITION_RELATIVE);
	ClassDB::bind_static_method("NovaParticleDef", D_METHOD("get_move_flag_table"), &NovaParticleDef::get_move_flag_table);
	ClassDB::bind_static_method("NovaParticleDef", D_METHOD("get_blend_mode_names"), &NovaParticleDef::get_blend_mode_names);
	ClassDB::bind_static_method("NovaParticleDef", D_METHOD("format_particle_flags", "bits"), &NovaParticleDef::format_particle_flags);
	ClassDB::bind_static_method("NovaParticleDef", D_METHOD("format_move_flags", "bits"), &NovaParticleDef::format_move_flags);
	ClassDB::bind_method(D_METHOD("clone"), &NovaParticleDef::clone);

	ADD_GROUP("Identity", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "child_id"), "set_child_id", "get_child_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "flags_raw"), "set_flags_raw", "get_flags_raw");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags"), "set_flags", "get_flags");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "move_raw"), "set_move_raw", "get_move_raw");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "move"), "set_move", "get_move");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod"), "set_lod", "get_lod");

	ADD_GROUP("Emission", "emit_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emit_dur"), "set_emit_dur", "get_emit_dur");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emit_dur_adj"), "set_emit_dur_adj", "get_emit_dur_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emit_rate"), "set_emit_rate", "get_emit_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emit_rate_adj"), "set_emit_rate_adj", "get_emit_rate_adj");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "emit_rate_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_emit_rate_func", "get_emit_rate_func");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emit_delay"), "set_emit_delay", "get_emit_delay");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "emit_burst"), "set_emit_burst", "get_emit_burst");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "emit_maxoverride"), "set_emit_maxoverride", "get_emit_maxoverride");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "emit_shape", PROPERTY_HINT_ENUM, "Point,Box,Sphere,Cone"),
			"set_emit_shape", "get_emit_shape");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "emit_shape_size"), "set_emit_shape_size", "get_emit_shape_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "emit_shape_size_skip"), "set_emit_shape_size_skip", "get_emit_shape_size_skip");

	ADD_GROUP("Lifetime", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "age"), "set_age", "get_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "age_adj"), "set_age_adj", "get_age_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "y_offset"), "set_y_offset", "get_y_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_offset"), "set_z_offset", "get_z_offset");

	ADD_GROUP("Visual", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha"), "set_alpha", "get_alpha");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color1"), "set_color1", "get_color1");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color2"), "set_color2", "get_color2");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color3"), "set_color3_prop", "get_color3_prop");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color4"), "set_color4", "get_color4");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bump_scale"), "set_bump_scale", "get_bump_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_value"), "set_scale_value", "get_scale_value");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale_adj"), "set_scale_adj", "get_scale_adj");

	ADD_GROUP("Curves", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scale_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_scale_func", "get_scale_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "alpha_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_alpha_func", "get_alpha_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "red_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_red_func", "get_red_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "green_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_green_func", "get_green_func");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blue_func", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleCurveRef"),
			"set_blue_func", "get_blue_func");

	ADD_GROUP("Motion", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "orientation"), "set_orientation", "get_orientation");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "orientationadj"), "set_orientationadj", "get_orientationadj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yaw_rot"), "set_yaw_rot", "get_yaw_rot");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yaw_rot_adj"), "set_yaw_rot_adj", "get_yaw_rot_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_rot"), "set_pitch_rot", "get_pitch_rot");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_rot_adj"), "set_pitch_rot_adj", "get_pitch_rot_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roll_rot"), "set_roll_rot", "get_roll_rot");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roll_rot_adj"), "set_roll_rot_adj", "get_roll_rot_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed"), "set_speed", "get_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed_adj"), "set_speed_adj", "get_speed_adj");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "elastic"), "set_elastic", "get_elastic");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "gravity_mask"), "set_gravity_mask", "get_gravity_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag"), "set_drag", "get_drag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread"), "set_spread", "get_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spread_skip"), "set_spread_skip", "get_spread_skip");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "orbitalspeed"), "set_orbitalspeed", "get_orbitalspeed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "orbitalspeed_adj"), "set_orbitalspeed_adj", "get_orbitalspeed_adj");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "orbital_axis"), "set_orbital_axis", "get_orbital_axis");

	ADD_GROUP("Sounds + Graphics", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "collide_sounds"), "set_collide_sounds", "get_collide_sounds");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "graphics", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleGraphicLayer"),
			"set_graphics", "get_graphics");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "unknown_keys", PROPERTY_HINT_NONE, "",
			PROPERTY_USAGE_STORAGE), "set_unknown_keys", "get_unknown_keys");
}

#define TRIVIAL_SET(name, type) \
	void NovaParticleDef::set_##name(type v) { name = v; emit_changed(); } \
	type NovaParticleDef::get_##name() const { return name; }

#define TRIVIAL_SET_REF(name, type) \
	void NovaParticleDef::set_##name(const type &v) { name = v; emit_changed(); } \
	type NovaParticleDef::get_##name() const { return name; }

TRIVIAL_SET_REF(id, String)
TRIVIAL_SET_REF(child_id, String)
TRIVIAL_SET_REF(flags_raw, String)
TRIVIAL_SET(flags, int)
TRIVIAL_SET_REF(move_raw, String)
TRIVIAL_SET(move, int)
TRIVIAL_SET(lod, float)

TRIVIAL_SET(emit_dur, float)
TRIVIAL_SET(emit_dur_adj, float)
TRIVIAL_SET(emit_rate, float)
TRIVIAL_SET(emit_rate_adj, float)
TRIVIAL_SET_REF(emit_rate_func, Ref<NovaParticleCurveRef>)
TRIVIAL_SET(emit_delay, float)
TRIVIAL_SET(emit_burst, int)
TRIVIAL_SET(emit_maxoverride, int)
TRIVIAL_SET(emit_shape, int)
TRIVIAL_SET_REF(emit_shape_size, Vector3)
TRIVIAL_SET_REF(emit_shape_size_skip, Vector3)

TRIVIAL_SET(y_offset, float)
TRIVIAL_SET(z_offset, float)
TRIVIAL_SET(age, float)
TRIVIAL_SET(age_adj, float)
TRIVIAL_SET(scale_value, float)
TRIVIAL_SET(scale_adj, float)
TRIVIAL_SET_REF(scale_func, Ref<NovaParticleCurveRef>)

TRIVIAL_SET(alpha, float)
TRIVIAL_SET_REF(alpha_func, Ref<NovaParticleCurveRef>)
TRIVIAL_SET_REF(red_func, Ref<NovaParticleCurveRef>)
TRIVIAL_SET_REF(green_func, Ref<NovaParticleCurveRef>)
TRIVIAL_SET_REF(blue_func, Ref<NovaParticleCurveRef>)
TRIVIAL_SET_REF(color1, Color)
TRIVIAL_SET_REF(color2, Color)

void NovaParticleDef::set_color3_prop(const Color &c) { color3 = c; emit_changed(); }
Color NovaParticleDef::get_color3_prop() const { return color3; }

TRIVIAL_SET_REF(color4, Color)
TRIVIAL_SET(bump_scale, float)

TRIVIAL_SET_REF(orientation, Vector3)
TRIVIAL_SET_REF(orientationadj, Vector3)
TRIVIAL_SET(yaw_rot, float)
TRIVIAL_SET(yaw_rot_adj, float)
TRIVIAL_SET(pitch_rot, float)
TRIVIAL_SET(pitch_rot_adj, float)
TRIVIAL_SET(roll_rot, float)
TRIVIAL_SET(roll_rot_adj, float)
TRIVIAL_SET(speed, float)
TRIVIAL_SET(speed_adj, float)
TRIVIAL_SET(elastic, float)
TRIVIAL_SET(gravity, float)
TRIVIAL_SET_REF(gravity_mask, Vector3)
TRIVIAL_SET(drag, float)
TRIVIAL_SET(spread, float)
TRIVIAL_SET(spread_skip, float)
TRIVIAL_SET(orbitalspeed, float)
TRIVIAL_SET(orbitalspeed_adj, float)
TRIVIAL_SET_REF(orbital_axis, Vector3)

TRIVIAL_SET_REF(collide_sounds, PackedStringArray)
TRIVIAL_SET_REF(graphics, TypedArray<NovaParticleGraphicLayer>)

void NovaParticleDef::set_unknown_keys(const Array &v) {
	unknown_keys = v.duplicate(true);
	emit_changed();
}

Array NovaParticleDef::get_unknown_keys() const {
	return unknown_keys.duplicate(true);
}

#undef TRIVIAL_SET
#undef TRIVIAL_SET_REF

void NovaParticleDef::copy_from_native(const opennova::particle::ParticleDef &def) {
	id = String::utf8(def.id.c_str());
	child_id = String::utf8(def.child_id.c_str());
	flags_raw = String::utf8(def.flags_raw.c_str());
	flags = static_cast<int>(def.flags);
	move_raw = String::utf8(def.move_raw.c_str());
	move = static_cast<int>(def.move);
	lod = def.lod;

	emit_dur = def.emit_dur;
	emit_dur_adj = def.emit_dur_adj;
	emit_rate = def.emit_rate;
	emit_rate_adj = def.emit_rate_adj;
	ensure_curve(emit_rate_func);
	emit_rate_func->copy_from_native(def.emit_rate_func);
	emit_delay = def.emit_delay;
	emit_burst = def.emit_burst;
	emit_maxoverride = def.emit_maxoverride;
	emit_shape = def.emit_shape;
	emit_shape_size = vec3_to_godot(def.emit_shape_size);
	emit_shape_size_skip = vec3_to_godot(def.emit_shape_size_skip);

	y_offset = def.y_offset;
	z_offset = def.z_offset;
	age = def.age;
	age_adj = def.age_adj;
	scale_value = def.scale;
	scale_adj = def.scale_adj;
	ensure_curve(scale_func);
	scale_func->copy_from_native(def.scale_func);

	alpha = def.alpha;
	ensure_curve(alpha_func);
	alpha_func->copy_from_native(def.alpha_func);
	ensure_curve(red_func);
	red_func->copy_from_native(def.red_func);
	ensure_curve(green_func);
	green_func->copy_from_native(def.green_func);
	ensure_curve(blue_func);
	blue_func->copy_from_native(def.blue_func);
	color1 = color3_to_godot(def.color1);
	color2 = color3_to_godot(def.color2);
	color3 = color3_to_godot(def.color3);
	color4 = color3_to_godot(def.color4);
	bump_scale = def.bump_scale;

	orientation = vec3_to_godot(def.orientation);
	orientationadj = vec3_to_godot(def.orientationadj);
	yaw_rot = def.yaw_rot;
	yaw_rot_adj = def.yaw_rot_adj;
	pitch_rot = def.pitch_rot;
	pitch_rot_adj = def.pitch_rot_adj;
	roll_rot = def.roll_rot;
	roll_rot_adj = def.roll_rot_adj;
	speed = def.speed;
	speed_adj = def.speed_adj;
	elastic = def.elastic;
	gravity = def.gravity;
	gravity_mask = vec3_to_godot(def.gravity_mask);
	drag = def.drag;
	spread = def.spread;
	spread_skip = def.spread_skip;
	orbitalspeed = def.orbitalspeed;
	orbitalspeed_adj = def.orbitalspeed_adj;
	orbital_axis = vec3_to_godot(def.orbital_axis);

	collide_sounds.clear();
	collide_sounds.resize(static_cast<int>(def.collide_sounds.size()));
	for (int i = 0; i < static_cast<int>(def.collide_sounds.size()); ++i) {
		collide_sounds[i] = String::utf8(def.collide_sounds[static_cast<size_t>(i)].c_str());
	}

	graphics.clear();
	graphics.resize(4);
	for (int i = 0; i < 4; ++i) {
		Ref<NovaParticleGraphicLayer> g;
		g.instantiate();
		g->copy_from_native(def.graphics[static_cast<size_t>(i)]);
		graphics[i] = g;
	}
	unknown_keys.clear();
	for (const auto &entry : def.unknown_keys) {
		Dictionary item;
		item["key"] = String::utf8(entry.first.c_str());
		item["value"] = String::utf8(entry.second.c_str());
		unknown_keys.push_back(item);
	}
	emit_changed();
}

Dictionary NovaParticleDef::get_particle_flag_table() {
	Dictionary out;
	for (const auto &entry : opennova::particle::particle_flag_entries()) {
		out[String::utf8(entry.first.c_str())] = static_cast<int>(entry.second);
	}
	return out;
}

Dictionary NovaParticleDef::get_move_flag_table() {
	Dictionary out;
	for (const auto &entry : opennova::particle::move_flag_entries()) {
		out[String::utf8(entry.first.c_str())] = static_cast<int>(entry.second);
	}
	return out;
}

PackedStringArray NovaParticleDef::get_blend_mode_names() {
	PackedStringArray out;
	for (const std::string &name : opennova::particle::blend_mode_names()) {
		out.push_back(String::utf8(name.c_str()));
	}
	return out;
}

String NovaParticleDef::format_particle_flags(int bits) {
	return String::utf8(opennova::particle::format_particle_flags(static_cast<std::uint32_t>(bits)).c_str());
}

String NovaParticleDef::format_move_flags(int bits) {
	return String::utf8(opennova::particle::format_move_bits(static_cast<std::uint32_t>(bits)).c_str());
}

Ref<NovaParticleDef> NovaParticleDef::clone() const {
	Ref<NovaParticleDef> out;
	out.instantiate();
	out->copy_from_native(this->to_native());
	return out;
}

opennova::particle::ParticleDef NovaParticleDef::to_native() const {
	opennova::particle::ParticleDef out;
	out.id = id.utf8().get_data();
	out.child_id = child_id.utf8().get_data();
	out.flags_raw = flags_raw.utf8().get_data();
	out.flags = static_cast<std::uint32_t>(flags);
	out.move_raw = move_raw.utf8().get_data();
	out.move = static_cast<std::uint32_t>(move);
	out.lod = lod;

	out.emit_dur = emit_dur;
	out.emit_dur_adj = emit_dur_adj;
	out.emit_rate = emit_rate;
	out.emit_rate_adj = emit_rate_adj;
	if (emit_rate_func.is_valid()) out.emit_rate_func = emit_rate_func->to_native();
	out.emit_delay = emit_delay;
	out.emit_burst = emit_burst;
	out.emit_maxoverride = emit_maxoverride;
	out.emit_shape = emit_shape;
	out.emit_shape_size = godot_to_vec3(emit_shape_size);
	out.emit_shape_size_skip = godot_to_vec3(emit_shape_size_skip);

	out.y_offset = y_offset;
	out.z_offset = z_offset;
	out.age = age;
	out.age_adj = age_adj;
	out.scale = scale_value;
	out.scale_adj = scale_adj;
	if (scale_func.is_valid()) out.scale_func = scale_func->to_native();

	out.alpha = alpha;
	if (alpha_func.is_valid()) out.alpha_func = alpha_func->to_native();
	if (red_func.is_valid()) out.red_func = red_func->to_native();
	if (green_func.is_valid()) out.green_func = green_func->to_native();
	if (blue_func.is_valid()) out.blue_func = blue_func->to_native();
	out.color1 = godot_to_color3(color1);
	out.color2 = godot_to_color3(color2);
	out.color3 = godot_to_color3(color3);
	out.color4 = godot_to_color3(color4);
	out.bump_scale = bump_scale;

	out.orientation = godot_to_vec3(orientation);
	out.orientationadj = godot_to_vec3(orientationadj);
	out.yaw_rot = yaw_rot;
	out.yaw_rot_adj = yaw_rot_adj;
	out.pitch_rot = pitch_rot;
	out.pitch_rot_adj = pitch_rot_adj;
	out.roll_rot = roll_rot;
	out.roll_rot_adj = roll_rot_adj;
	out.speed = speed;
	out.speed_adj = speed_adj;
	out.elastic = elastic;
	out.gravity = gravity;
	out.gravity_mask = godot_to_vec3(gravity_mask);
	out.drag = drag;
	out.spread = spread;
	out.spread_skip = spread_skip;
	out.orbitalspeed = orbitalspeed;
	out.orbitalspeed_adj = orbitalspeed_adj;
	out.orbital_axis = godot_to_vec3(orbital_axis);

	for (int i = 0; i < std::min<int>(20, collide_sounds.size()); ++i) {
		out.collide_sounds[static_cast<size_t>(i)] = collide_sounds[i].utf8().get_data();
	}

	for (int i = 0; i < std::min<int>(4, graphics.size()); ++i) {
		Ref<NovaParticleGraphicLayer> g = graphics[i];
		if (g.is_valid()) {
			out.graphics[static_cast<size_t>(i)] = g->to_native();
		}
	}
	for (int i = 0; i < unknown_keys.size(); ++i) {
		if (unknown_keys[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary item = unknown_keys[i];
		const String key = item.get("key", String());
		if (key.is_empty()) {
			continue;
		}
		const String value = item.get("value", String());
		out.unknown_keys.emplace_back(key.utf8().get_data(), value.utf8().get_data());
	}

	return out;
}
