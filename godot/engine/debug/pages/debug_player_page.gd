class_name DebugPlayerPage
extends VBoxContainer
## The authoritative local-player pose plus a one-click disk dump. The dump
## resamples the live runtime at click time (never the 0.25-Hz label cache) and
## writes one exact JSON snapshot under the OpenNova user-data folder; the
## camera arrives through the host's NovaDebugViewContext so the file
## reproduces the visual viewpoint, not just the player root.

## Fired after a fresh local-player pose snapshot lands on disk. The path is
## absolute so it can be pasted into an issue or opened directly.
signal local_player_pose_dumped(path: String)

const PLAYER_POSE_DUMP_DIR := "user://debug/player_locations"
const PLAYER_POSE_SCHEMA := "opennova.player_pose.v1"
const DEFAULT_PLAYER_FOV_H_DEG := 80.0
const MICROSECONDS_PER_SECOND := 1_000_000.0
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")

var _resolve_runtime := Callable()
var _resolve_sim := Callable()
var _view_context_source := Callable()

var _player_mission_label: Label
var _player_position_label: Label
var _player_orientation_label: Label
var _player_dump_button: Button
var _player_dump_status: Label
var _player_dump_sequence := 0
var _player_context_key := ""


func _init() -> void:
	name = "Player"
	add_theme_constant_override("separation", 8)

	_player_mission_label = _info_label("PlayerMission")
	_player_mission_label.text = "Mission: --"

	_player_position_label = _info_label("PlayerPosition")
	_player_position_label.text = "No local player."

	_player_orientation_label = _info_label("PlayerOrientation")
	_player_orientation_label.text = ""

	_player_dump_button = Button.new()
	_player_dump_button.name = "DumpPlayerPose"
	_player_dump_button.text = "Dump pose to disk"
	_player_dump_button.tooltip_text = \
			"Write a fresh local-player position/orientation snapshot as JSON."
	_player_dump_button.disabled = true
	_player_dump_button.pressed.connect(_on_dump_player_pose_pressed)
	add_child(_player_dump_button)

	_player_dump_status = Label.new()
	_player_dump_status.name = "PlayerDumpStatus"
	_player_dump_status.text = "Snapshots are written under the OpenNova user-data folder."
	_player_dump_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_player_dump_status)


## The dump re-resolves the runtime/sim at click time so it never serializes a
## stale reference across a mission reload.
func setup(resolve_runtime: Callable, resolve_sim: Callable) -> void:
	_resolve_runtime = resolve_runtime
	_resolve_sim = resolve_sim


## Optional supplier for the exact NovaDebugViewContext used to render and
## dispatch foliage. It is sampled with the player pose so a disk snapshot
## reproduces the visual viewpoint, not just the player root.
func set_view_context_source(source: Callable) -> void:
	_view_context_source = source


func refresh(runtime: Object, sim: Object) -> void:
	var snapshot := _capture_local_player_pose(runtime, sim)
	if snapshot.is_empty():
		clear_live()
		return
	_sync_player_context(runtime, snapshot)
	_apply_player_pose_to_ui(snapshot)
	_player_dump_button.disabled = false


func clear_live() -> void:
	_player_context_key = ""
	_player_mission_label.text = "Mission: --"
	_player_position_label.text = "No local player."
	_player_orientation_label.text = ""
	_player_dump_button.disabled = true
	_player_dump_status.text = "Start a playable mission to capture the local player."


