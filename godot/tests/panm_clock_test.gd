extends GutTest

const PanmClockScript := preload("res://engine/world/panm_clock.gd")
const NovaObjectModelScript := preload("res://engine/object/nova_object_model.gd")
const PMP := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"


func _open_pmp() -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PMP)), OK)
	return data


func test_clock_samples_once_per_frame_and_wraps_as_a_dword() -> void:
	var clock = PanmClockScript.new()
	assert_true(clock.sample(0x1ffffffff, 7))
	assert_eq(clock.time_ms, 0xffffffff)
	assert_false(clock.sample(123, 7), "a second consumer sees the same frame sample")
	assert_eq(clock.time_ms, 0xffffffff)
	assert_true(clock.sample(0x100000000, 8))
	assert_eq(clock.time_ms, 0, "retail GetTickCount storage wraps at 32 bits")


func test_panm_evaluator_accepts_the_full_unsigned_clock_domain() -> void:
	var data := _open_pmp()
	var at_zero: Dictionary = data.evaluate_panm(0, 0, {})
	var at_wrap: Dictionary = data.evaluate_panm(0, 0x100000000, {})
	assert_eq(at_wrap.keys(), at_zero.keys())
	for key in at_zero:
		assert_true((at_wrap[key] as Transform3D).is_equal_approx(
			at_zero[key] as Transform3D),
			"2^32 milliseconds wraps to zero without signed overflow")


func test_late_spawned_models_share_one_panm_epoch() -> void:
	var data := _open_pmp()
	var clock = PanmClockScript.new()
	clock.set_time_ms_for_test(0)
	var first: Node3D = add_child_autofree(NovaObjectModelScript.new())
	first.set_panm_clock(clock)
	first.set_object_data(data)

	var pose_a: Dictionary = data.evaluate_panm(0, 0, {})
	var pose_b: Dictionary = data.evaluate_panm(0, 640, {})
	var nodes_a: Dictionary = first.get_render_part_nodes()
	var target := -1
	for key in pose_a:
		var section := int(key)
		if nodes_a.has(section) and not (
				pose_a[key] as Transform3D).is_equal_approx(pose_b[key] as Transform3D):
			target = section
			break
	assert_gte(target, 0, "fixture exposes a time-driven visual part")
	if target < 0:
		return

	clock.set_time_ms_for_test(640)
	first.set_panm_clock(clock)
	var second: Node3D = add_child_autofree(NovaObjectModelScript.new())
	second.set_panm_clock(clock)
	second.set_object_data(data)
	assert_eq(first.get_animation_time_ms(), 640)
	assert_eq(second.get_animation_time_ms(), 640)
	var first_part := first.get_render_part_nodes()[target] as Node3D
	var second_part := second.get_render_part_nodes()[target] as Node3D
	assert_true(first_part.transform.is_equal_approx(second_part.transform),
		"spawn time does not create a second animation epoch")
	assert_true(first_part.transform.is_equal_approx(pose_b[target] as Transform3D),
		"visual PANM consumes the clock value passed to the evaluator")
