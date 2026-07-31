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
	var graphic_name: String = ""  # resolved items.def graphic basename for the identity block
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
	# Phase 4: PLAYPARTANIM preview hooks.
	var preview_calls: Array = []
	var stop_preview_calls: int = 0
	var can_preview: bool = false
	# Selected-object userpoint overlay hooks.
	var selected_user_points_available: bool = false
	var selected_user_points_visible: bool = false
	var user_point_visibility_calls: Array = []

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

	func get_selected_graphic_name() -> String:
		return graphic_name

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
	var game_mode_calls: Array = []          # [bit] per set_game_mode

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

	func set_game_mode(bit: int) -> void:
		game_mode_calls.append(bit)
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

	# B8: the Mission-tab bulk re-ground button delegates here.
	var reground_calls := 0

	func reground_drifted() -> int:
		reground_calls += 1
		return 0

	func selected_has_user_points() -> bool:
		return selected_user_points_available

	func is_selected_user_points_visible() -> bool:
		return selected_user_points_visible

	func set_selected_user_points_visible(value: bool) -> void:
		user_point_visibility_calls.append(value)
		selected_user_points_visible = value
		changed.emit()

	# Placed-objects browser state. `object_list` rows mirror MissionController.get_object_list
	# ({ kind, index, item_id, name, category }); select_object records the (kind, index) the
	# list row handler forwards and selects that entity (the real controller also frames the camera).
	var object_list: Array = []
	var has_item_db: bool = true
	var select_object_calls: Array = []

	func get_object_list() -> Array:
		return object_list

	func get_object_count() -> int:
		return object_list.size()

	# Pick-debug toggle surface used by the object browser's debug checkbox.
	var pick_debug: bool = false

	func is_pick_debug() -> bool:
		return pick_debug

	func set_pick_debug(value: bool) -> void:
		pick_debug = value

	# Transform-gizmo toggle surface used by the object browser's gizmo checkbox.
	var gizmo_enabled: bool = true

	func is_gizmo_enabled() -> bool:
		return gizmo_enabled

	func set_gizmo_enabled(value: bool) -> void:
		gizmo_enabled = value

	func has_item_database() -> bool:
		return has_item_db

	func select_object(kind: int, index: int) -> void:
		select_object_calls.append([kind, index])
		entity = { "kind": kind, "index": index, "position": Vector3.ZERO }
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

	# Phase 3: weapon loadout + groups (mission-global). `loadout`/`groups` are
	# NovaMissionData-shaped dictionaries; the *_calls arrays record what the inspector sent.
	var loadout: Array = []
	var groups: Array = []
	var set_loadout_calls: Array = []
	var set_group_calls: Array = []

	func get_weapon_loadout() -> Array:
		return loadout

	func set_weapon_loadout(entries: Array) -> void:
		set_loadout_calls.append(entries.duplicate(true))
		loadout = entries.duplicate(true)
		changed.emit()

	func get_group_count() -> int:
		return groups.size()

	# Flat entity list for scripting ENTITY_REF pickers: [{value, label}]. Settable per test.
	var all_entities: Array = []

	# Call counters: the inspector caches these and should only re-call them when the membership
	# revision changes, not on every refresh.
	var all_entities_calls: int = 0
	var group_options_calls: int = 0

	func get_all_entities() -> Array:
		all_entities_calls += 1
		return all_entities

	# Options for the entity-edit pickers (waypoint path / group). Settable per test; default empty
	# so the inspector's populate_id_option falls back to the entity's current value.
	var waypoint_options: Array = []
	var group_options: Array = []

	func get_waypoint_path_options() -> Array:
		return waypoint_options

	func get_group_options() -> Array:
		group_options_calls += 1
		return group_options

	# The inspector caches the option lists and rebuilds only when this changes (the real controller
	# bumps a counter on entity-set / group changes). Returning a content hash of the three lists makes
	# the cache transparent to tests: any test that swaps the arrays gets a fresh revision automatically.
	func get_membership_revision() -> int:
		return hash([all_entities, group_options, waypoint_options])

	func get_groups() -> Array:
		return groups

	func get_group(index: int) -> Dictionary:
		if index < 0 or index >= groups.size():
			return {}
		return groups[index]

	func set_group(index: int, f0: int, f8: int, f12: int) -> void:
		set_group_calls.append([index, f0, f8, f12])
		if index >= 0 and index < groups.size():
			groups[index] = {"index": index, "field0": f0, "field8": f8, "field12": f12}
		changed.emit()

	# Phase 4: mission scripting. `events` are NovaMissionData-shaped event dicts; `chain` is the
	# selected event's chain ({ event, triggers, actions, references, diagnostics }); the *_calls arrays
	# record what the inspector's Scripting tab sent. Mode 3 is SCRIPTING.
	var events: Array = []
	var selected_event: int = -1
	var chain: Dictionary = {}
	var select_event_calls: Array = []
	var add_event_calls: int = 0
	var delete_event_calls: int = 0
	var set_event_calls: Array = []
	var add_trigger_calls: int = 0
	var set_trigger_calls: Array = []
	var remove_trigger_calls: Array = []
	var move_trigger_calls: Array = []
	var add_action_calls: int = 0
	var set_action_calls: Array = []
	var remove_action_calls: Array = []
	var move_action_calls: Array = []

	func is_scripting_mode() -> bool:
		return mode == 3

	func get_events() -> Array:
		return events

	func get_event_count() -> int:
		return events.size()

	func get_selected_event_index() -> int:
		return selected_event

	func get_selected_event_chain() -> Dictionary:
		return chain

	func get_logic_summary() -> Dictionary:
		return {}

	func get_event_flag_bits() -> Array:
		return [{"value": 1, "name": "Reset after"}, {"value": 2, "name": "Pre-mission"}, {"value": 4, "name": "Post-mission"}]

	func get_ai_flag_bits() -> Array:
		return [{"value": 1, "name": "Blind"}, {"value": 2, "name": "Guarding"}, {"value": 1 << 22, "name": "Navigation waypoint"}]

	func get_trigger_main_types() -> Array:
		return [{"value": 1, "name": "Group"}, {"value": 2, "name": "Single"}]

	func get_trigger_sub_types(main_type: int) -> Array:
		if main_type == 2:
			return [{"value": 0, "name": "Null"}, {"value": 10, "name": "SingleIsWithinArea"}]
		return [{"value": 0, "name": "Null"}, {"value": 1, "name": "GroupSeesGroup"}, {"value": 10, "name": "GroupIsWithinArea"}]

	func get_action_types() -> Array:
		return [{"value": 0, "name": "Null"}, {"value": 6, "name": "OutputText"}, {"value": 34, "name": "ResetEvent"}]

	func get_action_sub_types(_action_type: int) -> Array:
		return [{"value": 0, "name": "Null"}]

	func can_preview_part_anim(_action: Dictionary) -> bool:
		return can_preview

	func preview_part_anim(action: Dictionary) -> bool:
		preview_calls.append(action.duplicate(true))
		return can_preview

	func stop_preview() -> void:
		stop_preview_calls += 1

	func select_event(index: int) -> void:
		select_event_calls.append(index)
		selected_event = index
		changed.emit()

	func add_event_default() -> int:
		add_event_calls += 1
		var idx := events.size()
		events.append({"index": idx, "flags": 0, "trigger_count": 0, "action_count": 0, "reset_after": 0, "delay": 0})
		selected_event = idx
		changed.emit()
		return idx

	func delete_selected_event() -> bool:
		delete_event_calls += 1
		if selected_event >= 0 and selected_event < events.size():
			events.remove_at(selected_event)
		selected_event = -1
		changed.emit()
		return true

	func set_selected_event(flags: int, reset_after: int, delay: int) -> void:
		set_event_calls.append([flags, reset_after, delay])
		changed.emit()

	func add_selected_event_trigger() -> void:
		add_trigger_calls += 1
		changed.emit()

	func set_selected_event_trigger(local_index: int, trigger: Dictionary) -> void:
		set_trigger_calls.append([local_index, trigger.duplicate(true)])
		changed.emit()

	func remove_selected_event_trigger(local_index: int) -> void:
		remove_trigger_calls.append(local_index)
		changed.emit()

	func move_selected_event_trigger(local_index: int, delta: int) -> void:
		move_trigger_calls.append([local_index, delta])
		changed.emit()

	func add_selected_event_action() -> void:
		add_action_calls += 1
		changed.emit()

	func set_selected_event_action(local_index: int, action: Dictionary) -> void:
		set_action_calls.append([local_index, action.duplicate(true)])
		changed.emit()

	func remove_selected_event_action(local_index: int) -> void:
		remove_action_calls.append(local_index)
		changed.emit()

	func move_selected_event_action(local_index: int, delta: int) -> void:
		move_action_calls.append([local_index, delta])
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


func _option(inspector, node_name: String) -> OptionButton:
	return inspector.find_child(node_name, true, false) as OptionButton


func _option_id(inspector, node_name: String) -> int:
	var opt := _option(inspector, node_name)
	return opt.get_item_id(opt.selected) if opt != null and opt.selected >= 0 else -1


func _option_row_for_id(opt: OptionButton, id: int) -> int:
	for i in opt.item_count:
		if opt.get_item_id(i) == id:
			return i
	return -1


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
	assert_eq(_option_id(ctx.inspector, "MissionTeam"), 1, "team reads from the entity as a named option")
	assert_eq(_option(ctx.inspector, "MissionTeam").get_item_text(_option(ctx.inspector, "MissionTeam").selected),
		"Good / blue", "team 1 uses the known faction label")
	assert_eq(_option_id(ctx.inspector, "MissionOpt_group"), 2,
		"group reads from the entity as the selected dropdown option")


func test_waypoint_pointing_at_empty_slot_shows_path_label() -> void:
	# waypoint_id is a fixed path NUMBER (0-127). A unit pointed at a valid but EMPTY slot (no markers --
	# common for units that man a gun / ride a vehicle and never path-follow) has no row in the curated
	# dropdown, so it must read "Path N (no markers)", not the generic "Value N".
	var entity := _sample_entity()
	entity["waypoint_id"] = 125
	var ctx := _make(entity)  # fake.waypoint_options defaults to [] -> path 125 is absent
	var opt := _option(ctx.inspector, "MissionOpt_waypoint_id")
	assert_not_null(opt, "the Waypoint path picker exists")
	assert_eq(opt.get_item_id(opt.selected), 125, "the picker preserves the real waypoint_id")
	assert_eq(opt.get_item_text(opt.selected), "Path 125 (no markers)",
		"an empty-slot reference is labelled as a path, not 'Value 125'")


func test_waypoint_pointing_at_populated_path_selects_it() -> void:
	var entity := _sample_entity()
	entity["waypoint_id"] = 3
	var ctx := _make(entity)
	ctx.fake.waypoint_options = [{ "id": 0, "label": "None" }, { "id": 3, "label": "Path 3  -  4 markers" }]
	ctx.inspector._refresh()  # swapping the option list bumps the membership revision -> cache rebuilds
	var opt := _option(ctx.inspector, "MissionOpt_waypoint_id")
	assert_eq(opt.get_item_id(opt.selected), 3, "a populated path is selected by id")
	assert_eq(opt.get_item_text(opt.selected), "Path 3  -  4 markers",
		"a real path shows its label, not a fallback")


func test_identity_shows_resolved_model_name() -> void:
	# With a resolved model name the identity heading reads the name and a muted kind + index
	# subline appears beneath it.
	var ctx := _make(_sample_entity())
	ctx.fake.display_name = "Spec Ops Soldier"
	ctx.inspector._refresh()  # re-read now that the name resolves
	assert_eq(ctx.inspector._identity_label.text, "Spec Ops Soldier", "the heading shows the model name")
	assert_true(ctx.inspector._identity_sub.visible, "the kind + index subline shows under the name")
	assert_eq(ctx.inspector._identity_sub.text, "Organic #4", "the subline carries the kind and index")