## Sample the live runtime now and write one exact JSON pose snapshot. An
## optional target is useful for automation; the button uses the timestamped
## user-data location. Returns the absolute file path, or an empty string.
func dump_local_player_pose(path_override: String = "") -> String:
	var runtime: Object = _resolve_runtime.call() if _resolve_runtime.is_valid() else null
	var sim: Object = _resolve_sim.call() if _resolve_sim.is_valid() else null
	var snapshot := _capture_local_player_pose(runtime, sim)
	if snapshot.is_empty():
		clear_live()
		return ""

	_sync_player_context(runtime, snapshot)
	var target_path := path_override
	if target_path.is_empty():
		target_path = _default_player_pose_path(snapshot)
	target_path = ProjectSettings.globalize_path(target_path)
	var directory := target_path.get_base_dir()
	var mkdir_error := DirAccess.make_dir_recursive_absolute(directory)
	if mkdir_error != OK:
		_player_dump_status.text = "Could not create dump folder: %s" % error_string(mkdir_error)
		return ""
	var file := FileAccess.open(target_path, FileAccess.WRITE)
	if file == null:
		_player_dump_status.text = "Could not write pose dump: %s" % \
				error_string(FileAccess.get_open_error())
		return ""
	file.store_string(JSON.stringify(snapshot, "\t") + "\n")
	file.flush()
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		DirAccess.remove_absolute(target_path)
		_player_dump_status.text = "Could not finish pose dump: %s" % error_string(write_error)
		return ""

	_apply_player_pose_to_ui(snapshot)
	_player_dump_button.disabled = false
	_player_dump_status.text = "Saved:\n%s" % target_path
	local_player_pose_dumped.emit(target_path)
	return target_path


func _info_label(node_name: String) -> Label:
	var label := Label.new()
	label.name = node_name
	add_child(label)
	return label


func _sync_player_context(runtime: Object, snapshot: Dictionary) -> void:
	var mission: Dictionary = snapshot.get("mission", {})
	var context_key := "%d|%s|%s" % [
		runtime.get_instance_id(),
		String(mission.get("file", "")),
		String(mission.get("name", "")),
	]
	if context_key == _player_context_key:
		return
	_player_context_key = context_key
	_player_dump_status.text = "No pose snapshot saved for this mission yet."


func _apply_player_pose_to_ui(snapshot: Dictionary) -> void:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_file := String(mission.get("file", ""))
	var mission_name := String(mission.get("name", ""))
	var mission_display := mission_file.get_file()
	if mission_display.is_empty():
		mission_display = mission_name
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


func _capture_local_player_pose(runtime: Object, sim: Object) -> Dictionary:
	if runtime == null or sim == null or not sim.has_method("has_local_player") \
			or not bool(sim.has_local_player()):
		return {}
	if not sim.has_method("get_local_player_position") \
			or not sim.has_method("get_local_player_yaw_deg") \
			or not sim.has_method("get_local_player_pitch_deg"):
		return {}

	var position_godot: Vector3 = sim.get_local_player_position()
	var position_bms: Vector3 = MissionObjectPlacer.godot_to_bms_position(position_godot)
	var yaw_deg := float(sim.get_local_player_yaw_deg())
	var pitch_deg := float(sim.get_local_player_pitch_deg())
	var view: Dictionary = sim.get_local_player_view() \
			if sim.has_method("get_local_player_view") else {}
	var view_roll_deg := float(view.get("fp_roll_deg", 0.0))
	var yaw_rad := deg_to_rad(yaw_deg)
	var pitch_rad := deg_to_rad(pitch_deg)
	var forward_godot := Vector3(
			sin(yaw_rad) * cos(pitch_rad),
			sin(pitch_rad),
			-cos(yaw_rad) * cos(pitch_rad))
	var mission_file := String(runtime.get_mission_file()) \
			if runtime.has_method("get_mission_file") else ""
	var mission_name := String(runtime.get_mission_name()) \
			if runtime.has_method("get_mission_name") else ""

	return {
		"schema": PLAYER_POSE_SCHEMA,
		"captured_at_utc": Time.get_datetime_string_from_system(true, false) + "Z",
		"logic_tick": int(sim.get_logic_tick()) if sim.has_method("get_logic_tick") else -1,
		"mission": {
			"file": mission_file,
			"name": mission_name,
		},
		"player": {
			"position_bms": _vector3_record(position_bms),
			"position_godot": _vector3_record(position_godot),
			"orientation_mission_deg": {
				"yaw": yaw_deg,
				"pitch": pitch_deg,
				"view_roll": view_roll_deg,
			},
			"forward_godot": _vector3_record(forward_godot),
		},
		"view": {
			"fov_horizontal_deg": float(view.get(
					"fov_h_deg", DEFAULT_PLAYER_FOV_H_DEG)),
			"scope_engaged": bool(view.get("scope_engaged", false)),
			"mounted": bool(view.get("mounted", false)),
			"camera": _capture_camera_snapshot(),
		},
		"coordinate_conventions": {
			"bms": "Mission coordinates (x, y horizontal; z up)",
			"godot": "Global world coordinates (x, z horizontal; y up)",
			"yaw": "Mission yaw: 0 faces BMS +Y / Godot -Z",
			"pitch": "Positive looks up",
			"camera_rotation": "Godot world-space quaternion",
		},
	}


