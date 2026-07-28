class_name EnvironmentEditor
extends "res://modtools/editor/editor_document.gd"

# Editor-side document controller for EnvFile.
# Core ENV behavior stays in libs/env via EnvFile; this node owns authoring
# state, active path, and live-preview time.
# IDA equivalents are referenced in libs/env: Environment_LoadTimeOfDayConfig @ 0x57db30
# loads/sorts, TimeOfDay_ParseProperty @ 0x57c590 maps keywords, and
# Environment_ComputeTimeOfDayColors @ 0x57de40 evaluates TOD colors.

signal environment_changed(env_file: EnvFile, time_of_day: float)


var env_file: EnvFile
var time_of_day: float = 1200.0

var _suspend_dirty: bool = false

# Undo/redo + the begin/commit session live on the shared EditorDocument
# snapshot history (NovaEditHistory over libs/oned_edit); the hooks below give
# it EnvFile's byte snapshots and this editor's signal fan-out.


func _ready() -> void:
	if env_file == null:
		create_default_environment(false)


func create_default_environment(mark_dirty: bool = true) -> void:
	_disconnect_env_file()
	env_file = EnvFile.new()
	env_file.reset_to_default()
	env_file.set_env_name("untitled")
	set_current_path("")
	time_of_day = float(env_file.get_curtime())
	is_dirty = mark_dirty
	clear_history()
	_connect_env_file()
	_emit_all_changed()


func open_env(path: String) -> Error:
	var next_file := EnvFile.new()
	next_file.set_source_path(path)
	var err := next_file.load()
	if err != OK:
		return err
	_disconnect_env_file()
	env_file = next_file
	set_current_path(path)
	remember_open_path(path)
	time_of_day = float(env_file.get_curtime())
	mark_clean()
	clear_history()
	_connect_env_file()
	_emit_all_changed()
	return OK


func open_env_from_resource_root(resources: NovaResourceRoot, name: String) -> Error:
	if resources == null:
		return ERR_INVALID_PARAMETER
	var next_file := EnvFile.new()
	var err := next_file.load_from_resource_root(resources, name)
	if err != OK:
		return err
	_disconnect_env_file()
	env_file = next_file
	set_current_path(name.get_file())
	remember_open_path(resources.get_root_dir().path_join(name.get_file()))
	time_of_day = float(env_file.get_curtime())
	mark_clean()
	clear_history()
	_connect_env_file()
	_emit_all_changed()
	return OK


func save_current() -> Error:
	if current_path.is_empty():
		return ERR_INVALID_PARAMETER
	return _save_to_path(current_path, true)


func save_as(dir_path: String) -> Error:
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	var filename := _export_filename()
	var path := dir_path.path_join(filename)
	var err := _save_to_path(path, true)
	if err == OK:
		set_current_path(path)
		remember_save_dir(dir_path)
	return err


func export_to_dir(dir_path: String) -> Error:
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	if env_file == null:
		return ERR_UNCONFIGURED
	var path := dir_path.path_join(_export_filename())
	var err := env_file.save_to_path(path)
	if err == OK:
		remember_export_dir(dir_path)
	return err


func _save_to_path(path: String, clear_dirty: bool) -> Error:
	if env_file == null:
		return ERR_UNCONFIGURED
	env_file.set_curtime(int(time_of_day))
	var err := env_file.save_to_path(path)
	if err != OK:
		return err
	if clear_dirty:
		mark_clean()
		state_changed.emit()
	return OK


func set_time_of_day(value: float, mark_dirty: bool = true) -> void:
	time_of_day = fposmod(value, NovaEnvironment.HHMM_DAY)
	if env_file:
		# Guard the inner EnvFile change so its environment_changed echo does not
		# fan out a second, redundant environment_changed/state_changed for this
		# step; we emit exactly once below. Dragging time-of-day otherwise rebuilt
		# the shell twice per step.
		_suspend_dirty = true
		env_file.set_curtime(int(time_of_day))
		_suspend_dirty = false
	if mark_dirty:
		_mark_dirty()
	environment_changed.emit(env_file, time_of_day)
	state_changed.emit()


# --- Editing sessions + undo/redo (byte snapshots of EnvFile) ---

func _snapshot() -> Variant:
	return env_file.to_bytes() if env_file != null else null


# Re-applying a snapshot must not re-dirty through EnvFile's changed signal
# (the base marks dirty itself), and the TOD slider follows the restored file.
func _apply_snapshot(snap: Variant) -> void:
	_suspend_dirty = true
	env_file.load_bytes(snap)
	_suspend_dirty = false
	time_of_day = float(env_file.get_curtime())


func _history_applied(kind: String) -> void:
	if kind == "commit":
		state_changed.emit()
	else: # "step" (bracketed keyframe add/remove), "undo", "redo"
		_emit_all_changed()


func get_project_title() -> String:
	var name := "untitled"
	if env_file:
		name = env_file.get_env_name().strip_edges()
		if name.is_empty():
			name = "untitled"
	return "%s%s" % [name, "*" if is_dirty else ""]


func get_status_context() -> String:
	if env_file == null:
		return "No environment loaded"
	var count := env_file.get_tod_keyframes().size()
	return "%04d  %d TOD keyframes" % [int(time_of_day), count]


func _connect_env_file() -> void:
	if env_file and not env_file.environment_changed.is_connected(_on_env_file_changed):
		env_file.environment_changed.connect(_on_env_file_changed)


func _disconnect_env_file() -> void:
	if env_file and env_file.environment_changed.is_connected(_on_env_file_changed):
		env_file.environment_changed.disconnect(_on_env_file_changed)


func _on_env_file_changed() -> void:
	if _suspend_dirty:
		return
	_mark_dirty()
	environment_changed.emit(env_file, time_of_day)
	state_changed.emit()


func _mark_dirty() -> void:
	mark_dirty()


func _emit_all_changed() -> void:
	environment_changed.emit(env_file, time_of_day)
	state_changed.emit()


func _export_filename() -> String:
	var name := ""
	if env_file:
		name = env_file.get_env_name().strip_edges()
	if name.is_empty() and not current_path.is_empty():
		name = current_path.get_file().get_basename()
	if name.is_empty():
		name = "untitled"
	name = name.replace(" ", "_").replace("/", "_").replace("\\", "_")
	if name.get_extension().to_lower() != "env":
		name += ".env"
	return name.to_lower()
