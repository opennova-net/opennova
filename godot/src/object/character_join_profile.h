#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/inmatch/join_character_profile.h>

namespace godot {

// The joiner's profile-to-wire projection (runtime/inmatch/join_character_profile.h):
// per side the packed Avatars.def character id (CI0/CI1), the class byte
// (CTA/CTB) and the avatar byte (VCA/VCB, the selected combo's head voice),
// plus the side request the companion fills (-1 = assign me). Produced by
// AvatarDatabase.character_join_profile, consumed by the Simulation's join and
// local-player installs.
class CharacterJoinProfile : public RefCounted {
	GDCLASS(CharacterJoinProfile, RefCounted)

	opennova::inmatch::JoinCharacterProfile value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::JoinCharacterProfile &p_value) { value_ = p_value; }
	const opennova::inmatch::JoinCharacterProfile &value() const { return value_; }

	// side 0 = blue/good, 1 = red/evil; any other side reads the blue value.
	int get_character_id(int p_side) const;
	int get_player_class(int p_side) const;
	int get_avatar(int p_side) const;
	int get_team_request() const { return value_.team_request; }
};

} // namespace godot