func test_identity_shows_resolved_graphic_name() -> void:
	var ctx := _make(_sample_entity())
	ctx.fake.graphic_name = "SpecOps"
	ctx.inspector._refresh()
	assert_true(ctx.inspector._identity_graphic_row.visible, "the graphic row shows when a graphic resolves")
	assert_eq(ctx.inspector._identity_graphic.get_value(), "SpecOps", "the graphic row names the items.def graphic")
	assert_false(ctx.inspector._identity_graphic.name_edit.editable, "the selected graphic is read-only derived data")
	assert_false(ctx.inspector._identity_graphic.browse_button.visible, "browse is hidden: this row does not set the graphic")
	assert_false(ctx.inspector._identity_graphic.clear_button.visible, "clear is hidden: this row does not set the graphic")


func test_identity_graphic_jump_opens_resolved_object_model() -> void:
	var ctx := _make(_sample_entity())
	ctx.fake.graphic_name = "SpecOps"
	var log := {}
	ctx.inspector.set_reference_services({
		"resolve": func(kind: String, name: String) -> Dictionary:
			log["resolve"] = [kind, name]
			return {"status": "found", "path": "C:/res/SpecOps.3di"},
		"pick": func(kind: String, title: String, _on_pick: Callable) -> void:
			log["pick"] = [kind, title],
		"jump": func(kind: String, path: String) -> void:
			log["jump"] = [kind, path],
	})
	ctx.inspector._refresh()
	assert_eq(log["resolve"], ["object_model", "SpecOps"], "the selected graphic resolves as an object model")
	assert_true(ctx.inspector._identity_graphic.jump_button.visible, "resolved graphics expose the jump button")
	assert_false(ctx.inspector._identity_graphic.jump_button.disabled, "resolved graphics can jump")
	assert_false(ctx.inspector._identity_graphic.browse_button.visible, "browse stays hidden even when picker services exist")
	ctx.inspector._identity_graphic.jump_button.pressed.emit()
	assert_eq(log["jump"], ["object_model", "C:/res/SpecOps.3di"],
		"jump carries the object_model kind and resolved .3di path")


func test_identity_hides_graphic_line_without_a_graphic() -> void:
	var ctx := _make(_sample_entity())
	assert_false(ctx.inspector._identity_graphic_row.visible, "no resolved graphic -> no graphic row clutter")
	assert_eq(ctx.inspector._identity_graphic.get_value(), "", "hidden graphic row is cleared")


func test_identity_falls_back_to_kind_and_index_without_a_name() -> void:
	var ctx := _make(_sample_entity())  # display_name left ""
	assert_eq(ctx.inspector._identity_label.text, "Organic #4", "no resolved name -> kind + index heading")
	assert_false(ctx.inspector._identity_sub.visible, "and no redundant subline")


func test_userpoint_checkbox_syncs_selected_overlay_state() -> void:
	var ctx := _make(_sample_entity())
	ctx.fake.selected_user_points_available = true
	ctx.fake.selected_user_points_visible = true
	ctx.inspector._refresh()
	var check := ctx.inspector.find_child("MissionUserPointsCheck", true, false) as CheckBox
	assert_not_null(check, "Selection editor should expose a userpoint visibility checkbox.")
	if check == null:
		return
	assert_false(check.disabled, "Userpoint checkbox is enabled when the selected model has points.")
	assert_true(check.button_pressed, "Userpoint checkbox mirrors the controller visibility state.")


func test_userpoint_checkbox_toggles_controller_once() -> void:
	var ctx := _make(_sample_entity())
	ctx.fake.selected_user_points_available = true
	ctx.inspector._refresh()
	var check := ctx.inspector.find_child("MissionUserPointsCheck", true, false) as CheckBox
	assert_not_null(check, "Selection editor should expose a userpoint visibility checkbox.")
	if check == null:
		return

	check.toggled.emit(true)

	assert_eq(ctx.fake.user_point_visibility_calls, [true], "Userpoint checkbox toggles the controller once.")
	assert_true(ctx.fake.selected_user_points_visible, "The controller visibility state is updated.")


func test_userpoint_checkbox_disables_without_selected_points() -> void:
	var ctx := _make(_sample_entity())
	var check := ctx.inspector.find_child("MissionUserPointsCheck", true, false) as CheckBox
	assert_not_null(check, "Selection editor should expose a userpoint visibility checkbox.")
	if check == null:
		return
	assert_true(check.disabled, "Userpoint checkbox disables when the selected model has no points.")
	assert_false(check.button_pressed, "Unavailable userpoints should not show as enabled.")


func test_editing_team_commits_exactly_once() -> void:
	var ctx := _make(_sample_entity())
	var team := _option(ctx.inspector, "MissionTeam")
	var evil_row := _option_row_for_id(team, 2)
	assert_ne(evil_row, -1, "the fixed Evil / red team row exists")
	team.select(evil_row)
	team.item_selected.emit(evil_row)
	assert_eq(ctx.fake.last_team, 2, "the team edit reached the controller")
	assert_eq(ctx.fake.team_calls, 1, "the changed-signal echo did not re-commit (the _loading guard holds)")
	assert_true(ctx.fake.is_dirty())


func test_team_picker_preserves_out_of_range_value() -> void:
	var entity := _sample_entity()
	entity["team"] = 5
	var ctx := _make(entity)
	var team := _option(ctx.inspector, "MissionTeam")
	assert_not_null(team, "Team is shown as a dropdown")
	assert_eq(team.get_item_id(team.selected), 5, "out-of-range authored value remains selected")
	assert_eq(team.get_item_text(team.selected), "Team 5", "unknown team gets a fallback label")
	assert_eq(ctx.fake.team_calls, 0, "refreshing an unknown team does not rewrite it")


func test_editing_one_position_axis_leaves_the_others() -> void:
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionPosX").value = 99
	assert_eq(ctx.fake.pos_calls, 1, "one commit, no echo loop")
	assert_eq(ctx.fake.last_pos, Vector3(99.0, 2.0, -5.0),
		"only X changed; Y and Z came from the model, not the sibling spins")


func test_editing_group_commits_through_set_selected_property() -> void:
	# Group is a "pick an available squad" dropdown now: choosing one commits through
	# set_selected_property("group", id), not the old clamped set_selected_group spin path.
	var ctx := _make(_sample_entity())
	ctx.fake.group_options = [{"id": 0, "label": "Ungrouped"}, {"id": 9, "label": "New group 9"}]
	ctx.inspector._refresh()  # refill the dropdown with the available groups
	var opt := _option(ctx.inspector, "MissionOpt_group")
	var row := _option_row_for_id(opt, 9)
	assert_gt(row, -1, "the chosen group is an available option")
	opt.selected = row
	opt.item_selected.emit(row)  # simulate the user picking the group
	assert_eq(ctx.fake.last_property, "group", "the group edit names the group field")
	assert_eq(ctx.fake.last_property_value, 9, "with the chosen group id")
	assert_eq(ctx.fake.property_calls, 1, "one commit, no echo loop (the binder guard holds)")


func test_group_picker_shows_a_single_fallback_for_an_out_of_set_value() -> void:
	# Regression: the Group/Waypoint pickers used to be populated TWICE per refresh (populate_id_option
	# + the binder), which stacked two different fallback rows for an out-of-set value. Now the binder
	# is the sole populator, so exactly one fallback row appears and repeated refreshes don't accumulate.
	var ctx := _make(_sample_entity())  # _sample_entity().group == 2
	ctx.fake.group_options = [{"id": 0, "label": "Ungrouped"}]  # 2 is NOT offered
	ctx.inspector._refresh()
	var opt := _option(ctx.inspector, "MissionOpt_group")
	assert_eq(opt.item_count, 2, "one offered row + exactly one fallback for the out-of-set group")
	assert_eq(opt.get_item_id(opt.selected), 2, "and the entity's real group value is selected via that fallback")
	ctx.inspector._refresh()
	assert_eq(opt.item_count, 2, "a repeated refresh does not stack another fallback row")


func test_option_lists_are_cached_until_membership_changes() -> void:
	# Regression (perf): get_group_options / get_all_entities marshal every entity, so they must be
	# fetched only when the membership revision changes, not on every `changed` (every edit / drag).
	var ctx := _make(_sample_entity())
	var base: int = ctx.fake.group_options_calls
	ctx.inspector._refresh()
	ctx.inspector._refresh()
	assert_eq(ctx.fake.group_options_calls, base, "unchanged membership reuses the cached option lists")
	ctx.fake.group_options = [{"id": 0, "label": "Ungrouped"}, {"id": 5, "label": "New group 5"}]
	ctx.inspector._refresh()  # a new membership revision (the fake hashes its option lists)
	assert_gt(ctx.fake.group_options_calls, base, "a membership change re-fetches the option lists")


# --- P7: the Behavior panel (per-entity AI + waypoint fields) ------------------

func test_behavior_panel_reads_the_selected_values() -> void:
	var ctx := _make(_sample_entity())
	var opt := _option(ctx.inspector, "MissionOpt_waypoint_id")
	assert_not_null(opt, "the behavior panel builds a waypoint_id picker")
	assert_eq(_option_id(ctx.inspector, "MissionOpt_waypoint_id"), 3,
		"waypoint_id reads from the entity (even while the section is collapsed)")
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
	# A still-numeric behavior field (wp_number) commits through set_selected_property once.
	var ctx := _make(_sample_entity())
	_spin(ctx.inspector, "MissionBeh_wp_number").value = 7  # simulate a user edit
	assert_eq(ctx.fake.last_property, "wp_number", "the edit names the field it changed")
	assert_eq(ctx.fake.last_property_value, 7, "with the new value")
	assert_eq(ctx.fake.property_calls, 1, "the FieldBinder guard holds: one commit, no echo loop")
	assert_true(ctx.fake.is_dirty())


func test_waypoint_path_picker_commits_through_set_selected_property() -> void:
	# Picking a path from the "Waypoint path" dropdown writes the waypoint_id field once.
	var ctx := _make(_sample_entity())
	ctx.fake.waypoint_options = [{"id": 0, "label": "None"}, {"id": 7, "label": "Path 7  -  2 markers"}]
	ctx.inspector._refresh()  # refill the dropdown with the available paths
	var opt := _option(ctx.inspector, "MissionOpt_waypoint_id")
	var row := _option_row_for_id(opt, 7)
	assert_gt(row, -1, "the chosen path is an available option")
	opt.selected = row
	opt.item_selected.emit(row)  # simulate the user picking the path
	assert_eq(ctx.fake.last_property, "waypoint_id", "picking a path writes the waypoint_id field")
	assert_eq(ctx.fake.last_property_value, 7, "with the chosen path id")
	assert_eq(ctx.fake.property_calls, 1, "one commit, no echo loop (the binder guard holds)")


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
	assert_false(inspector._palette._place_box.visible, "the palette hides when no mission is open")


func test_palette_populates_rows_from_the_controller() -> void:
	var ctx := _palette_ctx()
	assert_true(ctx.inspector._palette._place_box.visible, "the palette shows with a mission open")
	assert_eq(ctx.inspector._palette._place_list.item_count, 3, "every placeable item becomes a row")
	assert_false(ctx.inspector._palette._place_stop.visible, "Stop is hidden until something is armed")


