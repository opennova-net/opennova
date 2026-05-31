extends GutTest

# Phase 2: the editable Mission inspector. Drives the inspector against a fake
# controller (no terrain / assets needed) to check that the persistent edit panel
# shows the selected entity's values and that editing a SpinBox commits exactly once
# through the controller. The fake re-emits `changed` on every edit, just like the
# real controller, so a missing _loading guard would recurse forever: every test that
# completes also proves the guard holds.

const MissionInspector := preload("res://modtools/mission/mission_inspector.gd")
const MissionController := preload("res://modtools/mission/mission_controller.gd")


# Stands in for MissionController, recording what the inspector pushes and echoing the
# `changed` signal the way the real controller does after a mutation.
class FakeController:
	extends RefCounted

	signal changed

	var entity: Dictionary = {}
	var dirty: bool = false
	var team_calls: int = 0
	var group_calls: int = 0
	var pos_calls: int = 0
	var rot_calls: int = 0
	var last_team: int = 0
	var last_group: int = 0
	var last_pos: Vector3 = Vector3.ZERO
	var last_rot: Vector3 = Vector3.ZERO

	func get_mission():
		return null

	func get_stats() -> Dictionary:
		return {}

	func get_selection_summary() -> Dictionary:
		if entity.is_empty():
			return {}
		return {
			"kind": int(entity.get("kind", -1)),
			"index": int(entity.get("index", -1)),
			"position": entity.get("position", Vector3.ZERO),
			"animated": bool(entity.get("animated", false)),
		}

	func get_selected_entity() -> Dictionary:
		return entity

	func get_selected_position() -> Vector3:
		return entity.get("position", Vector3.ZERO)

	func get_selected_rotation() -> Vector3:
		return entity.get("rotation_deg", Vector3.ZERO)

	func set_selected_position(p: Vector3) -> void:
		pos_calls += 1
		last_pos = p
		entity["position"] = p
		dirty = true
		changed.emit()

	func set_selected_rotation(r: Vector3) -> void:
		rot_calls += 1
		last_rot = r
		entity["rotation_deg"] = r
		dirty = true
		changed.emit()

	func set_selected_team(v: int) -> void:
		team_calls += 1
		last_team = v
		entity["team"] = v
		dirty = true
		changed.emit()

	func set_selected_group(v: int) -> void:
		group_calls += 1
		last_group = v
		entity["group"] = v
		dirty = true
		changed.emit()

	func is_dirty() -> bool:
		return dirty


func _sample_entity() -> Dictionary:
	return {
		"kind": NovaMissionData.KIND_ORGANIC, "index": 4,
		"position": Vector3(10.0, 2.0, -5.0), "rotation_deg": Vector3(0.0, 45.0, 0.0),
		"team": 1, "group": 2,
	}


func _make(entity: Dictionary) -> Dictionary:
	var fake := FakeController.new()
	fake.entity = entity
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func _spin(inspector, node_name: String) -> SpinBox:
	return inspector.find_child(node_name, true, false) as SpinBox


func test_edit_panel_is_hidden_without_a_selection() -> void:
	var ctx := _make({})
	assert_not_null(_spin(ctx.inspector, "MissionPosX"), "the edit spins are built up front")
	assert_false(ctx.inspector._edit_box.visible, "the edit panel hides when nothing is selected")


func test_edit_panel_shows_the_selected_values() -> void:
	var ctx := _make(_sample_entity())
	assert_true(ctx.inspector._edit_box.visible, "the edit panel shows when an entity is selected")
	assert_eq(_spin(ctx.inspector, "MissionPosX").value, 10.0, "X reads from the entity")
	assert_eq(_spin(ctx.inspector, "MissionPosZ").value, -5.0, "Z reads from the entity")
	assert_eq(_spin(ctx.inspector, "MissionRotYaw").value, 45.0, "yaw reads from the entity")
	assert_eq(_spin(ctx.inspector, "MissionTeam").value, 1.0, "team reads from the entity")
	assert_eq(_spin(ctx.inspector, "MissionGroup").value, 2.0, "group reads from the entity")


func test_editing_team_commits_exactly_once() -> void:
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionTeam").value = 5  # simulate a user edit (outside the guard)
	assert_eq(ctx.fake.last_team, 5, "the team edit reached the controller")
	assert_eq(ctx.fake.team_calls, 1, "the changed-signal echo did not re-commit (the _loading guard holds)")
	assert_true(ctx.fake.is_dirty())


func test_editing_one_position_axis_leaves_the_others() -> void:
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionPosX").value = 99
	assert_eq(ctx.fake.pos_calls, 1, "one commit, no echo loop")
	assert_eq(ctx.fake.last_pos, Vector3(99.0, 2.0, -5.0),
		"only X changed; Y and Z came from the model, not the sibling spins")


func test_editing_group_commits_once() -> void:
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionGroup").value = 9
	assert_eq(ctx.fake.last_group, 9)
	assert_eq(ctx.fake.group_calls, 1, "no echo re-commit")


func test_real_controller_provides_every_method_the_inspector_calls() -> void:
	# The tests above drive a FakeController; this asserts the REAL MissionController
	# exposes the same surface, so a method the inspector calls that the controller does
	# not implement (a runtime crash in the editor) cannot hide behind the fake.
	var controller := MissionController.new(null)
	var required := [
		"get_mission", "get_stats", "get_selection_summary", "get_selected_entity",
		"get_selected_position", "get_selected_rotation",
		"set_selected_position", "set_selected_rotation", "set_selected_team",
		"set_selected_group", "is_dirty",
	]
	for method in required:
		assert_true(controller.has_method(method),
			"MissionController must implement %s (called by the inspector)" % method)
