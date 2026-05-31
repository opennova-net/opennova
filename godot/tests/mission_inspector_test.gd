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
	var delete_calls: int = 0
	var last_team: int = 0
	var last_group: int = 0
	var last_pos: Vector3 = Vector3.ZERO
	var last_rot: Vector3 = Vector3.ZERO

	# Phase 3 place-object palette state.
	var mission_ref  # NovaMissionData or null; non-null makes the inspector show the panels
	var placeable: Array = []
	var armed_id: int = 0
	var arm_calls: Array = []
	var disarm_calls: int = 0

	func get_mission():
		return mission_ref

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

	func delete_selected() -> bool:
		delete_calls += 1
		entity = {}  # mirror the real controller clearing the selection after a delete
		dirty = true
		changed.emit()
		return true

	func is_dirty() -> bool:
		return dirty

	func get_placeable_items() -> Array:
		return placeable

	func get_placement_item_id() -> int:
		return armed_id

	func arm_placement(id: int) -> void:
		arm_calls.append(id)
		armed_id = id
		changed.emit()

	func disarm_placement() -> void:
		disarm_calls += 1
		armed_id = 0
		changed.emit()


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


# --- Phase 4: delete the selected entity --------------------------------------

func test_delete_button_hidden_without_a_selection() -> void:
	var ctx := _make({})
	assert_not_null(ctx.inspector._delete_button, "the Delete button is built up front")
	assert_false(ctx.inspector._edit_box.visible, "the edit panel (which holds Delete) hides when nothing is selected")
	assert_false(ctx.inspector._delete_button.is_visible_in_tree(), "so the Delete button is not shown")


func test_delete_button_visible_with_a_selection() -> void:
	var ctx := _make(_sample_entity())
	assert_true(ctx.inspector._edit_box.visible, "the edit panel shows when an entity is selected")
	assert_true(ctx.inspector._delete_button.is_visible_in_tree(), "and the Delete button is shown within it")


func test_pressing_delete_button_deletes_through_the_controller() -> void:
	var ctx := _make(_sample_entity())
	ctx.inspector._delete_button.pressed.emit()  # simulate a click
	assert_eq(ctx.fake.delete_calls, 1, "the Delete button removes the entity via the controller exactly once")
	assert_false(ctx.inspector._edit_box.visible, "the panel hides after the delete clears the selection")


# --- Phase 3: place-object palette --------------------------------------------

func _palette_items() -> Array:
	return [
		{"id": 102001, "display_name": "Guard Tower", "type": NovaItemDatabase.TYPE_BUILDING},
		{"id": 101291, "display_name": "Dune Buggy", "type": NovaItemDatabase.TYPE_VEHICLE},
		{"id": 105311, "display_name": "Soldier", "type": NovaItemDatabase.TYPE_PERSON},
	]


func _palette_ctx() -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()  # a stable non-null ref so the panels show
	fake.placeable = _palette_items()
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func test_palette_is_hidden_without_a_mission() -> void:
	var fake := FakeController.new()  # mission_ref left null
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	assert_false(inspector._place_box.visible, "the palette hides when no mission is open")


func test_palette_populates_rows_from_the_controller() -> void:
	var ctx := _palette_ctx()
	assert_true(ctx.inspector._place_box.visible, "the palette shows with a mission open")
	assert_eq(ctx.inspector._place_list.item_count, 3, "every placeable item becomes a row")
	assert_false(ctx.inspector._place_stop.visible, "Stop is hidden until something is armed")


func test_selecting_a_palette_row_arms_that_item() -> void:
	var ctx := _palette_ctx()
	ctx.inspector._on_place_item_selected(0)  # simulate the user picking row 0
	assert_eq(ctx.fake.arm_calls.size(), 1, "selecting a row arms exactly once")
	assert_eq(int(ctx.fake.arm_calls[0]), int(ctx.inspector._place_row_ids[0]),
		"the armed id is the one on the selected row")
	assert_true(ctx.inspector._place_stop.visible, "the Stop button appears once armed")


func test_stop_button_disarms() -> void:
	var ctx := _palette_ctx()
	ctx.fake.armed_id = 102001
	ctx.inspector._refresh()  # reflect the armed state
	assert_true(ctx.inspector._place_stop.visible)
	ctx.inspector._on_place_stop()
	assert_eq(ctx.fake.disarm_calls, 1, "the Stop button disarms placement")


func test_palette_search_filters_rows() -> void:
	var ctx := _palette_ctx()
	ctx.inspector._on_place_search_changed("buggy")
	assert_eq(ctx.inspector._place_list.item_count, 1, "the search narrows to matching names")
	assert_eq(int(ctx.inspector._place_row_ids[0]), 101291, "and the surviving row is the match")
	ctx.inspector._on_place_search_changed("")
	assert_eq(ctx.inspector._place_list.item_count, 3, "clearing the search restores every row")


func test_empty_palette_repopulates_when_the_item_db_arrives_late() -> void:
	# Regression for the lifecycle dead-end: a mission can open before its items.def is
	# resolvable (empty palette, "no item database" status). Once items become available,
	# a later refresh must populate the palette WITHOUT the mission ref changing -- the
	# user must not have to close and reopen the mission.
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.placeable = []  # items.def not yet resolvable
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	assert_eq(inspector._place_list.item_count, 0, "empty until the database resolves")

	fake.placeable = _palette_items()  # the database becomes resolvable (same mission)
	inspector._refresh()
	assert_eq(inspector._place_list.item_count, 3, "the palette fills in without reopening the mission")


func test_active_search_filter_survives_a_changed_echo() -> void:
	# The palette's central contract (like the edit panel): the list is built once and
	# NOT repopulated on the `changed` signal that fires on every edit / select / place.
	# So a typed search filter and the filtered rows must survive a same-mission `changed`
	# -- otherwise the search box would clear and the full list would snap back on every
	# placed object. This pins that invariant.
	var ctx := _palette_ctx()
	# Simulate the user typing a filter: the LineEdit holds the text, and the text_changed
	# handler narrows the list (setting .text alone does not emit text_changed in Godot).
	ctx.inspector._place_search.text = "buggy"
	ctx.inspector._on_place_search_changed("buggy")
	assert_eq(ctx.inspector._place_list.item_count, 1, "precondition: filtered to one row")
	ctx.inspector._on_place_item_selected(0)  # arms the surviving item; fires `changed`
	ctx.fake.changed.emit()  # stand in for a later edit / placement on the same mission

	assert_eq(ctx.inspector._place_search.text, "buggy", "the search text survives a same-mission `changed`")
	assert_eq(ctx.inspector._place_list.item_count, 1, "the filtered rows survive; the list is not repopulated")
	assert_eq(int(ctx.inspector._place_row_ids[0]), 101291, "and the surviving row is still the match")
	assert_true(ctx.inspector._place_stop.visible, "still armed after the echo")


func test_real_controller_provides_every_method_the_inspector_calls() -> void:
	# The tests above drive a FakeController; this asserts the REAL MissionController
	# exposes the same surface, so a method the inspector calls that the controller does
	# not implement (a runtime crash in the editor) cannot hide behind the fake.
	var controller := MissionController.new(null)
	var required := [
		"get_mission", "get_stats", "get_selection_summary", "get_selected_entity",
		"get_selected_position", "get_selected_rotation",
		"set_selected_position", "set_selected_rotation", "set_selected_team",
		"set_selected_group", "delete_selected", "is_dirty",
		"get_placeable_items", "get_placement_item_id", "arm_placement", "disarm_placement",
	]
	for method in required:
		assert_true(controller.has_method(method),
			"MissionController must implement %s (called by the inspector)" % method)