func test_selecting_a_palette_row_arms_that_item() -> void:
	var ctx := _palette_ctx()
	ctx.inspector._palette._on_place_item_selected(0)  # simulate the user picking row 0
	assert_eq(ctx.fake.arm_calls.size(), 1, "selecting a row arms exactly once")
	assert_eq(int(ctx.fake.arm_calls[0]), int(ctx.inspector._palette._place_row_ids[0]),
		"the armed id is the one on the selected row")
	assert_true(ctx.inspector._palette._place_stop.visible, "the Stop button appears once armed")


func test_stop_button_disarms() -> void:
	var ctx := _palette_ctx()
	ctx.fake.armed_id = 102001
	ctx.inspector._refresh()  # reflect the armed state
	assert_true(ctx.inspector._palette._place_stop.visible)
	ctx.inspector._palette._on_place_stop()
	assert_eq(ctx.fake.disarm_calls, 1, "the Stop button disarms placement")


func test_palette_search_filters_rows() -> void:
	var ctx := _palette_ctx()
	ctx.inspector._palette._on_place_search_changed("buggy")
	assert_eq(ctx.inspector._palette._place_list.item_count, 1, "the search narrows to matching names")
	assert_eq(int(ctx.inspector._palette._place_row_ids[0]), 101291, "and the surviving row is the match")
	ctx.inspector._palette._on_place_search_changed("")
	assert_eq(ctx.inspector._palette._place_list.item_count, 3, "clearing the search restores every row")


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
	assert_eq(inspector._palette._place_list.item_count, 0, "empty until the database resolves")

	fake.placeable = _palette_items()  # the database becomes resolvable (same mission)
	inspector._refresh()
	assert_eq(inspector._palette._place_list.item_count, 3, "the palette fills in without reopening the mission")


func test_active_search_filter_survives_a_changed_echo() -> void:
	# The palette's central contract (like the edit panel): the list is built once and
	# NOT repopulated on the `changed` signal that fires on every edit / select / place.
	# So a typed search filter and the filtered rows must survive a same-mission `changed`
	# -- otherwise the search box would clear and the full list would snap back on every
	# placed object. This pins that invariant.
	var ctx := _palette_ctx()
	# Simulate the user typing a filter: the LineEdit holds the text, and the text_changed
	# handler narrows the list (setting .text alone does not emit text_changed in Godot).
	ctx.inspector._palette._place_search.text = "buggy"
	ctx.inspector._palette._on_place_search_changed("buggy")
	assert_eq(ctx.inspector._palette._place_list.item_count, 1, "precondition: filtered to one row")
	ctx.inspector._palette._on_place_item_selected(0)  # arms the surviving item; fires `changed`
	ctx.fake.changed.emit()  # stand in for a later edit / placement on the same mission

	assert_eq(ctx.inspector._palette._place_search.text, "buggy", "the search text survives a same-mission `changed`")
	assert_eq(ctx.inspector._palette._place_list.item_count, 1, "the filtered rows survive; the list is not repopulated")
	assert_eq(int(ctx.inspector._palette._place_row_ids[0]), 101291, "and the surviving row is still the match")
	assert_true(ctx.inspector._palette._place_stop.visible, "still armed after the echo")


# --- Placed-objects browser ---------------------------------------------------

func _object_rows() -> Array:
	return [
		{"kind": NovaMissionData.KIND_ORGANIC, "index": 0, "item_id": 105311, "name": "Soldier", "category": "Person"},
		{"kind": NovaMissionData.KIND_ORGANIC, "index": 1, "item_id": 105311, "name": "Soldier", "category": "Person"},
		{"kind": NovaMissionData.KIND_BUILDING, "index": 0, "item_id": 102001, "name": "Guard Tower", "category": "Building"},
		{"kind": NovaMissionData.KIND_ITEM, "index": 0, "item_id": 999, "name": "", "category": "Item"},
	]


func _browser_ctx() -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.object_list = _object_rows()
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func test_browser_is_hidden_without_a_mission() -> void:
	var fake := FakeController.new()  # mission_ref left null
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	assert_false(inspector._browser._objects_box.visible, "the placed-objects list hides when no mission is open")


func test_browser_lists_every_placed_object_with_ordinals() -> void:
	var ctx := _browser_ctx()
	assert_true(ctx.inspector._browser._objects_box.visible, "the list shows with a mission open")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 4, "every placed object becomes a row")
	# Duplicate base names get a "(n)" ordinal so the two Soldiers are distinguishable.
	assert_eq(ctx.inspector._browser._objects_list.get_item_text(0), "Soldier (1)")
	assert_eq(ctx.inspector._browser._objects_list.get_item_text(1), "Soldier (2)")
	assert_eq(ctx.inspector._browser._objects_list.get_item_text(2), "Guard Tower", "unique names carry no ordinal")
	assert_eq(ctx.inspector._browser._objects_list.get_item_text(3), "Item 999", "an unresolved name falls back to the item id")


func test_selecting_a_browser_row_selects_that_entity() -> void:
	var ctx := _browser_ctx()
	ctx.inspector._browser._on_object_row_selected(2)  # the Guard Tower row
	assert_eq(ctx.fake.select_object_calls.size(), 1, "the row drives exactly one select")
	assert_eq(ctx.fake.select_object_calls[0], [NovaMissionData.KIND_BUILDING, 0],
		"with the kind + index from that row (which also frames the camera in the real controller)")


func test_browser_search_filters_rows() -> void:
	var ctx := _browser_ctx()
	ctx.inspector._browser._on_object_search_changed("tower")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 1, "the search narrows to matching names")
	assert_eq(ctx.inspector._browser._objects_rows[0], {"kind": NovaMissionData.KIND_BUILDING, "index": 0},
		"and the surviving row maps to the matching entity")
	# Category words are searchable too, so "person" finds both Soldiers.
	ctx.inspector._browser._on_object_search_changed("person")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 2, "the category is part of the search key")
	ctx.inspector._browser._on_object_search_changed("")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 4, "clearing the search restores every row")


func test_browser_highlights_the_controllers_selection() -> void:
	var ctx := _browser_ctx()
	# A viewport pick selects an entity; the list must light up + scroll to the matching row.
	ctx.fake.entity = {"kind": NovaMissionData.KIND_BUILDING, "index": 0, "position": Vector3.ZERO}
	ctx.inspector._refresh()
	assert_eq(ctx.inspector._browser._objects_list.get_selected_items(), PackedInt32Array([2]),
		"the row for the current selection is highlighted")


func test_browser_repopulates_when_the_item_db_arrives_late() -> void:
	# Same lifecycle dead-end the palette guards: a mission can open before items.def resolves,
	# so the rows first show "Item <id>" placeholders. Once names resolve, a later refresh must
	# relabel them WITHOUT the object set (count) changing.
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.has_item_db = false
	fake.object_list = [
		{"kind": NovaMissionData.KIND_BUILDING, "index": 0, "item_id": 102001, "name": "", "category": "Building"},
	]
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	assert_eq(inspector._browser._objects_list.get_item_text(0), "Item 102001", "placeholder until the database resolves")

	fake.has_item_db = true
	fake.object_list = [
		{"kind": NovaMissionData.KIND_BUILDING, "index": 0, "item_id": 102001, "name": "Guard Tower", "category": "Building"},
	]
	inspector._refresh()
	assert_eq(inspector._browser._objects_list.get_item_text(0), "Guard Tower", "relabelled without the object set changing")


func test_browser_rows_survive_a_same_mission_changed_echo() -> void:
	# Like the palette: the list is rebuilt only when the object set (count) changes, not on the
	# `changed` that fires on every edit / drag frame. A typed filter and its rows must survive.
	var ctx := _browser_ctx()
	ctx.inspector._browser._objects_search.text = "tower"
	ctx.inspector._browser._on_object_search_changed("tower")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 1, "precondition: filtered to one row")
	ctx.fake.changed.emit()  # stand in for an edit on the same mission (no count change)
	assert_eq(ctx.inspector._browser._objects_search.text, "tower", "the search text survives a same-mission `changed`")
	assert_eq(ctx.inspector._browser._objects_list.item_count, 1, "the filtered rows survive; the list is not rebuilt")


# --- P7: edit-mode tabs + waypoint panel --------------------------------------

func _waypoint_ctx(summaries: Array, active: int) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 1  # WAYPOINTS
	# A populated selection makes the "_edit_box hidden in waypoint mode" gate assertion
	# load-bearing (the edit panel would otherwise be hidden just for lack of a selection).
	fake.entity = _sample_entity()
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


func test_inspector_has_no_embedded_runtime_controls() -> void:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	var inspector = MissionInspector.new()
	inspector.size = Vector2(280, 640)
	add_child_autofree(inspector)
	inspector.setup(fake)
	await get_tree().process_frame

	assert_null(inspector.find_child("MissionSimBar", true, false),
		"mission testing is launched by the editor toolbar, not owned by the inspector")
	assert_null(inspector.find_child("MissionDebugBtn", true, false),
		"F3 belongs to the launched game")


func test_waypoint_panel_shows_and_lists_paths_in_waypoint_mode() -> void:
	var ctx := _waypoint_ctx([
		{"index": 2, "flags": 0, "marker_count": 3},
		{"index": 5, "flags": NovaMissionData.WP_FLAG_DOES_NOT_LOOP, "marker_count": 0},
		{"index": 7, "flags": NovaMissionData.WP_FLAG_BLUE_TEAM, "marker_count": 2},
	], 2)
	assert_true(ctx.inspector._waypoints._wp_box.visible, "the waypoint panel shows in waypoint mode")
	assert_false(ctx.inspector._edit_box.visible, "the object edit panel is hidden in waypoint mode")
	assert_false(ctx.inspector._palette._place_box.visible, "the palette is hidden in waypoint mode")
	# Path 5 is empty and not the active path, so it is excluded; 2 (active) and 7 are listed.
	assert_eq(ctx.inspector._waypoints._wp_list.item_count, 2, "only populated (or the active) paths are listed")
	assert_eq(ctx.inspector._waypoints._wp_row_paths, [2, 7], "the listed path indices match")


func test_waypoint_panel_includes_the_active_path_even_when_empty() -> void:
	var ctx := _waypoint_ctx([
		{"index": 3, "flags": 0, "marker_count": 0},  # empty AND active -> shown
		{"index": 9, "flags": 0, "marker_count": 0},  # empty, not active -> hidden
	], 3)
	assert_eq(ctx.inspector._waypoints._wp_row_paths, [3], "the active path is listed even with no markers")


func test_waypoint_panel_row_selects_that_path() -> void:
	var ctx := _waypoint_ctx([
		{"index": 2, "flags": 0, "marker_count": 1},
		{"index": 7, "flags": 0, "marker_count": 1},
	], 2)
	ctx.inspector._waypoints._on_wp_path_selected(1)  # row 1 -> path 7
	assert_eq(ctx.fake.select_path_calls, [7], "selecting a path row focuses that path")


func test_waypoint_panel_reports_the_selected_marker() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	assert_string_contains(ctx.inspector._waypoints._wp_marker_label.text, "No marker", "with no marker, the panel says so")
	ctx.fake.selected_marker = {"path_index": 2, "marker_index": 9, "position": Vector3(1.0, 2.0, 3.0)}
	ctx.fake.changed.emit()
	assert_string_contains(ctx.inspector._waypoints._wp_marker_label.text, "#9", "a selected marker is reported by index")


func test_waypoint_panel_hidden_in_objects_mode() -> void:
	var ctx := _palette_ctx()  # waypoint_mode defaults to false
	assert_false(ctx.inspector._waypoints._wp_box.visible, "the waypoint panel is hidden in objects mode")
	assert_true(ctx.inspector._palette._place_box.visible, "and the object palette shows")


