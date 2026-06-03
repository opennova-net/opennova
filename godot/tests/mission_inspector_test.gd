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
	var display_name: String = ""  # resolved model name for the identity line
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
	# P7 Behavior panel: the generic per-entity property setter.
	var property_calls: int = 0
	var last_property: String = ""
	var last_property_value: int = 0

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

	func get_selected_display_name() -> String:
		return display_name

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

	func set_selected_property(name: String, v: int) -> void:
		property_calls += 1
		last_property = name
		last_property_value = v
		entity[name] = v
		dirty = true
		changed.emit()

	# Phase 1: hidden string fields + mission-header editing.
	var string_property_calls: Array = []   # [name, value] per call
	var header_string_calls: Array = []      # [field, value]
	var header_int_calls: Array = []         # [field, value]
	var header_flag_calls: Array = []        # [bit, on]

	func set_selected_string_property(name: String, v: String) -> void:
		string_property_calls.append([name, v])
		entity[name] = v
		dirty = true
		changed.emit()

	func set_header_string(field: String, value: String) -> void:
		header_string_calls.append([field, value])
		dirty = true
		changed.emit()

	func set_header_int(field: String, value: int) -> void:
		header_int_calls.append([field, value])
		dirty = true
		changed.emit()

	func set_header_flag(bit: int, on: bool) -> void:
		header_flag_calls.append([bit, on])
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

	# P7 waypoints + Phase 2 zones: the edit-mode tabs + panel surface. `mode` mirrors
	# MissionController.Mode (0 OBJECTS, 1 WAYPOINTS, 2 AREA_TRIGGERS); set_mode_calls records the
	# mode ints the tab handler passes.
	var mode: int = 0
	var selected_path: int = -1
	var waypoint_summaries: Array = []
	var selected_marker: Dictionary = {}
	var set_mode_calls: Array = []
	var select_path_calls: Array = []

	func get_mode() -> int:
		return mode

	func is_objects_mode() -> bool:
		return mode == 0

	func is_waypoint_mode() -> bool:
		return mode == 1

	func is_area_trigger_mode() -> bool:
		return mode == 2

	func set_mode(m: int) -> void:
		set_mode_calls.append(m)
		mode = m
		changed.emit()

	func set_waypoint_mode(enabled: bool) -> void:
		set_mode(1 if enabled else 0)

	func get_selected_waypoint_path_index() -> int:
		return selected_path

	func select_waypoint_path(index: int) -> void:
		select_path_calls.append(index)
		selected_path = index
		changed.emit()

	var new_path_calls: int = 0

	func select_new_waypoint_path() -> int:
		# Stand in for the controller focusing the first empty path.
		new_path_calls += 1
		selected_path = 0
		active_path = {"index": 0, "flags": 0, "marker_count": 0, "marker_indices": PackedInt32Array()}
		changed.emit()
		return 0

	func get_waypoint_summaries() -> Array:
		return waypoint_summaries

	func get_selected_marker() -> Dictionary:
		return selected_marker

	# P7c: active-path flags + ordered marker sub-list.
	var active_path: Dictionary = {}
	var set_flags_calls: Array = []
	var select_marker_calls: Array = []

	func get_active_waypoint_path() -> Dictionary:
		return active_path

	func set_waypoint_flags(loop: bool, blue: bool, red: bool) -> void:
		set_flags_calls.append([loop, blue, red])
		changed.emit()

	func select_waypoint_marker(marker_index: int) -> void:
		select_marker_calls.append(marker_index)
		changed.emit()

	# P7d: marker authoring (add tool, reorder, delete, clear).
	var marker_armed: bool = false
	var arm_marker_calls: int = 0
	var disarm_marker_calls: int = 0
	var move_marker_calls: Array = []
	var delete_marker_calls: int = 0
	var clear_path_calls: int = 0

	func is_marker_placement_armed() -> bool:
		return marker_armed

	func arm_marker_placement() -> void:
		arm_marker_calls += 1
		marker_armed = true
		changed.emit()

	func disarm_marker_placement() -> void:
		disarm_marker_calls += 1
		marker_armed = false
		changed.emit()

	func move_selected_marker(delta: int) -> void:
		move_marker_calls.append(delta)
		changed.emit()

	func delete_selected_marker() -> bool:
		delete_marker_calls += 1
		changed.emit()
		return true

	func clear_active_path() -> bool:
		clear_path_calls += 1
		changed.emit()
		return true

	# Phase 2: area-trigger (zone) surface. `zones` are NovaMissionData-shaped dicts.
	var zones: Array = []
	var selected_zone: int = -1
	var add_zone_calls: int = 0
	var delete_zone_calls: int = 0
	var select_zone_calls: Array = []
	var zone_bounds_calls: Array = []
	var zone_flags_calls: Array = []

	func get_area_triggers() -> Array:
		return zones

	func get_selected_zone_index() -> int:
		return selected_zone

	func get_selected_zone() -> Dictionary:
		if selected_zone < 0 or selected_zone >= zones.size():
			return {}
		return zones[selected_zone]

	func select_area_trigger(index: int) -> void:
		select_zone_calls.append(index)
		selected_zone = index
		changed.emit()

	func add_area_trigger_default() -> int:
		add_zone_calls += 1
		var idx := zones.size()
		zones.append({
			"index": idx, "id": 0, "min": Vector3(-1, -1, -1), "max": Vector3(1, 1, 1),
			"active": true, "constrain_z": false, "raw_flags": 1,
		})
		selected_zone = idx
		changed.emit()
		return idx

	func delete_selected_area_trigger() -> bool:
		delete_zone_calls += 1
		if selected_zone >= 0 and selected_zone < zones.size():
			zones.remove_at(selected_zone)
		selected_zone = -1
		changed.emit()
		return true

	func set_selected_zone_bounds(mn: Vector3, mx: Vector3) -> void:
		zone_bounds_calls.append([mn, mx])
		if selected_zone >= 0 and selected_zone < zones.size():
			zones[selected_zone]["min"] = mn
			zones[selected_zone]["max"] = mx
		changed.emit()

	func set_selected_zone_flags(active: bool, constrain_z: bool) -> void:
		zone_flags_calls.append([active, constrain_z])
		if selected_zone >= 0 and selected_zone < zones.size():
			zones[selected_zone]["active"] = active
			zones[selected_zone]["constrain_z"] = constrain_z
		changed.emit()


