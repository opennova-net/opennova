class_name DebugEntitiesPage
extends NovaDebugPage
## The rendered client entity list + a scalar detail card for the selection,
## plus the picked-entities section: the host-owned pick list rendered with
## per-row remove, and the one-click snapshot dump that embeds it. Discovery
## order follows the actual client presentation; host rows are joined to an
## authoritative AI index only when the selected entity supports edit actions.

const DebugEntities := preload("res://engine/debug/nova_debug_entities.gd")

var _entity_list: ItemList
var _entity_detail: Label
var _entity_rows: Array[Dictionary] = []
var _selected_entity := -1
var _selected_identity := ""
var _selected_ai_index := -1
var _selected_registry_present := false
var _edit_policy_reason := ""
var _health_editor: HBoxContainer
var _position_editor: VBoxContainer
var _health_value: SpinBox
var _position_values: Array[SpinBox] = []
var _set_health_button: Button
var _set_position_button: Button
var _edit_status: Label

var _picks_header: Label
var _picks_rows: VBoxContainer
var _picks_status: Label
var _picks_signature := ""
var _bound_pick_list: NovaDebugPickList = null
var _pending_pick: Dictionary = {}


func page_id() -> StringName:
	return &"Entities"


func page_category() -> StringName:
	return CATEGORY_SIM


## Overlay lifecycle seam: swapping the host-owned pick model must disconnect
## the old signals even while this page is hidden and not on its refresh tick.
func rebind_pick_list(pick_list: NovaDebugPickList) -> void:
	if _bound_pick_list != pick_list:
		_pending_pick.clear()
	_bind_pick_list(pick_list)
	_picks_signature = "__force_pick_row_rebuild__"
	_refresh_picks()


func _exit_tree() -> void:
	_bind_pick_list(null)


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_entity_list = ItemList.new()
	_entity_list.name = "EntityList"
	_entity_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_entity_list.item_selected.connect(_on_entity_selected)
	add_child(_entity_list)

	_entity_detail = Label.new()
	_entity_detail.name = "EntityDetail"
	_entity_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_entity_detail.text = "Select a unit to see its details."
	add_child(_entity_detail)

	_health_editor = HBoxContainer.new()
	_health_editor.name = "EntityEditHealth"
	add_child(_health_editor)
	var health_label := Label.new()
	health_label.text = "Health"
	_health_editor.add_child(health_label)
	_health_value = SpinBox.new()
	_health_value.name = "EntityHealthValue"
	_health_value.min_value = NovaDebugCatalog.ENTITY_HEALTH_MIN
	_health_value.max_value = NovaDebugCatalog.ENTITY_HEALTH_MAX
	_health_value.step = 1.0
	_health_value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_health_editor.add_child(_health_value)
	_set_health_button = Button.new()
	_set_health_button.name = "SetEntityHealth"
	_set_health_button.text = "Set"
	_set_health_button.pressed.connect(_on_set_health_pressed)
	_health_editor.add_child(_set_health_button)

	_position_editor = VBoxContainer.new()
	_position_editor.name = "EntityEditPosition"
	_position_editor.add_theme_constant_override("separation", 4)
	add_child(_position_editor)
	var position_header := Label.new()
	position_header.name = "EntityPositionHeader"
	position_header.text = "Position (mission coordinates)"
	_position_editor.add_child(position_header)
	var position_grid := GridContainer.new()
	position_grid.name = "EntityPositionValues"
	position_grid.columns = 3
	_position_editor.add_child(position_grid)
	for axis in ["X", "Y", "Z"]:
		var field := VBoxContainer.new()
		field.name = "EntityPosition%sField" % axis
		field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		field.add_theme_constant_override("separation", 2)
		position_grid.add_child(field)
		var field_label := Label.new()
		field_label.name = "EntityPosition%sLabel" % axis
		field_label.text = axis
		field.add_child(field_label)
		var value := SpinBox.new()
		value.name = "EntityPosition%s" % axis
		value.min_value = NovaDebugCatalog.MISSION_COORD_MIN
		value.max_value = NovaDebugCatalog.MISSION_COORD_MAX
		value.step = 0.1
		value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		value.tooltip_text = "%s mission coordinate" % axis
		field.add_child(value)
		_position_values.append(value)
	_set_position_button = Button.new()
	_set_position_button.name = "SetEntityPosition"
	_set_position_button.text = "Move"
	_set_position_button.pressed.connect(_on_set_position_pressed)
	_position_editor.add_child(_set_position_button)
	_set_edit_enabled(false)
	_set_editors_visible(false)
	_edit_status = Label.new()
	_edit_status.name = "EntityEditStatus"
	_edit_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_edit_status)

	_picks_header = Label.new()
	_picks_header.name = "PicksHeader"
	_picks_header.text = "Picked entities (0/%d)" % NovaDebugPickList.MAX_PICKS
	add_child(_picks_header)

	_picks_rows = VBoxContainer.new()
	_picks_rows.name = "PickRows"
	add_child(_picks_rows)

	var actions := HBoxContainer.new()
	actions.name = "PickActions"
	actions.add_theme_constant_override("separation", 4)
	add_child(actions)
	var clear_button := Button.new()
	clear_button.name = "ClearPicks"
	clear_button.text = "Clear picks"
	clear_button.focus_mode = Control.FOCUS_NONE
	clear_button.pressed.connect(_on_clear_picks_pressed)
	actions.add_child(clear_button)
	var dump_button := Button.new()
	dump_button.name = "DumpSnapshot"
	dump_button.text = "Dump snapshot"
	dump_button.tooltip_text = \
			"Write your position, view and every picked entity's live state as one JSON file."
	dump_button.focus_mode = Control.FOCUS_NONE
	dump_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	dump_button.pressed.connect(_on_dump_pressed)
	actions.add_child(dump_button)

	_picks_status = Label.new()
	_picks_status.name = "PicksStatus"
	_picks_status.text = "Aim and press the pick key, or click the world while this panel is open."
	_picks_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_picks_status)


