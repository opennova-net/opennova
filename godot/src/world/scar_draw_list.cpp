#include "world/scar_draw_list.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedVector2Array>() { return Variant::PACKED_VECTOR2_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedColorArray>() { return Variant::PACKED_COLOR_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt32Array>() { return Variant::PACKED_INT32_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt64Array>() { return Variant::PACKED_INT64_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedStringArray>() { return Variant::PACKED_STRING_ARRAY; }

} // namespace

void ScarDrawList::_bind_methods() {
#define SCAR_DRAW_LIST_BIND(m_type, m_name)                                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ScarDrawList::get_##m_name);                \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ScarDrawList::set_##m_name);       \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	SCAR_DRAW_LIST_FIELDS(SCAR_DRAW_LIST_BIND)
#undef SCAR_DRAW_LIST_BIND
	BIND_CONSTANT(FLAG_ENTITY_LOCAL);
	BIND_CONSTANT(FLAG_BUILDING);
}