func _sample_entity() -> Dictionary:
	return {
		"kind": NovaMissionData.KIND_ORGANIC, "index": 4,
		"position": Vector3(10.0, 2.0, -5.0), "rotation_deg": Vector3(0.0, 45.0, 0.0),
		"team": 1, "group": 2,
		# Behavior fields the format carries beyond team / group (P7).
		"waypoint_id": 3, "wp_number": 0, "perception": 0, "accuracy": 0, "alert_state": 0,
		"min_engagement_distance": 0, "max_engagement_distance": 0, "max_attack_distance": 0,
		"spawn_count": 0, "max_simultaneous": 0, "ai_flags": 0,
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


func _line(inspector, node_name: String) -> LineEdit:
	return inspector.find_child(node_name, true, false) as LineEdit


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


func test_identity_shows_resolved_model_name() -> void:
	# With a resolved model name the identity heading reads the name and a muted kind + index
	# subline appears beneath it.
	var ctx := _make(_sample_entity())
	ctx.fake.display_name = "Spec Ops Soldier"
	ctx.inspector._refresh()  # re-read now that the name resolves
	assert_eq(ctx.inspector._identity_label.text, "Spec Ops Soldier", "the heading shows the model name")
	assert_true(ctx.inspector._identity_sub.visible, "the kind + index subline shows under the name")
	assert_eq(ctx.inspector._identity_sub.text, "Organic #4", "the subline carries the kind and index")


func test_identity_falls_back_to_kind_and_index_without_a_name() -> void:
	var ctx := _make(_sample_entity())  # display_name left ""
	assert_eq(ctx.inspector._identity_label.text, "Organic #4", "no resolved name -> kind + index heading")
	assert_false(ctx.inspector._identity_sub.visible, "and no redundant subline")


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


# --- P7: the Behavior panel (per-entity AI + waypoint fields) ------------------

func test_behavior_panel_reads_the_selected_values() -> void:
	var ctx := _make(_sample_entity())
	var spin := _spin(ctx.inspector, "MissionBeh_waypoint_id")
	assert_not_null(spin, "the behavior panel builds a waypoint_id spin")
	assert_eq(spin.value, 3.0, "waypoint_id reads from the entity (even while the section is collapsed)")
	assert_false(ctx.inspector._behavior_box.visible, "the Behavior section is collapsed by default")


func test_behavior_panel_toggle_shows_the_fields() -> void:
	var ctx := _make(_sample_entity())
	ctx.inspector._behavior_toggle.button_pressed = true  # emits toggled
	assert_true(ctx.inspector._behavior_box.visible, "toggling Behavior reveals the fields")


func test_ai_flags_is_a_hex_field_that_round_trips() -> void:
	var entity := _sample_entity()
	entity["ai_flags"] = 255
	var ctx := _make(entity)
	var line := _line(ctx.inspector, "MissionBeh_ai_flags")
	assert_not_null(line, "ai_flags is edited as a text field, not a decimal spin")
	assert_eq(line.text, "0x000000FF", "ai_flags reads as zero-padded hexadecimal")

	line.text_submitted.emit("0x0000000A")  # simulate Enter
	assert_eq(ctx.fake.last_property, "ai_flags", "the hex edit writes ai_flags")
	assert_eq(ctx.fake.last_property_value, 10, "0x0A parses to 10")
	assert_eq(line.text, "0x0000000A", "and the field re-reads the applied value")


func test_ai_flags_high_bit_round_trips_as_signed_int32() -> void:
	var ctx := _make(_sample_entity())
	var line := _line(ctx.inspector, "MissionBeh_ai_flags")
	line.text_submitted.emit("0xFFFFFFFF")
	# Passed as signed int32 (-1) so the engine's int parameter carries all 32 bits; the
	# model's -1 reads back as the same hex.
	assert_eq(ctx.fake.last_property_value, -1, "the high-bit value is written as a signed int32")
	assert_eq(line.text, "0xFFFFFFFF", "a stored -1 displays as 0xFFFFFFFF")


func test_ai_flags_rejects_garbage_without_writing() -> void:
	var ctx := _make(_sample_entity())  # ai_flags 0
	var line := _line(ctx.inspector, "MissionBeh_ai_flags")
	ctx.fake.property_calls = 0
	line.text_submitted.emit("not hex")
	assert_eq(ctx.fake.property_calls, 0, "an unparseable entry writes nothing")
	assert_eq(line.text, "0x00000000", "and the field is restored to the model value")


func test_behavior_field_commits_through_set_selected_property() -> void:
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionBeh_waypoint_id").value = 7  # simulate a user edit
	assert_eq(ctx.fake.last_property, "waypoint_id", "the edit names the field it changed")
	assert_eq(ctx.fake.last_property_value, 7, "with the new value")
	assert_eq(ctx.fake.property_calls, 1, "the FieldBinder guard holds: one commit, no echo loop")
	assert_true(ctx.fake.is_dirty())


func test_behavior_field_does_not_clamp_to_a_byte() -> void:
	# Behavior fields like the engagement distances are int32 in the format, so the panel
	# must not clamp them to 0..255 the way team / group do. A large value reaches the
	# controller intact.
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionBeh_max_engagement_distance").value = 5000
	assert_eq(ctx.fake.last_property, "max_engagement_distance")
	assert_eq(ctx.fake.last_property_value, 5000, "a value above 255 is not clamped")


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


# --- P7: edit-mode tabs + waypoint panel --------------------------------------

func _waypoint_ctx(summaries: Array, active: int) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 1  # WAYPOINTS
	fake.selected_path = active
	fake.waypoint_summaries = summaries
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func test_mode_tab_handler_drives_controller_mode() -> void:
	var ctx := _palette_ctx()  # mission open so the tabs are live
	# Tab order: 0 Objects, 1 Waypoints, 2 Triggers -> Mode 0/1/2.
	ctx.inspector._on_mode_tab_changed(1)
	assert_eq(ctx.fake.set_mode_calls, [1], "the Waypoints tab enters waypoint mode")
	ctx.inspector._on_mode_tab_changed(2)
	assert_eq(ctx.fake.set_mode_calls, [1, 2], "the Triggers tab enters area-trigger mode")
	ctx.inspector._on_mode_tab_changed(0)
	assert_eq(ctx.fake.set_mode_calls, [1, 2, 0], "the Objects tab returns to objects mode")


func test_mode_tabs_hidden_without_a_mission() -> void:
	var ctx := _make(_sample_entity())  # FakeController with no mission_ref
	assert_false(ctx.inspector._mode_tabs.visible, "the edit-mode tabs hide when no mission is open")


func test_waypoint_panel_shows_and_lists_paths_in_waypoint_mode() -> void:
	var ctx := _waypoint_ctx([
		{"index": 2, "flags": 0, "marker_count": 3},
		{"index": 5, "flags": NovaMissionData.WP_FLAG_DOES_NOT_LOOP, "marker_count": 0},
		{"index": 7, "flags": NovaMissionData.WP_FLAG_BLUE_TEAM, "marker_count": 2},
	], 2)
	assert_true(ctx.inspector._wp_box.visible, "the waypoint panel shows in waypoint mode")
	assert_false(ctx.inspector._edit_box.visible, "the object edit panel is hidden in waypoint mode")
	assert_false(ctx.inspector._place_box.visible, "the palette is hidden in waypoint mode")
	# Path 5 is empty and not the active path, so it is excluded; 2 (active) and 7 are listed.
	assert_eq(ctx.inspector._wp_list.item_count, 2, "only populated (or the active) paths are listed")
	assert_eq(ctx.inspector._wp_row_paths, [2, 7], "the listed path indices match")


func test_waypoint_panel_includes_the_active_path_even_when_empty() -> void:
	var ctx := _waypoint_ctx([
		{"index": 3, "flags": 0, "marker_count": 0},  # empty AND active -> shown
		{"index": 9, "flags": 0, "marker_count": 0},  # empty, not active -> hidden
	], 3)
	assert_eq(ctx.inspector._wp_row_paths, [3], "the active path is listed even with no markers")


func test_waypoint_panel_row_selects_that_path() -> void:
	var ctx := _waypoint_ctx([
		{"index": 2, "flags": 0, "marker_count": 1},
		{"index": 7, "flags": 0, "marker_count": 1},
	], 2)
	ctx.inspector._on_wp_path_selected(1)  # row 1 -> path 7
	assert_eq(ctx.fake.select_path_calls, [7], "selecting a path row focuses that path")


func test_waypoint_panel_reports_the_selected_marker() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	assert_string_contains(ctx.inspector._wp_marker_label.text, "No marker", "with no marker, the panel says so")
	ctx.fake.selected_marker = {"path_index": 2, "marker_index": 9, "position": Vector3(1.0, 2.0, 3.0)}
	ctx.fake.changed.emit()
	assert_string_contains(ctx.inspector._wp_marker_label.text, "#9", "a selected marker is reported by index")


func test_waypoint_panel_hidden_in_objects_mode() -> void:
	var ctx := _palette_ctx()  # waypoint_mode defaults to false
	assert_false(ctx.inspector._wp_box.visible, "the waypoint panel is hidden in objects mode")
	assert_true(ctx.inspector._place_box.visible, "and the object palette shows")


func test_flag_checkboxes_reflect_the_active_path() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {
		"index": 2, "marker_count": 1, "marker_indices": PackedInt32Array([5]),
		"flags": NovaMissionData.WP_FLAG_DOES_NOT_LOOP | NovaMissionData.WP_FLAG_BLUE_TEAM,
	}
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._wp_loop_check.button_pressed, "DoesNotLoop set -> Loop unchecked")
	assert_true(ctx.inspector._wp_blue_check.button_pressed, "Blue flag -> Blue checked")
	assert_false(ctx.inspector._wp_red_check.button_pressed, "no Red flag -> Red unchecked")