func refresh() -> void:
	_bind_pick_list(_ctx.pick_list)
	_refresh_picks()
	var sim := _ctx.sim()
	if sim == null:
		_clear_live()
		return
	var retained_identity := _selected_identity
	_entity_rows = DebugEntities.list(sim)
	var count := _entity_rows.size()
	# Rebuild only on count change; steady-state refreshes update text in place.
	if _entity_list.item_count != count:
		_entity_list.clear()
		for i in range(count):
			_entity_list.add_item("")
	for i in range(count):
		var row: Dictionary = _entity_rows[i]
		var position: Vector3 = row.get("world_position", Vector3.ZERO)
		var flags := ""
		if not bool(row.get("alive", false)):
			flags += "  [down]"
		if bool(row.get("hidden", false)):
			flags += "  [hidden]"
		if not bool(row.get("registry_present", true)):
			flags += "  [despawned]"
		if not bool(row.get("presented", false)):
			flags += "  [not presented]"
		if not bool(row.get("editable", false)):
			flags += "  [read only]"
		var identity := "wire %04X" % int(row.get("wire_handle", 0))
		var net_id := int(row.get("net_id", 0))
		if net_id > 0:
			identity = "ssn %d" % net_id
		var ai_index := int(row.get("ai_index", -1))
		var source := "AI #%d" % ai_index if ai_index >= 0 else "view #%d" % i
		var display_name := String(row.get("name", ""))
		if not display_name.is_empty():
			source += "  %s" % display_name
		_entity_list.set_item_text(i, "%s  %s  (%.0f, %.0f)%s" % [
			source,
			identity,
			position.x,
			position.z,
			flags,
		])
	_selected_entity = _find_entity_identity(retained_identity)
	if _selected_entity >= 0:
		_entity_list.select(_selected_entity)
	elif not retained_identity.is_empty():
		_selected_identity = ""
		_selected_ai_index = -1
		_entity_list.deselect_all()
		_edit_status.text = "The selected entity is no longer available."
	_refresh_entity_detail()

	# Presentation can trail the click by a frame. A pending pick takes
	# precedence over retaining an unrelated old selection as soon as its row
	# arrives.
	if not _pending_pick.is_empty() and _select_pick(_pending_pick, false):
		_pending_pick.clear()

	# Picking can happen before the user opens this page. Make the newest card
	# the initial inspector selection instead of leaving a highlighted object
	# disconnected from an empty detail pane.
	if _selected_entity < 0 and _ctx.pick_list != null:
		var existing_picks := _ctx.pick_list.get_picks()
		if not existing_picks.is_empty():
			_select_pick(existing_picks.back(), false)


