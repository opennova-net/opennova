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
	clear_history()
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
	clear_history()
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


# --- Snapshot edit history (opt-in) --------------------------------------------
# The EditorDocument tier, verbatim, over the shared SnapshotEditSession
# (framework/snapshot_edit_session.gd): a subclass opts in by overriding
# _snapshot()/_apply_snapshot() (e.g. text snapshots via to_text/from_text) and
# emits its domain signals from _history_applied(kind). Documents that don't
# opt in (fonts, mnu) leave _snapshot() null and every method stays inert.
# create_new()/adopt_loaded() drop the history with the old document.

var _edit_session: SnapshotEditSession = null


# The document snapshot, or null when no document is loaded / the subclass does
# not opt in (every history method is then inert).
func _snapshot() -> Variant:
	return null


func _apply_snapshot(_snap: Variant) -> void:
	pass


# Post-change notification: "commit" (a session recorded a step), "step" (a
# bracketed structural mutation recorded), "undo" / "redo" (the document was
# swapped). Subclasses emit their edited/structure_changed/state signals here.
func _history_applied(_kind: String) -> void:
	pass


func _history_limit() -> int:
	return 100


func _session() -> SnapshotEditSession:
	if _edit_session == null:
		_edit_session = SnapshotEditSession.new(self, _history_limit())
	return _edit_session


## Open an editing burst: the next commit_edit() folds every change in between
## into a single undo step. Idempotent while a session is open.
func begin_edit() -> void:
	_session().begin_edit()


## Close the burst; records one step iff the document actually changed
## (equal-gated in the core), marking dirty and notifying on a real change.
func commit_edit() -> void:
	_session().commit_edit()


func flush_edit() -> void:
	_session().flush_edit()


## Record the CURRENT document as one undo step before a structural mutation
## the caller applies itself (silent: the caller emits its own signals after
## mutating). Clears redo.
func record_undo_step() -> void:
	_session().record_undo_step()


## Bracket a single structural mutation as one equal-gated undo step, marking
## dirty and notifying ("step") only when it really changed the document.
func push_undo_step(mutation: Callable) -> void:
	_session().push_undo_step(mutation)


func can_undo() -> bool:
	return _edit_session != null and _edit_session.can_undo()


func can_redo() -> bool:
	return _edit_session != null and _edit_session.can_redo()


func undo() -> void:
	_session().undo()


func redo() -> void:
	_session().redo()


## Drop the whole history (open / new / save-as flows). The dirty flag is
## untouched; mark_clean() runs separately when the baseline moves.
func clear_history() -> void:
	if _edit_session != null:
		_edit_session.clear()
