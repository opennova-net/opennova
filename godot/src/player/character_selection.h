#pragma once

// PLAYER_INFO's dialog as its save reads it back (engine profile::CharacterSelection,
// runtime/profile/player_profiles.h carries the rules): the PLAYERCLASS value, the shown side,
// that side's character when the screen's lists hold one, the shown side's kit page when a weapon
// table is loaded, and the PLAYERNAME text when the screen authors the control. The typed record
// PlayerProfiles.apply_character_selection / accept_player_info take.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/profile/player_profiles.h>

namespace godot {

class WeaponKitEntry;

class CharacterSelection : public RefCounted {
	GDCLASS(CharacterSelection, RefCounted)

	opennova::profile::CharacterSelection value_;

protected:
	static void _bind_methods();

public:
	const opennova::profile::CharacterSelection &native() const { return value_; }

	int get_player_class() const { return value_.player_class; }
	void set_player_class(int p_class) { value_.player_class = p_class; }
	int get_team() const { return value_.team; }
	void set_team(int p_team) { value_.team = p_team; }

	// The shown side's character bytes (the side block's +1, +2 and +4).
	void set_character(int p_avatar_a, int p_avatar_b, int p_avatar_packed);
	void clear_character() { value_.character.reset(); }
	bool has_character() const { return value_.character.has_value(); }
	int get_avatar_a() const { return value_.character ? value_.character->avatar_a : -1; }
	int get_avatar_b() const { return value_.character ? value_.character->avatar_b : -1; }
	int get_avatar_packed() const { return value_.character ? value_.character->avatar_packed : -1; }

	// The shown side's kit page: the class whose page the populate restored (outside 5..9 the
	// page of player_class) and its rows.
	void set_kit(int p_page_class, const TypedArray<WeaponKitEntry> &p_entries);
	void clear_kit() { value_.kit.reset(); }
	bool has_kit() const { return value_.kit.has_value(); }
	int get_kit_class() const { return value_.kit ? value_.kit->page_class : -1; }
	TypedArray<WeaponKitEntry> get_kit() const;

	// The PLAYERNAME text (a Godot string; the record keeps the game's code-page bytes).
	void set_name(const String &p_name);
	void clear_name() { value_.name.reset(); }
	bool has_name() const { return value_.name.has_value(); }
	String get_name() const;

	// The PLAYER_INFO companion's ACCEPT snapshot, the profile Dictionary the menu screens still
	// speak (engine inmatch::PlayerInfoScreen::snapshot plus the screen's `kit` / `kit_class` and
	// an optional `name`), read as the record: `player_class` and `team`, `side_profiles[team]`
	// {avatar_a, avatar_b, avatar_packed} as the character when it is not empty, `kit` (rows of
	// {name, ammo_primary, ammo_secondary, flags}) into the page of `kit_class` when present. A
	// missing value reads as one the save refuses. Null for a kit row that is not a Dictionary.
	static Ref<CharacterSelection> from_player_info_snapshot(const Dictionary &p_snapshot);
};

} // namespace godot
