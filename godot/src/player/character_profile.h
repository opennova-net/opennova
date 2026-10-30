#pragma once

// The profile's character half (engine inmatch::CharacterProfile,
// runtime/inmatch/join_character_profile.h carries the rules): the callsign, the side PLAYER_INFO
// shows first and both side blocks of the current weapon record with the tree indices their words
// resolve to. Read-only; PlayerProfiles.process_character_profile makes one.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/join_character_profile.h>

namespace godot {

class PlayerInfoSideSelection;

class CharacterProfile : public RefCounted {
	GDCLASS(CharacterProfile, RefCounted)

	opennova::inmatch::CharacterProfile value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::CharacterProfile &p_value) { value_ = p_value; }
	const opennova::inmatch::CharacterProfile &native() const { return value_; }

	String get_name() const;
	int get_team() const { return value_.team; }
	Ref<PlayerInfoSideSelection> get_blue() const;
	Ref<PlayerInfoSideSelection> get_red() const;
};

} // namespace godot
