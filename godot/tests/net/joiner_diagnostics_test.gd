extends GutTest

# The per-second joiner freeze-tripwire trace is opt-in: off for release play,
# switched on through the typed setter the `net_joiner_diagnostics` debug
# control (F3 / MCP game_debug) drives.


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


func test_the_debug_catalog_exposes_the_switch_as_a_net_check() -> void:
	var sim := Simulation.new()
	var session := DebugSession.new()
	DebugCatalog.install(session)
	session.set_target_source(DebugCatalog.TARGET_SIM, func(): return sim)
	var state := session.get_control_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_eq(state.value, false)
	assert_eq(session.set_control_value(&"net_joiner_diagnostics", true), OK)
	assert_true(sim.is_joiner_network_diagnostics_enabled())
	sim.free()
