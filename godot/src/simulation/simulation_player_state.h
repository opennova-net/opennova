// The Simulation's local-player shell state that outlives the kernel (ADR
// 0043 d9): the player's weapon profile record (weapon.sav, player-scoped —
// the kernel's loadout aggregate is mission-scoped), the resident-kit seed
// cursor and the 0x2F push latch, the HUD map-control state machine, and the
// F3 Weapon window's held-trigger latch. Plain data with no behavior.
#pragma once

#include <formats/playersav/weapon_sav.h> // weapon.sav: the per-side profile class + kit pages
#include <runtime/hud/hud_minimap.h>      // HudMapControl

namespace godot {

struct SimulationPlayerState {
	// The M-cycle map mode + the two radar zooms — the engine-side state
	// machine carries the retail lifecycle (cycle, zoom routing, spawn
	// reset, the dead-player clear); Simulation only routes requests and
	// tick edges into it (witness at hud::HudMapControl).
	opennova::hud::HudMapControl hud_map_control;
	// The ACTIVE player weapon profile record — retail's g_charSelClass slot: two
	// side blocks (blue/red), each carrying the class byte that selects both the wire
	// class and one of five 2048-byte kit pages, plus the single-player page.
	// (engine: base/gameprofile/required_resources.c)
	// Seeded from the shipped defaults so it is NEVER empty even when weapon.sav is
	// absent (engine: formats/playersav/weapon_sav.cpp).
	opennova::playersav::Record weapon_profile =
			opennova::playersav::make_defaults().slots[0];
	bool weapon_profile_loaded = false;
	// Which side the resident kit buffer was last copied from (-1 = never seeded).
	int weapon_profile_seeded_side = -1;
	// Re-entry latch: rebuild_local_player_loadout re-pushes the 0x2F seam once
	// the live equipped combo has settled, so the push must never drive a
	// rebuild back.
	bool pushing_joiner_loadout_kit = false;
	// The F3 Weapon window's held-trigger latch (OR'd into per-tick weapon input).
	bool debug_weapon_fire_held = false;
};

} // namespace godot
