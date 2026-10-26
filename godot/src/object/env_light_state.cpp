// EnvLightValues and EnvLightState: the environment's world light and fog record and its channel.

#include "object/env_light_state.h"
#include "object/object_model.h"

#include <godot_cpp/core/math.hpp>

namespace godot {

void EnvLightValues::_bind_methods() {
	ClassDB::bind_method(D_METHOD("equals", "other"), &EnvLightValues::equals);
	ClassDB::bind_static_method("EnvLightValues", D_METHOD("retail_noon_defaults"),
			&EnvLightValues::retail_noon_defaults);
#define ENV_PROP(m_type, m_name)                                             \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"),                       \
			&EnvLightValues::set_##m_name);                                   \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &EnvLightValues::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	ENV_PROP(Variant::VECTOR3, hemi_sky);
	ENV_PROP(Variant::VECTOR3, dir);
	ENV_PROP(Variant::VECTOR3, dir_color);
	ENV_PROP(Variant::VECTOR3, hemi_ground);
	ENV_PROP(Variant::VECTOR3, ceiling);
	ENV_PROP(Variant::VECTOR3, floor_color);
	ENV_PROP(Variant::VECTOR3, gain);
	ENV_PROP(Variant::BOOL, fog_enabled);
	ENV_PROP(Variant::VECTOR3, fog_color);
	ENV_PROP(Variant::FLOAT, fog_start);
	ENV_PROP(Variant::FLOAT, fog_end);
	ENV_PROP(Variant::INT, fog_type);
#undef ENV_PROP
}

bool EnvLightValues::equals(const Ref<EnvLightValues> &p_other) const {
	if (p_other.is_null()) {
		return false;
	}
	// Colours compare with is_equal_approx (the weather smoother quantises to
	// 8-bit, so real changes are >= 1/255, far above epsilon).
	const EnvLightValues &o = **p_other;
	return hemi_sky.is_equal_approx(o.hemi_sky) && dir.is_equal_approx(o.dir) &&
			dir_color.is_equal_approx(o.dir_color) &&
			hemi_ground.is_equal_approx(o.hemi_ground) &&
			ceiling.is_equal_approx(o.ceiling) &&
			floor_color.is_equal_approx(o.floor_color) &&
			gain.is_equal_approx(o.gain) && fog_enabled == o.fog_enabled &&
			thermal_view == o.thermal_view &&
			fog_color.is_equal_approx(o.fog_color) &&
			Math::is_equal_approx(fog_start, o.fog_start) &&
			Math::is_equal_approx(fog_end, o.fog_end) && fog_type == o.fog_type;
}

Ref<EnvLightValues> EnvLightValues::retail_noon_defaults() {
	Ref<EnvLightValues> v;
	v.instantiate();
	v->hemi_sky = ObjectModel::default_hemi_sky_color();
	v->dir = ObjectModel::default_dir_light_dir();
	v->dir_color = ObjectModel::default_dir_light_color();
	v->hemi_ground = ObjectModel::default_hemi_ground_color();
	v->ceiling = ObjectModel::default_hemi_sky_color();
	v->floor_color = ObjectModel::default_hemi_ground_color();
	v->gain = Vector3(1, 1, 1);
	v->fog_enabled = false;
	v->fog_color = Vector3(0.5f, 0.6f, 0.8f);
	v->fog_start = 0.0f;
	v->fog_end = 1024.0f;
	v->fog_type = 0;
	return v;
}

void EnvLightState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("publish", "values", "pass_changed"),
			&EnvLightState::publish, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_values"), &EnvLightState::get_values);
	ClassDB::bind_method(D_METHOD("get_generation"), &EnvLightState::get_generation);
	ADD_SIGNAL(MethodInfo("changed"));
	ADD_SIGNAL(MethodInfo("pass_changed"));
}

void EnvLightState::publish(const Ref<EnvLightValues> &p_values,
		bool p_pass_changed) {
	values_ = p_values;
	++generation_;
	emit_signal("changed");
	if (p_pass_changed) {
		emit_signal("pass_changed");
	}
}

} // namespace godot
