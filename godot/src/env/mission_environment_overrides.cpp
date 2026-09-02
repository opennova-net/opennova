#include "env/mission_environment_overrides.h"
#include "util/color_convert.h"

using namespace godot;

namespace {

} // namespace

bool MissionEnvironmentOverrides::is_empty() const {
	return !value_.has_water_height && !value_.has_fog_level && !value_.has_fog_color &&
			!value_.has_water_color && !value_.has_water_murk && !value_.has_start_time;
}

void MissionEnvironmentOverrides::set_water_height(float p_value) {
	value_.has_water_height = true;
	value_.water_height = p_value;
}

void MissionEnvironmentOverrides::set_fog_level(float p_value) {
	value_.has_fog_level = true;
	value_.fog_level = p_value;
}

Color MissionEnvironmentOverrides::get_fog_color() const { return opennova::color_from_env_rgb(value_.fog_color); }

void MissionEnvironmentOverrides::set_fog_color(const Color &p_value) {
	value_.has_fog_color = true;
	value_.fog_color = opennova::env_rgb_from_color(p_value);
}

Color MissionEnvironmentOverrides::get_water_color() const { return opennova::color_from_env_rgb(value_.water_color); }

void MissionEnvironmentOverrides::set_water_color(const Color &p_value) {
	value_.has_water_color = true;
	value_.water_color = opennova::env_rgb_from_color(p_value);
}

void MissionEnvironmentOverrides::set_water_murk(float p_value) {
	value_.has_water_murk = true;
	value_.water_murk = p_value;
}

void MissionEnvironmentOverrides::set_start_time(int p_value) {
	value_.has_start_time = true;
	value_.start_time = p_value;
}

void MissionEnvironmentOverrides::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_empty"), &MissionEnvironmentOverrides::is_empty);
#define OVERRIDE_GATE(m_name)                                                                        \
	ClassDB::bind_method(D_METHOD("get_has_" #m_name), &MissionEnvironmentOverrides::get_has_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "has_" #m_name, PROPERTY_HINT_NONE, "",               \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),                          \
			"", "get_has_" #m_name);
#define OVERRIDE_VALUE(m_variant, m_name)                                                            \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionEnvironmentOverrides::get_##m_name);     \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionEnvironmentOverrides::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	OVERRIDE_GATE(water_height)
	OVERRIDE_VALUE(Variant::FLOAT, water_height)
	ClassDB::bind_method(D_METHOD("get_water_height_world"), &MissionEnvironmentOverrides::get_water_height_world);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_height_world", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_water_height_world");
	OVERRIDE_GATE(fog_level)
	OVERRIDE_VALUE(Variant::FLOAT, fog_level)
	OVERRIDE_GATE(fog_color)
	OVERRIDE_VALUE(Variant::COLOR, fog_color)
	OVERRIDE_GATE(water_color)
	OVERRIDE_VALUE(Variant::COLOR, water_color)
	OVERRIDE_GATE(water_murk)
	OVERRIDE_VALUE(Variant::FLOAT, water_murk)
	OVERRIDE_GATE(start_time)
	OVERRIDE_VALUE(Variant::INT, start_time)
#undef OVERRIDE_GATE
#undef OVERRIDE_VALUE
}
