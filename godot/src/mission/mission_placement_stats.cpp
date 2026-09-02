#include "mission/mission_placement_stats.h"

using namespace godot;

Dictionary MissionPlacementStats::to_json_value() const {
	Dictionary out;
#define MISSION_PLACEMENT_STATS_JSON(m_name) out[#m_name] = m_name##_;
	MISSION_PLACEMENT_STATS_FIELDS(MISSION_PLACEMENT_STATS_JSON)
#undef MISSION_PLACEMENT_STATS_JSON
	Dictionary spans;
#define MISSION_PLACEMENT_SPAN_JSON(m_name) spans[#m_name] = span_##m_name##_usec_;
	MISSION_PLACEMENT_SPANS(MISSION_PLACEMENT_SPAN_JSON)
#undef MISSION_PLACEMENT_SPAN_JSON
	out["spans"] = spans;
	return out;
}

void MissionPlacementStats::_bind_methods() {
#define MISSION_PLACEMENT_STATS_BIND(m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionPlacementStats::get_##m_name);            \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionPlacementStats::set_##m_name);   \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MISSION_PLACEMENT_STATS_FIELDS(MISSION_PLACEMENT_STATS_BIND)
#undef MISSION_PLACEMENT_STATS_BIND
#define MISSION_PLACEMENT_SPAN_BIND(m_name)                                                           \
	ClassDB::bind_method(D_METHOD("get_span_" #m_name "_usec"),                                       \
			&MissionPlacementStats::get_span_##m_name##_usec);                                        \
	ClassDB::bind_method(D_METHOD("set_span_" #m_name "_usec", "value"),                              \
			&MissionPlacementStats::set_span_##m_name##_usec);                                        \
	ADD_PROPERTY(PropertyInfo(Variant::INT, "span_" #m_name "_usec"), "set_span_" #m_name "_usec",   \
			"get_span_" #m_name "_usec");
	MISSION_PLACEMENT_SPANS(MISSION_PLACEMENT_SPAN_BIND)
#undef MISSION_PLACEMENT_SPAN_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &MissionPlacementStats::to_json_value);
}