func test_toggling_a_flag_commits_through_set_waypoint_flags() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	ctx.inspector._wp_blue_check.button_pressed = true  # emits toggled (outside the sync guard)
	assert_eq(ctx.fake.set_flags_calls.size(), 1, "toggling a flag commits once")
	# [loop, blue, red]: loop stays on (DoesNotLoop clear), blue now on, red off.
	assert_eq(ctx.fake.set_flags_calls[0], [true, true, false], "the new flag set reaches the controller")


func test_marker_sublist_lists_active_markers_and_selects() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 2}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 2, "marker_indices": PackedInt32Array([5, 9])}
	ctx.fake.changed.emit()
	assert_eq(ctx.inspector._wp_marker_list.item_count, 2, "the sub-list lists the active path's markers in order")
	assert_eq(ctx.inspector._wp_marker_rows, [5, 9], "rows map to marker indices in route order")
	ctx.inspector._on_wp_marker_row_selected(1)  # second marker -> index 9
	assert_eq(ctx.fake.select_marker_calls, [9], "selecting a marker row selects that marker")


func test_add_marker_button_arms_and_stops() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	assert_eq(ctx.inspector._wp_add_button.text, "Add marker", "the button starts as Add marker")
	ctx.inspector._wp_add_button.pressed.emit()
	assert_eq(ctx.fake.arm_marker_calls, 1, "pressing Add marker arms the tool")
	assert_eq(ctx.inspector._wp_add_button.text, "Stop adding markers", "and the button flips to Stop")
	ctx.inspector._wp_add_button.pressed.emit()
	assert_eq(ctx.fake.disarm_marker_calls, 1, "pressing again disarms the tool")


