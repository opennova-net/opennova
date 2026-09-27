// opennova-3di weapon timing: compile a rig's authored weapon action windows
// (the Blender add-on's Action-local markers and firing cadence, as seconds)
// into weapon.def ACTION rows, and measure them with the engine's own weapon
// FSM (runtime/world/weapon_fsm.h). No imported weapon or animation is an
// input (ADR 0047 decision 14).
#pragma once

#include <string>
#include <vector>

#include <runtime/world/weapon_fsm.h>

namespace threedi_cli {

// One authored ACTION: the row's name (a weapon_action suffix) and references,
// its active phase (entry to the marked pose) and its recovery after it.
struct WeaponTimingAction {
	opennova::world::WeaponFsmActionRow row;
	double active_seconds = 0;
	double recovery_seconds = 0;
};

struct WeaponTimingRequest {
	std::string mode = "semi"; // semi, auto or burst
	double cycle_seconds = 0;  // the requested shot-to-shot period
	std::vector<WeaponTimingAction> actions;
};

// One FSM observation: `scenario` is the action the measured run requested
// (fire, or the non-firing action previewed on its own); tick 0 is its entry.
struct WeaponTimingEvent {
	int tick = 0;
	int action = 0;
	std::string kind;
	int clip_ticks = 0;
	int scenario = opennova::world::weapon_action::kFire;
};

struct WeaponTimingPlan {
	std::vector<opennova::world::WeaponFsmActionRow> rows;
	std::vector<WeaponTimingEvent> events;
	int cycle_ticks = 0;
	int ready_tick = -1;
};

bool plan_weapon_timing(const WeaponTimingRequest &request, WeaponTimingPlan &out, std::string &error);
int cmd_weapon_timing(const char *input, const char *output);

} // namespace threedi_cli
