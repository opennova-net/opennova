class_name ShellGameLaunch
extends RefCounted

const GameSession := preload("res://modtools/editor/shell/shell_game_session.gd")

## Top-bar adapter over the editor's one managed standalone game session. The
## three optional buttons mirror F5/F6/F8; keyboard and MCP callers use the
## same session. Every run mounts the exact current authoring directory with
## `/d`.
##
## ShellGameSession owns process lifecycle and is UI/MCP-neutral; this adapter
## owns only button presentation.
## All OS calls remain injected, so tests never need to spawn a process.

const TOOLTIP_READY := "Run game (F5) from the saved loose assets."
const TOOLTIP_RUNNING := "Game is running. F5 restarts it; F8 stops it."
const TOOLTIP_NEEDS_DIR := "Run game (F5): pick a resource directory first (Settings)."
const TOOLTIP_NO_RUNTIME := "Run game (F5): no game runtime is available beside this editor."
const TOOLTIP_MISSION_READY := "Run the current saved loose mission (F6)."
const TOOLTIP_MISSION_NEEDS_DIR := \
		"Run current mission (F6): pick a resource directory first (Settings)."
const TOOLTIP_MISSION_NO_RUNTIME := \
		"Run current mission (F6): no game runtime is available beside this editor."
const TOOLTIP_STOP_READY := "Stop the managed game (F8)."
const TOOLTIP_STOPPED := "No managed game is running."
const TOOLTIP_STOPPING := "The managed game is stopping."


var _button: Button
var _mission_button: Button
var _stop_button: Button
# func() -> String: the authoring/resource dir the editor has mounted ("" = none).
var _resource_dir: Callable
var _session := GameSession.new()


func setup(
	button: Button,
	resource_dir: Callable,
	expansion: Callable,
	game_code: Callable,
	spawn: Callable,
	file_exists: Callable,
	unsaved_workspaces: Callable,
	show_status: Callable,
	current_mission: Callable = Callable(),
	is_process_running: Callable = Callable(),
	kill_process: Callable = Callable(),
	now_msec: Callable = Callable(),
	mission_button: Button = null,
	stop_button: Button = null
) -> void:
	_button = button
	_mission_button = mission_button
	_stop_button = stop_button
	_resource_dir = resource_dir
	_session.setup(
		resource_dir,
		expansion,
		game_code,
		current_mission,
		unsaved_workspaces,
		spawn,
		file_exists,
		show_status,
		is_process_running,
		kill_process,
		now_msec)
	if _button != null:
		_button.icon = EditorIconLibrary.resolve(&"play_in_game")
		if not _button.pressed.is_connected(_on_pressed):
			_button.pressed.connect(_on_pressed)
	if _mission_button != null:
		_mission_button.icon = EditorIconLibrary.resolve(&"mission")
		if not _mission_button.pressed.is_connected(_on_mission_pressed):
			_mission_button.pressed.connect(_on_mission_pressed)
	if _stop_button != null:
		_stop_button.text = "■"
		if not _stop_button.pressed.is_connected(_on_stop_pressed):
			_stop_button.pressed.connect(_on_stop_pressed)
	refresh()


func available() -> bool:
	return _session.available()


## Re-gate the button (called by the shell whenever the resource root or the
## active workspace changes).
func refresh() -> void:
	var has_runtime := available()
	var has_dir := _resource_dir.is_valid() \
			and not String(_resource_dir.call()).is_empty()
	var state := String(_session.get_state().get("state", "stopped"))
	var stopping := state == "stopping"
	if _button != null:
		_button.disabled = not (has_runtime and has_dir) or stopping
		if stopping:
			_button.tooltip_text = TOOLTIP_STOPPING
		elif not has_runtime:
			_button.tooltip_text = TOOLTIP_NO_RUNTIME
		elif not has_dir:
			_button.tooltip_text = TOOLTIP_NEEDS_DIR
		elif _session.is_running():
			_button.tooltip_text = TOOLTIP_RUNNING
		else:
			_button.tooltip_text = TOOLTIP_READY
	if _mission_button != null:
		var mission_reason := ""
		if has_runtime and has_dir and not stopping:
			mission_reason = _session.get_current_mission_unavailable_reason()
		_mission_button.disabled = not (has_runtime and has_dir) \
				or stopping or not mission_reason.is_empty()
		if stopping:
			_mission_button.tooltip_text = TOOLTIP_STOPPING
		elif not has_runtime:
			_mission_button.tooltip_text = TOOLTIP_MISSION_NO_RUNTIME
		elif not has_dir:
			_mission_button.tooltip_text = TOOLTIP_MISSION_NEEDS_DIR
		elif not mission_reason.is_empty():
			_mission_button.tooltip_text = mission_reason
		else:
			_mission_button.tooltip_text = TOOLTIP_MISSION_READY
	if _stop_button != null:
		_stop_button.disabled = state != "running"
		_stop_button.tooltip_text = TOOLTIP_STOPPING \
				if stopping else TOOLTIP_STOP_READY \
				if state == "running" else TOOLTIP_STOPPED


func launch() -> bool:
	var managed_started := _session.start_mode("game")
	refresh()
	return managed_started


func run_current_mission() -> bool:
	var started := _session.start_mode("mission")
	refresh()
	return started


func stop() -> bool:
	var stopped := _session.stop()
	refresh()
	return stopped


func poll() -> void:
	var previous_state: String = String(_session.get_state().get("state", ""))
	_session.poll()
	if String(_session.get_state().get("state", "")) != previous_state:
		refresh()


func shutdown() -> bool:
	var stopped := _session.shutdown()
	refresh()
	return stopped


func get_session() -> ShellGameSession:
	return _session


func set_runtime_control_hooks(
	tool_forwarder: Callable,
	quit_requester: Callable = Callable()
) -> void:
	_session.set_runtime_control_hooks(tool_forwarder, quit_requester)


func _on_pressed() -> void:
	launch()


func _on_mission_pressed() -> void:
	run_current_mission()


func _on_stop_pressed() -> void:
	stop()