func test_reorder_and_delete_buttons_need_a_selected_marker() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 2}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 2, "marker_indices": PackedInt32Array([5, 9])}
	ctx.fake.changed.emit()
	assert_true(ctx.inspector._wp_up_button.disabled, "reorder is disabled with no marker selected")
	assert_true(ctx.inspector._wp_delete_button.disabled, "delete is disabled with no marker selected")
	assert_false(ctx.inspector._wp_clear_button.disabled, "clear is enabled for a non-empty path")

	ctx.fake.selected_marker = {"path_index": 2, "marker_index": 9, "position": Vector3.ZERO}
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._wp_up_button.disabled, "reorder enables once a marker is selected")
	ctx.inspector._wp_up_button.pressed.emit()
	assert_eq(ctx.fake.move_marker_calls, [-1], "Move up moves the marker one step earlier")
	ctx.inspector._wp_down_button.pressed.emit()
	assert_eq(ctx.fake.move_marker_calls, [-1, 1], "Move down moves it one step later")
	ctx.inspector._wp_delete_button.pressed.emit()
	assert_eq(ctx.fake.delete_marker_calls, 1, "Delete marker removes it")


func test_clear_path_button_clears_the_active_path() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	ctx.inspector._wp_clear_button.pressed.emit()
	assert_eq(ctx.fake.clear_path_calls, 1, "Clear path clears the active path")


