#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/inmatch/player_info_character.h>

namespace godot {

class AvatarDatabase;

// One side's PLAYER_INFO character selection (engine
// inmatch::PlayerInfoSideSelection): the side, the Avatars.def tree indices,
// the class byte, the authored nationality / division numbers and the packed
// id. Read-write, so the shell's profile Dictionary marshalling can build one.
#define PLAYER_INFO_SIDE_SELECTION_FIELDS(X) \
	X(int, team, -1)                         \
	X(int, nationality, -1)                  \
	X(int, division, -1)                     \
	X(int, combo, -1)                        \
	X(int, player_class, 8)                  \
	X(int, avatar_a, 0)                      \
	X(int, avatar_b, 0)                      \
	X(int, avatar_packed, 0)

class PlayerInfoSideSelection : public RefCounted {
	GDCLASS(PlayerInfoSideSelection, RefCounted)

protected:
	static void _bind_methods();

public:
#define PLAYER_INFO_SIDE_SELECTION_ACCESSORS(m_type, m_name, m_default) \
	m_type get_##m_name() const { return m_name##_; }                  \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
	PLAYER_INFO_SIDE_SELECTION_FIELDS(PLAYER_INFO_SIDE_SELECTION_ACCESSORS)
#undef PLAYER_INFO_SIDE_SELECTION_ACCESSORS

	static Ref<PlayerInfoSideSelection> from_native(const std::optional<opennova::inmatch::PlayerInfoSideSelection> &p_value);
	opennova::inmatch::PlayerInfoSideSelection to_native() const;

private:
#define PLAYER_INFO_SIDE_SELECTION_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;
	PLAYER_INFO_SIDE_SELECTION_FIELDS(PLAYER_INFO_SIDE_SELECTION_MEMBER)
#undef PLAYER_INFO_SIDE_SELECTION_MEMBER
};

// The PLAYER_INFO screen's character memory (engine
// inmatch::PlayerInfoCharacterState, runtime/inmatch/player_info_character.h
// carries the witnesses): the persisted sides validated against the mounted
// Avatars.def, the initial side, the class reads, and the dialog save's
// snapshot with the class stamped across both sides.
class PlayerInfoCharacterState : public RefCounted {
	GDCLASS(PlayerInfoCharacterState, RefCounted)

	opennova::inmatch::PlayerInfoCharacterState value_;

protected:
	static void _bind_methods();

public:
	// The profile's shown side and class when it names them, and each side's
	// saved selection (null = never saved).
	void set_persisted(bool p_has_team, int p_team, bool p_has_class, int p_player_class,
			const Ref<PlayerInfoSideSelection> &p_blue, const Ref<PlayerInfoSideSelection> &p_red);
	// Null = no Avatars.def (no side survives).
	void set_database(const Ref<AvatarDatabase> &p_db);
	int initial_team(int p_authored_team) const { return value_.initial_team(p_authored_team); }
	Ref<PlayerInfoSideSelection> side_selection(int p_side) const;
	int player_class(int p_side) const { return value_.player_class(p_side); }
	// [current, blue, red], each null when absent.
	TypedArray<PlayerInfoSideSelection> snapshot(int p_team, int p_nationality, int p_division,
			int p_combo, int p_selected_class);
};

} // namespace godot
