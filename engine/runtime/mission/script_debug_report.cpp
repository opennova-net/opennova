#include <runtime/mission/script_debug_report.h>

#include <runtime/mission/mission_kernel.h>

namespace opennova::mission {

ScriptDebugReport script_debug_report(const MissionKernel &kernel) {
	ScriptDebugReport out;
	const wac::WacSystem &wac = kernel.wac;
	const wac::Program &program = wac.program();
	const wac::WacSystem::RuntimeState state = wac.capture_runtime_state();
	out.wac_loaded = !program.code.empty();
	out.wac_paused = wac.paused;
	out.wac_initial_executed = state.initial_executed;
	// [orig: Debug_DrawScriptState @0x4F64C0 reads the first compile error
	//  @0x4f652a]
	out.first_error = program.first_error();
	out.source_names = program.source_names;
	for (const wac::Diagnostic &d : program.diagnostics) {
		const std::string file = d.source < program.source_names.size() ? program.source_names[d.source]
																		 : std::string();
		out.diagnostics.push_back({file + " (" + std::to_string(d.line) + ") " + d.message, d.error});
	}
	out.code_words = static_cast<uint32_t>(program.code.size());
	out.runs = state.runs;
	out.time = state.vm.time;
	out.divider = state.accumulator;
	out.accumulator = state.vm.accumulator;
	out.rng_seed = state.vm.rng_seed;
	out.dispatch_count = wac.vm().dispatch_count();
	out.current_event = state.vm.current_event;
	for (const wac::EventState &e : state.vm.events) {
		out.wac_events.push_back({e.last_fired_tick, e.fired_count, e.ever_fired, e.active});
	}

	const auto &events = kernel.events.events();
	for (size_t i = 0; i < events.size(); ++i) {
		const ScriptedEvent &e = events[i];
		ScriptBmsEventRow row;
		row.index = static_cast<int32_t>(i);
		row.trigger_count = static_cast<int32_t>(e.triggers.size());
		row.action_count = static_cast<int32_t>(e.actions.size());
		row.active = e.active != 0;
		row.fired = kernel.events.event_fired(i);
		row.repeat_countdown = e.repeat_countdown;
		row.repeat_reload = e.repeat_reload;
		row.activate_countdown = e.activate_countdown;
		row.activate_reload = e.activate_reload;
		out.bms_events.push_back(row);
	}
	out.awol_count = kernel.events.awol_count();
	out.second_time_through = BmsEventSystem::second_time_through();

	const world::ScriptVarStore &vars = kernel.world.script.vars;
	for (int i = 0; i < world::ScriptVarStore::kMissionVars; ++i) {
		out.mission_vars[static_cast<size_t>(i)] = vars.get_mission(i);
	}
	for (int i = 0; i < world::ScriptVarStore::kGlobalVars; ++i) {
		out.global_vars[static_cast<size_t>(i)] = vars.get_global(i);
	}
	out.runtime_gaps = kernel.world.diagnostics.gaps();
	out.runtime_gap_calls = kernel.world.diagnostics.total_calls();
	return out;
}

}  // namespace opennova::mission
