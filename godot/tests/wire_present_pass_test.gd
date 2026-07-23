extends GutTest

const WirePresentPass := preload("res://engine/world/wire_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class FakeModel:
	extends Node3D
	var overlay_calls: Array = []
	var right_hand_collapse_calls: Array[bool] = []
	var body_calls: Array = []
	var remote_body_calls: Array = []
	var reset_remote_body_calls := 0
	var part_calls: Array = []
	var pose_call_order: Array[String] = []
	var ctrl_values: Dictionary = {}
	var cleared_controls: Array[String] = []
	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append(["at", key, phase_ticks])
		pose_call_order.append("body")
	func play_body_clip(key: String) -> void:
		body_calls.append(["free", key])
		pose_call_order.append("body")
	func apply_remote_body_state(state_id: int, key: String, flags: int,
			phase_ticks: int = -1) -> void:
		remote_body_calls.append([state_id, key, flags, phase_ticks])
		pose_call_order.append("body")
	func reset_remote_body_state() -> void:
		reset_remote_body_calls += 1
		pose_call_order.append("reset")
	func set_part_phase(channel: int, phase: int) -> void:
		part_calls.append([channel, phase])
	func set_aim_overlay(deltas: Array) -> void:
		overlay_calls.append(deltas)
		pose_call_order.append("overlay")
	func set_right_hand_collapsed(collapsed: bool) -> void:
		right_hand_collapse_calls.append(collapsed)
		pose_call_order.append("right_hand")
	func set_ctrl_value(name: String, value: int) -> void:
		ctrl_values[name] = value
	func clear_ctrl_value(name: String) -> void:
		ctrl_values.erase(name)
		cleared_controls.append(name)


class FakeSim:
	extends RefCounted
	var entities: Array = []

	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE

	func get_local_player_wire_handle() -> int:
		return 1

	func get_present_snapshot() -> PackedFloat32Array:
		var stride := NovaSimulation.PF_STRIDE
		var out := PackedFloat32Array()
		out.resize(entities.size() * stride)
		for i in range(entities.size()):
			var entity: Dictionary = entities[i]
			var base := i * stride
			out[base + NovaSimulation.PF_TYPE_ID] = float(entity.get("type_id", 0))
			out[base + NovaSimulation.PF_WIRE_HANDLE] = float(entity.get("handle", 0))
			out[base + NovaSimulation.PF_KIND] = float(entity.get("kind", -1))
			out[base + NovaSimulation.PF_INDEX] = float(entity.get("index", -1))
			out[base + NovaSimulation.PF_BMS_ID] = float(entity.get("bms_id", 0))
			out[base + NovaSimulation.PF_POS_X] = float(entity.get("x", 0.0))
			out[base + NovaSimulation.PF_POS_Y] = float(entity.get("y", 0.0))
			out[base + NovaSimulation.PF_POS_Z] = float(entity.get("z", 0.0))
			out[base + NovaSimulation.PF_YAW_DEG] = float(entity.get("yaw", 0.0))
			out[base + NovaSimulation.PF_PITCH_DEG] = float(entity.get("pitch", 0.0))
			out[base + NovaSimulation.PF_ROLL_DEG] = float(entity.get("roll", 0.0))
			out[base + NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED] = float(
					entity.get("local_view_suppressed", 0))
			out[base + NovaSimulation.PF_HIDDEN] = float(entity.get("hidden", 0))
			out[base + NovaSimulation.PF_ALIVE] = float(entity.get("alive", 1))
			out[base + NovaSimulation.PF_RESPAWN_REVISION] = float(
					entity.get("respawn_revision", 0))
			out[base + NovaSimulation.PF_ACTIVE1] = float(entity.get("active1", 0))
			out[base + NovaSimulation.PF_PHASE1] = float(entity.get("phase1", 0))
			out[base + NovaSimulation.PF_ACTIVE2] = float(entity.get("active2", 0))
			out[base + NovaSimulation.PF_PHASE2] = float(entity.get("phase2", 0))
			out[base + NovaSimulation.PF_ANIM_STATE] = float(entity.get("anim_state", -1))
			out[base + NovaSimulation.PF_ANIM_PHASE_TICKS] = float(
					entity.get("anim_phase", -1))
			out[base + NovaSimulation.PF_ANIM_REMOTE_REQUEST] = float(
					entity.get("anim_remote_request", 1))
			out[base + NovaSimulation.PF_AIM_OVERLAY_VALID] = float(
					entity.get("aim_overlay_valid", 0))
			var body: Vector3 = entity.get("aim_body", Vector3.ZERO)
			out[base + NovaSimulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[base + NovaSimulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[base + NovaSimulation.PF_AIM_BODY_ROLL_DEG] = body.z
			out[base + NovaSimulation.PF_EMPLACED_CONTROLS_VALID] = float(
					entity.get("emplaced_controls_valid", 0))
			out[base + NovaSimulation.PF_EWEAP_GUNYAW] = float(
					entity.get("emplaced_gun_yaw", 0))
			out[base + NovaSimulation.PF_EWEAP_GUNPITCH] = float(
					entity.get("emplaced_gun_pitch", 0))
			out[base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED] = float(
					entity.get("right_hand_collapsed", 0))
			var angles: PackedVector3Array = entity.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var ob := (base + NovaSimulation.PF_AIM_ANGLES
						+ cls * NovaSimulation.PF_AIM_CLASS_STRIDE)
				out[ob] = angles[cls].x
				out[ob + 1] = angles[cls].y
				out[ob + 2] = angles[cls].z
		return out


class RevisionFakeSim:
	extends FakeSim
	var layout_revision := 1
	func get_present_layout_revision() -> int:
		return layout_revision


class FakePlacer:
	extends RefCounted
	var built: Array[Node3D] = []

	func build_player_animated_model(_type_id: int, parent: Node3D,
			_env_node: Node = null) -> Node3D:
		var node := FakeModel.new()
		parent.add_child(node)
		built.append(node)
		return node


class ResolvingFakePlacer:
	extends FakePlacer
	func resolve_player_visual_item_id(type_id: int) -> int:
		return type_id + 100000 if type_id < 100000 else type_id


class SelectiveFakePlacer:
	extends FakePlacer
	var failed_types: Dictionary = {}
	var attempts: Array[int] = []
	func build_player_animated_model(type_id: int, parent: Node3D,
			_env_node: Node = null) -> Node3D:
		attempts.append(type_id)
		if bool(failed_types.get(type_id, false)):
			return null
		return super.build_player_animated_model(type_id, parent, _env_node)


class EmptyIndex:
	extends RefCounted
	func resolve(_bms_id: int, _kind: int, _index: int):
		return null


class CountingDeferIndex:
	extends RefCounted
	var by_bms_id: Dictionary = {}
	var resolve_calls := 0
	var generation := 1
	func resolve(bms_id: int, _kind: int, _index: int):
		resolve_calls += 1
		return by_bms_id.get(bms_id)
	func get_generation() -> int:
		return generation


class SpawnObserver:
	extends RefCounted
	var calls: Array = []

	func on_spawned(node: Node3D, kind: int, item_id: int) -> void:
		calls.append({
			"node": node,
			"kind": kind,
			"item_id": item_id,
			"position": node.position,
		})


func test_sp_synthetic_filter_materializes_only_attachment_origin_rows() -> void:
	var sim := FakeSim.new()
	sim.entities = [
		{
			"type_id": 164,
			"handle": 0x1004,
			"kind": 1,
			"index": 0,
			"bms_id": 11,
		},
		{
			"type_id": 166,
			"handle": 0x1005,
			"kind": 255,
			"index": 0xFFFFFF,
			"bms_id": 0,
		},
	]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		"synthetic_origin_only": true,
	})
	presenter.present()
	assert_eq(placer.built.size(), 1,
			"ordinary SP rows stay with MissionPresentPass; synthetic children materialize")
	var ref: Dictionary = placer.built[0].get_meta("entity_ref", {})
	assert_eq(int(ref.get("item_id", 0)), 100166)
	assert_eq(int(ref.get("runtime_type_id", 0)), 166)
	assert_eq(int(ref.get("origin_kind", 0)), 255)


func test_wire_handle_resolver_keeps_synthetic_siblings_distinct() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		'type_id': 166,
		'handle': 0x1004,
		'kind': 255,
		'index': 0xffffff,
	}, {
		'type_id': 166,
		'handle': 0x1005,
		'kind': 255,
		'index': 0xffffff,
	}]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		'synthetic_origin_only': true,
	})

	presenter.present()

	assert_eq(presenter.resolve_wire_handle(0x1004), placer.built[0])
	assert_eq(presenter.resolve_wire_handle(0x1005), placer.built[1],
			'each attachment sibling resolves to its own live node')
	sim.entities = []
	presenter.present()
	assert_null(presenter.resolve_wire_handle(0x1004),
			'a retired wire row no longer resolves through its reused pool slot')


