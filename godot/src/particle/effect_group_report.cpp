#include "particle/effect_group_report.h"
#include "util/record_bind.h"

using namespace godot;

void EffectEmitterReport::_bind_methods() {
	EFFECT_EMITTER_REPORT_FIELDS(OPENNOVA_RECORD_FIELD)
}

void EffectGroupReport::_bind_methods() {
	EFFECT_GROUP_REPORT_FIELDS(OPENNOVA_RECORD_FIELD)
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