func test_flag_checkboxes_reflect_the_active_path() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {
		"index": 2, "marker_count": 1, "marker_indices": PackedInt32Array([5]),
		"flags": NovaMissionData.WP_FLAG_DOES_NOT_LOOP | NovaMissionData.WP_FLAG_BLUE_TEAM,
	}
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._waypoints._wp_loop_check.button_pressed, "DoesNotLoop set -> Loop unchecked")
	assert_true(ctx.inspector._waypoints._wp_blue_check.button_pressed, "Blue flag -> Blue checked")
	assert_false(ctx.inspector._waypoints._wp_red_check.button_pressed, "no Red flag -> Red unchecked")


func test_toggling_a_flag_commits_through_set_waypoint_flags() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	ctx.inspector._waypoints._wp_blue_check.button_pressed = true  # emits toggled (outside the sync guard)
	assert_eq(ctx.fake.set_flags_calls.size(), 1, "toggling a flag commits once")
	# [loop, blue, red]: loop stays on (DoesNotLoop clear), blue now on, red off.
	assert_eq(ctx.fake.set_flags_calls[0], [true, true, false], "the new flag set reaches the controller")


func test_marker_sublist_lists_active_markers_and_selects() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 2}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 2, "marker_indices": PackedInt32Array([5, 9])}
	ctx.fake.changed.emit()
	assert_eq(ctx.inspector._waypoints._wp_marker_list.item_count, 2, "the sub-list lists the active path's markers in order")
	assert_eq(ctx.inspector._waypoints._wp_marker_rows, [5, 9], "rows map to marker indices in route order")
	ctx.inspector._waypoints._on_wp_marker_row_selected(1)  # second marker -> index 9
	assert_eq(ctx.fake.select_marker_calls, [9], "selecting a marker row selects that marker")


func test_add_marker_button_arms_and_stops() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	assert_eq(ctx.inspector._waypoints._wp_add_button.text, "Add marker", "the button starts as Add marker")
	ctx.inspector._waypoints._wp_add_button.pressed.emit()
	assert_eq(ctx.fake.arm_marker_calls, 1, "pressing Add marker arms the tool")
	assert_eq(ctx.inspector._waypoints._wp_add_button.text, "Stop adding markers", "and the button flips to Stop")
	ctx.inspector._waypoints._wp_add_button.pressed.emit()
	assert_eq(ctx.fake.disarm_marker_calls, 1, "pressing again disarms the tool")


func test_reorder_and_delete_buttons_need_a_selected_marker() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 2}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 2, "marker_indices": PackedInt32Array([5, 9])}
	ctx.fake.changed.emit()
	assert_true(ctx.inspector._waypoints._wp_up_button.disabled, "reorder is disabled with no marker selected")
	assert_true(ctx.inspector._waypoints._wp_delete_button.disabled, "delete is disabled with no marker selected")
	assert_false(ctx.inspector._waypoints._wp_clear_button.disabled, "clear is enabled for a non-empty path")

	ctx.fake.selected_marker = {"path_index": 2, "marker_index": 9, "position": Vector3.ZERO}
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._waypoints._wp_up_button.disabled, "reorder enables once a marker is selected")
	ctx.inspector._waypoints._wp_up_button.pressed.emit()
	assert_eq(ctx.fake.move_marker_calls, [-1], "Move up moves the marker one step earlier")
	ctx.inspector._waypoints._wp_down_button.pressed.emit()
	assert_eq(ctx.fake.move_marker_calls, [-1, 1], "Move down moves it one step later")
	ctx.inspector._waypoints._wp_delete_button.pressed.emit()
	assert_eq(ctx.fake.delete_marker_calls, 1, "Delete marker removes it")


func test_clear_path_button_clears_the_active_path() -> void:
	var ctx := _waypoint_ctx([{"index": 2, "flags": 0, "marker_count": 1}], 2)
	ctx.fake.active_path = {"index": 2, "flags": 0, "marker_count": 1, "marker_indices": PackedInt32Array([5])}
	ctx.fake.changed.emit()
	ctx.inspector._waypoints._wp_clear_button.pressed.emit()
	assert_eq(ctx.fake.clear_path_calls, 1, "Clear path clears the active path")


func test_clear_path_disabled_for_an_empty_path() -> void:
	var ctx := _waypoint_ctx([{"index": 3, "flags": 0, "marker_count": 0}], 3)
	ctx.fake.active_path = {"index": 3, "flags": 0, "marker_count": 0, "marker_indices": PackedInt32Array()}
	ctx.fake.changed.emit()
	assert_true(ctx.inspector._waypoints._wp_clear_button.disabled, "clear is disabled when the path has no markers")


func test_new_path_button_is_available_when_the_list_is_empty() -> void:
	# Regression (review): an all-empty mission lists zero paths, so the user must still have a
	# way to start a route. The always-present New path button is that escape hatch.
	var ctx := _waypoint_ctx([{"index": 0, "flags": 0, "marker_count": 0}], -1)  # no active path, all empty
	assert_eq(ctx.inspector._waypoints._wp_list.item_count, 0, "precondition: the path list is empty (no active, all-empty)")
	assert_true(ctx.inspector._waypoints._wp_new_path_button.is_visible_in_tree(), "the New path button is still available")
	ctx.inspector._waypoints._wp_new_path_button.pressed.emit()
	assert_eq(ctx.fake.new_path_calls, 1, "New path focuses a fresh path through the controller")


