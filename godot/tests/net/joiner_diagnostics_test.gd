extends GutTest

const DIAGNOSTICS_ENV := "OPENNOVA_NET_DIAGNOSTICS"

var _saved_value := ""
var _had_value := false


func before_each() -> void:
	_saved_value = OS.get_environment(DIAGNOSTICS_ENV)
	_had_value = not _saved_value.is_empty()


func after_each() -> void:
	OS.set_environment(DIAGNOSTICS_ENV, _saved_value if _had_value else "")


func test_joiner_network_diagnostics_are_explicitly_opt_in() -> void:
	var sim := NovaSimulation.new()
	OS.set_environment(DIAGNOSTICS_ENV, "")
	assert_true(sim.has_method("is_joiner_network_diagnostics_enabled"),
			"the diagnostic gate has a public observation seam")
	if not sim.has_method("is_joiner_network_diagnostics_enabled"):
		sim.free()
		return
	assert_false(bool(sim.call("is_joiner_network_diagnostics_enabled")),
			"release/default play emits no per-second joiner diagnostic")
	OS.set_environment(DIAGNOSTICS_ENV, "1")
	assert_true(bool(sim.call("is_joiner_network_diagnostics_enabled")),
			"OPENNOVA_NET_DIAGNOSTICS=1 enables the live trace")
	OS.set_environment(DIAGNOSTICS_ENV, "false")
	assert_false(bool(sim.call("is_joiner_network_diagnostics_enabled")),
			"an explicit false value does not accidentally enable diagnostics")
	sim.free()
