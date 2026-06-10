class_name EditorDocument
extends Node

# Shared document state for editor-side authoring controllers. No direct IDA
# equivalent; this is editor-only glue around ported data/model classes.

signal state_changed

var current_path: String = ""
var is_dirty: bool = false

var _last_open_dir: String = ""
var _last_save_dir: String = ""
var _last_export_dir: String = ""


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


func remember_export_dir(dir_path: String) -> void:
	_last_export_dir = dir_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_last_save_dir() -> String:
	return _last_save_dir


func get_last_export_dir() -> String:
	return _last_export_dir if not _last_export_dir.is_empty() else _last_save_dir


# --- Snapshot edit history (opt-in) --------------------------------------------
# Whole-document undo/redo on the engine's shared core (NovaEditHistory boxing
# libs/oned_edit): a subclass overrides _snapshot()/_apply_snapshot() (e.g. byte
# snapshots via to_bytes/load_bytes) and inherits the begin/commit session
# bracket (one editing burst = one equal-gated step), the silent pre-mutation
# step (record_undo_step), the bracketed structural step (push_undo_step), and
# undo/redo. _history_applied(kind) is where the subclass emits its domain
# signals. Dirty stays this class's coarse bool ON PURPOSE — undo/redo always
# mark_dirty, even back at the baseline; adopting the core's exact baseline
# compare is a tracked future behavior change, not part of this consolidation.

var _history: NovaEditHistory = null


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


func _ensure_history() -> NovaEditHistory:
	if _history == null:
		_history = NovaEditHistory.new()
		_history.set_limit(_history_limit())
	return _history


## Open an editing burst: the next commit_edit() folds every change in between
## into a single undo step. Idempotent while a session is open.
func begin_edit() -> void:
	var snap: Variant = _snapshot()
	if snap != null:
		_ensure_history().begin_edit(snap)


## Close the burst; records one step iff the document actually changed
## (equal-gated in the core), marking dirty and notifying on a real change.
func commit_edit() -> void:
	var snap: Variant = _snapshot()
	if snap == null:
		return
	if _ensure_history().commit_edit(snap):
		mark_dirty()
		_history_applied("commit")


func flush_edit() -> void:
	commit_edit()


## Record the CURRENT document as one undo step before a structural mutation
## the caller applies itself (silent: the caller emits its own signals after
## mutating). Clears redo.
func record_undo_step() -> void:
	var snap: Variant = _snapshot()
	if snap != null:
		_ensure_history().push_step(snap)


## Bracket a single structural mutation as one equal-gated undo step, marking
## dirty and notifying ("step") only when it really changed the document.
func push_undo_step(mutation: Callable) -> void:
	var before: Variant = _snapshot()
	if before == null:
		mutation.call()
		return
	flush_edit()
	_ensure_history().begin_edit(before)
	mutation.call()
	var after: Variant = _snapshot()
	if _ensure_history().commit_edit(after):
		mark_dirty()
		_history_applied("step")


func can_undo() -> bool:
	return _history != null and _history.can_undo()


func can_redo() -> bool:
	return _history != null and _history.can_redo()


func undo() -> void:
	flush_edit()
	if _history == null:
		return
	var live: Variant = _snapshot()
	if live == null:
		return
	var snap: Variant = _history.undo_swap(live)
	if snap == null:
		return
	_apply_snapshot(snap)
	mark_dirty()
	_history_applied("undo")


func redo() -> void:
	flush_edit()
	if _history == null:
		return
	var live: Variant = _snapshot()
	if live == null:
		return
	var snap: Variant = _history.redo_swap(live)
	if snap == null:
		return
	_apply_snapshot(snap)
	mark_dirty()
	_history_applied("redo")


## Drop the whole history (open / new / save-as flows). The dirty flag is
## untouched; call mark_clean() separately when the baseline moves.
func clear_history() -> void:
	if _history != null:
		_history.clear()
