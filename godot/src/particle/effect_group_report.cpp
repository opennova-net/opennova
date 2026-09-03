#include "particle/effect_group_report.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<AABB>() { return Variant::AABB; }
template <>
constexpr Variant::Type variant_type_of<Transform3D>() { return Variant::TRANSFORM3D; }

} // namespace

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
	ClassDB::bind_method(D_METHOD("add_emitter", "row"), &EffectGroupReport::add_emitter);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "emitters", PROPERTY_HINT_ARRAY_TYPE,
						 "EffectEmitterReport"),
			"set_emitters", "get_emitters");
}

#undef EFFECT_REPORT_BIND_FIELD
