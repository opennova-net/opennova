#include "terrain/foliage_frame_stats.h"
#include "util/variant_type_of.h"

using namespace godot;

Dictionary FoliageFrameStats::to_json_value() const {
	Dictionary out;
#define FOLIAGE_FRAME_STATS_JSON(m_type, m_name) out[#m_name] = m_name##_;
	FOLIAGE_FRAME_STATS_FIELDS(FOLIAGE_FRAME_STATS_JSON)
#undef FOLIAGE_FRAME_STATS_JSON
	return out;
}

void FoliageFrameStats::_bind_methods() {
#define FOLIAGE_FRAME_STATS_BIND(m_type, m_name)                                                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &FoliageFrameStats::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &FoliageFrameStats::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	FOLIAGE_FRAME_STATS_FIELDS(FOLIAGE_FRAME_STATS_BIND)
#undef FOLIAGE_FRAME_STATS_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &FoliageFrameStats::to_json_value);
}
