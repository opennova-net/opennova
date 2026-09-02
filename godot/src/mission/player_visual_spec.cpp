#include "mission/player_visual_spec.h"

using namespace godot;

void PlayerVisualSpec::_bind_methods() {
#define PLAYER_VISUAL_SPEC_PROPERTY(m_variant, m_name)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PlayerVisualSpec::get_##m_name);          \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PlayerVisualSpec::set_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, character_id)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, item_id)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::STRING, head)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::VECTOR3I, head_camo)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::STRING, body)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::VECTOR3I, body_camo)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::STRING, arms)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::VECTOR3I, arms_camo)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, avatar)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, sex)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, nationality_index)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, division_index)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::INT, combo_index)
	PLAYER_VISUAL_SPEC_PROPERTY(Variant::BOOL, fallback)
#undef PLAYER_VISUAL_SPEC_PROPERTY
}