func _clear_live() -> void:
	if _entity_list.item_count > 0:
		_entity_list.clear()
	_entity_rows.clear()
	_selected_entity = -1
	_selected_identity = ""
	_selected_ai_index = -1
	_selected_registry_present = false
	_entity_detail.text = "Select a unit to see its details."
	_edit_status.text = ""
	_set_edit_enabled(false)
	_set_editors_visible(false)


func _refresh_entity_detail(force_editor_values := false) -> void:
	if _selected_entity < 0 or _selected_entity >= _entity_rows.size():
		_selected_ai_index = -1
		_selected_registry_present = false
		_entity_detail.text = "Select a unit to see its details."
		_set_edit_enabled(false)
		_set_editors_visible(false)
		return
	var row: Dictionary = _entity_rows[_selected_entity]
	var card: Dictionary = row.get("detail", {})
	if card.is_empty():
		_selected_ai_index = -1
		_selected_registry_present = false
		_entity_detail.text = "Select a unit to see its details."
		_set_edit_enabled(false)
		_set_editors_visible(false)
		return
	_selected_ai_index = int(row.get("ai_index", -1))
	_selected_registry_present = bool(row.get("registry_present", true))
	var name := String(card.get("name", ""))
	var pos: Vector3 = row.get("world_position", Vector3.ZERO)
	var lines := PackedStringArray()
	var identity := "wire %04X" % int(row.get("wire_handle", 0))
	if int(row.get("net_id", 0)) > 0:
		identity = "ssn %d" % int(row.get("net_id", 0))
	if _selected_ai_index >= 0:
		identity += "  AI #%d" % _selected_ai_index
	lines.append("%s  (%s)" % [
		name if not name.is_empty() else "unnamed",
		identity,
	])
	lines.append("client view: %s  edits: %s" % [
		"presented" if bool(row.get("presented", false)) else "not presented",
		"available" if bool(row.get("editable", false)) else "read only",
	])
	lines.append("state: %s (%d)  alert %d" % [
		String(card.get("state_name", "?")), int(card.get("state", 0)), int(card.get("alert", 0))])
	lines.append("health: %d (ai %d)  team %d" % [
		int(card.get("health", 0)), int(card.get("ai_health", 0)), int(card.get("team", 0))])
	lines.append("position: (%.1f, %.1f, %.1f)  facing %.0f°" % [
		pos.x, pos.y, pos.z, float(card.get("yaw_deg", 0.0))])
	lines.append("route %d  node %d  distance %d  speed %d" % [
		int(card.get("waypoint_id", 0)), int(card.get("wp_node", 0)),
		int(card.get("wp_distance", 0)), int(card.get("out_speed", 0))])
	if bool(card.get("mounted", false)):
		var seat_local: Vector3 = card.get("mount_seat_local", Vector3.ZERO)
		lines.append("mounted: target ssn %d  seat %d/%d  %s  type %d  bone %d" % [
			int(card.get("mount_target_net_id", 0)), int(card.get("mount_seat", -1)),
			int(card.get("mount_target_seat_count", 0)),
			String(card.get("mount_seat_source_name", "")),
			int(card.get("mount_type", 0)), int(card.get("mount_seat_bone", 0))])
		lines.append("seat local: (%.2f, %.2f, %.2f)  pose %d  yaw %+d  anim %s (%d)" % [
			seat_local.x, seat_local.y, seat_local.z,
			int(card.get("mount_seat_pose_index", 0)),
			int(card.get("mount_seat_yaw_offset", 0)),
			String(card.get("anim_key", "")), int(card.get("anim_state", -1))])
	var traits := PackedStringArray()
	if bool(card.get("infantry", false)):
		traits.append("on foot")
	if bool(card.get("hidden", false)):
		traits.append("hidden")
	if bool(card.get("held", false)):
		traits.append("held")
	if bool(card.get("disabled", false)):
		traits.append("disabled")
	if not traits.is_empty():
		lines.append(", ".join(traits))
	if card.has("bms_id") or card.has("item_id"):
		lines.append("BMS id %d  item %d  pool %d  handle %d" % [
			int(card.get("bms_id", 0)), int(card.get("item_id", 0)),
			int(card.get("pool", -1)), int(card.get("wire_handle", 0))])
	if card.has("sight_range_u"):
		lines.append("combat: sight %.0fu  attack %.0fu  ammo %d/%d" % [
			float(card.get("sight_range_u", 0.0)),
			float(card.get("attack_range_u", 0.0)),
			int(card.get("magazine", 0)), int(card.get("clip_size", 0))])
	if bool(card.get("combat_target_valid", false)):
		lines.append("combat target acquired")
	if bool(card.get("muzzle_valid", false)):
		var muzzle: Vector3 = card.get("muzzle", Vector3.ZERO)
		lines.append("muzzle: (%.1f, %.1f, %.1f)" % [
			muzzle.x, muzzle.y, muzzle.z])
	_entity_detail.text = "\n".join(lines)
	if force_editor_values or not _entity_editor_has_focus():
		_health_value.value = int(card.get("health", 0))
		# get_entity_debug.position is Godot world; the public edit seam accepts
		# mission coordinates (x, y, z) = (gx, -gz, gy).
		_position_values[0].value = pos.x
		_position_values[1].value = -pos.z
		_position_values[2].value = pos.y
	_set_editors_visible(true)
	_refresh_edit_state()


