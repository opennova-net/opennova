#include "particle/effect_group_report.h"
#include "util/variant_type_of.h"

using namespace godot;

#define EFFECT_REPORT_BIND_FIELD(m_type, m_name, m_default)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void EffectEmitterReport::_bind_methods() {
	EFFECT_EMITTER_REPORT_FIELDS(EFFECT_REPORT_BIND_FIELD)
}

void EffectGroupReport::_bind_methods() {
	EFFECT_GROUP_REPORT_FIELDS(EFFECT_REPORT_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("get_owner_key"), &EffectGroupReport::get_owner_key);
	ClassDB::bind_method(D_METHOD("set_owner_key", "value"), &EffectGroupReport::set_owner_key);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "owner_key", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT),
			"set_owner_key", "get_owner_key");
	ClassDB::bind_method(D_METHOD("get_emitters"), &EffectGroupReport::get_emitters);
	ClassDB::bind_method(D_METHOD("set_emitters", "value"), &EffectGroupReport::set_emitters);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "emitters", PROPERTY_HINT_ARRAY_TYPE,
						 "EffectEmitterReport"),
			"set_emitters", "get_emitters");
}

#undef EFFECT_REPORT_BIND_FIELD
