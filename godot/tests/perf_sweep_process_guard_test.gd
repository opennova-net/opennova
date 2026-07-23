extends GutTest

const ProcessGuard := preload("res://tests/perf_probe_process_guard.gd")


class ProcessingNode:
	extends Node

	func _process(_delta: float) -> void:
		pass

	func _physics_process(_delta: float) -> void:
		pass


func test_restore_uses_state_at_phase_start_not_initial_census() -> void:
	var node: Node = add_child_autofree(ProcessingNode.new())
	var census_nodes: Array = [node]
	# Simulate a legitimate mode change while earlier sweep groups are measured.
	node.set_process(false)
	node.set_physics_process(true)

	var states := ProcessGuard.disable_processing(census_nodes)
	assert_false(node.is_processing())
	assert_false(node.is_physics_processing())
	ProcessGuard.restore_processing(states)
	assert_false(node.is_processing(), "the phase-local disabled process state is restored")
	assert_true(node.is_physics_processing(), "the phase-local physics state is restored")
