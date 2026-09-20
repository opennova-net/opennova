#include "hud/hud_draw_list_stats.h"
#include "util/variant_type_of.h"

using namespace godot;

Dictionary HudDrawListStats::to_json_value() const {
	Dictionary out;
#define HUD_DRAW_LIST_STATS_JSON(m_type, m_name, m_default) out[#m_name] = m_name##_;
	HUD_DRAW_LIST_STATS_FIELDS(HUD_DRAW_LIST_STATS_JSON)
#undef HUD_DRAW_LIST_STATS_JSON
	return out;
}

void HudDrawListStats::_bind_methods() {
#define HUD_DRAW_LIST_STATS_BIND(m_type, m_name, m_default)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &HudDrawListStats::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &HudDrawListStats::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	HUD_DRAW_LIST_STATS_FIELDS(HUD_DRAW_LIST_STATS_BIND)
#undef HUD_DRAW_LIST_STATS_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &HudDrawListStats::to_json_value);
}
