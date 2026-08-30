// The Weapon window's pushed value records (ADR 0042 d6): what the embedder
// hands the F3 Weapon window each frame, and the slower-changing name catalogs
// its pickers offer. Plain values — the window never reaches into a live World,
// a MissionKernel or Godot.
#pragma once

#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_fsm.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::devtools {

// One action slot as the window draws it: the BAKED runtime values (what the
// FSM actually runs on) beside the AUTHORED weapon.def row behind them (what a
// re-bake would start from). They differ wherever a delay was authored `auto`.
struct WeaponActionRow {
	// --- the baked slot (world::WeaponFsmAction) ---
	int32_t delay_start = 0;
	int32_t delay_end = 0;
	bool has_anim = false;
	std::string anim_key;
	std::string soundset;
	std::string soundsetend;
	std::string particle;
	std::string particle_userpoint;

	// --- the authored weapon.def row, when one exists ---
	// Three of the twelve slots (scopeup, scopedown, overheated) are never
	// authored in shipped data; they bake from generated defaults and have no
	// row to write back to, so an edit to them is live-only.
	bool authored = false;
	std::string authored_name;  // the ACTION "NAME" exactly as written
	std::string function;       // read-only: the handler registry is unported (D-WPN-1)
	// -1 == `auto`, which is NOT the same authoring as an explicit 0. The
	// parser cannot tell an explicit 0 from an absent key, so those two look
	// alike here; `auto` is always distinguishable.
	int32_t authored_delay_start = 0;
	int32_t authored_delay_end = 0;

	// The resolved length of anim_key in 62.5 Hz ticks; 0 = unresolved. This is
	// what an `auto` delay bakes from, so it is what the ghost bar draws.
	int32_t clip_ticks = 0;
};

// The per-frame record. Pushed every frame rather than on the Entities window's
// 0.5 s cadence: the trace pane is a live scope, and this is a small record.
struct WeaponActionSnapshot {
	bool valid = false;
	std::string weapon_name;
	int32_t adm_index = -1;
	uint64_t logic_tick = 0;

	WeaponActionRow actions[world::weapon_action::kCount];

	// --- the live slot (world::WeaponSlotState) ---
	int32_t current = 0;
	int32_t next = 0;
	int32_t prev = 0;
	uint8_t phase = 0;
	int32_t counter = 0;
	int32_t clip = 0;
	int32_t reserve = 0;
	int32_t heat = 0;
	int32_t clip_capacity = 0;
	bool auto_fire = false;
	bool burst3 = false;

	// The real input-dispatcher gates, evaluated by the embedder through the
	// engine predicates, as the ONED readiness idiom: an empty string enables
	// the trigger, a non-empty one disables it AND is the tooltip that names
	// which leg of the gate refused. The window never queues a request the
	// FSM would reject.
	std::string reload_block;
	std::string scope_block;
	bool player_alive = false;
	bool fire_held = false;

	// Trace samples recorded since the last push, oldest first. The window
	// accumulates them into its own ring; an empty delta is the common case
	// between logic ticks.
	std::vector<world::WeaponTraceSample> trace;
	bool trace_armed = false;
};

// The ANIM picker's catalog. Pushed only when `serial` changes (a weapon
// install) — the window keeps the last one it was given.
//
// There is deliberately no SOUNDSET catalog: the loaded .lwf trigger-set names
// live in the shell's GDScript SoundBank, and the one wiring line that would
// carry them has no legal home (main_game.gd sits exactly on its 1200-line
// ratchet, and the alternative is a 3192-line god file). The sound legs are
// therefore free text plus the trace markers that show when each one fired.
struct WeaponCatalog {
	uint64_t serial = 0;
	std::vector<std::string> clip_keys;  // .adm clip keys the weapon registered
};

}  // namespace opennova::devtools
