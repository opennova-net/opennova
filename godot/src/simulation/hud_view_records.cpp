#include "simulation/hud_view_records.h"

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
constexpr Variant::Type variant_type_of<PackedStringArray>() { return Variant::PACKED_STRING_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt32Array>() { return Variant::PACKED_INT32_ARRAY; }

} // namespace

// One read-write property per field of the named record.
#define HUD_VIEW_RECORD_BIND(m_class, m_fields)                                                       \
	void m_class::_bind_methods() {                                                                  \
		m_fields(HUD_VIEW_BIND_FIELD)                                                                \
	}
#define HUD_VIEW_BIND_FIELD(m_type, m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                        \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);               \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

HUD_VIEW_RECORD_BIND(WaypointHudView, WAYPOINT_HUD_VIEW_FIELDS)
HUD_VIEW_RECORD_BIND(HudMapGridOrigin, HUD_MAP_GRID_ORIGIN_FIELDS)
HUD_VIEW_RECORD_BIND(VehiclePanelView, VEHICLE_PANEL_VIEW_FIELDS)
HUD_VIEW_RECORD_BIND(ScoreFeedback, SCORE_FEEDBACK_FIELDS)
HUD_VIEW_RECORD_BIND(ScoreboardHeader, SCOREBOARD_HEADER_FIELDS)
HUD_VIEW_RECORD_BIND(EndRoundOverlay, END_ROUND_OVERLAY_FIELDS)
HUD_VIEW_RECORD_BIND(EndRoundStatistics, END_ROUND_STATISTICS_FIELDS)
HUD_VIEW_RECORD_BIND(EndRoundColumn, END_ROUND_COLUMN_FIELDS)
HUD_VIEW_RECORD_BIND(EndRoundRow, END_ROUND_ROW_FIELDS)
HUD_VIEW_RECORD_BIND(AttachLabelRow, ATTACH_LABEL_ROW_FIELDS)
HUD_VIEW_RECORD_BIND(FriendlyTagRow, FRIENDLY_TAG_ROW_FIELDS)
HUD_VIEW_RECORD_BIND(DeployStatus, DEPLOY_STATUS_FIELDS)

#undef HUD_VIEW_BIND_FIELD
#undef HUD_VIEW_RECORD_BIND
