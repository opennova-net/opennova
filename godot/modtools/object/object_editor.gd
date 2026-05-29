class_name ObjectEditor
extends "res://modtools/editor/editor_document.gd"

signal object_changed(object_data: NovaObjectData)

var object_data: NovaObjectData


func _ready() -> void:
	if object_data == null:
		create_empty_object(false)


func create_empty_object(mark_dirty_state: bool = true) -> void:
	_set_object_data(NovaObjectData.new())
	object_data.reset_empty("untitled")
	set_current_path("")
	is_dirty = mark_dirty_state
	_emit_changed()


func open_object(path: String) -> Error:
	var next_data := NovaObjectData.new()
	var err := next_data.open_file(path)
	if err != OK:
		return err
	_set_object_data(next_data)
	set_current_path(path)
	remember_open_path(path)
	mark_clean()
	_emit_changed()
	return OK


func add_lod_scene(path: String, lod_index: int = -1) -> Error:
	if path.is_empty() or object_data == null:
		return ERR_INVALID_PARAMETER
	var err := object_data.set_lod_scene(lod_index, path)
	if err == OK:
		mark_dirty()
		_emit_changed()
	return err


func save_current() -> Error:
	if current_path.is_empty() or current_path.get_extension().to_lower() != "3dp":
		return ERR_INVALID_PARAMETER
	var err := object_data.save_project_to_dir(current_path.get_base_dir())
	if err == OK:
		mark_clean()
		state_changed.emit()
	return err


func save_as(dir_path: String) -> Error:
	if dir_path.is_empty() or object_data == null:
		return ERR_INVALID_PARAMETER
	var mkdir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir_err != OK:
		return mkdir_err
	var err := object_data.save_project_to_dir(dir_path)
	if err == OK:
		set_current_path(dir_path.path_join(_export_basename() + ".3dp"))
		remember_save_dir(dir_path)
		mark_clean()
		state_changed.emit()
	return err


func export_to_dir(dir_path: String, update_mask: int = 0) -> Error:
	if dir_path.is_empty() or object_data == null:
		return ERR_INVALID_PARAMETER
	var mkdir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir_err != OK:
		return mkdir_err
	var err := object_data.export_3di_to_dir(dir_path, update_mask)
	if err == OK:
		remember_export_dir(dir_path)
	return err


func get_project_title() -> String:
	var name := "untitled"
	if object_data != null and object_data.has_document():
		name = object_data.get_object_name()
	return "%s%s" % [name, "*" if is_dirty else ""]


func get_status_context() -> String:
	if object_data == null or not object_data.has_document():
		return "No object loaded"
	var summary := object_data.get_summary()
	return "%s  %d LODs  %d materials  %d lights" % [
		String(summary.get("source_kind", "object")).to_upper(),
		int(summary.get("lod_count", 0)),
		int(summary.get("material_count", 0)),
		int(summary.get("light_count", 0)),
	]


func _export_basename() -> String:
	var name := "untitled"
	if object_data != null:
		name = object_data.get_object_name().strip_edges()
	if name.is_empty():
		name = "untitled"
	var ext := name.get_extension().to_lower()
	if ext in ["3di", "3dp", "ase"]:
		name = name.get_basename()
	return name.replace(" ", "_").replace("/", "_").replace("\\", "_")


func _set_object_data(next_data: NovaObjectData) -> void:
	SignalRebind.rebind(object_data, next_data, &"object_changed", _on_object_data_changed, CONNECT_DEFERRED)
	object_data = next_data


func _on_object_data_changed() -> void:
	mark_dirty()
	_emit_changed()


func _emit_changed() -> void:
	object_changed.emit(object_data)
	state_changed.emit()
