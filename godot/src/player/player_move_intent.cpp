#include "player/player_move_intent.h"

using namespace godot;

void PlayerMoveIntent::_bind_methods() {
#define PLAYER_MOVE_INTENT_BIND(m_name)                                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerMoveIntent::get_##m_name);       \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerMoveIntent::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_MOVE_INTENT_FIELDS(PLAYER_MOVE_INTENT_BIND)
#undef PLAYER_MOVE_INTENT_BIND
}
