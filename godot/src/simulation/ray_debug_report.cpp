#include "simulation/ray_debug_report.h"

using namespace godot;

Ref<RayDebugCount> RayDebugCount::make(const String &p_name, int64_t p_held, int64_t p_total) {
	Ref<RayDebugCount> out;
	out.instantiate();
	out->name_ = p_name;
	out->held_ = p_held;
	out->total_ = p_total;
	return out;
}

void RayDebugCount::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_name"), &RayDebugCount::get_name);
	ClassDB::bind_method(D_METHOD("set_name", "value"), &RayDebugCount::set_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ClassDB::bind_method(D_METHOD("get_held"), &RayDebugCount::get_held);
	ClassDB::bind_method(D_METHOD("set_held", "value"), &RayDebugCount::set_held);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "held"), "set_held", "get_held");
	ClassDB::bind_method(D_METHOD("get_total"), &RayDebugCount::get_total);
	ClassDB::bind_method(D_METHOD("set_total", "value"), &RayDebugCount::set_total);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "total"), "set_total", "get_total");
	ClassDB::bind_static_method("RayDebugCount", D_METHOD("make", "name", "held", "total"),
			&RayDebugCount::make);
}

void RayDebugReport::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_stride"), &RayDebugReport::get_stride);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stride", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_stride");
	ClassDB::bind_method(D_METHOD("get_events"), &RayDebugReport::get_events);
	ClassDB::bind_method(D_METHOD("set_events", "value"), &RayDebugReport::set_events);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "events"), "set_events", "get_events");
	ClassDB::bind_method(D_METHOD("get_counts"), &RayDebugReport::get_counts);
	ClassDB::bind_method(D_METHOD("set_counts", "value"), &RayDebugReport::set_counts);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "counts", PROPERTY_HINT_ARRAY_TYPE, "RayDebugCount"),
			"set_counts", "get_counts");
	ClassDB::bind_method(D_METHOD("add_count", "count"), &RayDebugReport::add_count);
	ClassDB::bind_method(D_METHOD("get_mask"), &RayDebugReport::get_mask);
	ClassDB::bind_method(D_METHOD("set_mask", "value"), &RayDebugReport::set_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask"), "set_mask", "get_mask");
	ClassDB::bind_method(D_METHOD("get_ttl"), &RayDebugReport::get_ttl);
	ClassDB::bind_method(D_METHOD("set_ttl", "value"), &RayDebugReport::set_ttl);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ttl"), "set_ttl", "get_ttl");
	ClassDB::bind_method(D_METHOD("get_recording"), &RayDebugReport::get_recording);
	ClassDB::bind_method(D_METHOD("set_recording", "value"), &RayDebugReport::set_recording);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "recording"), "set_recording", "get_recording");
	ClassDB::bind_method(D_METHOD("get_tick"), &RayDebugReport::get_tick);
	ClassDB::bind_method(D_METHOD("set_tick", "value"), &RayDebugReport::set_tick);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tick"), "set_tick", "get_tick");
}
