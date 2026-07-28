extends GutTest

# The unified MissionPresentPass applies each entity's transform + PANM part channels + visibility onto
# its placed node every tick, from ONE batched sim snapshot. It consolidates the old game path
# (MissionCommandHost: PANM only, by bms_id) and editor path (MissionSimDriver._apply: transform only,
# by (kind,index)). Asset-free: a fake sim emitting a PF-layout snapshot, the real MissionEntityRegistry
# resolver behaviour faked by a tiny index, and fake models capturing transform/phase/visible.

const PresentPass := preload("res://engine/world/mission_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class FakeModel:
	extends Node3D
	var phases: Array = []        # [channel, phase]
	var body_calls: Array = []    # [key_or_slot, phase]
	var overlay_calls: Array = []
	var right_hand_collapse_calls: Array[bool] = []
	var ctrl_values: Dictionary = {}
	var cleared_controls: Array[String] = []
	var part_control_names: Dictionary = {}
	func set_part_phase(channel: int, phase: int) -> void:
		phases.append([channel, phase])
		var control_name := String(part_control_names.get(channel, ""))
		if not control_name.is_empty():
			ctrl_values[control_name] = phase
	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append([key, phase_ticks])
	func play_body_anim_at(slot: int, phase_ticks: int) -> void:
		body_calls.append([slot, phase_ticks])
	func play_body_anim(slot: int) -> void:
		body_calls.append([slot, -1])
	func set_aim_overlay(deltas: Array) -> void:
		overlay_calls.append(deltas)
	func set_right_hand_collapsed(collapsed: bool) -> void:
		right_hand_collapse_calls.append(collapsed)
	func set_ctrl_value(name: String, value: int) -> void:
		ctrl_values[name] = value
	func clear_ctrl_value(name: String) -> void:
		ctrl_values.erase(name)
		cleared_controls.append(name)


# resolve(bms_id, kind, index) like MissionEntityRegistry: bms_id primary, (kind,index) fallback.
class FakeIndex:
	extends RefCounted
	var by_bms_id: Dictionary = {}
	var by_kind_index: Dictionary = {}  # "kind:index" -> Node
	func resolve(bms_id: int, kind: int, index: int) -> Node:
		var n: Variant = null
		if bms_id != 0:
			n = by_bms_id.get(bms_id, null)
		if (n == null or not is_instance_valid(n)) and kind >= 0 and index >= 0:
			n = by_kind_index.get("%d:%d" % [kind, index], null)
		return n if (n != null and is_instance_valid(n)) else null


class CountingIndex:
	extends FakeIndex
	var resolve_calls := 0
	var generation := 1
	func resolve(bms_id: int, kind: int, index: int) -> Node:
		resolve_calls += 1
		return super.resolve(bms_id, kind, index)
	func get_generation() -> int:
		return generation


# Emits a flat PF-layout snapshot, just like NovaSimulation.get_present_snapshot(). Each entity is a
# Dictionary of overrides; unset fields default sanely (alive, not hidden, identity).
class FakeSim:
	extends RefCounted
	var entities: Array = []
	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE
	func get_present_snapshot() -> PackedFloat32Array:
		var stride: int = NovaSimulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var e: Dictionary = entities[i]
			var b := i * stride
			out[b + NovaSimulation.PF_KIND] = float(e.get("kind", -1))
			out[b + NovaSimulation.PF_INDEX] = float(e.get("index", -1))
			out[b + NovaSimulation.PF_BMS_ID] = float(e.get("bms_id", 0))
			out[b + NovaSimulation.PF_TYPE_ID] = float(e.get("type_id", 0))
			out[b + NovaSimulation.PF_WIRE_HANDLE] = float(e.get("handle", 0))
			out[b + NovaSimulation.PF_NET_ID] = float(e.get("net_id", 0))
			out[b + NovaSimulation.PF_POS_X] = float(e.get("pos_x", 0.0))
			out[b + NovaSimulation.PF_POS_Y] = float(e.get("pos_y", 0.0))
			out[b + NovaSimulation.PF_POS_Z] = float(e.get("pos_z", 0.0))
			out[b + NovaSimulation.PF_YAW_DEG] = float(e.get("yaw_deg", 0.0))
			out[b + NovaSimulation.PF_PHASE1] = float(e.get("phase1", 0))
			out[b + NovaSimulation.PF_ACTIVE1] = float(e.get("active1", 0))
			out[b + NovaSimulation.PF_PHASE2] = float(e.get("phase2", 0))
			out[b + NovaSimulation.PF_ACTIVE2] = float(e.get("active2", 0))
			out[b + NovaSimulation.PF_BODY_ANIM_SLOT] = float(e.get("body_anim_slot", -1))
			out[b + NovaSimulation.PF_ANIM_STATE] = float(e.get("anim_state", -1))
			out[b + NovaSimulation.PF_ANIM_PHASE_TICKS] = float(e.get("anim_phase", 0))
			out[b + NovaSimulation.PF_HIDDEN] = float(e.get("hidden", 0))
			out[b + NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					e.get("local_view_suppressed", 0))
			out[b + NovaSimulation.PF_ALIVE] = float(e.get("alive", 1))
			out[b + NovaSimulation.PF_AIM_OVERLAY_VALID] = float(
					e.get("aim_overlay_valid", 0))
			var body: Vector3 = e.get("aim_body", Vector3.ZERO)
			out[b + NovaSimulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[b + NovaSimulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[b + NovaSimulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[b + NovaSimulation.PF_EMPLACED_CONTROLS_VALID] = float(
					e.get("emplaced_controls_valid", 0))
			out[b + NovaSimulation.PF_EWEAP_GUNYAW] = float(
					e.get("emplaced_gun_yaw", 0))
			out[b + NovaSimulation.PF_EWEAP_GUNPITCH] = float(
					e.get("emplaced_gun_pitch", 0))
			out[b + NovaSimulation.PF_RIGHT_HAND_COLLAPSED] = float(
					e.get("right_hand_collapsed", 0))
			var angles: PackedVector3Array = e.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var a := angles[cls]
				var ob := (b + NovaSimulation.PF_AIM_ANGLES
						+ cls * NovaSimulation.PF_AIM_CLASS_STRIDE)
				out[ob] = a.x
				out[ob + 1] = a.y
				out[ob + 2] = a.z
		return out


class RevisionFakeSim:
	extends FakeSim
	var layout_revision := 1
	func get_present_layout_revision() -> int:
		return layout_revision


func _make_pass(index, sim, options: Dictionary = {}) -> Object:
	var p := PresentPass.new()
	p.setup(sim, index, options)
	return p


func test_active_channel_poses_to_phase() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1001: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1001, "active1": 1, "phase1": 32768 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 1, "one channel posed")
	assert_eq(int((model.phases[0] as Array)[0]), 1, "channel 1")
	assert_eq(int((model.phases[0] as Array)[1]), 32768, "engine-computed phase passes through")


func test_inactive_channel_not_posed() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1, "active1": 0, "phase1": 5 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 0, "an untouched channel is left at its default pose")


func test_both_channels_posed() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 7: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 7, "active1": 1, "phase1": 100, "active2": 1, "phase2": 200 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 2, "both active channels posed")


func test_emplaced_weapon_uses_named_controls_and_clears_them() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 8: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 8,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x1234,
		"emplaced_gun_pitch": 0xFEDC,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x1234,
		"EWEAP_GUNPITCH": 0xFEDC,
	}, "semantic controls do not alias model-order PLAYPARTANIM channels")
	assert_true(model.phases.is_empty())

	sim.entities[0]["emplaced_controls_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty(),
			"dismount/death clears retained EWEAP controls")
	assert_has(model.cleared_controls, "EWEAP_GUNYAW")
	assert_has(model.cleared_controls, "EWEAP_GUNPITCH")


