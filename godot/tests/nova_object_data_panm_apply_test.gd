extends GutTest

# apply_panm_to_nodes: the shared-per-graphic PANM frame cache behind the
# model hot path. Contracts: a fresh caller (revision 0) gets the full pose;
# an unchanged evaluation writes nothing (revision equal, poison survives);
# advancing time writes exactly the parts that moved; a caller that skipped
# revisions (a hidden model re-shown) still lands on the current pose. The
# Dictionary form (evaluate_panm) stays cache-neutral and is the reference.

const PMP := "res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di"


func _data() -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(PMP)), OK)
	return data


func _nodes(count: int) -> Array:
	var out: Array = []
	for _i in range(count):
		var node := Node3D.new()
		add_child_autofree(node)
		out.append(node)
	return out


func test_fresh_apply_matches_evaluate_panm() -> void:
	var data := _data()
	var reference: Dictionary = data.evaluate_panm(0, 0, {})
	assert_false(reference.is_empty(), "the pump jack has parts")
	var nodes := _nodes(reference.size())
	var revision := int(data.apply_panm_to_nodes(0, 0, {}, nodes, 0))
	assert_gt(revision, 0, "the first evaluation mints a revision")
	for key in reference.keys():
		var i := int(key)
		assert_true((nodes[i] as Node3D).transform.is_equal_approx(reference[key]),
				"part %d lands on the evaluate_panm pose" % i)


func test_unchanged_evaluation_writes_nothing() -> void:
	var data := _data()
	var count := (data.evaluate_panm(0, 0, {}) as Dictionary).size()
	var nodes := _nodes(count)
	var revision := int(data.apply_panm_to_nodes(0, 0, {}, nodes, 0))
	var poison := Transform3D(Basis(), Vector3(123, 456, 789))
	(nodes[0] as Node3D).transform = poison
	var second := int(data.apply_panm_to_nodes(0, 0, {}, nodes, revision))
	assert_eq(second, revision, "same time, same ctrl -> same revision")
	assert_true((nodes[0] as Node3D).transform.is_equal_approx(poison),
			"an up-to-date caller gets no writes (the poison survives)")


func test_same_time_noise_calls_sample_each_graphic_instance_independently() -> void:
	var data := _data()
	assert_gt(data.get_part_anim_count(0), 0,
			"the fixture needs one PANM row to turn into a noise probe")
	var anim := 0
	var info: Dictionary = data.get_part_anim_info(0, anim)
	var target_part := int(info.get("transform_as", -1))
	var part_count := int(data.get_render_lod_info(0).get("part_count", 0))
	assert_between(target_part, 0, part_count - 1)
	if target_part < 0 or target_part >= part_count:
		return

	# Low nibble 6 is retail's rand()-backed wave lookup. Keep time, bus, and
	# authored track identical across both calls so only the per-submission CRT
	# sample can distinguish the two graphic instances.
	# [orig: PANM_SampleTrack @ 0x5B2270; wave_lookup @ 0x5DE6B0]
	assert_true(data.set_part_anim_channel_enabled(
			0, anim, "translation", true))
	assert_true(data.set_part_anim_track_field(
			0, anim, "translation", "control", 0x36))
	assert_true(data.set_part_anim_track_field(
			0, anim, "translation", "control_param", 0))
	assert_true(data.set_part_anim_track_field(
			0, anim, "translation", "rate", 0))
	assert_true(data.set_part_anim_track_field(
			0, anim, "translation", "start", 0))
	assert_true(data.set_part_anim_track_field(
			0, anim, "translation", "end", 32767))

	var first_nodes := _nodes(part_count)
	var second_nodes := _nodes(part_count)
	var serial_before := data.get_panm_evaluation_serial()
	data.apply_panm_to_nodes(
			0, 0, {}, first_nodes, 0)
	var serial_after_first := data.get_panm_evaluation_serial()
	data.apply_panm_to_nodes(
			0, 0, {}, second_nodes, 0)
	var serial_after_second := data.get_panm_evaluation_serial()

	assert_eq(serial_after_first, serial_before + 1,
			"the first graphic instance evaluates its noise track")
	assert_eq(serial_after_second, serial_after_first + 1,
			"same-time noise evaluates again for the second graphic instance")


func test_time_advance_writes_only_moved_parts() -> void:
	var data := _data()
	var t0: Dictionary = data.evaluate_panm(0, 0, {})
	var t1: Dictionary = data.evaluate_panm(0, 640, {})
	var moved := -1
	var still := -1
	for key in t0.keys():
		if (t0[key] as Transform3D).is_equal_approx(t1[key]):
			if still < 0:
				still = int(key)
		elif moved < 0:
			moved = int(key)
		if moved >= 0 and still >= 0:
			break
	assert_gte(moved, 0, "the pump jack animates at least one part over 640 ms")
	assert_gte(still, 0, "the pump jack keeps at least one part still over 640 ms")
	var nodes := _nodes(t0.size())
	var revision := int(data.apply_panm_to_nodes(0, 0, {}, nodes, 0))
	var poison := Transform3D(Basis(), Vector3(9, 9, 9))
	(nodes[still] as Node3D).transform = poison
	var second := int(data.apply_panm_to_nodes(0, 640, {}, nodes, revision))
	assert_gt(second, revision, "movement mints a new revision")
	assert_true((nodes[moved] as Node3D).transform.is_equal_approx(t1[moved]),
			"the moved part is rewritten to the new pose")
	assert_true((nodes[still] as Node3D).transform.is_equal_approx(poison),
			"an unmoved part is not rewritten (the poison survives)")


func test_stale_caller_catches_up_after_skipped_revisions() -> void:
	var data := _data()
	var count := (data.evaluate_panm(0, 0, {}) as Dictionary).size()
	var nodes := _nodes(count)
	var revision := int(data.apply_panm_to_nodes(0, 0, {}, nodes, 0))
	# Another instance of the same graphic (its own nodes elsewhere) advances
	# the shared cache twice while our caller is hidden.
	var elsewhere: Array = []
	var r1 := int(data.apply_panm_to_nodes(0, 640, {}, elsewhere, revision))
	var r2 := int(data.apply_panm_to_nodes(0, 1280, {}, elsewhere, r1))
	assert_gt(r2, revision, "the shared evaluation moved on")
	# The re-shown caller applies with its stale revision: every part that
	# moved since must land on the CURRENT pose, not the missed intermediate.
	var final := int(data.apply_panm_to_nodes(0, 1280, {}, nodes, revision))
	assert_eq(final, r2)
	var reference: Dictionary = data.evaluate_panm(0, 1280, {})
	for key in reference.keys():
		var i := int(key)
		assert_true((nodes[i] as Node3D).transform.is_equal_approx(reference[key]),
				"part %d caught up to the live pose" % i)
