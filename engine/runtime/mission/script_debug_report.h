// mission::script_debug_report — the mission scripts' live state for the F3
// Script window (ADR 0039 dev tooling; ADR 0042 d5, the one engine function
// per fact): the WAC program and VM (compile diagnostics, per-event fired /
// active state, the clock, the RNG, the dispatch count), the BMS events
// (latches and timers), the mission and global variable banks, and the
// runtime-gap census (the script commands the port has not witnessed yet).
// The retail engine's script-state debug page showed the first compile error
// [orig: Debug_DrawScriptState @0x4F64C0, the read @0x4f652a]; everything
// else here is port-side tooling.
#pragma once

#include <runtime/world/mission_diagnostics.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission {

class MissionKernel;

struct ScriptWacEventRow {
	uint32_t last_fired_tick = 0;
	uint32_t fired_count = 0;
	bool ever_fired = false;
	bool active = false;
};

struct ScriptWacDiagnosticRow {
	std::string text; // "file (line) message"
	bool error = false;
};

struct ScriptBmsEventRow {
	int32_t index = 0;
	int32_t trigger_count = 0;
	int32_t action_count = 0;
	bool active = false;
	bool fired = false; // active and its activation delay elapsed (the Event trigger's read)
	uint16_t repeat_countdown = 0;
	uint16_t repeat_reload = 0;
	uint16_t activate_countdown = 0;
	uint16_t activate_reload = 0;
};

struct ScriptDebugReport {
	// WAC.
	bool wac_loaded = false;
	bool wac_paused = false;
	bool wac_initial_executed = false;
	std::string first_error; // the retail page's line; empty = compiled clean
	std::vector<ScriptWacDiagnosticRow> diagnostics;
	std::vector<std::string> source_names;
	uint32_t code_words = 0;
	uint32_t runs = 0;           // completed executions (diagnostic count)
	uint32_t time = 0;           // the WAC time word (Ticks)
	int32_t divider = 0;         // ticks toward the next execution (of 62)
	int32_t accumulator = 0;     // the VM accumulator
	uint32_t rng_seed = 0;
	uint64_t dispatch_count = 0;
	int32_t current_event = 0;
	std::vector<ScriptWacEventRow> wac_events;
	// BMS.
	std::vector<ScriptBmsEventRow> bms_events;
	int32_t awol_count = 0;
	bool second_time_through = false;
	// The variable banks (V0..V511, G0..G255), raw 32-bit.
	std::array<int32_t, 512> mission_vars{};
	std::array<int32_t, 256> global_vars{};
	// The runtime-gap census.
	std::vector<world::RuntimeGap> runtime_gaps;
	uint64_t runtime_gap_calls = 0;
};

ScriptDebugReport script_debug_report(const MissionKernel &kernel);

}  // namespace opennova::mission