func test_clear_path_disabled_for_an_empty_path() -> void:
	var ctx := _waypoint_ctx([{"index": 3, "flags": 0, "marker_count": 0}], 3)
	ctx.fake.active_path = {"index": 3, "flags": 0, "marker_count": 0, "marker_indices": PackedInt32Array()}
	ctx.fake.changed.emit()
	assert_true(ctx.inspector._wp_clear_button.disabled, "clear is disabled when the path has no markers")


func test_new_path_button_is_available_when_the_list_is_empty() -> void:
	# Regression (review): an all-empty mission lists zero paths, so the user must still have a
	# way to start a route. The always-present New path button is that escape hatch.
	var ctx := _waypoint_ctx([{"index": 0, "flags": 0, "marker_count": 0}], -1)  # no active path, all empty
	assert_eq(ctx.inspector._wp_list.item_count, 0, "precondition: the path list is empty (no active, all-empty)")
	assert_true(ctx.inspector._wp_new_path_button.is_visible_in_tree(), "the New path button is still available")
	ctx.inspector._wp_new_path_button.pressed.emit()
	assert_eq(ctx.fake.new_path_calls, 1, "New path focuses a fresh path through the controller")


func test_real_controller_provides_every_method_the_inspector_calls() -> void:
	# The tests above drive a FakeController; this asserts the REAL MissionController
	# exposes the same surface, so a method the inspector calls that the controller does
	# not implement (a runtime crash in the editor) cannot hide behind the fake.
	var controller := MissionController.new(null)
	var required := [
		"get_mission", "get_stats", "get_selection_summary", "get_selected_entity",
		"get_selected_position", "get_selected_rotation",
		"set_selected_position", "set_selected_rotation", "set_selected_team",
		"set_selected_group", "set_selected_property", "delete_selected", "is_dirty",
		"get_placeable_items", "get_placement_item_id", "arm_placement", "disarm_placement",
		# P7 waypoints surface.
		"is_waypoint_mode", "set_waypoint_mode", "select_waypoint_path", "select_new_waypoint_path",
		"get_selected_waypoint_path_index", "get_waypoint_summaries", "get_selected_marker",
		"get_active_waypoint_path", "set_waypoint_flags", "select_waypoint_marker",
		"is_marker_placement_armed", "arm_marker_placement", "disarm_marker_placement",
		"move_selected_marker", "delete_selected_marker", "clear_active_path",
		# Phase 1: hidden string fields + mission-header editing.
		"set_selected_string_property", "set_header_string", "set_header_int", "set_header_flag",
		# Phase 2: edit-mode + area-trigger (zone) surface.
		"set_mode", "get_mode", "is_objects_mode", "is_area_trigger_mode",
		"get_area_triggers", "get_selected_zone_index", "get_selected_zone", "select_area_trigger",
		"add_area_trigger_default", "delete_selected_area_trigger",
		"set_selected_zone_bounds", "set_selected_zone_flags",
	]
	for method in required:
		assert_true(controller.has_method(method),
			"MissionController must implement %s (called by the inspector)" % method)