func test_unresolved_slot_retries_after_disappearance_and_reuse() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	placer.failed_types[166] = true
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	assert_eq(placer.attempts, [166])
	assert_eq(presenter.entity_count(), 0)

	sim.entities = []
	presenter.present()
	placer.failed_types[166] = false
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	presenter.present()
	assert_eq(placer.attempts, [166, 166],
			"retired failure cache cannot poison slot reuse")
	assert_eq(presenter.entity_count(), 1)


func test_unresolved_slot_retries_immediately_when_type_changes() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	placer.failed_types[166] = true
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()

	sim.entities[0]["type_id"] = 167
	presenter.present()
	assert_eq(placer.attempts, [166, 167])
	assert_eq(presenter.entity_count(), 1)


func test_live_slot_type_change_rebuilds_the_visual() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)

	sim.entities[0]["type_id"] = 167
	presenter.present()
	var second := presenter.resolve_wire_handle(0x1004)
	assert_eq(placer.attempts, [166, 167])
	assert_ne(second, first, "recycled handle cannot keep the prior type model")
	assert_eq(int(second.get_meta("entity_ref", {}).get("runtime_type_id", 0)), 167)


func test_stable_host_layout_skips_repeat_defer_resolution() -> void:
	var sim := RevisionFakeSim.new()
	sim.entities = [{
		"type_id": 166,
		"handle": 0x1004,
		"bms_id": 11,
		"kind": 1,
		"index": 0,
	}]
	var placed := Node3D.new()
	add_child_autofree(placed)
	var index := CountingDeferIndex.new()
	index.by_bms_id = { 11: placed }
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, index)
	presenter.present()
	assert_eq(index.resolve_calls, 1)
	assert_true(placer.attempts.is_empty(),
			"the placed row remains owned by MissionPresentPass")

	sim.entities[0]["x"] = 9.0
	presenter.present()
	assert_eq(index.resolve_calls, 1,
			"stable topology with no wire nodes takes the empty fast path")

	sim.entities[0] = {
		"type_id": 167,
		"handle": 0x1005,
		"bms_id": 12,
		"kind": -1,
		"index": -1,
	}
	sim.layout_revision += 1
	presenter.present()
	assert_eq(index.resolve_calls, 2)
	assert_eq(placer.attempts, [167],
			"same-size placed-to-wire replacement rebuilds classification")


