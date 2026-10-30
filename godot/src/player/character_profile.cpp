#include "player/character_profile.h"

#include "player/player_info_character_state.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

String CharacterProfile::get_name() const { return opennova::cp1252_to_gd(value_.name); }

Ref<PlayerInfoSideSelection> CharacterProfile::get_blue() const {
	return PlayerInfoSideSelection::from_native(value_.sides[0]);
}

Ref<PlayerInfoSideSelection> CharacterProfile::get_red() const {
	return PlayerInfoSideSelection::from_native(value_.sides[1]);
}

void CharacterProfile::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(CharacterProfile, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(CharacterProfile, Variant::INT, team)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(CharacterProfile, blue, PlayerInfoSideSelection)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(CharacterProfile, red, PlayerInfoSideSelection)
}
