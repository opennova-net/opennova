extends GutTest

const PropertyGuard := preload("res://tests/perf_probe_property_guard.gd")


class ProbeTarget:
	extends Node

	var probe_enabled := false
	var setter_values: Array[bool] = []

	func set_probe_enabled(enabled: bool) -> void:
		probe_enabled = enabled
		setter_values.append(enabled)

	func is_probe_enabled() -> bool:
		return probe_enabled


func test_restore_all_preserves_originally_false_and_true_values() -> void:
	var guard = PropertyGuard.new()
	var false_target := ProbeTarget.new()
	var true_target := ProbeTarget.new()
	true_target.probe_enabled = true
	assert_eq(guard.set_temporary(false_target, &"probe_enabled", true), OK)
	assert_eq(guard.set_temporary(true_target, &"probe_enabled", false), OK)
	assert_true(false_target.probe_enabled)
	assert_false(true_target.probe_enabled)

	assert_eq(guard.restore_all(), OK)
	assert_false(false_target.probe_enabled)
	assert_true(true_target.probe_enabled)
	false_target.free()
	true_target.free()


func test_first_temporary_write_owns_the_restore_snapshot() -> void:
	var guard = PropertyGuard.new()
	var target := ProbeTarget.new()
	assert_eq(guard.set_temporary(target, &"probe_enabled", true), OK)
	assert_eq(guard.set_temporary(target, &"probe_enabled", false), OK)
	assert_eq(guard.restore_all(), OK)
	assert_false(target.probe_enabled, "later writes do not replace the original snapshot")
	target.free()


func test_restore_property_is_immediate_and_uses_the_public_setter() -> void:
	var guard = PropertyGuard.new()
	var target := ProbeTarget.new()
	assert_eq(guard.set_temporary(
			target, &"probe_enabled", true, &"set_probe_enabled"), OK)
	assert_true(target.probe_enabled)
	assert_eq(guard.restore_property(target, &"probe_enabled"), OK)
	assert_false(target.probe_enabled)
	assert_eq(target.setter_values, [true, false])
	target.free()


func test_invalid_or_released_objects_are_safe() -> void:
	var guard = PropertyGuard.new()
	assert_eq(guard.set_temporary(null, &"probe_enabled", true), ERR_INVALID_PARAMETER)
	var target := ProbeTarget.new()
	assert_eq(guard.set_temporary(target, &"probe_enabled", true), OK)
	target.free()
	assert_eq(guard.restore_all(), OK, "released targets need no restoration")


func test_restore_calls_are_idempotent() -> void:
	var guard = PropertyGuard.new()
	var target := ProbeTarget.new()
	assert_eq(guard.set_temporary(target, &"probe_enabled", true), OK)
	assert_eq(guard.restore_property(target, &"probe_enabled"), OK)
	assert_eq(guard.restore_property(target, &"probe_enabled"), OK)
	assert_eq(guard.restore_all(), OK)
	assert_eq(guard.restore_all(), OK)
	assert_false(target.probe_enabled)
	target.free()


func test_method_state_is_snapshotted_and_restored() -> void:
	var guard = PropertyGuard.new()
	var target := ProbeTarget.new()
	target.probe_enabled = true
	assert_eq(guard.set_temporary_method(
			target,
			&"process_enabled",
			&"is_probe_enabled",
			&"set_probe_enabled",
			false), OK)
	assert_false(target.probe_enabled)
	assert_eq(guard.restore_property(target, &"process_enabled"), OK)
	assert_true(target.probe_enabled)
	assert_eq(target.setter_values, [false, true])
	target.free()