func _on_entity_selected(index: int) -> void:
	var next_identity := _entity_identity(_entity_rows[index]) \
			if index >= 0 and index < _entity_rows.size() else ""
	var identity_changed := next_identity != _selected_identity
	_selected_entity = index
	_selected_identity = next_identity
	_edit_policy_reason = ""
	_edit_status.text = ""
	_refresh_entity_detail(identity_changed)


func _select_pick(pick: Dictionary, report_missing: bool = true) -> bool:
	for index in range(_entity_rows.size()):
		if not _pick_matches_entity(pick, _entity_rows[index]):
			continue
		_entity_list.select(index)
		_entity_list.ensure_current_is_visible()
		_on_entity_selected(index)
		_picks_status.text = "Inspector selected: %s" % \
				PickDebugView.describe_pick(pick)
		return true
	if report_missing:
		_picks_status.text = (
				"Picked %s, but it has no live entity inspector row."
				% PickDebugView.describe_pick(pick))
	return false


static func _pick_matches_entity(
		pick: Dictionary,
		row: Dictionary) -> bool:
	var pick_net := int(pick.get("net_id", 0))
	var row_net := int(row.get("net_id", 0))
	if pick_net > 0 and row_net > 0:
		return pick_net == row_net

	var handle := int(pick.get("entity_handle", -1))
	if handle >= 0 and int(row.get("wire_handle", -2)) == handle:
		return true

	var pick_bms := int(pick.get("bms_id", 0))
	if pick_bms != 0 and int(row.get("bms_id", 0)) == pick_bms:
		return true

	return int(pick.get("kind", -1)) == int(row.get("kind", -2)) \
			and int(pick.get("index", -1)) == int(
					row.get("source_index", -2))


func _find_entity_identity(identity: String) -> int:
	if identity.is_empty():
		return -1
	for index in range(_entity_rows.size()):
		if _entity_identity(_entity_rows[index]) == identity:
			return index
	return -1


## The discovery index is transient presentation order. Keep selection keyed
## to a runtime identity so a reordered snapshot cannot retarget focused edit
## fields. Extra fields make SSN reuse fail closed instead of selecting a new
## incarnation that happens to occupy the same numeric slot.
static func _entity_identity(row: Dictionary) -> String:
	var net_id := int(row.get("net_id", 0))
	var type_id := int(row.get("type_id", 0))
	var bms_id := int(row.get("bms_id", 0))
	if net_id > 0:
		return "net:%d:type:%d:bms:%d" % [net_id, type_id, bms_id]
	var ai_index := int(row.get("ai_index", -1))
	var wire_handle := int(row.get("wire_handle", 0))
	if ai_index >= 0:
		return "ai:%d:wire:%d:type:%d" % [
			ai_index, wire_handle, type_id]
	return "wire:%d:type:%d:kind:%d:source:%d" % [
		wire_handle,
		type_id,
		int(row.get("kind", -1)),
		int(row.get("source_index", -1)),
	]


