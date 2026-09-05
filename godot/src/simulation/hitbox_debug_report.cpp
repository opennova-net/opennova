#include "simulation/hitbox_debug_report.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedByteArray>() { return Variant::PACKED_BYTE_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt32Array>() { return Variant::PACKED_INT32_ARRAY; }

} // namespace

#define HITBOX_DEBUG_BIND_FIELD(m_type, m_name, m_default)                                         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void HitboxDebugEntity::_bind_methods() { HITBOX_DEBUG_ENTITY_FIELDS(HITBOX_DEBUG_BIND_FIELD) }

Ref<HitboxDebugOrganic> HitboxDebugOrganic::make(int p_entity_handle, int p_section,
		const Vector3 &p_pos, float p_radius, float p_authored_radius, bool p_masked,
		bool p_fallback) {
	Ref<HitboxDebugOrganic> out;
	out.instantiate();
	out->entity_handle_ = p_entity_handle;
	out->section_ = p_section;
	out->pos_ = p_pos;
	out->radius_ = p_radius;
	out->authored_radius_ = p_authored_radius;
	out->masked_ = p_masked;
	out->fallback_ = p_fallback;
	return out;
}

void HitboxDebugOrganic::_bind_methods() {
	HITBOX_DEBUG_ORGANIC_FIELDS(HITBOX_DEBUG_BIND_FIELD)
	ClassDB::bind_static_method("HitboxDebugOrganic",
			D_METHOD("make", "entity_handle", "section", "pos", "radius", "authored_radius",
					"masked", "fallback"),
			&HitboxDebugOrganic::make, DEFVAL(false), DEFVAL(false));
}

Ref<HitboxDebugReport> HitboxDebugReport::make(const TypedArray<HitboxDebugEntity> &p_entities,
		const TypedArray<HitboxDebugOrganic> &p_organics) {
	Ref<HitboxDebugReport> out;
	out.instantiate();
	out->entities_ = p_entities;
	out->organics_ = p_organics;
	return out;
}

void HitboxDebugReport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_entities"), &HitboxDebugReport::get_entities);
	ClassDB::bind_method(D_METHOD("set_entities", "value"), &HitboxDebugReport::set_entities);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "entities", PROPERTY_HINT_ARRAY_TYPE, "HitboxDebugEntity"),
			"set_entities", "get_entities");
	ClassDB::bind_method(D_METHOD("get_organics"), &HitboxDebugReport::get_organics);
	ClassDB::bind_method(D_METHOD("set_organics", "value"), &HitboxDebugReport::set_organics);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "organics", PROPERTY_HINT_ARRAY_TYPE, "HitboxDebugOrganic"),
			"set_organics", "get_organics");
	ClassDB::bind_method(D_METHOD("add_entity", "row"), &HitboxDebugReport::add_entity);
	ClassDB::bind_method(D_METHOD("add_organic", "row"), &HitboxDebugReport::add_organic);
	ClassDB::bind_static_method("HitboxDebugReport", D_METHOD("make", "entities", "organics"),
			&HitboxDebugReport::make, DEFVAL(TypedArray<HitboxDebugEntity>()),
			DEFVAL(TypedArray<HitboxDebugOrganic>()));
}

#undef HITBOX_DEBUG_BIND_FIELD