func test_real_controller_provides_every_method_the_inspector_calls() -> void:
	# The tests above drive a FakeController; this asserts the REAL MissionController
	# exposes the same surface, so a method the inspector calls that the controller does
	# not implement (a runtime crash in the editor) cannot hide behind the fake.
	var controller := MissionController.new(null)
	var required := [
		"get_mission", "get_stats", "get_selection_summary", "get_selected_entity",
		"get_selected_display_name", "get_selected_graphic_name",
		"get_selected_position", "get_selected_rotation",
		"set_selected_position", "set_selected_rotation", "set_selected_team",
		"set_selected_group", "set_selected_property", "delete_selected", "is_dirty",
		"get_waypoint_path_options", "get_group_options",
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
		# Phase 3: weapon loadout + groups (mission-global).
		"get_weapon_loadout", "set_weapon_loadout",
		"get_group_count", "get_groups", "get_group", "set_group",
		# Phase 4: mission scripting (events / triggers / actions).
		"is_scripting_mode", "get_events", "get_selected_event_index", "get_selected_event_chain",
		"get_event_flag_bits", "get_ai_flag_bits", "get_trigger_main_types", "get_trigger_sub_types",
		"get_action_types", "get_action_sub_types", "select_event",
		"can_preview_part_anim", "preview_part_anim", "stop_preview",
		"add_event_default", "delete_selected_event", "set_selected_event",
		"add_selected_event_trigger", "set_selected_event_trigger",
		"remove_selected_event_trigger", "move_selected_event_trigger",
		"add_selected_event_action", "set_selected_event_action",
		"remove_selected_event_action", "move_selected_event_action",
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
	assert_false(ctx.inspector._properties._props_toggle.visible, "the props toggle hides without a mission")


func test_props_form_reads_header_values() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	assert_true(ctx.inspector._properties._props_toggle.visible, "the props toggle shows with a mission")
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


func test_props_form_shows_world_ref_widgets() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var terrain := ctx.inspector.find_child("MissionProp_terrain", true, false) as ResourceRefWidget
	var env := ctx.inspector.find_child("MissionProp_environment", true, false) as ResourceRefWidget
	assert_not_null(terrain, "the terrain link widget is built")
	assert_not_null(env, "the environment link widget is built")
	assert_eq(terrain.get_value(), ctx.fake.mission_ref.get_terrain_ref(),
		"the terrain widget reads the header ref")
	assert_eq(env.get_value(), ctx.fake.mission_ref.get_environment_ref(),
		"the environment widget reads the header ref")


func test_editing_terrain_ref_commits_through_set_header_string() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var terrain := ctx.inspector.find_child("MissionProp_terrain", true, false) as ResourceRefWidget
	terrain.name_edit.text = "newmap"
	terrain.name_edit.text_submitted.emit("newmap")
	assert_eq(ctx.fake.header_string_calls.size(), 1, "one header_string commit")
	assert_eq(ctx.fake.header_string_calls[0][0], "terrain", "field is terrain")
	assert_eq(ctx.fake.header_string_calls[0][1], "newmap", "value carried through")


func test_editing_environment_ref_commits_through_set_header_string() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var env := ctx.inspector.find_child("MissionProp_environment", true, false) as ResourceRefWidget
	env.name_edit.text = "storm_01"
	env.name_edit.text_submitted.emit("storm_01")
	assert_eq(ctx.fake.header_string_calls.size(), 1, "one header_string commit")
	assert_eq(ctx.fake.header_string_calls[0][0], "environment", "field is environment")
	assert_eq(ctx.fake.header_string_calls[0][1], "storm_01", "value carried through")


func test_reference_services_enable_browse_on_world_widgets() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var terrain := ctx.inspector.find_child("MissionProp_terrain", true, false) as ResourceRefWidget
	assert_false(terrain.browse_button.visible, "no services yet: browse hidden")
	# Services arrive after the form is built (the workspace injects them
	# post-build); the setter must re-configure the live widgets.
	ctx.inspector.set_reference_services({
		"resolve": func(_kind: String, _name: String) -> Dictionary:
			return {"status": "found", "path": "C:/res/x.trn"},
		"pick": func(_kind: String, _title: String, _on_pick: Callable) -> void:
			pass,
	})
	assert_true(terrain.browse_button.visible, "services injected: browse shows")
	assert_true(terrain.badge.visible, "services injected: badge resolves")
	assert_eq(terrain.badge.text, "●", "fake resolve reports found")


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


func test_start_with_nvg_is_a_distinct_mission_option() -> void:
	assert_eq(int(NovaMissionData.ATTRIB_START_WITH_NVG_ON), 0x400000,
		"StartWithNVGOn binds the retail mission attribute bit")
	assert_ne(int(NovaMissionData.ATTRIB_START_WITH_NVG_ON), int(NovaMissionData.ATTRIB_ENABLE_NVG),
		"starting enabled is distinct from the mission's NVG/night semantics")
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.fake.changed.emit()
	var check_name := "MissionFlag_%d" % int(NovaMissionData.ATTRIB_START_WITH_NVG_ON)
	var check := ctx.inspector.find_child(check_name, true, false) as CheckBox
	assert_not_null(check, "mission properties expose Start with night vision on")
	if check == null:
		return
	assert_eq(check.text, "Start with night vision on", "the authoring label is explicit")
	check.toggled.emit(true)
	assert_eq(ctx.fake.header_flag_calls.size(), 1, "one header-flag commit")
	assert_eq(int(ctx.fake.header_flag_calls[0][0]), 0x400000, "the checkbox writes StartWithNVGOn")
	assert_true(bool(ctx.fake.header_flag_calls[0][1]), "the checkbox writes the enabled state")


func test_selecting_game_mode_commits_through_set_game_mode() -> void:
	var ctx := _make({})
	ctx.fake.mission_ref = _loaded_mission_for_props()
	ctx.inspector._refresh()
	var option := ctx.inspector.find_child("MissionProp_game_mode", true, false) as OptionButton
	assert_not_null(option, "game mode dropdown built")
	# 12 entries: Single Player + the 11 engine modes (single-select, replacing the old checkboxes).
	assert_eq(option.item_count, 12, "dropdown lists Single Player + 11 modes")
	# Pick "Deathmatch" (index 2 in engine combobox order) and confirm it routes the bit, not a flag.
	option.selected = 2
	option.item_selected.emit(2)
	assert_eq(ctx.fake.game_mode_calls.size(), 1, "one set_game_mode commit")
	assert_eq(int(ctx.fake.game_mode_calls[0]), int(NovaMissionData.ATTRIB_DEATHMATCH), "bit is Deathmatch")
	assert_eq(ctx.fake.header_flag_calls.size(), 0, "game mode no longer routes through set_header_flag")


# --- Phase 2: area-trigger (zone) panel ---------------------------------------

func _trigger_ctx(zones: Array, selected: int) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 2  # AREA_TRIGGERS
	fake.zones = zones
	fake.selected_zone = selected
	# Populate a selection so the object edit panel WOULD be visible but for the mode gate; this
	# makes the "_edit_box hidden in trigger mode" assertion load-bearing (the real controller
	# clears the selection on set_mode, but the inspector's gate must not rely on that).
	fake.entity = _sample_entity()
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
	assert_true(ctx.inspector._zones._at_box.visible, "the trigger panel shows in trigger mode")
	assert_false(ctx.inspector._edit_box.visible, "the object edit panel is hidden in trigger mode")
	assert_false(ctx.inspector._palette._place_box.visible, "the palette is hidden in trigger mode")
	assert_eq(ctx.inspector._zones._at_list.item_count, 2, "both zones are listed")
	assert_eq(ctx.inspector._zones._at_rows, [0, 1], "the listed zone indices match")
	# The selected zone's bounds populate the spins.
	assert_eq(ctx.inspector._zones._at_min_spins[0].value, -5.0, "min X spin reflects the selected zone")
	assert_eq(ctx.inspector._zones._at_max_spins[1].value, 6.0, "max Y spin reflects the selected zone")
	assert_true(ctx.inspector._zones._at_active_check.button_pressed, "active toggle reflects the selected zone")


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
	ctx.inspector._zones._at_list.item_selected.emit(1)
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


func test_trigger_panel_editors_disabled_when_zones_exist_but_none_selected() -> void:
	# Zones present, but selection is -1: the bounds spins, flag toggles, and Delete must all be
	# disabled so the user cannot edit "nothing". (The selected-zone test pins the enabled side.)
	var ctx := _trigger_ctx([
		_zone(0, Vector3.ZERO, Vector3.ONE, true, false),
		_zone(1, Vector3.ZERO, Vector3.ONE, false, true),
	], -1)
	assert_eq(ctx.inspector._zones._at_list.item_count, 2, "both zones still listed with no selection")
	for axis in 3:
		assert_false(ctx.inspector._zones._at_min_spins[axis].editable, "min spin %d disabled with no selection" % axis)
		assert_false(ctx.inspector._zones._at_max_spins[axis].editable, "max spin %d disabled with no selection" % axis)
	assert_true(ctx.inspector._zones._at_active_check.disabled, "Active toggle disabled with no selection")
	assert_true(ctx.inspector._zones._at_constrain_check.disabled, "Constrain toggle disabled with no selection")
	assert_true(ctx.inspector._zones._at_delete_button.disabled, "Delete disabled with no selection")


# --- Phase 3: weapon loadout + groups panels ----------------------------------

func _loadout_ctx(loadout: Array) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 0
	fake.entity = _sample_entity()
	fake.loadout = loadout
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	# Expand the (collapsed-by-default) section so the list / editors populate.
	inspector._loadout_groups._loadout_toggle.button_pressed = true
	inspector._loadout_groups._loadout_toggle.toggled.emit(true)
	return {"fake": fake, "inspector": inspector}


func _groups_ctx(groups: Array) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 0
	fake.entity = _sample_entity()
	fake.groups = groups
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	inspector._loadout_groups._groups_toggle.button_pressed = true
	inspector._loadout_groups._groups_toggle.toggled.emit(true)
	return {"fake": fake, "inspector": inspector}


func _loadout_entry(name: String, ammo_pri: String, ammo_sec: String) -> Dictionary:
	return {"name": name, "ammo_primary": ammo_pri, "ammo_secondary": ammo_sec, "flags": "-1"}


func test_loadout_panel_lists_entries() -> void:
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1"), _loadout_entry("WPN_M9", "-1", "-1")])
	assert_true(ctx.inspector._loadout_groups._loadout_toggle.visible, "the loadout toggle shows when a mission is loaded")
	assert_eq(ctx.inspector._loadout_groups._loadout_list.item_count, 2, "both weapons listed")


func test_loadout_panel_hidden_without_a_mission() -> void:
	var fake := FakeController.new()
	fake.mission_ref = null
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	assert_false(inspector._loadout_groups._loadout_toggle.visible, "no loadout toggle without a mission")
	assert_false(inspector._loadout_groups._groups_toggle.visible, "no groups toggle without a mission")


func test_loadout_add_calls_controller() -> void:
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	var add := ctx.inspector.find_child("MissionLoadoutAdd", true, false) as Button
	assert_not_null(add, "Add weapon button built")
	add.pressed.emit()
	assert_eq(ctx.fake.set_loadout_calls.size(), 1, "add commits a new loadout")
	assert_eq((ctx.fake.set_loadout_calls.back() as Array).size(), 2, "the appended weapon grows the list")


func test_loadout_edit_commits_on_submit() -> void:
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	ctx.inspector._loadout_groups._loadout_list.item_selected.emit(0)
	ctx.inspector._loadout_groups._loadout_name.text = "WPN_FOO"
	ctx.inspector._loadout_groups._loadout_name.text_submitted.emit("WPN_FOO")
	var committed := ctx.fake.set_loadout_calls.back() as Array
	assert_eq(String((committed[0] as Dictionary)["name"]), "WPN_FOO", "the edited name is committed")


func test_loadout_ammo_and_damage_class_edits_commit() -> void:
	# One submit commits ALL four editors in a single set_weapon_loadout call, and each value
	# lands under its kit-tuple key — the write half of the stringly-typed panel seam (the read
	# half is pinned by the no-op guard + blank-name revert tests). Editors are addressed by
	# node name — the public seam — so this adds no private pokes (ADR 0018).
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	(ctx.inspector.find_child("MissionLoadoutList", true, false) as ItemList).item_selected.emit(0)
	var ammo_pri := ctx.inspector.find_child("MissionLoadoutAmmoPrimary", true, false) as LineEdit
	var ammo_sec := ctx.inspector.find_child("MissionLoadoutAmmoSecondary", true, false) as LineEdit
	var damage := ctx.inspector.find_child("MissionLoadoutDamageClass", true, false) as LineEdit
	ammo_pri.text = "5"
	ammo_sec.text = "2"
	damage.text = "1"
	damage.text_submitted.emit("1")
	var committed := ctx.fake.set_loadout_calls.back() as Array
	assert_eq(String((committed[0] as Dictionary)["ammo_primary"]), "5", "the edited primary-ammo request is committed")
	assert_eq(String((committed[0] as Dictionary)["ammo_secondary"]), "2", "the edited secondary-ammo request is committed")
	assert_eq(String((committed[0] as Dictionary)["flags"]), "1", "the damage-class editor commits as the flags field")


func test_loadout_unchanged_edit_does_not_commit() -> void:
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	ctx.inspector._loadout_groups._loadout_list.item_selected.emit(0)
	# Re-submit the same text: no commit (no spurious undo step).
	ctx.inspector._loadout_groups._loadout_name.text_submitted.emit("WPN_KNIFE")
	assert_eq(ctx.fake.set_loadout_calls.size(), 0, "an unchanged edit commits nothing")


func test_loadout_blank_name_is_rejected_and_reverts_all_fields() -> void:
	# Regression (review #3 / adversarial): blanking a weapon's name must reject the whole edit (an empty
	# name is the .bms chunk terminator and would drop the weapon), revert ALL four fields to the stored
	# entry (not just the name, so a simultaneous value edit can't be half-applied or left visually stale),
	# and never reach the controller (no commit, no crash from a private call).
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	ctx.inspector._loadout_groups._loadout_list.item_selected.emit(0)
	# Change a value AND blank the name, then submit.
	ctx.inspector._loadout_groups._loadout_ammo_pri.text = "5"
	ctx.inspector._loadout_groups._loadout_name.text = ""
	ctx.inspector._loadout_groups._loadout_name.text_submitted.emit("")
	assert_eq(ctx.fake.set_loadout_calls.size(), 0, "a blank name commits nothing")
	assert_eq(ctx.inspector._loadout_groups._loadout_name.text, "WPN_KNIFE", "name field reverts to the stored value")
	assert_eq(ctx.inspector._loadout_groups._loadout_ammo_pri.text, "-1", "primary-ammo field reverts too (no half-applied edit)")
	assert_eq(ctx.inspector._loadout_groups._loadout_ammo_sec.text, "-1", "secondary-ammo field stays consistent with the model")
	assert_eq((ctx.inspector.find_child("MissionLoadoutDamageClass", true, false) as LineEdit).text, "-1",
		"damage-class field stays consistent with the model")


func test_loadout_delete_calls_controller() -> void:
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1"), _loadout_entry("WPN_M9", "-1", "-1")])
	ctx.inspector._loadout_groups._loadout_list.item_selected.emit(1)
	var del := ctx.inspector.find_child("MissionLoadoutDelete", true, false) as Button
	del.pressed.emit()
	assert_eq(ctx.fake.set_loadout_calls.size(), 1, "delete commits the shorter loadout")
	assert_eq((ctx.fake.set_loadout_calls.back() as Array).size(), 1, "one weapon removed")


func test_groups_panel_lists_and_syncs_selection() -> void:
	var ctx := _groups_ctx([
		{"index": 0, "field0": 0, "field8": 0, "field12": 10},
		{"index": 1, "field0": 3, "field8": 22, "field12": 10},
	])
	assert_true(ctx.inspector._loadout_groups._groups_toggle.visible, "the groups toggle shows when a mission is loaded")
	assert_eq(ctx.inspector._loadout_groups._groups_list.item_count, 2, "both groups listed")
	ctx.inspector._loadout_groups._groups_list.item_selected.emit(1)
	assert_eq(ctx.inspector._loadout_groups._group_spins[0].value, 3.0, "flags spin syncs to the selected group")
	assert_eq(ctx.inspector._loadout_groups._group_spins[1].value, 22.0, "value spin syncs")
	assert_eq(ctx.inspector._loadout_groups._group_spins[2].value, 10.0, "constant spin syncs")
	assert_false(ctx.inspector._loadout_groups._group_spins[2].editable, "constant is read-only")


func test_group_spin_commits_to_controller() -> void:
	var ctx := _groups_ctx([{"index": 0, "field0": 0, "field8": 0, "field12": 10}])
	ctx.inspector._loadout_groups._groups_list.item_selected.emit(0)
	ctx.inspector._loadout_groups._group_spins[0].value = 3.0  # fires value_changed
	assert_eq(ctx.fake.set_group_calls.size(), 1, "changing a spin commits the group")
	var call := ctx.fake.set_group_calls.back() as Array
	assert_eq(int(call[0]), 0, "group index 0")
	assert_eq(int(call[1]), 3, "flags sent")
	assert_eq(int(call[2]), 0, "value sent (unchanged)")
	assert_eq(int(call[3]), 10, "constant sent")


func test_loadout_edit_survives_external_refresh_while_focused() -> void:
	# The critical guard: an external `changed` (e.g. an undo elsewhere) fires a full refresh; an
	# in-flight, uncommitted keystroke in a focused field must NOT be clobbered by the model sync.
	var ctx := _loadout_ctx([_loadout_entry("WPN_KNIFE", "-1", "-1")])
	ctx.inspector._loadout_groups._loadout_list.item_selected.emit(0)
	ctx.inspector._loadout_groups._loadout_name.grab_focus()
	if not ctx.inspector._loadout_groups._loadout_name.has_focus():
		pass_test("headless focus unavailable; focus guard is unit-covered by the spin variant")
		return
	ctx.inspector._loadout_groups._loadout_name.text = "WPN_TYPING"  # uncommitted
	ctx.fake.changed.emit()                          # external refresh
	assert_eq(ctx.inspector._loadout_groups._loadout_name.text, "WPN_TYPING", "focused in-flight edit survives an external refresh")


func test_group_spin_edit_survives_external_refresh_while_focused() -> void:
	var ctx := _groups_ctx([{"index": 0, "field0": 0, "field8": 5, "field12": 10}])
	ctx.inspector._loadout_groups._groups_list.item_selected.emit(0)  # value spin synced to 5
	var inner: LineEdit = ctx.inspector._loadout_groups._group_spins[1].get_line_edit()
	inner.grab_focus()
	if not inner.has_focus():
		pass_test("headless focus unavailable")
		return
	# The user is mid-typing in the spin's inner field (not yet applied), while an external change
	# (e.g. an undo) moves the model to a different value. The refresh must not clobber the typed text.
	inner.text = "70"
	ctx.fake.groups[0] = {"index": 0, "field0": 0, "field8": 9, "field12": 10}
	ctx.fake.changed.emit()
	assert_eq(inner.text, "70", "focused, in-flight spin text survives an external refresh")


# --- Phase 4: mission scripting (events / triggers / actions) tab --------------

func _sc_event(index: int, flags: int, reset_after: int, delay: int, trig_count: int, act_count: int) -> Dictionary:
	return {
		"index": index, "flags": flags, "trigger_index": 0, "action_index": 0,
		"trigger_count": trig_count, "action_count": act_count,
		"reset_after": reset_after, "delay": delay, "unknown5": 0, "unknown6": 0,
	}


func _sc_trig(main_type: int, main_name: String, sub_type: int, sub_name: String, params: Array, op: String = "and") -> Dictionary:
	return {
		"index": 0, "condition_flags": 0,
		"main_type": main_type, "main_type_name": main_name,
		"sub_type": sub_type, "sub_type_name": sub_name,
		"param1": params[0], "param2": params[1], "param3": params[2], "param4": params[3],
		"unknown7": 0, "negated": op == "not", "logic_or": op == "or", "logic_xor": op == "xor",
		"logic_operator": op,
	}


func _sc_act(action_type: int, type_name: String, sub_type: int, sub_name: String, params: Array) -> Dictionary:
	return {
		"index": 0, "action_type": action_type, "action_type_name": type_name,
		"action_sub_type": sub_type, "action_sub_type_name": sub_name,
		"param1": params[0], "param2": params[1], "param3": params[2], "param4": params[3],
		"reserved0": 0, "reserved1": 0,
	}


func _sc_chain_dict(event: Dictionary, triggers: Array, actions: Array, diagnostics: Array = []) -> Dictionary:
	return {"event": event, "triggers": triggers, "actions": actions, "references": [], "diagnostics": diagnostics}


func _scripting_ctx(events: Array, selected: int, chain: Dictionary) -> Dictionary:
	var fake := FakeController.new()
	fake.mission_ref = NovaMissionData.new()
	fake.mode = 3  # SCRIPTING
	fake.events = events
	fake.selected_event = selected
	fake.chain = chain
	# A live selection would show the object panel but for the mode gate (load-bearing for the hide test).
	fake.entity = _sample_entity()
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake)
	return {"fake": fake, "inspector": inspector}