# --- Phase 1: hidden entity fields + mission-properties (header) form ----------

func _loaded_mission_for_props() -> NovaMissionData:
	var m := NovaMissionData.new()
	m.open_file(ProjectSettings.globalize_path("res://../fixtures/bms/ash_i5b.reference.bms"))
	return m


func test_behavior_panel_shows_hidden_fields() -> void:
	var ctx := _make(_sample_entity())
	assert_not_null(_spin(ctx.inspector, "MissionBeh_no_less_than"), "no_less_than spin built")
	assert_not_null(_spin(ctx.inspector, "MissionBeh_map_symbol"), "map_symbol spin built")
	assert_not_null(_line(ctx.inspector, "MissionBeh_name1"), "name1 line built")
	assert_not_null(_line(ctx.inspector, "MissionBeh_name2"), "name2 line built")


func test_editing_ai_class_commits_through_set_selected_string_property() -> void:
	var ctx := _make(_sample_entity())
	var name1 := _line(ctx.inspector, "MissionBeh_name1")
	name1.text = "rifle"
	name1.text_submitted.emit("rifle")
	assert_eq(ctx.fake.string_property_calls.size(), 1, "one string-property commit")
	assert_eq(ctx.fake.string_property_calls[0][0], "name1", "field is name1")
	assert_eq(ctx.fake.string_property_calls[0][1], "rifle", "value carried through")


func test_props_form_is_hidden_without_a_mission() -> void:
	var ctx := _make({})
	assert_false(ctx.inspector._props_toggle.visible, "the props toggle hides without a mission")


func test_props_form_reads_header_values() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	assert_true(ctx.inspector._props_toggle.visible, "the props toggle shows with a mission")
	var name_line := _line(ctx.inspector, "MissionProp_mission_name")
	assert_not_null(name_line, "the name field is built")
	assert_eq(name_line.text, String(ctx.fake.mission_ref.get_info()["mission_name"]),
		"the name field reads the header")


func test_editing_name_commits_through_set_header_string() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var name_line := _line(ctx.inspector, "MissionProp_mission_name")
	name_line.text = "Renamed"
	name_line.text_submitted.emit("Renamed")
	assert_eq(ctx.fake.header_string_calls.size(), 1, "one header_string commit")
	assert_eq(ctx.fake.header_string_calls[0][0], "mission_name", "field is mission_name")
	assert_eq(ctx.fake.header_string_calls[0][1], "Renamed", "value carried through")


func test_changing_climate_commits_through_set_header_int() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var climate := ctx.inspector.find_child("MissionProp_climate", true, false) as OptionButton
	assert_not_null(climate, "climate option built")
	for i in climate.item_count:
		if climate.get_item_id(i) == 2:  # Snow
			climate.selected = i
			climate.item_selected.emit(i)
			break
	assert_eq(ctx.fake.header_int_calls.size(), 1, "one header_int commit")
	assert_eq(ctx.fake.header_int_calls[0][0], "climate", "field is climate")
	assert_eq(ctx.fake.header_int_calls[0][1], 2, "selected id (Snow) carried through")


func test_toggling_a_game_mode_flag_commits_through_set_header_flag() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var coop := ctx.inspector.find_child("MissionFlag_%d" % NovaMissionData.ATTRIB_COOP, true, false) as CheckBox
	assert_not_null(coop, "co-op flag checkbox built")
	# Emit the user-toggle signal directly (setting button_pressed would itself emit, double-firing).
	coop.toggled.emit(true)
	assert_eq(ctx.fake.header_flag_calls.size(), 1, "one header_flag commit")
	assert_eq(ctx.fake.header_flag_calls[0][0], NovaMissionData.ATTRIB_COOP, "bit is COOP")
	assert_eq(ctx.fake.header_flag_calls[0][1], true, "on carried through")


# --- Phase 2: area-trigger (zone) panel ---------------------------------------

