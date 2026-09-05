extends GutTest

# The per-second joiner freeze-tripwire trace is opt-in: off for release play,
# switched on through the typed setter the `net_joiner_diagnostics` debug
# control (MCP game_debug, DebugControlTable) drives.


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


func test_the_debug_control_table_exposes_the_switch_as_a_net_check() -> void:
	# A real MissionRoot over an in-memory default mission owns the
	# Simulation the row writes through; the table reads it through the
	# faked shell seam (DebugHostFixture, rule 11's interface fake).
	var runtime: MissionRoot = WorldFixture.boot_mission_data(
			self, WorldFixture.default_mission(0))
	var sim: Simulation = runtime.get_sim()
	assert_not_null(sim, "the runtime owns a live Simulation")
	var controls := DebugControlTable.new()
	controls.setup(DebugHostFixture.for_runtime(runtime))
	var state := controls.get_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_eq(state.value, false)
	assert_eq(controls.set_value(&"net_joiner_diagnostics", true), OK)
	assert_true(sim.is_joiner_network_diagnostics_enabled())
