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


# The in-world shell the table reads: a GameShell answering one real runtime
# (rule 11's sanctioned fake: public verbs of a GDScript shell class).
class RuntimeShell:
	extends GameShell

	var runtime: MissionRoot = null

	func get_runtime() -> MissionRoot:
		return runtime


func test_the_debug_control_table_exposes_the_switch_as_a_net_check() -> void:
	# A real MissionRoot over an in-memory default mission owns the
	# Simulation the row writes through.
	var runtime: MissionRoot = WorldFixture.boot_mission_data(
			self, WorldFixture.default_mission(0))
	var sim: Simulation = runtime.get_sim()
	assert_not_null(sim, "the runtime owns a live Simulation")
	var shell: RuntimeShell = autofree(RuntimeShell.new())
	shell.runtime = runtime
	var adapter: GameDebugAdapter = autofree(GameDebugAdapter.new())
	adapter.configure(shell)
	var controls := adapter.get_debug_controls()
	var state := controls.get_control_state(&"net_joiner_diagnostics")
	assert_true(state.available)
	assert_eq(state.value, false)
	assert_eq(controls.set_control_value(&"net_joiner_diagnostics", true), OK)
	assert_true(sim.is_joiner_network_diagnostics_enabled())
