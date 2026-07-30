class_name DebugPlayerPage
extends NovaDebugPage
## The authoritative local-player pose plus the one-click debug snapshot. The
## dump (DebugSnapshotWriter, orchestrated by the overlay through
## ctx.dump_snapshot) resamples the live runtime at click time — never the
## 0.25-Hz label cache — and writes pose + picked entities as one JSON file
## under the OpenNova user-data folder; the camera arrives through the host's
## NovaDebugViewContext so the file reproduces the visual viewpoint, not just
## the player root.

var _player_mission_label: Label
var _player_position_label: Label
var _player_orientation_label: Label
var _player_combat_label: Label
var _player_inventory_label: Label
var _player_inventory_toggle: Button
var _player_dump_button: Button
var _player_dump_status: Label
var _teleport_values: Array[SpinBox] = []
var _teleport_button: Button
var _teleport_status: Label
var _teleport_policy_reason := ""
var _player_context_key := ""


func page_id() -> StringName:
	return &"Player"


func page_category() -> StringName:
	return CATEGORY_PLAYER


func _build() -> void:
	add_theme_constant_override("separation", 8)

	add_child(_section_label("PlayerPoseHeading", "POSE"))
	_player_mission_label = _info_label("PlayerMission")
	_player_mission_label.text = "Mission: --"

	_player_position_label = _info_label("PlayerPosition")
	_player_position_label.text = "No local player."

	_player_orientation_label = _info_label("PlayerOrientation")
	_set_optional_text(_player_orientation_label, "")

	add_child(_section_label("PlayerCombatHeading", "COMBAT"))
	_player_combat_label = _info_label("PlayerCombat")
	_set_optional_text(_player_combat_label, "")

	add_child(_section_label("PlayerActionsHeading", "ACTIONS"))
	_player_dump_button = Button.new()
	_player_dump_button.name = "DumpSnapshot"
	_player_dump_button.text = "Dump snapshot"
	_player_dump_button.tooltip_text = \
			"Write your position, view and every picked entity's live state as one JSON file."
	_player_dump_button.disabled = true
	_player_dump_button.pressed.connect(_on_dump_pressed)
	add_child(_player_dump_button)

	_player_dump_status = Label.new()
	_player_dump_status.name = "PlayerDumpStatus"
	_player_dump_status.text = "Snapshots are written under the OpenNova user-data folder."
	_player_dump_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_player_dump_status)

	# The FP viewmodel debug experiments live with the player they act on.
	add_option_check(&"force_fp_arms")
	add_option_check(&"body_in_first_person")

	var edit_header := Label.new()
	edit_header.text = "Teleport (mission coordinates)"
	add_child(edit_header)
	var edit_grid := GridContainer.new()
	edit_grid.name = "PlayerTeleportValues"
	edit_grid.columns = 3
	add_child(edit_grid)
	for axis in ["X", "Y", "Z", "Yaw", "Pitch"]:
		var field := VBoxContainer.new()
		field.name = "Teleport%sField" % axis
		field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		field.add_theme_constant_override("separation", 2)
		edit_grid.add_child(field)
		var field_label := Label.new()
		field_label.name = "Teleport%sLabel" % axis
		field_label.text = axis
		field.add_child(field_label)
		var value := SpinBox.new()
		value.name = "Teleport%s" % axis
		if axis in ["X", "Y", "Z"]:
			value.min_value = NovaDebugCatalog.MISSION_COORD_MIN
			value.max_value = NovaDebugCatalog.MISSION_COORD_MAX
		elif axis == "Yaw":
			value.min_value = -360.0
			value.max_value = 360.0
		else:
			value.min_value = -90.0
			value.max_value = 90.0
		value.step = 0.1
		value.custom_arrow_step = 1.0
		value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		value.tooltip_text = axis
		field.add_child(value)
		_teleport_values.append(value)
	_teleport_button = Button.new()
	_teleport_button.name = "TeleportPlayer"
	_teleport_button.text = "Teleport"
	_teleport_button.pressed.connect(_on_teleport_pressed)
	add_child(_teleport_button)

	_teleport_status = Label.new()
	_teleport_status.name = "PlayerTeleportStatus"
	_teleport_status.text = "Enable Live edits to teleport the player."
	_teleport_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_teleport_status)

	_player_inventory_toggle = Button.new()
	_player_inventory_toggle.name = "PlayerInventoryToggle"
	_player_inventory_toggle.text = "Inventory"
	_player_inventory_toggle.toggle_mode = true
	_player_inventory_toggle.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_player_inventory_toggle.tooltip_text = \
			"Show the local player's weapon slots and ammo pools."
	_player_inventory_toggle.visible = false
	_player_inventory_toggle.toggled.connect(_on_inventory_toggled)
	add_child(_player_inventory_toggle)

	_player_inventory_label = _info_label("PlayerInventory")
	_player_inventory_label.visible = false


