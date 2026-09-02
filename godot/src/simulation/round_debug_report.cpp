#include "simulation/round_debug_report.h"

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

void RoundDebugEvent::_bind_methods() {
#define ROUND_DEBUG_BIND_FIELD(m_type, m_name, m_default)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &RoundDebugEvent::get_##m_name);               \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &RoundDebugEvent::set_##m_name);      \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	ROUND_DEBUG_EVENT_FIELDS(ROUND_DEBUG_BIND_FIELD)
#undef ROUND_DEBUG_BIND_FIELD
}

void RoundDebugReport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_tick"), &RoundDebugReport::get_tick);
	ClassDB::bind_method(D_METHOD("set_tick", "value"), &RoundDebugReport::set_tick);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tick"), "set_tick", "get_tick");
	ClassDB::bind_method(D_METHOD("get_events"), &RoundDebugReport::get_events);
	ClassDB::bind_method(D_METHOD("set_events", "value"), &RoundDebugReport::set_events);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "events", PROPERTY_HINT_ARRAY_TYPE, "RoundDebugEvent"),
			"set_events", "get_events");
	ClassDB::bind_method(D_METHOD("add_event", "event"), &RoundDebugReport::add_event);
}