func test_dismount_restores_generic_part_values_for_the_same_registers() -> void:
	var model := FakeModel.new()
	model.part_control_names = {
		1: "EWEAP_GUNYAW",
		2: "EWEAP_GUNPITCH",
	}
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 9: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 9,
		"active1": 1,
		"phase1": 0x1111,
		"active2": 1,
		"phase2": 0xEEEE,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2222,
		"emplaced_gun_pitch": 0xDDDD,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x2222,
		"EWEAP_GUNPITCH": 0xDDDD,
	}, "live gunner controls outrank generic model-order values")

	sim.entities[0]["emplaced_controls_valid"] = 0
	sim.entities[0]["phase1"] = 0x3333
	sim.entities[0]["phase2"] = 0xCCCC
	presenter.present()
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x3333,
		"EWEAP_GUNPITCH": 0xCCCC,
	}, "dismount clears stale semantic ownership before generic PLAYPARTANIM")


func test_body_clip_poses_to_sim_anim_state_phase() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 11: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 11, "body_anim_slot": 1, "anim_state": 43, "anim_phase": 9 }]
	_make_pass(index, sim).present()
	assert_eq(model.body_calls.size(), 1, "one body clip posed")
	assert_eq(String((model.body_calls[0] as Array)[0]), "anim_idle", "infantry anim state resolves to .adm key")
	assert_eq(int((model.body_calls[0] as Array)[1]), 9, "sim clip phase passes through")