## Re-resolve immediately before a mutation as well as during refresh. The
## simulation cannot tick between this lookup and the synchronous engine call.
func _resolve_selected_ai_index() -> int:
	if _selected_identity.is_empty():
		return -1
	var sim := _ctx.sim()
	if sim == null:
		return -1
	for row in DebugEntities.list(sim):
		if _entity_identity(row) == _selected_identity \
				and bool(row.get("editable", false)):
			return int(row.get("ai_index", -1))
	return -1


func _refresh_edit_state() -> void:
	if _ctx.session == null or _selected_entity < 0:
		_set_edit_enabled(false)
		_set_edit_policy("")
		return
	if _selected_ai_index < 0:
		_set_edit_enabled(false)
		_set_editors_visible(false)
		var reason := "Read only: no authoritative AI edit target."
		_set_health_button.tooltip_text = reason
		_set_position_button.tooltip_text = reason
		_set_edit_policy(reason)
		return
	if not _selected_registry_present:
		_set_edit_enabled(false)
		_set_editors_visible(false)
		var reason := "Read only: this AI entry has despawned from the world."
		_set_health_button.tooltip_text = reason
		_set_position_button.tooltip_text = reason
		_set_edit_policy(reason)
		return
	_set_edit_policy("")
	var health := _ctx.session.get_control_state(&"set_entity_health")
	var position := _ctx.session.get_control_state(&"set_entity_position")
	_set_health_button.disabled = not health.available or not health.writable
	_set_position_button.disabled = not position.available or not position.writable
	_set_health_button.tooltip_text = health.reason
	_set_position_button.tooltip_text = position.reason
	_health_value.editable = not _set_health_button.disabled
	for value in _position_values:
		value.editable = not _set_position_button.disabled


func _set_edit_enabled(enabled: bool) -> void:
	_set_health_button.disabled = not enabled
	_set_position_button.disabled = not enabled
	_health_value.editable = enabled
	for value in _position_values:
		value.editable = enabled


func _set_editors_visible(visible: bool) -> void:
	_health_editor.visible = visible
	_position_editor.visible = visible


func _set_edit_policy(reason: String) -> void:
	if reason == _edit_policy_reason:
		if not reason.is_empty():
			_edit_status.text = reason
		return
	_edit_policy_reason = reason
	# A policy transition invalidates prior mutation feedback. Read-only state
	# takes precedence; becoming editable starts with a clean result line.
	_edit_status.text = reason


func _entity_editor_has_focus() -> bool:
	var viewport := get_viewport()
	if viewport == null:
		return false
	var focused := viewport.gui_get_focus_owner()
	if focused == null:
		return false
	return focused == _health_editor or _health_editor.is_ancestor_of(focused) \
			or focused == _position_editor \
			or _position_editor.is_ancestor_of(focused)


func _on_set_health_pressed() -> void:
	if _ctx.session == null:
		return
	var ai_index := _resolve_selected_ai_index()
	if ai_index < 0:
		_edit_status.text = "The selected entity is no longer editable."
		_set_edit_enabled(false)
		return
	var result := _ctx.session.invoke_control(&"set_entity_health",
			[ai_index, int(_health_value.value)])
	if int(result.get("error", ERR_UNAVAILABLE)) != OK:
		_edit_status.text = NovaDebugSession.invoke_error_message(
				result, "Could not set entity health.")
	else:
		_edit_status.text = "Health updated."
		if _ctx.request_refresh.is_valid():
			_ctx.request_refresh.call()


func _on_set_position_pressed() -> void:
	if _ctx.session == null:
		return
	var ai_index := _resolve_selected_ai_index()
	if ai_index < 0:
		_edit_status.text = "The selected entity is no longer editable."
		_set_edit_enabled(false)
		return
	var mission_position := Vector3(
			_position_values[0].value,
			_position_values[1].value,
			_position_values[2].value)
	var result := _ctx.session.invoke_control(&"set_entity_position",
			[ai_index, mission_position])
	if int(result.get("error", ERR_UNAVAILABLE)) != OK:
		_edit_status.text = NovaDebugSession.invoke_error_message(
				result, "Could not move entity.")
	else:
		_edit_status.text = "Position updated."
		if _ctx.request_refresh.is_valid():
			_ctx.request_refresh.call()


