extends GutTest

const WirePresentPass := preload("res://engine/world/wire_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class FakeModel:
	extends Node3D
	var overlay_calls: Array = []
	var body_calls: Array = []
	var pose_call_order: Array[String] = []
	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append([key, phase_ticks])
		pose_call_order.append("body")
	func set_aim_overlay(deltas: Array) -> void:
		overlay_calls.append(deltas)
		pose_call_order.append("overlay")


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
			out[base + NovaSimulation.PF_ALIVE] = float(entity.get("alive", 1))
			out[base + NovaSimulation.PF_ANIM_STATE] = float(entity.get("anim_state", -1))
			out[base + NovaSimulation.PF_ANIM_PHASE_TICKS] = float(
					entity.get("anim_phase", 0))
			out[base + NovaSimulation.PF_AIM_OVERLAY_VALID] = float(
					entity.get("aim_overlay_valid", 0))
			var body: Vector3 = entity.get("aim_body", Vector3.ZERO)
			out[base + NovaSimulation.PF_AIM_BODY_PITCH_DEG] = body.x
			out[base + NovaSimulation.PF_AIM_BODY_YAW_DEG] = body.y
			out[base + NovaSimulation.PF_AIM_BODY_ROLL_DEG] = body.z
			var angles: PackedVector3Array = entity.get(
					"aim_angles", PackedVector3Array())
			for cls in range(mini(angles.size(), 9)):
				var ob := (base + NovaSimulation.PF_AIM_ANGLES
						+ cls * NovaSimulation.PF_AIM_CLASS_STRIDE)
				out[ob] = angles[cls].x
				out[ob + 1] = angles[cls].y
				out[ob + 2] = angles[cls].z
		return out


class FakePlacer:
	extends RefCounted
	var built: Array[Node3D] = []

	func build_player_animated_model(_type_id: int, parent: Node3D,
			_env_node: Node = null) -> Node3D:
		var node := FakeModel.new()
		parent.add_child(node)
		built.append(node)
		return node


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
	assert_eq(model.body_calls, [["anim_emplaced", 11]],
			"wire presentation selects the replicated mounted body clip")
	assert_eq(model.pose_call_order, ["overlay", "body"],
			"the current packed overlay is installed before the body clip evaluates")
	assert_eq(model.overlay_calls.size(), 1)
	var deltas: Array = model.overlay_calls[0]
	assert_eq(deltas.size(), 9)
	var body_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(6.0, 33.0, -2.0))
	assert_true(model.basis.is_equal_approx(body_basis))
	assert_true((deltas[8] as Basis).is_equal_approx(
			body_basis.inverse() * MissionObjectPlacer.bms_to_godot_basis(angles[8])))


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
