#include "world/runtime_perf_counters.h"

using namespace godot;

Dictionary RuntimePerfCounters::to_json_value() const {
	Dictionary out;
	out["tick_us"] = tick_us_;
	out["foliage_us"] = foliage_us_;
	out["runtime_us"] = runtime_us_;
	out["audio_us"] = audio_us_;
	out["runtime"] = runtime_.is_valid() ? runtime_->to_json_value() : Dictionary();
	out["foliage"] = foliage_.is_valid() ? foliage_->to_json_value() : Dictionary();
	out["foliage_backend"] = foliage_backend_;
	out["framefx"] = framefx_;
	out["mission_placement"] = mission_placement_.is_valid()
			? mission_placement_->to_json_value()
			: Dictionary();
	out["static_live_populations"] = static_live_populations_;
	out["audio"] = audio_.is_valid() ? audio_->to_json_value() : Dictionary();
	Dictionary estimate;
	estimate["total"] = estimate_total_;
	estimate["budget"] = estimate_budget_;
	estimate["foliage_pool"] = estimate_foliage_pool_;
	estimate["static_populations"] = estimate_static_populations_;
	estimate["object_geometry"] = estimate_object_geometry_;
	out["instance_uniform_geometry_estimate"] = estimate;
	return out;
}

void RuntimePerfCounters::_bind_methods() {
#define RUNTIME_PERF_COUNTER_BIND(m_name)                                                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &RuntimePerfCounters::get_##m_name);    \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &RuntimePerfCounters::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	RUNTIME_PERF_COUNTER_FIELDS(RUNTIME_PERF_COUNTER_BIND)
	RUNTIME_PERF_ESTIMATE_FIELDS(RUNTIME_PERF_COUNTER_BIND)
#undef RUNTIME_PERF_COUNTER_BIND
	ClassDB::bind_method(D_METHOD("get_runtime"), &RuntimePerfCounters::get_runtime);
	ClassDB::bind_method(D_METHOD("set_runtime", "value"), &RuntimePerfCounters::set_runtime);
	ClassDB::bind_method(D_METHOD("get_foliage"), &RuntimePerfCounters::get_foliage);
	ClassDB::bind_method(D_METHOD("set_foliage", "value"), &RuntimePerfCounters::set_foliage);
	ClassDB::bind_method(D_METHOD("get_mission_placement"), &RuntimePerfCounters::get_mission_placement);
	ClassDB::bind_method(D_METHOD("set_mission_placement", "value"),
			&RuntimePerfCounters::set_mission_placement);
	ClassDB::bind_method(D_METHOD("get_audio"), &RuntimePerfCounters::get_audio);
	ClassDB::bind_method(D_METHOD("set_audio", "value"), &RuntimePerfCounters::set_audio);
	ClassDB::bind_method(D_METHOD("get_static_live_populations"),
			&RuntimePerfCounters::get_static_live_populations);
	ClassDB::bind_method(D_METHOD("set_static_live_populations", "value"),
			&RuntimePerfCounters::set_static_live_populations);
	ClassDB::bind_method(D_METHOD("to_json_value"), &RuntimePerfCounters::to_json_value);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "runtime", PROPERTY_HINT_RESOURCE_TYPE,
			"MissionPerfCounters"), "set_runtime", "get_runtime");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "foliage", PROPERTY_HINT_RESOURCE_TYPE,
			"FoliageFrameStats"), "set_foliage", "get_foliage");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mission_placement", PROPERTY_HINT_RESOURCE_TYPE,
			"MissionPlacementStats"), "set_mission_placement", "get_mission_placement");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "audio", PROPERTY_HINT_RESOURCE_TYPE,
			"MissionAudioPerf"), "set_audio", "get_audio");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "static_live_populations"),
			"set_static_live_populations", "get_static_live_populations");
}