# --- The picked-entities section ---------------------------------------------

func _refresh_picks() -> void:
	var pick_list := _ctx.pick_list
	var picks: Array[Dictionary] = []
	if pick_list != null:
		picks = pick_list.get_picks()
	_picks_header.text = "Picked entities (%d/%d)" % [
		picks.size(), NovaDebugPickList.MAX_PICKS]
	# Rebuild rows only when the set changes; row text is cheap to recompute.
	var signature := ""
	for pick in picks:
		signature += "%d@%d|" % [int(pick.get("entity_handle", -1)),
				int(pick.get("tick", -1))]
	if signature == _picks_signature:
		return
	_picks_signature = signature
	for child in _picks_rows.get_children():
		_picks_rows.remove_child(child)
		child.queue_free()
	for i in range(picks.size()):
		var pick: Dictionary = picks[i]
		var row := HBoxContainer.new()
		row.name = "PickRow%d" % i
		var select_button := Button.new()
		select_button.name = "SelectPick%d" % i
		select_button.text = "%s   %.0fu" % [PickDebugView.describe_pick(pick),
				float(pick.get("distance_units", 0.0))]
		select_button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		select_button.clip_text = true
		select_button.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		select_button.tooltip_text = "Select this object in the entity inspector."
		select_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		select_button.pressed.connect(_on_select_pick_pressed.bind(i))
		row.add_child(select_button)
		var remove_button := Button.new()
		remove_button.name = "RemovePick"
		remove_button.text = "X"
		remove_button.tooltip_text = "Remove this entity from the pick list."
		remove_button.focus_mode = Control.FOCUS_NONE
		remove_button.pressed.connect(_on_remove_pick_pressed.bind(i))
		row.add_child(remove_button)
		_picks_rows.add_child(row)


func _bind_pick_list(pick_list: NovaDebugPickList) -> void:
	if _bound_pick_list == pick_list:
		return
	if _bound_pick_list != null:
		if _bound_pick_list.changed.is_connected(_on_pick_list_changed):
			_bound_pick_list.changed.disconnect(_on_pick_list_changed)
		if _bound_pick_list.picked.is_connected(_on_pick_added):
			_bound_pick_list.picked.disconnect(_on_pick_added)
	_bound_pick_list = pick_list
	if _bound_pick_list == null:
		return
	_bound_pick_list.changed.connect(_on_pick_list_changed)
	_bound_pick_list.picked.connect(_on_pick_added)


func _on_pick_list_changed() -> void:
	if not _pending_pick.is_empty():
		var pending_handle := int(_pending_pick.get("entity_handle", -1))
		var still_listed := false
		if _ctx.pick_list != null:
			for pick in _ctx.pick_list.get_picks():
				if int(pick.get("entity_handle", -2)) == pending_handle:
					still_listed = true
					break
		if not still_listed:
			_pending_pick.clear()
	_refresh_picks()


func _on_pick_added(pick: Dictionary) -> void:
	_pending_pick = pick.duplicate(true)
	_refresh_picks()
	if _select_pick(pick):
		_pending_pick.clear()


func _on_select_pick_pressed(index: int) -> void:
	if _ctx.pick_list == null:
		return
	var picks := _ctx.pick_list.get_picks()
	if index >= 0 and index < picks.size():
		_select_pick(picks[index])


func _on_remove_pick_pressed(index: int) -> void:
	if _ctx.pick_list != null:
		_ctx.pick_list.remove_at(index)
	_refresh_picks()


func _on_clear_picks_pressed() -> void:
	if _ctx.pick_list != null:
		_ctx.pick_list.clear()
	_refresh_picks()


func _on_dump_pressed() -> void:
	if _ctx.dump_snapshot.is_valid():
		_ctx.dump_snapshot.call("")  # the overlay pushes the result back below


## Every dump (button or programmatic) reports here so the status label always
## carries the newest outcome.
func show_dump_result(result: Dictionary) -> void:
	var path := String(result.get("path", ""))
	if path.is_empty():
		_picks_status.text = String(result.get("error", "Could not write the snapshot."))
	else:
		_picks_status.text = "Saved:\n%s" % path