func test_wire_plan_survives_reorder_then_prunes_and_rebuilds_reused_type() -> void:
	var sim := RevisionFakeSim.new()
	sim.entities = [
		{"type_id": 166, "handle": 0x1004, "x": 4.0},
		{"type_id": 167, "handle": 0x1005, "x": 5.0},
	]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)
	var second := presenter.resolve_wire_handle(0x1005)

	sim.entities = [
		{"type_id": 167, "handle": 0x1005, "x": 50.0},
		{"type_id": 166, "handle": 0x1004, "x": 40.0},
	]
	sim.layout_revision += 1
	presenter.present()
	assert_eq(presenter.resolve_wire_handle(0x1004), first)
	assert_eq(presenter.resolve_wire_handle(0x1005), second)
	assert_almost_eq(first.position.x, 40.0, 0.001)
	assert_almost_eq(second.position.x, 50.0, 0.001)

	sim.entities = [
		{"type_id": 167, "handle": 0x1005, "x": 51.0},
	]
	sim.layout_revision += 1
	presenter.present()
	assert_null(presenter.resolve_wire_handle(0x1004),
			"despawn prunes the retired row plan and visual")
	assert_eq(presenter.entity_count(), 1)

	sim.entities[0] = {"type_id": 168, "handle": 0x1005, "x": 60.0}
	sim.layout_revision += 1
	presenter.present()
	var replacement := presenter.resolve_wire_handle(0x1005)
	assert_ne(replacement, second,
			"a recycled handle with a new type cannot retain the old visual")
	assert_almost_eq(replacement.position.x, 60.0, 0.001)
	assert_eq(placer.attempts, [166, 167, 168])


