// The Simulation's local-player shell state that outlives the kernel (ADR
// 0043 d9): the player's weapon profile record (weapon.sav, player-scoped —
// the kernel's loadout aggregate is mission-scoped), the resident-kit seed
// cursor and the 0x2F push latch, and the
// F3 Weapon window's held-trigger latch. Plain data with no behavior.
#pragma once

#include <formats/playersav/player_sav.h> // player.sav: the profile record's session words
#include <runtime/inmatch/loadout_submit.h> // SessionWeaponProfile (weapon.sav's record)

namespace godot {

struct SimulationPlayerState {
	// The ACTIVE player weapon profile record and the resident kit's seed cursor
	// (engine: inmatch::SessionWeaponProfile carries the record's scope and its legs).
	opennova::inmatch::SessionWeaponProfile weapon;
	// The current player.sav record, whose session words single player's
	// session takes (unset: a fresh profile's).
	opennova::playersav::ProfileRecord profile_record;
	bool profile_record_set = false;
	// Re-entry latch: rebuild_local_player_loadout re-pushes the 0x2F seam once
	// the live equipped combo has settled, so the push must never drive a
	// rebuild back.
	bool pushing_joiner_loadout_kit = false;
	// The F3 Weapon window's held-trigger latch (OR'd into per-tick weapon input).
	bool debug_weapon_fire_held = false;
};

} // namespace godot