func _capture_camera_snapshot() -> Dictionary:
	if not _view_context_source.is_valid():
		return {}
	var context_value: Variant = _view_context_source.call()
	if not (context_value is DebugViewContext):
		return {}
	var context := context_value as DebugViewContext
	var camera := context.camera
	if camera == null or not is_instance_valid(camera):
		return {}
	var camera_transform := camera.global_transform
	var camera_basis := camera_transform.basis.orthonormalized()
	var camera_position := camera_transform.origin
	var viewport_size := Vector2.ZERO
	var viewport := camera.get_viewport()
	if viewport != null:
		viewport_size = viewport.get_visible_rect().size
	var viewport_aspect := viewport_size.x / viewport_size.y \
			if viewport_size.y > 0.0 else 0.0
	var mode := "unknown"
	if context.camera_mode_known:
		mode = "third_person" if context.third_person else "first_person"
	return {
		"mode": mode,
		"position_godot": _vector3_record(camera_position),
		"position_bms": _vector3_record(
				MissionObjectPlacer.godot_to_bms_position(camera_position)),
		"orientation_quaternion_godot": _quaternion_record(
				camera_basis.get_rotation_quaternion()),
		"right_godot": _vector3_record(camera_basis.x),
		"up_godot": _vector3_record(camera_basis.y),
		"forward_godot": _vector3_record(-camera_basis.z),
		"godot_fov_deg": camera.fov,
		"projection": int(camera.projection),
		"keep_aspect": int(camera.keep_aspect),
		"viewport_size": _vector2_record(viewport_size),
		"viewport_aspect": viewport_aspect,
		"near": camera.near,
		"far": camera.far,
	}


func _vector2_record(value: Vector2) -> Dictionary:
	return {"x": value.x, "y": value.y}


func _vector3_record(value: Vector3) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z}


func _quaternion_record(value: Quaternion) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z, "w": value.w}


func _default_player_pose_path(snapshot: Dictionary) -> String:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_stem := String(mission.get("file", "")).get_file().get_basename()
	if mission_stem.is_empty():
		mission_stem = String(mission.get("name", ""))
	if mission_stem.is_empty():
		mission_stem = "mission"
	mission_stem = mission_stem.validate_filename().replace(" ", "_")
	var stamp := String(snapshot.get("captured_at_utc", "")) \
			.replace("-", "").replace(":", "")
	var unix_usec := int(Time.get_unix_time_from_system() * MICROSECONDS_PER_SECOND)
	var target_path := ""
	while target_path.is_empty() or FileAccess.file_exists(target_path):
		_player_dump_sequence += 1
		target_path = PLAYER_POSE_DUMP_DIR.path_join("%s_%s_%d_%03d.json" % [
			mission_stem, stamp, unix_usec, _player_dump_sequence])
	return target_path


func _on_dump_player_pose_pressed() -> void:
	dump_local_player_pose()
