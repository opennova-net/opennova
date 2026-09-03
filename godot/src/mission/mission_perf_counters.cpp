#include "mission/mission_perf_counters.h"

namespace godot {

Dictionary MissionPerfCounters::to_json_value() const {
	Dictionary out;
#define MISSION_PERF_COUNTER_JSON(m_name) out[#m_name] = m_name##_;
	MISSION_PERF_COUNTER_FIELDS(MISSION_PERF_COUNTER_JSON)
#undef MISSION_PERF_COUNTER_JSON
	out["did_tick"] = did_tick_;
	out["ticks"] = ticks_;
	out["sim"] = sim_counters_;
	return out;
}

void MissionPerfCounters::_bind_methods() {
#define MISSION_PERF_COUNTER_BIND(m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MissionPerfCounters::get_##m_name);           \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MissionPerfCounters::set_##m_name);  \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MISSION_PERF_COUNTER_FIELDS(MISSION_PERF_COUNTER_BIND)
#undef MISSION_PERF_COUNTER_BIND
	ClassDB::bind_method(D_METHOD("get_did_tick"), &MissionPerfCounters::get_did_tick);
	ClassDB::bind_method(D_METHOD("set_did_tick", "value"), &MissionPerfCounters::set_did_tick);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "did_tick"), "set_did_tick", "get_did_tick");
	ClassDB::bind_method(D_METHOD("get_ticks"), &MissionPerfCounters::get_ticks);
	ClassDB::bind_method(D_METHOD("set_ticks", "value"), &MissionPerfCounters::set_ticks);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ticks"), "set_ticks", "get_ticks");
	ClassDB::bind_method(D_METHOD("to_json_value"), &MissionPerfCounters::to_json_value);
}

} // namespace godot