func test_scripting_panel_shows_and_lists_events_in_scripting_mode() -> void:
	var event := _sc_event(0, 1, 5, 2, 1, 1)
	var chain := _sc_chain_dict(event,
		[_sc_trig(2, "Single", 10, "SingleIsWithinArea", [0, 3, 0, 0])],
		[_sc_act(34, "ResetEvent", 0, "Null", [0, 0, 0, 0])])
	var ctx := _scripting_ctx([event, _sc_event(1, 0, 0, 0, 0, 0)], 0, chain)
	assert_true(ctx.inspector._scripting._sc_box.visible, "the scripting panel shows in scripting mode")
	assert_false(ctx.inspector._edit_box.visible, "the object edit panel is hidden in scripting mode")
	assert_false(ctx.inspector._palette._place_box.visible, "the palette is hidden in scripting mode")
	assert_eq(ctx.inspector._scripting._sc_event_list.item_count, 2, "both events listed")
	assert_eq(ctx.inspector._scripting._sc_event_rows, [0, 1], "the listed event indices match")
	assert_eq(ctx.inspector._scripting._sc_trigger_list.item_count, 1, "the selected event's trigger is listed")
	assert_eq(ctx.inspector._scripting._sc_action_list.item_count, 1, "the selected event's action is listed")
	assert_eq(ctx.inspector._scripting._sc_reset_spin.value, 5.0, "reset_after spin reflects the event")
	assert_eq(ctx.inspector._scripting._sc_delay_spin.value, 2.0, "delay spin reflects the event")
	assert_true((ctx.inspector._scripting._sc_flag_checks[0]["check"] as CheckBox).button_pressed, "the Reset-after flag reflects the event")


func test_scripting_panel_hidden_in_objects_mode() -> void:
	var ctx := _scripting_ctx([], -1, {})
	ctx.fake.mode = 0
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._scripting._sc_box.visible, "the scripting panel hides outside scripting mode")


func test_scripting_add_event_calls_controller() -> void:
	var ctx := _scripting_ctx([], -1, {})
	var add := ctx.inspector.find_child("MissionScAddEvent", true, false) as Button
	assert_not_null(add, "Add event button built")
	add.pressed.emit()
	assert_eq(ctx.fake.add_event_calls, 1, "Add event calls the controller")


func test_scripting_delete_event_calls_controller() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 0)
	var ctx := _scripting_ctx([event], 0, _sc_chain_dict(event, [], []))
	var del := ctx.inspector.find_child("MissionScDeleteEvent", true, false) as Button
	del.pressed.emit()
	assert_eq(ctx.fake.delete_event_calls, 1, "Delete event calls the controller")


func test_scripting_event_row_selects_event() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 0)
	var ctx := _scripting_ctx([event, _sc_event(1, 0, 0, 0, 0, 0)], 0, _sc_chain_dict(event, [], []))
	ctx.inspector._scripting._sc_event_list.item_selected.emit(1)
	assert_eq(ctx.fake.select_event_calls, [1], "selecting a row focuses that event")


func test_scripting_event_flag_toggle_commits() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 0)
	var ctx := _scripting_ctx([event], 0, _sc_chain_dict(event, [], []))
	var flag := ctx.inspector._scripting._sc_flag_checks[0]["check"] as CheckBox  # Reset after (bit 1)
	flag.button_pressed = true  # fires toggled
	assert_eq(ctx.fake.set_event_calls.size(), 1, "toggling a flag commits the event")
	assert_eq(int((ctx.fake.set_event_calls[0] as Array)[0]), 1, "the Reset-after bit is set in the committed flags")


func test_scripting_reset_spin_commits() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 0)
	var ctx := _scripting_ctx([event], 0, _sc_chain_dict(event, [], []))
	ctx.inspector._scripting._sc_reset_spin.value = 12.0  # fires value_changed
	assert_eq(ctx.fake.set_event_calls.size(), 1, "changing reset_after commits the event")
	assert_eq(int((ctx.fake.set_event_calls[0] as Array)[1]), 12, "the new reset_after is sent")


func test_scripting_trigger_editor_typed_pickers() -> void:
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(2, "Single", 10, "SingleIsWithinArea", [7, 3, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	assert_eq(ctx.inspector._scripting._sc_trigger_main.get_selected_id(), 2, "the main-type dropdown shows Single")
	assert_eq(ctx.inspector._scripting._sc_trigger_sub.get_selected_id(), 10, "the sub-type dropdown shows SingleIsWithinArea")
	# SingleIsWithinArea: param1 = entity (unit) picker, param2 = zone picker. Both expose a dropdown and
	# round-trip the stored raw value even when nothing in the (empty) collections matches.
	assert_true(ctx.inspector._scripting._sc_trigger_params[0].is_picker(), "param1 renders as a typed picker (unit)")
	assert_eq(ctx.inspector._scripting._sc_trigger_params[0].read_value(), 7, "param1 round-trips the stored unit id")
	assert_true(ctx.inspector._scripting._sc_trigger_params[1].is_picker(), "param2 renders as a typed picker (zone)")
	assert_eq(ctx.inspector._scripting._sc_trigger_params[1].read_value(), 3, "param2 round-trips the stored zone index")
	assert_true(ctx.inspector._scripting._sc_trigger_desc.text.to_lower().find("zone") != -1, "the type description mentions the zone")


func test_scripting_subselection_resets_when_event_changes_without_a_click() -> void:
	# Regression: the trigger/action sub-selection is reset by the event-list click handler, but the
	# controller can also switch events without a click (set_mode auto-focus, add_event). A stale
	# sub-selection must be dropped on the next refresh so the editor never binds to another event's
	# trigger.
	var ev0 := _sc_event(0, 0, 0, 0, 3, 0)
	var ev1 := _sc_event(1, 0, 0, 0, 3, 0)
	var trigs := [
		_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [0, 0, 0, 0]),
		_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [0, 0, 0, 0]),
		_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [0, 0, 0, 0]),
	]
	var ctx := _scripting_ctx([ev0, ev1], 0, _sc_chain_dict(ev0, trigs, []))
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(2)  # user selects trigger row 2 of event 0
	assert_eq(ctx.inspector._scripting._sc_trigger_selected, 2, "precondition: trigger 2 of event 0 is selected")
	# The controller switches the selected event WITHOUT a click on the event list, then emits changed.
	ctx.fake.selected_event = 1
	ctx.fake.chain = _sc_chain_dict(ev1, trigs, [])
	ctx.inspector._refresh()
	assert_eq(ctx.inspector._scripting._sc_trigger_selected, -1,
		"the stale trigger sub-selection is dropped when the event changed without a click")