func test_placed_model_applies_snapshot_overlay_in_body_frame() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 12: model }
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(2.0 + i, 20.0 + i, -3.0 + i))
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 12,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(7.0, 41.0, -5.0),
		"aim_angles": angles,
	}]
	_make_pass(index, sim).present()
	assert_eq(model.overlay_calls.size(), 1)
	var deltas: Array = model.overlay_calls[0]
	assert_eq(deltas.size(), 9, "all overlay classes use the packed selector result")
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(7.0, 41.0, -5.0))
	assert_true(model.basis.is_equal_approx(body_basis),
			"placed body renders in the same frame used to form overlay deltas")
	var expected_head := (
			body_basis.inverse()
			* MissionObjectPlacer.bms_to_godot_basis(angles[8]))
	assert_true((deltas[8] as Basis).is_equal_approx(expected_head))


func test_placed_model_clears_overlay_when_snapshot_selector_is_invalid() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 13: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 13, "aim_overlay_valid": 0 }]
	_make_pass(index, sim).present()
	assert_eq(model.overlay_calls, [[]],
			"an unknown selector clears any pose retained by the model")


func test_placed_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 14: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 14,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	sim.entities[0]["right_hand_collapsed"] = 0
	presenter.present()
	assert_eq(model.right_hand_collapse_calls, [true, false],
			"the placed pose consumes the packed mount verdict and restores on dismount")


func test_resolves_by_kind_index_fallback() -> void:
	# Editor path: the in-memory mission has no stable bms_id (0). The pass must fall back to (kind,index).
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_kind_index = { "3:2": model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 0, "kind": 3, "index": 2, "active1": 1, "phase1": 77 }]
	_make_pass(index, sim).present()
	assert_eq(model.phases.size(), 1, "resolved by (kind,index) when bms_id is 0")
	assert_eq(int((model.phases[0] as Array)[1]), 77)


func test_transform_applied_from_snapshot() -> void:
	# The consolidated pass MOVES entities (the old game path did not). Position is Godot-space; yaw-only
	# builds RotY(180 - yaw) through the shared placer convention.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 5: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 5, "pos_x": 10.0, "pos_y": 2.0, "pos_z": -4.0, "yaw_deg": 90.0 }]
	_make_pass(index, sim).present()
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, -4.0)), "position applied from snapshot")
	# yaw 90 -> RotY(180 - 90) = RotY(90deg); model +x maps toward -z.
	var fwd := model.transform.basis * Vector3(1, 0, 0)
	assert_almost_eq(fwd.z, -1.0, 0.001, "yaw drives the heading basis (RotY(180 - yaw))")


func test_stable_snapshot_does_not_redirty_the_transform_tree() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 21: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 21,
		"pos_x": 12.0,
		"yaw_deg": 15.0,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(3.0, 27.0, -2.0),
	}]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(int(presenter.get_stats()["moved"]), 1)
	presenter.present()
	assert_eq(int(presenter.get_stats()["moved"]), 1,
			"an unchanged row cannot recursively dirty every model descendant again")
	assert_true(model.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(
			Vector3(3.0, 27.0, -2.0))),
			"aim body rotation is composed into the one root transform write")