func test_runtime_reset_rematerializes_the_restored_same_type_slot() -> void:
	var sim := FakeSim.new()
	sim.entities = [{"type_id": 166, "handle": 0x1004}]
	var placer := SelectiveFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var first := presenter.resolve_wire_handle(0x1004)

	presenter.reset_runtime_state()
	assert_eq(presenter.entity_count(), 0)
	assert_null(presenter.resolve_wire_handle(0x1004))
	presenter.present()
	var restored := presenter.resolve_wire_handle(0x1004)
	assert_eq(placer.attempts, [166, 166])
	assert_ne(restored, first,
			"restart builds a fresh restored-incarnation visual")


func test_synthetic_attachment_uses_panm_and_hidden_visibility_contract() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 166,
		"handle": 0x1005,
		"kind": 255,
		"index": 0xFFFFFF,
		"alive": 0,
		"active1": 1,
		"phase1": 0x2345,
		"active2": 1,
		"phase2": 0x6789,
	}]
	var placer := ResolvingFakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new(), {
		"synthetic_origin_only": true,
	})
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.part_calls, [[1, 0x2345], [2, 0x6789]],
			"attached items consume the same two PANM channels as placed items")
	assert_true(model.visible,
			"a dead attached item retains its graphic or husk until explicitly hidden")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(model.visible, "PF_HIDDEN ends attached-item presentation")


func test_wire_model_spawn_registers_after_identity_and_transform_are_ready() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		# A real joiner's ClientState has no authoritative BMS origin, so PF_KIND
		# is -1. The callback must derive pool 1 -> KIND_ITEM from the wire handle.
		"kind": -1,
		"index": 9,
		"bms_id": 77,
		"x": 4.0,
		"y": 5.0,
		"z": 6.0,
		"yaw": 90.0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var observer := SpawnObserver.new()
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.set_node_spawned_callback(Callable(observer, "on_spawned"))
	presenter.present()

	assert_eq(observer.calls.size(), 1, "the new wire model is registered exactly once")
	var call: Dictionary = observer.calls[0]
	assert_eq(int(call.kind), NovaMissionData.KIND_ITEM)
	assert_eq(int(call.item_id), 4567)
	assert_eq(call.position, Vector3(4, 5, 6),
			"registration runs after the production transform is applied")
	var node := call.node as Node3D
	var ref: Dictionary = node.get_meta("entity_ref", {})
	assert_eq(int(ref.get("wire_handle", 0)), 0x1004)
	assert_eq(int(ref.get("item_id", 0)), 4567)
	assert_eq(int(ref.get("origin_kind", 0)), -1)

	presenter.present()
	assert_eq(observer.calls.size(), 1, "steady presentation never re-registers the model")
	var late := SpawnObserver.new()
	presenter.set_node_spawned_callback(Callable(late, "on_spawned"))
	assert_eq(late.calls.size(), 1, "late consumers receive every already-live wire node")


func test_wire_model_applies_the_same_packed_overlay_result() -> void:
	var angles := PackedVector3Array()
	for i in range(9):
		angles.append(Vector3(-4.0 + i, 70.0 + i, 1.0 + i))
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"anim_state": 67,
		"anim_phase": 11,
		"aim_overlay_valid": 1,
		"aim_body": Vector3(6.0, 33.0, -2.0),
		"aim_angles": angles,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [[67, "anim_emplaced",
			NovaSimulation.infantry_anim_flags(67), 11]],
			"presentation forwards state, retail flags, and player phase")
	assert_eq(model.pose_call_order, ["right_hand", "overlay", "body"],
			"the current packed overlay is installed before the body clip evaluates")
	assert_eq(model.overlay_calls.size(), 1)
	var deltas: Array = model.overlay_calls[0]
	assert_eq(deltas.size(), 9)
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(6.0, 33.0, -2.0))
	assert_true(model.basis.is_equal_approx(body_basis))
	assert_true((deltas[8] as Basis).is_equal_approx(
			body_basis.inverse() * MissionObjectPlacer.bms_to_godot_basis(angles[8])))

	# The model owns transition acceptance and completion, so presentation keeps
	# forwarding raw requests rather than filtering state changes itself.
	sim.entities[0]["anim_phase"] = 22
	presenter.present()
	sim.entities[0]["anim_state"] = 68
	sim.entities[0]["anim_phase"] = 6
	presenter.present()
	assert_eq(model.remote_body_calls, [
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), 11],
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), 22],
		[68, "anim_emplaced_2", NovaSimulation.infantry_anim_flags(68), 6],
	], "raw compact requests reach the completion-aware remote animation channel")


