class_name SnapshotEditSession
extends RefCounted

## Whole-document undo/redo over the engine's shared core (NovaEditHistory
## boxing libs/oned_edit), shared by the Node documents (EditorDocument) and
## the RefCounted resource documents (EditorResourceDocument). The host
## provides `_snapshot()` (null = not opted in / nothing loaded; every method
## is then inert), `_apply_snapshot(snap)`, `_history_applied(kind)` and
## `mark_dirty()`; the session owns the begin/commit bracket (one editing
## burst = one equal-gated step), the silent pre-mutation step
## (record_undo_step), the bracketed structural step (push_undo_step), and
## undo/redo. The host is held through a weakref: resource documents are
## RefCounted, and a bound-method Callable back at the host would
## refcount-cycle document and session together.

var _host_ref: WeakRef
var _limit: int = 100
var _history: NovaEditHistory = null


func _init(host: Object, limit: int = 100) -> void:
	_host_ref = weakref(host)
	_limit = limit


func _host() -> Object:
	return _host_ref.get_ref() if _host_ref != null else null


func _ensure_history() -> NovaEditHistory:
	if _history == null:
		_history = NovaEditHistory.new()
		_history.set_limit(_limit)
	return _history


## Open an editing burst: the next commit_edit() folds every change in between
## into a single undo step. Idempotent while a session is open.
func begin_edit() -> void:
	var host := _host()
	if host == null:
		return
	var snap: Variant = host._snapshot()
	if snap != null:
		_ensure_history().begin_edit(snap)


## Close the burst; records one step iff the document actually changed
## (equal-gated in the core), marking dirty and notifying on a real change.
func commit_edit() -> void:
	var host := _host()
	if host == null:
		return
	var snap: Variant = host._snapshot()
	if snap == null:
		return
	if _ensure_history().commit_edit(snap):
		host.mark_dirty()
		host._history_applied("commit")


func flush_edit() -> void:
	commit_edit()


## Record the CURRENT document as one undo step before a structural mutation
## the caller applies itself (silent: the caller emits its own signals after
## mutating). Clears redo.
func record_undo_step() -> void:
	var host := _host()
	if host == null:
		return
	var snap: Variant = host._snapshot()
	if snap != null:
		_ensure_history().push_step(snap)


## Bracket a single structural mutation as one equal-gated undo step, marking
## dirty and notifying ("step") only when it really changed the document.
func push_undo_step(mutation: Callable) -> void:
	var host := _host()
	if host == null:
		mutation.call()
		return
	var before: Variant = host._snapshot()
	if before == null:
		mutation.call()
		return
	flush_edit()
	_ensure_history().begin_edit(before)
	mutation.call()
	var after: Variant = host._snapshot()
	if _ensure_history().commit_edit(after):
		host.mark_dirty()
		host._history_applied("step")


func can_undo() -> bool:
	return _history != null and _history.can_undo()


func can_redo() -> bool:
	return _history != null and _history.can_redo()


func undo() -> void:
	flush_edit()
	if _history == null:
		return
	var host := _host()
	if host == null:
		return
	var live: Variant = host._snapshot()
	if live == null:
		return
	var snap: Variant = _history.undo_swap(live)
	if snap == null:
		return
	host._apply_snapshot(snap)
	host.mark_dirty()
	host._history_applied("undo")


func redo() -> void:
	flush_edit()
	if _history == null:
		return
	var host := _host()
	if host == null:
		return
	var live: Variant = host._snapshot()
	if live == null:
		return
	var snap: Variant = _history.redo_swap(live)
	if snap == null:
		return
	host._apply_snapshot(snap)
	host.mark_dirty()
	host._history_applied("redo")


## Drop the whole history (open / new / save-as flows). The host's dirty flag
## is untouched; the host calls mark_clean() separately when the baseline moves.
func clear() -> void:
	if _history != null:
		_history.clear()
