#include "simulation/ai_debug_report.h"

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
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedFloat32Array>() { return Variant::PACKED_FLOAT32_ARRAY; }

} // namespace

#define AI_DEBUG_BIND_FIELD(m_type, m_name, m_default)                                             \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void AiDebugRow::_bind_methods() { AI_DEBUG_ROW_FIELDS(AI_DEBUG_BIND_FIELD) }
void AiDebugChannel::_bind_methods() { AI_DEBUG_CHANNEL_FIELDS(AI_DEBUG_BIND_FIELD) }

Ref<AiDebugGroup> AiDebugGroup::make(int p_id, int p_alert, int p_initial_count, int p_live_count) {
	Ref<AiDebugGroup> out;
	out.instantiate();
	out->id_ = p_id;
	out->alert_ = p_alert;
	out->initial_count_ = p_initial_count;
	out->live_count_ = p_live_count;
	return out;
}

void AiDebugGroup::_bind_methods() {
	AI_DEBUG_GROUP_FIELDS(AI_DEBUG_BIND_FIELD)
	ClassDB::bind_static_method("AiDebugGroup",
			D_METHOD("make", "id", "alert", "initial_count", "live_count"), &AiDebugGroup::make);
}

void AiDebugReport::_bind_methods() {
	AI_DEBUG_REPORT_FIELDS(AI_DEBUG_BIND_FIELD)
#define AI_DEBUG_REPORT_ROWS(m_name, m_class)                                                       \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &AiDebugReport::get_##m_name);                  \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &AiDebugReport::set_##m_name);         \
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, #m_name, PROPERTY_HINT_ARRAY_TYPE, #m_class),        \
			"set_" #m_name, "get_" #m_name);
	AI_DEBUG_REPORT_ROWS(rows, AiDebugRow)
	AI_DEBUG_REPORT_ROWS(channels, AiDebugChannel)
	AI_DEBUG_REPORT_ROWS(groups, AiDebugGroup)
#undef AI_DEBUG_REPORT_ROWS
	ClassDB::bind_method(D_METHOD("add_row", "row"), &AiDebugReport::add_row);
	ClassDB::bind_method(D_METHOD("add_channel", "row"), &AiDebugReport::add_channel);
	ClassDB::bind_method(D_METHOD("add_group", "row"), &AiDebugReport::add_group);
}

#undef AI_DEBUG_BIND_FIELD