func refresh() -> void:
	var snapshot := DebugSnapshotWriter.capture(_ctx)
	if snapshot.is_empty():
		_clear_live()
		return
	_sync_player_context(snapshot)
	_apply_player_pose_to_ui(snapshot)
	_refresh_player_details()
	_refresh_teleport_state()
	_player_dump_button.disabled = false


func _clear_live() -> void:
	_player_context_key = ""
	_player_mission_label.text = "Mission: --"
	_player_position_label.text = "No local player."
	_set_optional_text(_player_orientation_label, "")
	_set_optional_text(_player_combat_label, "")
	_set_inventory_text("", "Inventory", "")
	_player_dump_button.disabled = true
	if _teleport_button != null:
		_teleport_button.disabled = true
	if _teleport_status != null:
		_teleport_policy_reason = "Start a playable mission to teleport the player."
		_teleport_status.text = _teleport_policy_reason
	_player_dump_status.text = "Start a playable mission to capture the local player."


func _info_label(node_name: String) -> Label:
	var label := Label.new()
	label.name = node_name
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(label)
	return label


func _section_label(node_name: String, text: String) -> Label:
	var label := Label.new()
	label.name = node_name
	label.text = text
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", Color(0.64, 0.68, 0.72))
	return label


func _set_optional_text(label: Label, text: String) -> void:
	label.text = text
	label.visible = not text.is_empty()


func _set_inventory_text(text: String, summary: String, tooltip: String) -> void:
	_player_inventory_label.text = text
	_player_inventory_label.tooltip_text = tooltip
	_player_inventory_toggle.text = summary
	_player_inventory_toggle.tooltip_text = \
			"Show or hide the local player's weapon slots and ammo pools." \
			if not text.is_empty() \
			else "Show the local player's weapon slots and ammo pools."
	_player_inventory_toggle.visible = not text.is_empty()
	_sync_inventory_visibility()


func _sync_inventory_visibility() -> void:
	_player_inventory_label.visible = _player_inventory_toggle.visible \
			and _player_inventory_toggle.button_pressed \
			and not _player_inventory_label.text.is_empty()


func _on_inventory_toggled(_pressed: bool) -> void:
	_sync_inventory_visibility()


## A mission swap clears the previous mission's "Saved:" path so the status
## never advertises another world's file.
func _sync_player_context(snapshot: Dictionary) -> void:
	var runtime := _ctx.runtime()
	if runtime == null:
		return
	var mission: Dictionary = snapshot.get("mission", {})
	var context_key := "%d|%s|%s" % [
		runtime.get_instance_id(),
		String(mission.get("file", "")),
		String(mission.get("name", "")),
	]
	if context_key == _player_context_key:
		return
	_player_context_key = context_key
	_teleport_policy_reason = ""
	_teleport_status.text = ""
	_player_dump_status.text = "No snapshot saved for this mission yet."


