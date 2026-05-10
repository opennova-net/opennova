class_name CreditsEditorDocument
extends "res://modtools/editor/editor_document.gd"

signal resource_loaded(resource)
signal resource_changed

var resource: CbinCreditsResource

func _init() -> void:
	_set_new_resource()

func create_new() -> Error:
	_set_new_resource()
	set_current_path("")
	mark_clean()
	state_changed.emit()
	resource_loaded.emit(resource)
	return OK

func open_kda(path: String) -> Error:
	var loaded := ResourceLoader.load(path, "CbinCreditsResource", ResourceLoader.CACHE_MODE_REPLACE) as CbinCreditsResource
	if loaded == null:
		return ERR_CANT_OPEN
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
	var name := "credits"
	if not current_path.is_empty():
		name = current_path.get_file().get_basename()
	var target := dir_path.path_join("%s.kda" % name)
	var err := _save_to(target)
	if err == OK:
		remember_save_dir(dir_path)
		set_current_path(target)
		state_changed.emit()
	return err

func _save_to(path: String) -> Error:
	var err := ResourceSaver.save(resource, path)
	if err == OK:
		mark_clean()
		state_changed.emit()
	return err

func _set_new_resource() -> void:
	var fresh := CbinCreditsResource.new()
	_replace_resource(fresh)

func _replace_resource(value: CbinCreditsResource) -> void:
	if resource and resource.changed.is_connected(_on_resource_changed):
		resource.changed.disconnect(_on_resource_changed)
	resource = value
	if resource:
		resource.changed.connect(_on_resource_changed)

func _on_resource_changed() -> void:
	mark_dirty()
	resource_changed.emit()
	state_changed.emit()
