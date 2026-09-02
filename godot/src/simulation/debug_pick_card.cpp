#include "simulation/debug_pick_card.h"

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

} // namespace

Ref<DebugPickCard> DebugPickCard::duplicate() const {
	Ref<DebugPickCard> out;
	out.instantiate();
#define DEBUG_PICK_CARD_COPY(m_type, m_name, m_default) out->m_name##_ = m_name##_;
	DEBUG_PICK_CARD_FIELDS(DEBUG_PICK_CARD_COPY)
#undef DEBUG_PICK_CARD_COPY
	return out;
}

void DebugPickCard::_bind_methods() {
#define DEBUG_PICK_CARD_BIND(m_type, m_name, m_default)                                            \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &DebugPickCard::get_##m_name);                 \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &DebugPickCard::set_##m_name);        \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	DEBUG_PICK_CARD_FIELDS(DEBUG_PICK_CARD_BIND)
#undef DEBUG_PICK_CARD_BIND
	ClassDB::bind_method(D_METHOD("duplicate"), &DebugPickCard::duplicate);
}
