#include "mission/mission_info.h"
#include "util/string_convert.h"

#include <formats/mission/bms.h> // AttribFlags

using namespace godot;

void MissionInfo::assign(const opennova::mission::MissionInfo &p_value, int p_game_mode) {
	value_ = p_value;
	game_mode_ = p_game_mode;
}

String MissionInfo::get_mission_name() const { return opennova::to_gd(value_.mission_name); }
String MissionInfo::get_designer() const { return opennova::to_gd(value_.designer); }
String MissionInfo::get_briefing() const { return opennova::to_gd(value_.briefing); }
String MissionInfo::get_terrain() const { return opennova::to_gd(value_.terrain); }
String MissionInfo::get_environment() const { return opennova::to_gd(value_.environment); }

Color MissionInfo::get_fog_color() const {
	return Color(value_.fog_color[0] / 255.0f, value_.fog_color[1] / 255.0f, value_.fog_color[2] / 255.0f);
}

Color MissionInfo::get_water_color() const {
	return Color(value_.water_color[0] / 255.0f, value_.water_color[1] / 255.0f, value_.water_color[2] / 255.0f);
}

bool MissionInfo::get_has_water_override() const {
	return opennova::bms::has_flag(static_cast<opennova::bms::AttribFlags>(value_.attrib_flags),
			opennova::bms::AttribFlags::WaterOverrideEnable);
}

bool MissionInfo::get_has_fog_distance_override() const {
	return opennova::bms::has_flag(static_cast<opennova::bms::AttribFlags>(value_.attrib_flags),
			opennova::bms::AttribFlags::FogDistanceOverrideEnable);
}

bool MissionInfo::get_has_fog_color_override() const {
	return opennova::bms::has_flag(static_cast<opennova::bms::AttribFlags>(value_.attrib_flags),
			opennova::bms::AttribFlags::FogColorOverrideEnable);
}

void MissionInfo::_bind_methods() {
#define MISSION_INFO_FIELD(m_variant, m_name)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionInfo::get_##m_name);           \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                 \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),              \
			"", "get_" #m_name);
	MISSION_INFO_FIELD(Variant::STRING, mission_name)
	MISSION_INFO_FIELD(Variant::STRING, designer)
	MISSION_INFO_FIELD(Variant::STRING, briefing)
	MISSION_INFO_FIELD(Variant::STRING, terrain)
	MISSION_INFO_FIELD(Variant::STRING, environment)
	MISSION_INFO_FIELD(Variant::INT, climate)
	MISSION_INFO_FIELD(Variant::INT, weather)
	MISSION_INFO_FIELD(Variant::INT, mission_type)
	MISSION_INFO_FIELD(Variant::INT, attrib_flags)
	MISSION_INFO_FIELD(Variant::INT, game_mode)
	MISSION_INFO_FIELD(Variant::INT, start_time)
	MISSION_INFO_FIELD(Variant::INT, minutes_per_day)
	MISSION_INFO_FIELD(Variant::INT, player_health)
	MISSION_INFO_FIELD(Variant::INT, max_saves)
	MISSION_INFO_FIELD(Variant::INT, music)
	MISSION_INFO_FIELD(Variant::INT, reverb)
	MISSION_INFO_FIELD(Variant::INT, wind_speed)
	MISSION_INFO_FIELD(Variant::INT, wind_direction)
	MISSION_INFO_FIELD(Variant::FLOAT, map_zoom)
	MISSION_INFO_FIELD(Variant::INT, water_override)
	MISSION_INFO_FIELD(Variant::INT, fog_override)
	MISSION_INFO_FIELD(Variant::COLOR, fog_color)
	MISSION_INFO_FIELD(Variant::COLOR, water_color)
	MISSION_INFO_FIELD(Variant::INT, water_murk)
	MISSION_INFO_FIELD(Variant::BOOL, has_water_override)
	MISSION_INFO_FIELD(Variant::BOOL, has_fog_distance_override)
	MISSION_INFO_FIELD(Variant::BOOL, has_fog_color_override)
#undef MISSION_INFO_FIELD
}
