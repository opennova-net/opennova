#include "world/scar_draw_list.h"
#include "util/variant_type_of.h"

using namespace godot;

void ScarDrawList::_bind_methods() {
#define SCAR_DRAW_LIST_BIND(m_type, m_name)                                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ScarDrawList::get_##m_name);                \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ScarDrawList::set_##m_name);       \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	SCAR_DRAW_LIST_FIELDS(SCAR_DRAW_LIST_BIND)
#undef SCAR_DRAW_LIST_BIND
	BIND_CONSTANT(FLAG_ENTITY_LOCAL);
	BIND_CONSTANT(FLAG_BUILDING);
}