func _trigger_ctx(zones: Array, selected: int) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 2  # AREA_TRIGGERS
	fake.zones = zones
	fake.selected_zone = selected
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func _zone(index: int, mn: Vector3, mx: Vector3, active: bool, constrain_z: bool) -> Dictionary:
	return {"index": index, "id": 0, "min": mn, "max": mx, "active": active, "constrain_z": constrain_z, "raw_flags": (1 if active else 0) | (2 if constrain_z else 0)}


func test_trigger_panel_shows_and_lists_zones_in_trigger_mode() -> void:
	var ctx := _trigger_ctx([
		_zone(0, Vector3(-5, -6, -7), Vector3(5, 6, 7), true, false),
		_zone(1, Vector3(0, 0, 0), Vector3(10, 10, 10), false, true),
	], 0)
	assert_true(ctx.inspector._at_box.visible, "the trigger panel shows in trigger mode")
	assert_false(ctx.inspector._edit_box.visible, "the object edit panel is hidden in trigger mode")
	assert_false(ctx.inspector._place_box.visible, "the palette is hidden in trigger mode")
	assert_eq(ctx.inspector._at_list.item_count, 2, "both zones are listed")
	assert_eq(ctx.inspector._at_rows, [0, 1], "the listed zone indices match")
	# The selected zone's bounds populate the spins.
	assert_eq(ctx.inspector._at_min_spins[0].value, -5.0, "min X spin reflects the selected zone")
	assert_eq(ctx.inspector._at_max_spins[1].value, 6.0, "max Y spin reflects the selected zone")
	assert_true(ctx.inspector._at_active_check.button_pressed, "active toggle reflects the selected zone")


func test_trigger_panel_add_button_calls_controller() -> void:
	var ctx := _trigger_ctx([], -1)
	var add := ctx.inspector.find_child("MissionAtAddZone", true, false) as Button
	assert_not_null(add, "Add zone button built")
	add.pressed.emit()
	assert_eq(ctx.fake.add_zone_calls, 1, "Add zone calls the controller")


func test_trigger_panel_row_selects_zone() -> void:
	var ctx := _trigger_ctx([
		_zone(0, Vector3.ZERO, Vector3.ONE, true, false),
		_zone(1, Vector3.ZERO, Vector3.ONE, true, false),
	], 0)
	ctx.inspector._at_list.item_selected.emit(1)
	assert_eq(ctx.fake.select_zone_calls, [1], "selecting a row focuses that zone")


func test_trigger_panel_bounds_spin_commits() -> void:
	var ctx := _trigger_ctx([_zone(0, Vector3(-1, -1, -1), Vector3(1, 1, 1), true, false)], 0)
	var min_x := ctx.inspector.find_child("MissionAtMinX", true, false) as SpinBox
	assert_not_null(min_x, "Min X spin built")
	# Setting .value fires value_changed naturally (as a user edit would); the handler reads the
	# new value back off the spin.
	min_x.value = -50.0
	assert_eq(ctx.fake.zone_bounds_calls.size(), 1, "one bounds commit")
	assert_eq((ctx.fake.zone_bounds_calls[0][0] as Vector3).x, -50.0, "the new min X is sent")


func test_trigger_panel_flag_toggle_commits() -> void:
	var ctx := _trigger_ctx([_zone(0, Vector3.ZERO, Vector3.ONE, true, false)], 0)
	var constrain := ctx.inspector.find_child("MissionAtConstrainZ", true, false) as CheckBox
	assert_not_null(constrain, "Constrain height toggle built")
	# Setting button_pressed fires toggled with the new state; the handler reads both checks.
	constrain.button_pressed = true
	assert_eq(ctx.fake.zone_flags_calls.size(), 1, "one flag commit")
	assert_eq(ctx.fake.zone_flags_calls[0][0], true, "active carried through")
	assert_eq(ctx.fake.zone_flags_calls[0][1], true, "constrain_z carried through")


func test_trigger_panel_delete_button_calls_controller() -> void:
	var ctx := _trigger_ctx([_zone(0, Vector3.ZERO, Vector3.ONE, true, false)], 0)
	var del := ctx.inspector.find_child("MissionAtDeleteZone", true, false) as Button
	assert_not_null(del, "Delete zone button built")
	del.pressed.emit()
	assert_eq(ctx.fake.delete_zone_calls, 1, "Delete zone calls the controller")
