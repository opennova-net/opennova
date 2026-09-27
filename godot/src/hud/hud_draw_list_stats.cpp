#include "hud/hud_draw_list_stats.h"
#include "util/record_bind.h"

using namespace godot;

Dictionary HudDrawListStats::to_json_value() const {
	Dictionary out;
#define HUD_DRAW_LIST_STATS_JSON(m_type, m_name, m_default) out[#m_name] = m_name##_;
	HUD_DRAW_LIST_STATS_FIELDS(HUD_DRAW_LIST_STATS_JSON)
#undef HUD_DRAW_LIST_STATS_JSON
	return out;
}

void HudDrawListStats::_bind_methods() {
	HUD_DRAW_LIST_STATS_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_method(D_METHOD("to_json_value"), &HudDrawListStats::to_json_value);
}
