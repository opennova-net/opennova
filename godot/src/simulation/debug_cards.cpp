#include "simulation/debug_cards.h"

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
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }

} // namespace

#define DEBUG_CARD_BIND_FIELD(m_type, m_name, m_default)                                           \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void WacState::_bind_methods() { WAC_STATE_FIELDS(DEBUG_CARD_BIND_FIELD) }
void NativePoseStats::_bind_methods() { NATIVE_POSE_STATS_FIELDS(DEBUG_CARD_BIND_FIELD) }
void DestructionDebugCard::_bind_methods() { DESTRUCTION_DEBUG_CARD_FIELDS(DEBUG_CARD_BIND_FIELD) }

#undef DEBUG_CARD_BIND_FIELD
