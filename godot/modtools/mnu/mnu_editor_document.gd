class_name MnuEditorDocument
extends RefCounted

# Editor-side document over a NovaMnuDocument resource: open / save / dirty
# tracking (mirrors fonts/fnt_editor_document.gd). All format behavior lives in
# the engine wrapper; this only owns the open-file lifecycle.
#
# Signals: resource_loaded == a full load (open/new); resource_changed == an
# in-place mutation; state_changed == either of the above plus a save (the broad
# "something changed" cue). The editor binds resource_loaded + resource_changed;
# state_changed is kept for API symmetry and future consumers.

signal resource_loaded(resource)
signal resource_changed
signal state_changed

var resource: NovaMnuDocument
var current_path: String = ""
var is_dirty: bool = false

var _last_open_dir: String = ""
var _last_save_dir: String = ""


func _init() -> void:
	_set_new_resource()


func create_new() -> Error:
	_set_new_resource()
	set_current_path("")
	mark_clean()
	state_changed.emit()
	resource_loaded.emit(resource)
	return OK


func open_mnu(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return FileAccess.get_open_error()
	var loaded := NovaMnuDocument.new()
	var err := loaded.load_from_bytes(bytes)
	if err != OK:
		return err
	_replace_resource(loaded)
	set_current_path(path)
	remember_open_path(path)
	mark_clean()
	state_changed.emit()
	resource_loaded.emit(resource)
	return OK


func save_current() -> Error:
	if current_path.is_empty():
		return ERR_UNAVAILABLE
	return _save_to(current_path)


func save_as(dir_path: String) -> Error:
	var name := "menu"
	if not current_path.is_empty():
		name = current_path.get_file().get_basename()
	var target := dir_path.path_join("%s.mnu" % name)
	var err := _save_to(target)
	if err == OK:
		remember_save_dir(dir_path)
		set_current_path(target)
		state_changed.emit()
	return err


func _save_to(path: String) -> Error:
	if resource == null:
		return ERR_UNAVAILABLE
	var err := resource.save_to_path(path)
	if err == OK:
		mark_clean()
		state_changed.emit()
	return err


func _set_new_resource() -> void:
	var fresh := NovaMnuDocument.new()
	fresh.create_empty()
	_replace_resource(fresh)


func _replace_resource(value: NovaMnuDocument) -> void:
	if resource != null and resource.changed.is_connected(_on_resource_changed):
		resource.changed.disconnect(_on_resource_changed)
	resource = value
	if resource != null:
		resource.changed.connect(_on_resource_changed)


func _on_resource_changed() -> void:
	mark_dirty()
	resource_changed.emit()
	state_changed.emit()


func set_current_path(path: String) -> void:
	current_path = path


func mark_dirty() -> void:
	is_dirty = true


func mark_clean() -> void:
	is_dirty = false


func remember_open_path(path: String) -> void:
	_last_open_dir = path.get_base_dir()


func remember_save_dir(dir_path: String) -> void:
	_last_save_dir = dir_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_last_save_dir() -> String:
	return _last_save_dir
