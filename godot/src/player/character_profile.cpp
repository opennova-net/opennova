#include "player/character_profile.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

String CharacterProfile::get_name() const { return opennova::cp1252_to_gd(value_.name); }

void CharacterProfile::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(CharacterProfile, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(CharacterProfile, Variant::INT, team)
	ClassDB::bind_method(D_METHOD("side_team", "side"), &CharacterProfile::side_team);
	ClassDB::bind_method(D_METHOD("side_nationality", "side"), &CharacterProfile::side_nationality);
	ClassDB::bind_method(D_METHOD("side_division", "side"), &CharacterProfile::side_division);
	ClassDB::bind_method(D_METHOD("side_combo", "side"), &CharacterProfile::side_combo);
	ClassDB::bind_method(D_METHOD("side_player_class", "side"), &CharacterProfile::side_player_class);
	ClassDB::bind_method(D_METHOD("side_avatar_a", "side"), &CharacterProfile::side_avatar_a);
	ClassDB::bind_method(D_METHOD("side_avatar_b", "side"), &CharacterProfile::side_avatar_b);
	ClassDB::bind_method(D_METHOD("side_avatar_packed", "side"),
			&CharacterProfile::side_avatar_packed);
}