func _apply_player_pose_to_ui(snapshot: Dictionary) -> void:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_file := String(mission.get("file", ""))
	var mission_display := mission_file.get_file()
	if mission_display.is_empty():
		mission_display = String(mission.get("name", ""))
	if mission_display.is_empty():
		mission_display = "unknown"
	_player_mission_label.text = "Mission: %s" % mission_display

	var player: Dictionary = snapshot.get("player", {})
	var bms: Dictionary = player.get("position_bms", {})
	var godot: Dictionary = player.get("position_godot", {})
	var position_text := "BMS: x %.3f | y %.3f | z %.3f\n" + \
			"World: x %.3f | y %.3f | z %.3f"
	_player_position_label.text = position_text % [
				float(bms.get("x", 0.0)), float(bms.get("y", 0.0)),
				float(bms.get("z", 0.0)), float(godot.get("x", 0.0)),
				float(godot.get("y", 0.0)), float(godot.get("z", 0.0)),
			]

	var orientation: Dictionary = player.get("orientation_mission_deg", {})
	var orientation_text := \
			"Aim: yaw %.3f | pitch %.3f | roll %.3f" % [
				float(orientation.get("yaw", 0.0)),
				float(orientation.get("pitch", 0.0)),
				float(orientation.get("view_roll", 0.0)),
			]
	var view: Dictionary = snapshot.get("view", {})
	var camera: Dictionary = view.get("camera", {})
	var camera_mode := String(camera.get("mode", ""))
	if not camera_mode.is_empty() and camera_mode != "unknown":
		orientation_text += "\nCamera: %s" % camera_mode.replace("_", " ")
	_set_optional_text(_player_orientation_label, orientation_text)
	if not _teleport_editor_has_focus():
		_teleport_values[0].value = float(bms.get("x", 0.0))
		_teleport_values[1].value = float(bms.get("y", 0.0))
		_teleport_values[2].value = float(bms.get("z", 0.0))
		_teleport_values[3].value = float(orientation.get("yaw", 0.0))
		_teleport_values[4].value = float(orientation.get("pitch", 0.0))


func _refresh_player_details() -> void:
	var sim := _ctx.nova_simulation()
	if sim == null:
		_set_optional_text(_player_combat_label, "")
		_set_inventory_text("", "Inventory", "")
		return
	var combat := PackedStringArray()
	combat.append("Health: %d / %d | Team: %d | Class: %d" % [
		sim.get_local_player_health(),
		sim.get_local_player_max_health(),
		sim.get_local_player_team(),
		sim.get_local_player_class()])
	var weapon_name := sim.get_local_player_weapon_name()
	var weapon: Dictionary = sim.get_local_player_weapon_state()
	if not weapon_name.is_empty():
		combat.append("Weapon: %s | Clip: %d | Reserve: %d" % [
			weapon_name,
			int(weapon.get("clip", 0)),
			int(weapon.get("reserve", 0)),
		])
	if bool(weapon.get("active", false)):
		combat.append("State: phase %d | Animation: %s" % [
			int(weapon.get("phase", 0)), String(weapon.get("anim_key", "")),
		])
		combat.append("Events: shot %d | reload %d" % [
			int(weapon.get("fired_serial", 0)),
			int(weapon.get("reload_serial", 0))])
	_set_optional_text(_player_combat_label, "\n".join(combat))

	var inventory_lines := PackedStringArray()
	var tooltip_lines := PackedStringArray(["All loadout slots"])
	var inventory: Dictionary = sim.get_local_player_inventory()
	var slots: Array = inventory.get("slots", [])
	var equipped_combo := int(inventory.get("equipped_combo", -2))
	var hidden_slot_count := 0
	for slot_value in slots:
		var slot: Dictionary = slot_value
		var slot_name := String(slot.get("name", "unknown"))
		var rounds := int(slot.get("clip", 0))
		var equipped := int(slot.get("combo", -1)) == equipped_combo
		var slot_line := "%s: %s%s" % [
			slot_name,
			_rounds_text(rounds),
			"  [equipped]" if equipped else "",
		]
		tooltip_lines.append(slot_line)
		if rounds > 0 or equipped:
			inventory_lines.append(slot_line)
		else:
			hidden_slot_count += 1
	if hidden_slot_count > 0:
		inventory_lines.append("%d empty slot%s hidden." % [
			hidden_slot_count, "" if hidden_slot_count == 1 else "s"])
	var pools: Dictionary = inventory.get("pools", {})
	var populated_pool_count := 0
	if not pools.is_empty():
		var pool_names := PackedStringArray()
		for key in pools:
			pool_names.append(String(key))
		pool_names.sort()
		tooltip_lines.append("")
		tooltip_lines.append("All ammo pools")
		inventory_lines.append("")
		inventory_lines.append("Ammo pools")
		var all_pool_lines := PackedStringArray()
		for pool_name in pool_names:
			var count := int(pools[pool_name])
			all_pool_lines.append("%s: %d" % [pool_name, count])
			if count != 0:
				populated_pool_count += 1
				inventory_lines.append("  %s: %d" % [pool_name, count])
		tooltip_lines.append_array(all_pool_lines)
		var empty_pool_count := pool_names.size() - populated_pool_count
		if populated_pool_count == 0:
			inventory_lines.append("  All %d pools are empty." % empty_pool_count)
		elif empty_pool_count > 0:
			inventory_lines.append("  %d empty pool%s hidden." % [
				empty_pool_count, "" if empty_pool_count == 1 else "s"])
	if inventory_lines.is_empty():
		inventory_lines.append("Loadout entries: %d" % \
				sim.get_local_player_loadout().size())
	var summary := "Inventory (%d slot%s, %d active pool%s)" % [
		slots.size(),
		"" if slots.size() == 1 else "s",
		populated_pool_count,
		"" if populated_pool_count == 1 else "s",
	]
	_set_inventory_text(
			"\n".join(inventory_lines),
			summary,
			"\n".join(tooltip_lines))