func test_scripting_trigger_type_change_commits_and_resets_sub() -> void:
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(2, "Single", 10, "SingleIsWithinArea", [0, 0, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	var main: OptionButton = ctx.inspector._scripting._sc_trigger_main
	main.select(0)  # Group (id 1) is the first main-type entry
	main.item_selected.emit(0)
	assert_eq(ctx.fake.set_trigger_calls.size(), 1, "changing the main type commits")
	var sent := (ctx.fake.set_trigger_calls[0] as Array)[1] as Dictionary
	assert_eq(int(sent["main_type"]), 1, "the new main type (Group) is committed")
	assert_eq(int(sent["sub_type"]), 0, "the sub-type resets to 0 on a main-type change")


func test_scripting_trigger_raw_param_commits() -> void:
	# GroupHasLostMoreUnits: param2 is a raw count (not a picker); editing its spinbox commits once.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [2, 0, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	assert_false(ctx.inspector._scripting._sc_trigger_params[1].is_picker(), "param2 is a raw count spinbox")
	ctx.inspector._scripting._sc_trigger_params[1].get_spin().value = 42.0  # fires value_changed -> committed
	assert_eq(ctx.fake.set_trigger_calls.size(), 1, "editing a param commits the trigger exactly once")
	var sent := (ctx.fake.set_trigger_calls.back() as Array)[1] as Dictionary
	assert_eq(int(sent["param2"]), 42, "the new param2 is committed")


func test_scripting_unmapped_type_param_roundtrips_raw() -> void:
	# A trigger type with no schema entry: every param stays a raw spinbox and round-trips untouched.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(1, "Group", 99, "Value 99", [11, 22, 33, 44])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	for i in 4:
		assert_false(ctx.inspector._scripting._sc_trigger_params[i].is_picker(), "unmapped param %d is raw" % i)
	assert_eq(ctx.inspector._scripting._sc_trigger_params[0].read_value(), 11, "raw param1 round-trips")
	assert_eq(ctx.inspector._scripting._sc_trigger_params[3].read_value(), 44, "raw param4 round-trips")
	ctx.inspector._scripting._sc_trigger_params[2].get_spin().value = 12345.0
	var sent := (ctx.fake.set_trigger_calls.back() as Array)[1] as Dictionary
	assert_eq(int(sent["param3"]), 12345, "an unmapped param commits its exact raw value")


func test_param_schema_marks_unused_slots() -> void:
	# Described, fixed count: KillGroup (2) uses 1 param -> slot 0 used, 1-3 unused.
	var kill := MissionParamSchema.action_slots(2)
	assert_true(kill.params[0].used, "KillGroup slot 1 is used")
	assert_false(kill.params[1].used, "KillGroup slot 2 is unused")
	assert_false(kill.params[3].used, "KillGroup slot 4 is unused")
	# Zero params: BlueWin (8) -> every slot unused.
	var blue := MissionParamSchema.action_slots(8)
	for i in 4:
		assert_false(blue.params[i].used, "BlueWin slot %d is unused" % (i + 1))
	# AI-change family is sub-type-aware: PLAYPARTANIM (34) uses all four (target + ANIMNUM / play / time)...
	var ai_anim := MissionParamSchema.action_slots(3, 34)
	for i in 4:
		assert_true(ai_anim.params[i].used, "ChangeGroupAI/PlayPartAnim slot %d is used" % (i + 1))
	# ...while a single-value sub-type (ACCURACY 8) uses only the target + one value.
	var ai_acc := MissionParamSchema.action_slots(3, 8)
	assert_true(ai_acc.params[0].used, "ChangeGroupAI target is used")
	assert_true(ai_acc.params[1].used, "ChangeGroupAI/Accuracy value is used")
	assert_false(ai_acc.params[2].used, "ChangeGroupAI/Accuracy slot 3 is unused")
	# AreaAiRed (12) is now modelled (was raw): a Zone target plus the sub-type's slots.
	var area := MissionParamSchema.action_slots(12, 34)
	assert_eq(area.params[0].kind, MissionParamSchema.Kind.ZONE, "AreaAiRed targets a zone")
	for i in 4:
		assert_true(area.params[i].used, "AreaAiRed/PlayPartAnim slot %d is used" % (i + 1))
	# Triggers likewise: GroupAtRedAlert (main 1 / sub 3) uses only param1.
	var trig := MissionParamSchema.trigger_slots(1, 3)
	assert_true(trig.params[0].used, "GroupAtRedAlert slot 1 is used")
	assert_false(trig.params[1].used, "GroupAtRedAlert slot 2 is unused")


func test_scripting_action_disables_unused_param_slots() -> void:
	# KillGroup uses one param (the group); the inspector greys the other three.
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var chain := _sc_chain_dict(event, [], [_sc_act(2, "KillGroup", 0, "Null", [5, 0, 0, 0])])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_true(ctx.inspector._scripting._sc_action_params[0].is_editable(), "the used param (group) stays editable")
	assert_false(ctx.inspector._scripting._sc_action_params[1].is_editable(), "unused param 2 is disabled")
	assert_false(ctx.inspector._scripting._sc_action_params[2].is_editable(), "unused param 3 is disabled")
	assert_false(ctx.inspector._scripting._sc_action_params[3].is_editable(), "unused param 4 is disabled")


func test_scripting_zero_param_action_disables_all_slots() -> void:
	# BlueWin takes no params -> all four rows greyed.
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var chain := _sc_chain_dict(event, [], [_sc_act(8, "BlueWin", 0, "Null", [0, 0, 0, 0])])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	for i in 4:
		assert_false(ctx.inspector._scripting._sc_action_params[i].is_editable(), "BlueWin param %d is disabled" % (i + 1))


func test_scripting_ai_action_enables_exactly_its_sub_type_slots() -> void:
	# PLAYPARTANIM uses all four (unit target + ANIMNUM / play type / time), so every row is editable.
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var anim_chain := _sc_chain_dict(event, [], [_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1, 2, 1, 65536])])
	var anim_ctx := _scripting_ctx([event], 0, anim_chain)
	anim_ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	for i in 4:
		assert_true(anim_ctx.inspector._scripting._sc_action_params[i].is_editable(), "PlayPartAnim param %d is editable" % (i + 1))
	# A single-value sub-type (ACCURACY) enables only the target + one value; the rest grey out.
	var acc_chain := _sc_chain_dict(event, [], [_sc_act(3, "ChangeGroupAI", 8, "Accuracy", [1, 90, 0, 0])])
	var acc_ctx := _scripting_ctx([event], 0, acc_chain)
	acc_ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_true(acc_ctx.inspector._scripting._sc_action_params[0].is_editable(), "Accuracy target stays editable")
	assert_true(acc_ctx.inspector._scripting._sc_action_params[1].is_editable(), "Accuracy value stays editable")
	assert_false(acc_ctx.inspector._scripting._sc_action_params[2].is_editable(), "Accuracy param 3 greys out")
	assert_false(acc_ctx.inspector._scripting._sc_action_params[3].is_editable(), "Accuracy param 4 greys out")
	# A genuinely unknown action type still degrades to four raw, editable slots so no param is ever blocked.
	var raw_chain := _sc_chain_dict(event, [], [_sc_act(999, "Action 999", 0, "Null", [1, 2, 3, 4])])
	var raw_ctx := _scripting_ctx([event], 0, raw_chain)
	raw_ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	for i in 4:
		assert_true(raw_ctx.inspector._scripting._sc_action_params[i].is_editable(), "unknown action param %d stays editable" % (i + 1))


# --- Phase 4: PLAYPARTANIM in-editor preview ----------------------------------

func test_scripting_preview_row_shows_only_for_playpartanim() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var ppa := _sc_chain_dict(event, [], [_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1001, 2, 1, 65536])])
	var ctx := _scripting_ctx([event], 0, ppa)
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_true(ctx.inspector._scripting._sc_action_preview_row.visible, "the Preview row shows for a PLAYPARTANIM action")
	# An AI-change action with a different sub-type hides it.
	var acc := _sc_chain_dict(event, [], [_sc_act(3, "ChangeGroupAI", 8, "Accuracy", [1, 90, 0, 0])])
	var ctx2 := _scripting_ctx([event], 0, acc)
	ctx2.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_false(ctx2.inspector._scripting._sc_action_preview_row.visible, "hidden for a non-PLAYPARTANIM AI sub-type")
	# A non-AI action hides it too.
	var other := _sc_chain_dict(event, [], [_sc_act(34, "ResetEvent", 0, "Null", [0, 0, 0, 0])])
	var ctx3 := _scripting_ctx([event], 0, other)
	ctx3.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_false(ctx3.inspector._scripting._sc_action_preview_row.visible, "hidden for a non-AI action")


func test_scripting_preview_button_enabled_state_follows_target() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var ppa := _sc_chain_dict(event, [], [_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1001, 2, 1, 65536])])
	# No resolvable target -> the button is disabled with a hint.
	var ctx := _scripting_ctx([event], 0, ppa)
	ctx.fake.can_preview = false
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_true(ctx.inspector._scripting._sc_action_preview.disabled, "Preview is disabled without an animated target")
	assert_string_contains(ctx.inspector._scripting._sc_action_preview.tooltip_text, "animated")
	# With a target -> enabled.
	var ctx2 := _scripting_ctx([event], 0, ppa)
	ctx2.fake.can_preview = true
	ctx2.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_false(ctx2.inspector._scripting._sc_action_preview.disabled, "Preview is enabled when a target resolves")


func test_scripting_preview_button_calls_controller_with_action() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	# channel 2, play, time raw 2*65536 (== 2.0s).
	var ppa := _sc_chain_dict(event, [], [_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1001, 2, 1, 131072])])
	var ctx := _scripting_ctx([event], 0, ppa)
	ctx.fake.can_preview = true
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	ctx.inspector._scripting._sc_action_preview.pressed.emit()
	assert_eq(ctx.fake.preview_calls.size(), 1, "Preview calls the controller once")
	var called: Dictionary = ctx.fake.preview_calls[0]
	assert_eq(int(called["param2"]), 2, "channel (param2) forwarded")
	assert_eq(int(called["param3"]), 1, "play type (param3) forwarded")
	assert_eq(int(called["param4"]), 131072, "time (param4, raw 16.16) forwarded")


func test_scripting_preview_stop_button_calls_controller() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var ppa := _sc_chain_dict(event, [], [_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1001, 2, 1, 65536])])
	var ctx := _scripting_ctx([event], 0, ppa)
	ctx.fake.can_preview = true
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	var before: int = ctx.fake.stop_preview_calls
	ctx.inspector._scripting._sc_action_preview_stop.pressed.emit()
	assert_eq(ctx.fake.stop_preview_calls, before + 1, "Stop calls the controller")


func test_scripting_switching_away_from_playpartanim_stops_preview() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 2)
	var chain := _sc_chain_dict(event, [], [
		_sc_act(21, "ChangeSingleAI", 34, "PlayPartAnim", [1001, 2, 1, 65536]),
		_sc_act(34, "ResetEvent", 0, "Null", [0, 0, 0, 0]),
	])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.fake.can_preview = true
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)  # PLAYPARTANIM -> row visible
	assert_true(ctx.inspector._scripting._sc_action_preview_row.visible)
	var before: int = ctx.fake.stop_preview_calls
	ctx.inspector._scripting._sc_action_list.item_selected.emit(1)  # ResetEvent -> row hidden, preview stopped
	assert_false(ctx.inspector._scripting._sc_action_preview_row.visible, "the row hides on a non-PLAYPARTANIM action")
	assert_gt(ctx.fake.stop_preview_calls, before, "leaving PLAYPARTANIM stops any running preview")


func test_scripting_trigger_disables_unused_param_slots() -> void:
	# GroupAtRedAlert uses only param1; the rest are greyed.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(1, "Group", 3, "GroupAtRedAlert", [5, 0, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	assert_true(ctx.inspector._scripting._sc_trigger_params[0].is_editable(), "the used param (group) stays editable")
	assert_false(ctx.inspector._scripting._sc_trigger_params[1].is_editable(), "unused trigger param 2 is disabled")
	assert_false(ctx.inspector._scripting._sc_trigger_params[3].is_editable(), "unused trigger param 4 is disabled")


func test_scripting_picker_out_of_range_shows_raw_row() -> void:
	# A zone ref past the end of the (empty) zone table is shown as its own raw row, not snapped to entry 0.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(2, "Single", 10, "SingleIsWithinArea", [0, 99, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	var opt: OptionButton = ctx.inspector._scripting._sc_trigger_params[1].get_option()
	assert_eq(opt.get_item_text(opt.selected), "Value 99", "out-of-range zone shows as a raw Value row")
	assert_eq(ctx.inspector._scripting._sc_trigger_params[1].read_value(), 99, "and read_value preserves the raw value")


