#include "object/character_join_profile.h"

using namespace godot;

namespace {
int side_index(int p_side) { return p_side == 1 ? 1 : 0; }
} // namespace

int CharacterJoinProfile::get_character_id(int p_side) const {
	return value_.character_ids[side_index(p_side)];
}

int CharacterJoinProfile::get_player_class(int p_side) const {
	return value_.player_classes[side_index(p_side)];
}

int CharacterJoinProfile::get_avatar(int p_side) const {
	return value_.avatars[side_index(p_side)];
}

void CharacterJoinProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_character_id", "side"), &CharacterJoinProfile::get_character_id);
	ClassDB::bind_method(D_METHOD("get_player_class", "side"), &CharacterJoinProfile::get_player_class);
	ClassDB::bind_method(D_METHOD("get_avatar", "side"), &CharacterJoinProfile::get_avatar);
	ClassDB::bind_method(D_METHOD("get_team_request"), &CharacterJoinProfile::get_team_request);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "team_request", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_team_request");
}
