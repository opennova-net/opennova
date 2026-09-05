#include "mnu/menu_draw_list_stats.h"

using namespace godot;

Dictionary MenuDrawListStats::to_json_value() const {
	Dictionary out;
#define MENU_DRAW_LIST_STATS_JSON(m_name) out[#m_name] = m_name##_;
	MENU_DRAW_LIST_STATS_FIELDS(MENU_DRAW_LIST_STATS_JSON)
#undef MENU_DRAW_LIST_STATS_JSON
	return out;
}

void MenuDrawListStats::_bind_methods() {
#define MENU_DRAW_LIST_STATS_BIND(m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &MenuDrawListStats::get_##m_name);             \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &MenuDrawListStats::set_##m_name);    \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MENU_DRAW_LIST_STATS_FIELDS(MENU_DRAW_LIST_STATS_BIND)
#undef MENU_DRAW_LIST_STATS_BIND
	ClassDB::bind_method(D_METHOD("to_json_value"), &MenuDrawListStats::to_json_value);
}