func test_scripting_zone_picker_commits_selected_index() -> void:
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(2, "Single", 10, "SingleIsWithinArea", [0, 0, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.fake.zones = [{"index": 0, "id": 10}, {"index": 1, "id": 20}, {"index": 2, "id": 30}]
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)
	var opt: OptionButton = ctx.inspector._scripting._sc_trigger_params[1].get_option()
	# Select the zone whose id is 30 (array index 2) and fire the selection.
	for i in opt.item_count:
		if opt.get_item_id(i) == 2:
			opt.select(i)
			opt.item_selected.emit(i)
			break
	var sent := (ctx.fake.set_trigger_calls.back() as Array)[1] as Dictionary
	assert_eq(int(sent["param2"]), 2, "picking a zone commits its array index as param2")


func test_scripting_trigger_add_remove_move_call_controller() -> void:
	var event := _sc_event(0, 0, 0, 0, 2, 0)
	var chain := _sc_chain_dict(event, [
		_sc_trig(1, "Group", 1, "GroupSeesGroup", [0, 0, 0, 0]),
		_sc_trig(1, "Group", 1, "GroupSeesGroup", [0, 0, 0, 0]),
	], [])
	var ctx := _scripting_ctx([event], 0, chain)
	(ctx.inspector.find_child("MissionScTrigAdd", true, false) as Button).pressed.emit()
	assert_eq(ctx.fake.add_trigger_calls, 1, "Add trigger calls the controller")
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(1)
	(ctx.inspector.find_child("MissionScTrigRemove", true, false) as Button).pressed.emit()
	assert_eq(ctx.fake.remove_trigger_calls, [1], "Remove sends the selected local index")
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(1)
	(ctx.inspector.find_child("MissionScTrigUp", true, false) as Button).pressed.emit()
	assert_eq(ctx.fake.move_trigger_calls, [[1, -1]], "Up moves the selected trigger toward the front")


func test_scripting_action_editor_populates_and_commits() -> void:
	var event := _sc_event(0, 0, 0, 0, 0, 1)
	var chain := _sc_chain_dict(event, [], [_sc_act(34, "ResetEvent", 0, "Null", [4, 0, 0, 0])])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.inspector._scripting._sc_action_list.item_selected.emit(0)
	assert_eq(ctx.inspector._scripting._sc_action_type.get_selected_id(), 34, "the action-type dropdown shows ResetEvent")
	# ResetEvent param1 = event picker. Stored value 4 has no matching event (only event 0), so it shows raw.
	assert_true(ctx.inspector._scripting._sc_action_params[0].is_picker(), "ResetEvent param1 renders as an event picker")
	assert_eq(ctx.inspector._scripting._sc_action_params[0].read_value(), 4, "param1 round-trips the stored event index")
	assert_true(ctx.inspector._scripting._sc_action_desc.text.to_lower().find("event") != -1, "the action description mentions the event")
	# Pick the real event 0 and fire the selection.
	var opt: OptionButton = ctx.inspector._scripting._sc_action_params[0].get_option()
	for i in opt.item_count:
		if opt.get_item_id(i) == 0:
			opt.select(i)
			opt.item_selected.emit(i)
			break
	assert_eq(ctx.fake.set_action_calls.size(), 1, "picking an event commits the action exactly once")
	assert_eq(int(((ctx.fake.set_action_calls.back() as Array)[1] as Dictionary)["param1"]), 0, "the chosen event index is committed")


func test_scripting_diagnostics_render() -> void:
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event,
		[_sc_trig(2, "Single", 10, "SingleIsWithinArea", [0, 99, 0, 0])], [],
		[{"severity": "warning", "code": "logic.area_reference_out_of_range",
			"message": "Trigger references an area trigger index outside the mission area table.",
			"subject_kind": "trigger", "subject_index": 0}])
	var ctx := _scripting_ctx([event], 0, chain)
	assert_true(ctx.inspector._scripting._sc_diagnostics.text.find("area trigger index") != -1, "the diagnostic message renders")


func test_scripting_trigger_param_survives_external_refresh_while_focused() -> void:
	# The same focus guard as the other panels: a mid-typed RAW param spin must survive an external refresh.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var ctx := _scripting_ctx([event], 0, _sc_chain_dict(event, [_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [0, 5, 0, 0])], []))
	ctx.inspector._scripting._sc_trigger_list.item_selected.emit(0)  # param2 (raw count) synced to 5
	var inner: LineEdit = ctx.inspector._scripting._sc_trigger_params[1].get_spin().get_line_edit()
	inner.grab_focus()
	if not inner.has_focus():
		pass_test("headless focus unavailable")
		return
	inner.text = "70"
	# An external change moves the model under the user; the refresh must not clobber the typed text.
	ctx.fake.chain = _sc_chain_dict(event, [_sc_trig(1, "Group", 6, "GroupHasLostMoreUnits", [0, 9, 0, 0])], [])
	ctx.fake.changed.emit()
	assert_eq(inner.text, "70", "focused, in-flight trigger param survives an external refresh")


func test_scripting_event_summary_renders() -> void:
	var event := _sc_event(0, 0, 0, 0, 1, 1)
	var chain := _sc_chain_dict(event,
		[_sc_trig(1, "Group", 5, "GroupAlive", [3, 0, 0, 0])],
		[_sc_act(2, "KillGroup", 0, "Null", [4, 0, 0, 0])])
	var ctx := _scripting_ctx([event], 0, chain)
	var summary: String = ctx.inspector._scripting._sc_event_summary.text
	assert_true(summary.begins_with("When "), "the summary reads as a when/then sentence")
	assert_true(summary.find("Group 3 is alive") != -1, "the trigger phrase substitutes its param")
	assert_true(summary.find("Kill group 4") != -1, "the action phrase substitutes its param")


func test_scripting_ref_integrity_flags_bad_group() -> void:
	# A group ref past the group table is flagged by the editor-side ref-integrity pass.
	var event := _sc_event(0, 0, 0, 0, 1, 0)
	var chain := _sc_chain_dict(event, [_sc_trig(1, "Group", 5, "GroupAlive", [7, 0, 0, 0])], [])
	var ctx := _scripting_ctx([event], 0, chain)
	ctx.fake.groups = [{"index": 0}, {"index": 1}]  # only 2 groups, ref is 7
	ctx.fake.changed.emit()
	assert_true(ctx.inspector._scripting._sc_diagnostics.text.find("references group 7") != -1, "an out-of-range group ref is flagged")


# --- Two-mount split: browser left, editor in the right dock -------------------
# When the workspace forwards a dock mount, the per-selection editor + the Mission form mount in the
# dock while the mode tabs + lists stay in the inspector's own _root. With no mount (the one-arg
# setup the rest of this file uses) the whole tree stays under _root, so every find_child /
# is_visible_in_tree assertion above keeps working unchanged.

func _make_split(entity: Dictionary) -> Dictionary:
	var fake := FakeController.new()
	fake.entity = entity
	fake.mission_ref = NovaMissionData.new()  # a mission makes the palette + Mission form live
	var dock := PanelContainer.new()
	dock.custom_minimum_size = Vector2(280, 0)
	add_child_autofree(dock)
	var inspector = MissionInspector.new()
	add_child_autofree(inspector)
	inspector.setup(fake, dock)
	return {"fake": fake, "inspector": inspector, "dock": dock}


func test_split_editor_lands_in_dock_browser_stays_left() -> void:
	var ctx := _make_split(_sample_entity())  # Objects mode (default) with a selection
	# The entity editor mounts in the dock, not the left root.
	assert_not_null(ctx.dock.find_child("MissionPosX", true, false), "the entity editor is in the dock")
	assert_null(ctx.inspector._root.find_child("MissionPosX", true, false), "and not in the left root")
	# The mode tabs + the place palette stay in the left root.
	assert_not_null(ctx.inspector._root.find_child("MissionModeTabs", true, false), "the mode tabs stay left")
	assert_not_null(ctx.inspector._root.find_child("MissionScEvents", true, false), "the event list (a browser) stays left")
	# The Mission form (header) mounts in the dock too.
	assert_not_null(ctx.dock.find_child("MissionProp_mission_name", true, false), "the Mission header form is in the dock")


func test_split_null_mount_keeps_everything_under_root() -> void:
	# The regression guard for the one-arg path the rest of this file uses: with no dock, the editor
	# subtree falls back under _root, so find_child / is_visible_in_tree still reach it.
	var ctx := _make(_sample_entity())
	assert_not_null(ctx.inspector._root.find_child("MissionPosX", true, false), "no dock -> editor under _root")
	assert_true(ctx.inspector._edit_box.visible, "the edit panel shows with a selection")
	assert_true(ctx.inspector._delete_button.is_visible_in_tree(), "and the Delete button is on screen (Selection is the default tab)")


func test_split_mode_switch_reroutes_dock_content() -> void:
	var ctx := _make_split(_sample_entity())
	assert_true(ctx.inspector._edit_box.visible, "Objects mode shows the entity editor in the dock")
	assert_false(ctx.inspector._at_detail_box.visible, "and not the zone editor")
	var dock_box: Node = ctx.inspector._detail_root.get_parent()
	# Switch to Triggers (area-trigger) mode.
	ctx.fake.mode = 2
	ctx.fake.changed.emit()
	assert_false(ctx.inspector._edit_box.visible, "the entity editor hides outside Objects mode")
	assert_true(ctx.inspector._at_detail_box.visible, "the zone editor shows in Triggers mode")
	assert_eq(ctx.inspector._detail_root.get_parent(), dock_box, "the dock subtree is not reparented on a mode switch")


func test_split_edit_through_dock_widget_commits_via_controller() -> void:
	var ctx := _make_split(_sample_entity())
	var team := ctx.dock.find_child("MissionTeam", true, false) as OptionButton
	assert_not_null(team, "the Team picker lives in the dock")
	var evil_row := _option_row_for_id(team, 2)
	assert_ne(evil_row, -1, "the fixed Evil / red team row exists")
	team.select(evil_row)
	team.item_selected.emit(evil_row)
	assert_eq(ctx.fake.team_calls, 1, "an edit in the dock commits exactly once via the controller")
	assert_eq(ctx.fake.last_team, 2, "with the new value")


func test_split_set_detail_mount_null_evacuates_without_freeing() -> void:
	var ctx := _make_split(_sample_entity())
	var edit_box = ctx.inspector._edit_box
	assert_not_null(ctx.dock.find_child("MissionPosX", true, false), "editor starts in the dock")
	# Evacuate (the shell calls this on switch-away, before it frees the dock's children).
	ctx.inspector.set_detail_mount(null)
	assert_true(is_instance_valid(edit_box), "the editor is reparented, not freed")
	assert_not_null(ctx.inspector._root.find_child("MissionPosX", true, false), "and now lives under _root")
	assert_null(ctx.dock.find_child("MissionPosX", true, false), "no longer under the dock")
	# Re-mount (switch-back).
	ctx.inspector.set_detail_mount(ctx.dock)
	assert_not_null(ctx.dock.find_child("MissionPosX", true, false), "re-mounting puts it back in the dock")
	# A repeated set with the same mount is an idempotent no-op (the shell re-asserts the dock on
	# every editor-state sync).
	var parent_before: Node = ctx.inspector._detail_root.get_parent()
	ctx.inspector.set_detail_mount(ctx.dock)
	assert_eq(ctx.inspector._detail_root.get_parent(), parent_before, "same-mount re-mount does not thrash the subtree")


# --- B8: Mission-tab bulk re-ground button --------------------------------------

func test_reground_button_shows_with_a_mission_and_delegates() -> void:
	var ctx := _make({})
	var button := ctx.inspector.find_child("MissionRegroundAll", true, false) as Button
	assert_not_null(button, "the Re-ground button is built up front")
	assert_false(button.visible, "and hidden while no mission is loaded")

	ctx.fake.mission_ref = NovaMissionData.new()
	ctx.fake.changed.emit()
	assert_true(button.visible, "a loaded mission shows the button")
	assert_false(button.disabled, "authoring controls remain available in the editor")

	button.pressed.emit()
	assert_eq(ctx.fake.reground_calls, 1, "the press delegates to the controller once")
