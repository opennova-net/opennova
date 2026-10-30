#pragma once

// The profile's character half (engine inmatch::CharacterProfile,
// runtime/inmatch/join_character_profile.h carries the rules): the callsign, the side PLAYER_INFO
// shows first and both side blocks of the current weapon record with the tree indices their words
// resolve to (-1 where the table does not resolve them). Read-only; PlayerProfiles.
// process_character_profile makes one. A side is 0 (blue) or 1 (red); any other reads as blue.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/join_character_profile.h>

namespace godot {

class CharacterProfile : public RefCounted {
	GDCLASS(CharacterProfile, RefCounted)

	opennova::inmatch::CharacterProfile value_;

	const opennova::inmatch::PlayerInfoSideSelection &side_(int p_side) const {
		return value_.sides[p_side == 1 ? 1 : 0];
	}

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::CharacterProfile &p_value) { value_ = p_value; }
	const opennova::inmatch::CharacterProfile &native() const { return value_; }

	String get_name() const;
	int get_team() const { return value_.team; }

	// Each side block's fields.
	int side_team(int p_side) const { return side_(p_side).team; }
	int side_nationality(int p_side) const { return side_(p_side).nationality; }
	int side_division(int p_side) const { return side_(p_side).division; }
	int side_combo(int p_side) const { return side_(p_side).combo; }
	int side_player_class(int p_side) const { return side_(p_side).player_class; }
	int side_avatar_a(int p_side) const { return side_(p_side).avatar_a; }
	int side_avatar_b(int p_side) const { return side_(p_side).avatar_b; }
	int side_avatar_packed(int p_side) const { return side_(p_side).avatar_packed; }
};

} // namespace godot
