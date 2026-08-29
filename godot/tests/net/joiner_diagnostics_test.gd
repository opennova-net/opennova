extends GutTest

# The per-second joiner freeze-tripwire trace is opt-in: off for release play,
# switched on through the typed setter the `net_joiner_diagnostics` debug
# control (MCP game_debug, DebugControls) drives.


func test_joiner_network_diagnostics_are_explicitly_opt_in() -> void:
	var sim := Simulation.new()
	assert_false(sim.is_joiner_network_diagnostics_enabled(),
			"release/default play emits no per-second joiner diagnostic")
	assert_false(bool(sim.get_joiner_network_diagnostics()["enabled"]))
	sim.set_joiner_network_diagnostics_enabled(true)
	assert_true(sim.is_joiner_network_diagnostics_enabled(),
			"the debug control enables the live trace")
	assert_true(bool(sim.get_joiner_network_diagnostics()["enabled"]),
			"the snapshot reports the live switch")
	sim.set_joiner_network_diagnostics_enabled(false)
	assert_false(sim.is_joiner_network_diagnostics_enabled(),
			"switching it back off is immediate")
	sim.free()


class RuntimeStub:
	extends MissionPresentation

	var stub_sim: Simulation = null

	func get_sim() -> Simulation:
		return stub_sim


func test_the_debug_control_table_exposes_the_switch_as_a_net_check() -> void:
	var sim: Simulation = autofree(Simulation.new())
	var runtime: RuntimeStub = autofree(RuntimeStub.new())
	runtime.stub_sim = sim
	var seams := GameShellSeams.new()
	seams.runtime_source = func(): return runtime
	seams.world_source = func(): return null
	seams.presenter_source = func(): return null
	var adapter: GameDebugAdapter = autofree(GameDebugAdapter.new())
	adapter.configure(seams)
	var controls := adapter.get_debug_controls()
	var state := controls.get_control_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_eq(state.value, false)
	assert_eq(controls.set_control_value(&"net_joiner_diagnostics", true), OK)
	assert_true(sim.is_joiner_network_diagnostics_enabled())
