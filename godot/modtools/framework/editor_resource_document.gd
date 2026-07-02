class_name EditorResourceDocument
extends RefCounted

# Shared open/save/dirty lifecycle for the RefCounted resource documents (fonts
# .fnt, menus .mnu, credits .kda) that previously each carried a verbatim copy
# of this flow. A subclass supplies the blank-document factory, the file
# extension, and (when ResourceSaver is not the right writer) the save call;
# its typed open_* entry points parse, then hand the loaded resource to
# adopt_loaded(). Signal contract (names are load-bearing — editors connect by
# name): resource_loaded = a full load (open/new); resource_changed = an
# in-place mutation; state_changed = either of the above plus a save.

signal resource_loaded(resource)
signal resource_changed
signal state_changed

# Untyped on purpose (GDScript cannot re-type a base var per subclass); the
# typed entry points and consumers carry the concrete Resource type.
var resource
var current_path: String = ""
var is_dirty: bool = false

var _last_open_dir: String = ""
var _last_save_dir: String = ""


func _init() -> void:
	_replace_resource(_make_new_resource())


# --- Subclass hooks ---------------------------------------------------------

# The blank document create_new() adopts (e.g. NovaFntResource.create_blank).
func _make_new_resource():
	return null


# The save-as filename when no path exists yet ("font", "menu", "credits").
func _default_basename() -> String:
	return "document"


func _file_extension() -> String:
	return "res"


# The writer. ResourceSaver covers the registered formats (fnt/kda); a document
# whose resource writes itself (mnu's save_to_path) overrides this.
func _save_resource(path: String) -> Error:
	return ResourceSaver.save(resource, path)


# --- Lifecycle ----------------------------------------------------------------

func create_new() -> Error:
	_replace_resource(_make_new_resource())
	set_current_path("")
	mark_clean()
	state_changed.emit()
	resource_loaded.emit(resource)
	return OK


## Adopt a freshly parsed resource as the open document (the typed open_*
## entry points call this after their format-specific load succeeded).
func adopt_loaded(loaded, display_path: String) -> void:
	_replace_resource(loaded)
	set_current_path(display_path)
	remember_open_path(display_path)
	mark_clean()
	state_changed.emit()
	resource_loaded.emit(resource)


func save_current() -> Error:
	if current_path.is_empty():
		return ERR_UNAVAILABLE
	return _save_to(current_path)


func save_as(dir_path: String) -> Error:
	var name := _default_basename()
	if not current_path.is_empty():
		name = current_path.get_file().get_basename()
	var target := dir_path.path_join("%s.%s" % [name, _file_extension()])
	var err := _save_to(target)
	if err == OK:
		remember_save_dir(dir_path)
		set_current_path(target)
		state_changed.emit()
	return err


## Save to an exact path and adopt it as the document's current path. The
## workspace Save As flow passes a directory (save_as, filename derived);
## scripted flows — the MCP save tools — pass exact filenames. Mirrors the
## mission controller's save_as_path.
func save_as_path(path: String) -> Error:
	if path.is_empty() or path.get_extension().to_lower() != _file_extension():
		return ERR_INVALID_PARAMETER
	var mkdir := DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	if mkdir != OK:
		return mkdir
	var err := _save_to(path)
	if err == OK:
		remember_save_dir(path.get_base_dir())
		set_current_path(path)
		state_changed.emit()
	return err


func has_unsaved_changes() -> bool:
	return is_dirty


func _save_to(path: String) -> Error:
	if resource == null:
		return ERR_UNAVAILABLE
	var err := _save_resource(path)
	if err == OK:
		mark_clean()
		state_changed.emit()
	return err


func _replace_resource(value) -> void:
	if resource != null and resource.changed.is_connected(_on_resource_changed):
		resource.changed.disconnect(_on_resource_changed)
	resource = value
	if resource != null:
		resource.changed.connect(_on_resource_changed)


func _on_resource_changed() -> void:
	mark_dirty()
	resource_changed.emit()
	state_changed.emit()


# --- Path / dirty / remembered dirs --------------------------------------------

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