func test_stable_layout_reuses_resolution_but_reads_fresh_pose() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := CountingIndex.new()
	index.by_bms_id = { 21: model }
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{ "bms_id": 21, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 999, "handle": 3, "type_id": 102, "pos_x": 50.0 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_eq(index.resolve_calls, 2, "the cold layout resolves each row once")
	assert_almost_eq(model.position.x, 1.0, 0.001)

	sim.entities[0]["pos_x"] = 9.0
	presenter.present()
	assert_eq(index.resolve_calls, 2,
			"pose-only updates reuse the revision-bound row plan")
	assert_almost_eq(model.position.x, 9.0, 0.001,
			"cached routing still reads fresh row values")


func test_layout_plan_rebinds_after_reorder_removal_and_replacement() -> void:
	var a := FakeModel.new()
	var b := FakeModel.new()
	var c := FakeModel.new()
	add_child_autofree(a)
	add_child_autofree(b)
	add_child_autofree(c)
	var index := CountingIndex.new()
	index.by_bms_id = { 11: a, 22: b, 33: c }
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 1.0 },
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 2.0 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()

	sim.entities = [
		{ "bms_id": 22, "handle": 3, "type_id": 102, "pos_x": 20.0 },
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 10.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 4, "row reorder rebuilds the plan")
	assert_almost_eq(a.position.x, 10.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	sim.entities = [
		{ "bms_id": 11, "handle": 2, "type_id": 101, "pos_x": 12.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 5, "row removal cannot retain a stale base")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(b.position.x, 20.0, 0.001)

	sim.entities = [
		{ "bms_id": 33, "handle": 4, "type_id": 103, "pos_x": 30.0 },
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 6,
			"same-size identity replacement is resolved against the new row")
	assert_almost_eq(a.position.x, 12.0, 0.001)
	assert_almost_eq(c.position.x, 30.0, 0.001)


func test_transform_ignores_body_clip_visual_offsets() -> void:
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 6: model }
	var sim := FakeSim.new()
	sim.entities = [{
		"bms_id": 6,
		"pos_x": 10.0,
		"pos_y": 2.0,
		"pos_z": 3.0,
		"yaw_deg": 180.0,
		"anim_state": 86,
	}]
	_make_pass(index, sim).present()
	assert_true(model.position.is_equal_approx(Vector3(10.0, 2.0, 3.0)),
		"sim entity position remains authoritative even for seated infantry clips")


func test_visibility_from_hidden_and_alive() -> void:
	var shown := FakeModel.new()
	var hidden := FakeModel.new()
	var dead := FakeModel.new()
	var corpse := FakeModel.new()
	var despawned := FakeModel.new()
	add_child_autofree(shown)
	add_child_autofree(hidden)
	add_child_autofree(dead)
	add_child_autofree(corpse)
	add_child_autofree(despawned)
	var index := FakeIndex.new()
	index.by_bms_id = { 1: shown, 2: hidden, 3: dead, 4: corpse, 5: despawned }
	var sim := FakeSim.new()
	sim.entities = [
		{ "bms_id": 1, "hidden": 0, "alive": 1 },
		{ "bms_id": 2, "hidden": 1, "alive": 1 },
		{ "bms_id": 3, "hidden": 0, "alive": 0 },
		# A dead ORGANIC keeps rendering as a corpse until the sim despawns it via
		# PF_HIDDEN (the corpse timer + watch rule; world-wac-ai-re §19.4).
		{ "bms_id": 4, "hidden": 0, "alive": 0, "kind": 3 },
		{ "bms_id": 5, "hidden": 1, "alive": 0, "kind": 3 },
	]
	_make_pass(index, sim).present()
	assert_true(shown.visible, "visible entity stays visible")
	assert_false(hidden.visible, "hidden entity is hidden")
	# A dead non-organic RENDERS: the destruction pass swaps its model to the
	# husk, and a def with no husk keeps the graphic standing — the witnessed
	# render pick [orig: Flags&4 && huskModel ? husk : graphic @0x413086;
	# world-wac-ai-re §24.6].
	assert_true(dead.visible, "a dead non-organic renders (husk swap / graphic fallback)")
	assert_true(corpse.visible, "a dead organic renders as a corpse")
	assert_false(despawned.visible, "the sim ends the corpse via PF_HIDDEN")


func test_local_first_person_usegun_parent_is_not_world_rendered() -> void:
	# Retail skips the local UseGun PARENT'S own vehicle-model submit after its
	# embedded MountSlot becomes EquippedSlot in first person. This is a local
	# render verdict, independent of authoritative Entity.hidden; attached actors
	# render through a separate child walk. [orig: Entity_RenderVehicleModel
	# @0x4407d0, cull @0x4407f6..0x44084c, submit @0x440918;
	# RenderSlot_RenderEntityAndChildren child walk @0x5d7938+]
	var mount := FakeModel.new()
	var unrelated := FakeModel.new()
	add_child_autofree(mount)
	add_child_autofree(unrelated)
	var index := FakeIndex.new()
	index.by_bms_id = { 41: mount, 42: unrelated }
	var sim := FakeSim.new()
	sim.entities = [
		{ "bms_id": 41, "hidden": 0, "local_view_suppressed": 1 },
		{ "bms_id": 42, "hidden": 0, "local_view_suppressed": 0 },
	]
	var presenter := _make_pass(index, sim)
	presenter.present()
	assert_false(mount.visible,
			"the committed first-person UseGun parent skips its own world model")
	assert_true(unrelated.visible,
			"local UseGun suppression cannot hide unrelated world entities")

	# Camera-mode changes, detach, and pre-commit slot mismatch all clear the
	# transient verdict. PF_HIDDEN remains independently authoritative.
	sim.entities[0]["local_view_suppressed"] = 0
	presenter.present()
	assert_true(mount.visible, "clearing the render verdict restores the parent immediately")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(mount.visible, "authoritative PF_HIDDEN still wins independently")


func test_options_gate_each_channel() -> void:
	# The editor/game can disable a channel; e.g. transform-only leaves part phases untouched.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 9: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 9, "active1": 1, "phase1": 50, "pos_x": 3.0 }]
	_make_pass(index, sim, { "drive_part_anim": false }).present()
	assert_eq(model.phases.size(), 0, "part-anim channel disabled -> nothing posed")
	assert_almost_eq(model.position.x, 3.0, 0.001, "transform still applied")


func test_unresolved_target_does_not_crash() -> void:
	var index := FakeIndex.new()  # empty -> resolves nothing
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1234, "active1": 1, "phase1": 1 }]
	_make_pass(index, sim).present()  # must not crash
	assert_eq(int(_make_pass(index, sim).get_stats()["posed"]), 0, "nothing posed when the target is unresolved")


func test_occlusion_claim_blocks_the_show_but_never_the_hide() -> void:
	# Two-bit visibility ownership: the render-occlusion apply owns hides
	# through a claim set shared by reference (the occlusion_hidden_ids setup
	# option). A sim-wants-visible write is withheld while the claim stands, a
	# sim hide always lands, and clearing the claim (as the occlusion release
	# does) returns sole ownership to this pass.
	var model := FakeModel.new()
	add_child_autofree(model)
	var index := FakeIndex.new()
	index.by_bms_id = { 1001: model }
	var sim := FakeSim.new()
	sim.entities = [{ "bms_id": 1001 }]
	var claims := { 1001: true }
	var present_pass := _make_pass(index, sim, {"occlusion_hidden_ids": claims})

	model.visible = false  # occlusion hid it; the sim wants it visible
	present_pass.present()
	assert_false(model.visible, "a claimed node is not re-shown by the present drive")

	sim.entities = [{ "bms_id": 1001, "hidden": 1 }]
	present_pass.present()
	assert_false(model.visible, "a sim hide lands regardless of the claim")

	sim.entities = [{ "bms_id": 1001 }]
	claims.clear()
	present_pass.present()
	assert_true(model.visible,
			"with the claim cleared the present drive owns visibility again")
