class_name EnvironmentEditor
extends "res://modtools/editor/editor_document.gd"

# Editor-side document controller for EnvFile.
# Core ENV behavior stays in libs/env via EnvFile; this node owns authoring
# state, active path, and live-preview time.
# IDA equivalents are referenced in libs/env: Environment_LoadTimeOfDayConfig @ 0x57db30
# loads/sorts, TimeOfDay_ParseProperty @ 0x57c590 maps keywords, and
# Environment_ComputeTimeOfDayColors @ 0x57de40 evaluates TOD colors.

signal environment_changed(env_file: EnvFile, time_of_day: float)

const UNDO_LIMIT := 100

var env_file: EnvFile
var time_of_day: float = 1200.0

var _suspend_dirty: bool = false

# Undo/redo over byte snapshots of the EnvFile (EnvFile.to_bytes <-> load_bytes),
# mirroring the SoundController/StringsEditor pattern. A scalar editing burst is
# bracketed by begin_edit()/commit_edit() so one drag = one undo step.
var _undo_stack: Array[PackedByteArray] = []
var _redo_stack: Array[PackedByteArray] = []
var _pending_snapshot: PackedByteArray = PackedByteArray()
var _editing: bool = false


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
	_clear_history()
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
	_clear_history()
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
	_clear_history()
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
	time_of_day = fposmod(value, 2400.0)
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

## Open an editing burst: the next commit_edit() folds every change made in
## between into a single undo step. Pair with commit_edit(); idempotent.
func begin_edit() -> void:
	if not _editing and env_file:
		_pending_snapshot = env_file.to_bytes()
		_editing = true


func commit_edit() -> void:
	if not _editing or env_file == null:
		_editing = false
		return
	_editing = false
	var now := env_file.to_bytes()
	if now != _pending_snapshot:
		_undo_stack.append(_pending_snapshot)
		_trim_undo()
		_redo_stack.clear()
		_mark_dirty()
		state_changed.emit()


## Push a single undo step for a structural change (keyframe add/duplicate/
## remove) captured by the caller around the mutation.
func push_undo_step(mutation: Callable) -> void:
	if env_file == null:
		mutation.call()
		return
	var before := env_file.to_bytes()
	mutation.call()
	var after := env_file.to_bytes()
	if after != before:
		_undo_stack.append(before)
		_trim_undo()
		_redo_stack.clear()
		_mark_dirty()
		environment_changed.emit(env_file, time_of_day)
		state_changed.emit()


func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func undo() -> void:
	commit_edit()
	if _undo_stack.is_empty() or env_file == null:
		return
	_redo_stack.append(env_file.to_bytes())
	_suspend_dirty = true
	env_file.load_bytes(_undo_stack.pop_back())
	_suspend_dirty = false
	time_of_day = float(env_file.get_curtime())
	_mark_dirty()
	_emit_all_changed()


func redo() -> void:
	commit_edit()
	if _redo_stack.is_empty() or env_file == null:
		return
	_undo_stack.append(env_file.to_bytes())
	_suspend_dirty = true
	env_file.load_bytes(_redo_stack.pop_back())
	_suspend_dirty = false
	time_of_day = float(env_file.get_curtime())
	_mark_dirty()
	_emit_all_changed()


func _trim_undo() -> void:
	while _undo_stack.size() > UNDO_LIMIT:
		_undo_stack.pop_front()


func _clear_history() -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_pending_snapshot = PackedByteArray()
	_editing = false


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
