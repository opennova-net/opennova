#include "terrain/foliage_frame_stats.h"
#include "util/record_bind.h"

using namespace godot;

Dictionary FoliageFrameStats::to_json_value() const {
	Dictionary out;
#define FOLIAGE_FRAME_STATS_JSON(m_type, m_name) out[#m_name] = m_name##_;
	FOLIAGE_FRAME_STATS_FIELDS(FOLIAGE_FRAME_STATS_JSON)
#undef FOLIAGE_FRAME_STATS_JSON
	return out;
}

void FoliageFrameStats::_bind_methods() {
	FOLIAGE_FRAME_STATS_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_method(D_METHOD("to_json_value"), &FoliageFrameStats::to_json_value);
}
