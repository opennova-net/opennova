#include "env/env_records.h"

using namespace godot;

void EnvDayPhase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_night"), &EnvDayPhase::is_night);
	ClassDB::bind_method(D_METHOD("set_night", "night"), &EnvDayPhase::set_night);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "night"), "set_night", "is_night");
	ClassDB::bind_method(D_METHOD("get_blend"), &EnvDayPhase::get_blend);
	ClassDB::bind_method(D_METHOD("set_blend", "blend"), &EnvDayPhase::set_blend);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend"), "set_blend", "get_blend");
}

void EnvSunGlare::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_glare"), &EnvSunGlare::get_glare);
	ClassDB::bind_method(D_METHOD("set_glare", "glare"), &EnvSunGlare::set_glare);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "glare"), "set_glare", "get_glare");
	ClassDB::bind_method(D_METHOD("get_fog_whiten"), &EnvSunGlare::get_fog_whiten);
	ClassDB::bind_method(D_METHOD("set_fog_whiten", "value"), &EnvSunGlare::set_fog_whiten);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fog_whiten"), "set_fog_whiten", "get_fog_whiten");
}
