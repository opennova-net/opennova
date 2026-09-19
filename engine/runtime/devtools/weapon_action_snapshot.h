// The Weapon window's pushed value records (ADR 0042 d6), split by cadence:
// the DEFINITION moves only on an install or an applied edit and is pushed on
// a serial bump; the LIVE state is a scope on a 62.5 Hz signal and is pushed
// every frame the window shows. Plain values — the window never reaches into
// a live World, a MissionKernel or Godot.
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

// The definition: what the dope sheet draws and the properties panel edits.
// Pushed when `serial` moves — a weapon install, an applied edit, the clip
// rings resolving — never per frame. An invalid one clears the window.
struct WeaponDefinitionSnapshot {
	bool valid = false;
	uint64_t serial = 0;
	std::string weapon_name;
	int32_t adm_index = -1;
	int32_t clip_capacity = 0;  // < 0: no clipsize authored, no magazine to track
	bool auto_fire = false;
	bool burst3 = false;
	WeaponActionRow actions[world::weapon_action::kCount];
	std::vector<std::string> clip_keys;  // the ANIM picker: .adm clip keys the weapon registered
};

// The live state: the ACTIVE slot (the borrowed UseGun parent slot when one is
// engaged, else the personal slot), the input gates, and the trace delta.
struct WeaponLiveSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;

	int32_t current = 0;
	int32_t next = 0;
	int32_t prev = 0;
	uint8_t phase = 0;
	int32_t counter = 0;
	int32_t clip = 0;
	int32_t reserve = 0;
	int32_t heat = 0;

	// The real input gates, evaluated by the embedder through the engine
	// predicates, using the readiness contract: an empty string enables the
	// trigger, a non-empty one disables it AND is the tooltip that names which
	// leg refused. The window never queues a request the FSM would reject.
	std::string fire_block;    // the pump's own input gate (dead, seat, UseGun switch)
	std::string reload_block;  // the reload dispatch gate
	std::string scope_block;   // the ADS toggle gate

	bool fire_held = false;    // the devtools hold latch as the embedder holds it
	bool trace_armed = false;  // the engine ring's state, for the window to reconcile REC against

	// Trace samples recorded since the last push, oldest first. The window
	// accumulates them into its own ring; an empty delta is the common case
	// between logic ticks.
	std::vector<world::WeaponTraceSample> trace;
};

}  // namespace opennova::devtools
