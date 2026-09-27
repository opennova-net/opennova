#include "world/scar_draw_list.h"
#include "util/record_bind.h"

using namespace godot;

void ScarDrawList::_bind_methods() {
	SCAR_DRAW_LIST_FIELDS(OPENNOVA_RECORD_FIELD)
	BIND_CONSTANT(FLAG_ENTITY_LOCAL);
	BIND_CONSTANT(FLAG_BUILDING);
}
