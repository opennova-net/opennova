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
var _player_dump_button: Button
var _player_dump_status: Label
var _player_context_key := ""


func page_id() -> StringName:
	return &"Player"


func page_category() -> StringName:
	return CATEGORY_PLAYER


func _build() -> void:
	add_theme_constant_override("separation", 8)

	_player_mission_label = _info_label("PlayerMission")
	_player_mission_label.text = "Mission: --"

	_player_position_label = _info_label("PlayerPosition")
	_player_position_label.text = "No local player."

	_player_orientation_label = _info_label("PlayerOrientation")
	_player_orientation_label.text = ""

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


func refresh() -> void:
	var snapshot := DebugSnapshotWriter.capture(_ctx)
	if snapshot.is_empty():
		_clear_live()
		return
	_sync_player_context(snapshot)
	_apply_player_pose_to_ui(snapshot)
	_player_dump_button.disabled = false


func _clear_live() -> void:
	_player_context_key = ""
	_player_mission_label.text = "Mission: --"
	_player_position_label.text = "No local player."
	_player_orientation_label.text = ""
	_player_dump_button.disabled = true
	_player_dump_status.text = "Start a playable mission to capture the local player."


func _info_label(node_name: String) -> Label:
	var label := Label.new()
	label.name = node_name
	add_child(label)
	return label


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
	_player_position_label.text = \
			"Position (BMS)\n  x %.3f   y %.3f   z %.3f\nPosition (Godot world)\n  x %.3f   y %.3f   z %.3f" % [
				float(bms.get("x", 0.0)), float(bms.get("y", 0.0)),
				float(bms.get("z", 0.0)), float(godot.get("x", 0.0)),
				float(godot.get("y", 0.0)), float(godot.get("z", 0.0)),
			]

	var orientation: Dictionary = player.get("orientation_mission_deg", {})
	_player_orientation_label.text = \
			"Orientation (mission degrees)\n  yaw %.3f   pitch %.3f\n  view roll %.3f" % [
				float(orientation.get("yaw", 0.0)),
				float(orientation.get("pitch", 0.0)),
				float(orientation.get("view_roll", 0.0)),
			]
	var view: Dictionary = snapshot.get("view", {})
	var camera: Dictionary = view.get("camera", {})
	var camera_mode := String(camera.get("mode", ""))
	if not camera_mode.is_empty() and camera_mode != "unknown":
		_player_orientation_label.text += "\nView camera: %s" % camera_mode.replace("_", " ")


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
