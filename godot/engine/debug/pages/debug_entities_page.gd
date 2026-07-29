class_name DebugEntitiesPage
extends NovaDebugPage
## The present-snapshot entity list + a scalar detail card for the selection,
## plus the picked-entities section: the host-owned pick list rendered with
## per-row remove, and the one-click snapshot dump that embeds it. The entity
## list reads ONE packed snapshot per refresh and only the selected entity
## pays for the detail card.

var _entity_list: ItemList
var _entity_detail: Label
var _selected_entity := -1

var _picks_header: Label
var _picks_rows: VBoxContainer
var _picks_status: Label
var _picks_signature := ""


func page_id() -> StringName:
	return &"Entities"


func page_category() -> StringName:
	return CATEGORY_SIM


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
	_refresh_picks()
	var sim := _ctx.sim()
	if sim == null:
		_clear_live()
		return
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var stride: int = sim.get_present_stride()
	var count := 0 if stride <= 0 else snap.size() / stride
	# Rebuild only on count change; steady-state refreshes update text in place.
	if _entity_list.item_count != count:
		_entity_list.clear()
		for i in range(count):
			_entity_list.add_item("")
		if _selected_entity >= count:
			_selected_entity = -1
	for i in range(count):
		var base := i * stride
		var flags := ""
		if snap[base + NovaSimulation.PF_ALIVE] == 0.0:
			flags += "  [down]"
		if snap[base + NovaSimulation.PF_HIDDEN] != 0.0:
			flags += "  [hidden]"
		_entity_list.set_item_text(i, "#%d  ssn %d  (%.0f, %.0f)%s" % [
			i,
			int(snap[base + NovaSimulation.PF_NET_ID]),
			snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Z],
			flags,
		])
	_refresh_entity_detail(sim)


func _clear_live() -> void:
	if _entity_list.item_count > 0:
		_entity_list.clear()
	_selected_entity = -1
	_entity_detail.text = "Select a unit to see its details."


func _refresh_entity_detail(sim: Object) -> void:
	if _selected_entity < 0:
		_entity_detail.text = "Select a unit to see its details."
		return
	var card: Dictionary = sim.get_entity_debug(_selected_entity)
	if card.is_empty():
		_entity_detail.text = "Select a unit to see its details."
		return
	var name := String(card.get("name", ""))
	var pos: Vector3 = card.get("position", Vector3.ZERO)
	var lines := PackedStringArray()
	lines.append("%s  (ssn %d)" % [name if not name.is_empty() else "unnamed", int(card.get("net_id", 0))])
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
	_entity_detail.text = "\n".join(lines)


func _on_entity_selected(index: int) -> void:
	_selected_entity = index
	var sim := _ctx.sim()
	if sim != null:
		_refresh_entity_detail(sim)


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
		var label := Label.new()
		label.name = "PickLabel"
		label.text = "%s   %.0fu" % [PickDebugView.describe_pick(pick),
				float(pick.get("distance_units", 0.0))]
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		row.add_child(label)
		var remove_button := Button.new()
		remove_button.name = "RemovePick"
		remove_button.text = "X"
		remove_button.tooltip_text = "Remove this entity from the pick list."
		remove_button.focus_mode = Control.FOCUS_NONE
		remove_button.pressed.connect(_on_remove_pick_pressed.bind(i))
		row.add_child(remove_button)
		_picks_rows.add_child(row)


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
