#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/join_character_profile.h>
#include <runtime/world/player_present.h>

namespace godot {

// The local player's staged PLAYER_INFO selection for the next mission spawn
// (ADR 0043 slice G8; the former `_local_player_spawn_loadout` Dictionary):
// the three kit slots with their clip counts, the profile class, and the two
// per-side character selections the join profile projects (nationality /
// division / combo tree indices + class). Presence is part of the record --
// a slot whose name was never set is absent (the historical fallback), a slot
// set to "" explicitly requests NONE; the class likewise commits only when it
// was set. The shell decodes the PLAYER_INFO snapshot once through
// `from_profile`; a test authors the record through its properties. Consumed
// by GameWorld.set_local_player_spawn_loadout (LocalPlayerVisuals applies it
// at runtime start) and by the join-profile projection
// (AvatarDatabase.character_join_profile_from_loadout).
#define PLAYER_SPAWN_SIDE_FIELDS(X) \
	X(nationality, -1)              \
	X(division, -1)                 \
	X(combo, -1)                    \
	X(player_class, opennova::inmatch::kJoinDefaultPlayerClass)

class PlayerSpawnLoadout : public RefCounted {
	GDCLASS(PlayerSpawnLoadout, RefCounted)

public:
	enum Side {
		SIDE_BLUE = 0,
		SIDE_RED = 1,
	};

	// The shell edge: decode the PLAYER_INFO profile snapshot (primary /
	// secondary / accessory + <slot>_clips, player_class, side_profiles).
	static Ref<PlayerSpawnLoadout> from_profile(const Dictionary &p_profile);

	// The kit slots: setting a name marks the slot present.
	String get_primary() const { return slots_[0].name; }
	void set_primary(const String &p_name) { set_slot(0, p_name); }
	bool has_primary() const { return slots_[0].present; }
	int get_primary_clips() const { return slots_[0].clips; }
	void set_primary_clips(int p_clips) { slots_[0].clips = p_clips; }
	String get_secondary() const { return slots_[1].name; }
	void set_secondary(const String &p_name) { set_slot(1, p_name); }
	bool has_secondary() const { return slots_[1].present; }
	int get_secondary_clips() const { return slots_[1].clips; }
	void set_secondary_clips(int p_clips) { slots_[1].clips = p_clips; }
	String get_accessory() const { return slots_[2].name; }
	void set_accessory(const String &p_name) { set_slot(2, p_name); }
	bool has_accessory() const { return slots_[2].present; }
	int get_accessory_clips() const { return slots_[2].clips; }
	void set_accessory_clips(int p_clips) { slots_[2].clips = p_clips; }
	// The profile class: setting it marks it present.
	int get_player_class() const { return player_class_; }
	void set_player_class(int p_class) {
		player_class_ = p_class;
		has_player_class_ = true;
	}
	bool has_player_class() const { return has_player_class_; }

	// The two character selections (SIDE_BLUE / SIDE_RED).
	bool side_present(int p_side) const;
	void set_side_present(int p_side, bool p_present);
#define PLAYER_SPAWN_SIDE_ACCESSORS(m_name, m_default)     \
	int side_##m_name(int p_side) const;                    \
	void set_side_##m_name(int p_side, int p_value);
	PLAYER_SPAWN_SIDE_FIELDS(PLAYER_SPAWN_SIDE_ACCESSORS)
#undef PLAYER_SPAWN_SIDE_ACCESSORS

	// The engine projections (C++ only): the kit slots for the spawn
	// projection and the two join-side selections.
	opennova::world::SpawnLoadoutInput engine_input() const;
	void fill_join_sides(opennova::inmatch::JoinSideSelection (&r_sides)[2]) const;

protected:
	static void _bind_methods();

private:
	struct Slot {
		bool present = false;
		String name;
		int clips = -1;
	};
	struct SideSelection {
		bool present = false;
#define PLAYER_SPAWN_SIDE_MEMBER(m_name, m_default) int m_name = m_default;
		PLAYER_SPAWN_SIDE_FIELDS(PLAYER_SPAWN_SIDE_MEMBER)
#undef PLAYER_SPAWN_SIDE_MEMBER
	};

	void set_slot(int p_index, const String &p_name) {
		slots_[p_index].present = true;
		slots_[p_index].name = p_name;
	}
	static bool valid_side(int p_side) { return p_side == SIDE_BLUE || p_side == SIDE_RED; }

	Slot slots_[opennova::world::kSpawnLoadoutSlotCount];
	bool has_player_class_ = false;
	int player_class_ = 0;
	SideSelection sides_[2];
};

} // namespace godot

VARIANT_ENUM_CAST(godot::PlayerSpawnLoadout::Side);