func test_wire_model_free_runs_compact_infantry_when_phase_is_absent() -> void:
	# Production infantry compacts carry the state byte but no player-channel
	# phase byte. Re-presenting that snapshot must preserve local playback
	# instead of externally pinning the selected clip to tick zero.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	presenter.present()

	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), -1],
		[67, "anim_emplaced", NovaSimulation.infantry_anim_flags(67), -1],
	], "phase-less compact infantry forwards an unavailable phase sentinel")


func test_host_current_body_state_is_posed_without_remote_rearbitration() -> void:
	# Host-loopback snapshots carry AiEntity's already-accepted current state and
	# phase, not a compact pending request. It must retain the direct pose path.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
		"anim_phase": 11,
		"anim_remote_request": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()

	var model := placer.built[0] as FakeModel
	assert_eq(model.remote_body_calls, [],
			"host current state is not submitted to the receive-side request channel")
	assert_eq(model.body_calls, [["at", "anim_emplaced", 11]],
			"host current state keeps the authoritative direct-phase pose path")


func test_wire_model_resets_remote_body_channel_on_respawn_revision_change() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x0004,
		"anim_state": 67,
		"anim_phase": 11,
		"respawn_revision": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.reset_remote_body_calls, 0,
			"the first lifecycle sample initializes rather than resets a new model")

	# A revision jump represents one or more dead->alive edges folded before this
	# render. Reset must precede the animation request so an identical state ID is
	# accepted into a fresh remote body-channel epoch.
	model.pose_call_order.clear()
	sim.entities[0]["respawn_revision"] = 2
	sim.entities[0]["anim_phase"] = 6
	presenter.present()
	assert_eq(model.reset_remote_body_calls, 1)
	assert_eq(model.pose_call_order, ["right_hand", "overlay", "reset", "body"],
			"respawn reset runs before the compact animation is applied")
	assert_eq(model.remote_body_calls[-1], [67, "anim_emplaced",
			NovaSimulation.infantry_anim_flags(67), 6])

	model.pose_call_order.clear()
	presenter.present()
	assert_eq(model.reset_remote_body_calls, 1,
			"a steady revision does not repeatedly reset local clip playback")
	assert_eq(model.pose_call_order, ["right_hand", "overlay", "body"])


func test_wire_model_applies_and_restores_mounted_right_hand_collapse() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 1,
		"right_hand_collapsed": 1,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	sim.entities[0]["right_hand_collapsed"] = 0
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.right_hand_collapse_calls, [true, false],
			"the wire pose consumes the same mount verdict and restores on dismount")


func test_wire_model_applies_and_clears_named_emplaced_controls() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"emplaced_controls_valid": 1,
		"emplaced_gun_yaw": 0x2000,
		"emplaced_gun_pitch": 0xE000,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.ctrl_values, {
		"EWEAP_GUNYAW": 0x2000,
		"EWEAP_GUNPITCH": 0xE000,
	})

	sim.entities[0]["emplaced_controls_valid"] = 0
	presenter.present()
	assert_true(model.ctrl_values.is_empty())
	assert_has(model.cleared_controls, "EWEAP_GUNYAW")
	assert_has(model.cleared_controls, "EWEAP_GUNPITCH")


func test_wire_model_honors_local_first_person_parent_cull() -> void:
	# Host-side dynamic/unplaced mount targets are owned by this pass rather than
	# MissionPresentPass. They consume the same transient retail render verdict.
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"alive": 1,
		"local_view_suppressed": 1,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container, null, EmptyIndex.new())
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_false(model.visible,
			"the dynamic local UseGun parent skips its own world model")

	sim.entities[0]["local_view_suppressed"] = 0
	presenter.present()
	assert_true(model.visible, "clearing the transient verdict restores the parent")
	sim.entities[0]["alive"] = 0
	presenter.present()
	assert_true(model.visible,
			"a dead non-hidden organic remains visible as a corpse")
	sim.entities[0]["hidden"] = 1
	presenter.present()
	assert_false(model.visible,
			"the authoritative compact hidden bit suppresses the world model")


func test_wire_model_clears_overlay_when_snapshot_selector_is_invalid() -> void:
	var sim := FakeSim.new()
	sim.entities = [{
		"type_id": 4567,
		"handle": 0x1004,
		"aim_overlay_valid": 0,
	}]
	var placer := FakePlacer.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := WirePresentPass.new()
	presenter.setup(sim, placer, container)
	presenter.present()
	var model := placer.built[0] as FakeModel
	assert_eq(model.overlay_calls, [[]],
			"a remote model cannot retain an overlay after selector invalidation")