static func _rounds_text(rounds: int) -> String:
	return "%d %s" % [rounds, "round" if rounds == 1 else "rounds"]


func _refresh_teleport_state() -> void:
	if _ctx.session == null:
		_teleport_button.disabled = true
		_teleport_status.text = "The debug session is unavailable."
		return
	var state := _ctx.session.get_control_state(&"teleport_local_player")
	_teleport_button.disabled = not state.available or not state.writable
	var reason := state.reason
	_teleport_button.tooltip_text = reason if not reason.is_empty() \
			else "Move the player to these mission-space coordinates."
	if reason != _teleport_policy_reason or _teleport_status.text.is_empty():
		_teleport_policy_reason = reason
		_teleport_status.text = reason if not reason.is_empty() \
				else "Ready. Coordinates follow the live player until you edit them."


func _teleport_editor_has_focus() -> bool:
	for value in _teleport_values:
		if value.get_line_edit().has_focus():
			return true
	return false


func _on_teleport_pressed() -> void:
	if _ctx.session == null:
		return
	var position := Vector3(
			_teleport_values[0].value,
			_teleport_values[1].value,
			_teleport_values[2].value)
	var result := _ctx.session.invoke_control(&"teleport_local_player", [
		position, _teleport_values[3].value, _teleport_values[4].value])
	if int(result.get("error", ERR_UNAVAILABLE)) != OK:
		_teleport_status.text = NovaDebugSession.invoke_error_message(
				result, "Teleport was unavailable.")
	else:
		if _ctx.request_refresh.is_valid():
			_ctx.request_refresh.call()
		_teleport_status.text = "Player moved."


func _on_dump_pressed() -> void:
	if _ctx.dump_snapshot.is_valid():
		_ctx.dump_snapshot.call("")  # the overlay pushes the result back below


## Every dump (button or programmatic) reports here so the status label always
## carries the newest outcome.
func show_dump_result(result: Dictionary) -> void:
	var path := String(result.get("path", ""))
	if path.is_empty():
		_player_dump_status.text = String(result.get("error", "Could not write the snapshot."))
	else:
		_player_dump_status.text = "Saved:\n%s" % path
