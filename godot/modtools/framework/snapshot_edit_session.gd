class_name SnapshotEditSession
extends RefCounted

## Whole-document undo/redo over the engine's shared core (NovaEditHistory
## boxing libs/oned_edit), shared by the Node documents (EditorDocument) and
## the RefCounted resource documents (EditorResourceDocument). The document
## provides `_snapshot()` (null = not opted in / nothing loaded; every method
## is then inert), `_apply_snapshot(snap)`, `_history_applied(kind)` and
## `mark_dirty()`; the session owns the begin/commit bracket (one editing
## burst = one equal-gated step), the silent pre-mutation step
## (record_undo_step), the bracketed structural step (push_undo_step), and
## undo/redo. The document is held through a weakref: resource documents are
## RefCounted, and a bound-method Callable back at the document would
## refcount-cycle document and session together.

var _document_ref: WeakRef
var _limit: int = 100
var _history: NovaEditHistory = null
# Mirrors the native session's begin/commit state so shadow-step recorders
# (B4's object funnel) can tell "a coalescing burst is open" without a native
# query.
var _burst_open := false


func _init(document: Object, limit: int = 100) -> void:
	_document_ref = weakref(document)
	_limit = limit


func _document() -> Object:
	return _document_ref.get_ref() if _document_ref != null else null


func _ensure_history() -> NovaEditHistory:
	if _history == null:
		_history = NovaEditHistory.new()
		_history.set_limit(_limit)
	return _history


## Open an editing burst: the next commit_edit() folds every change in between
## into a single undo step. Idempotent while a session is open.
func begin_edit() -> void:
	var document := _document()
	if document == null:
		return
	var snap: Variant = document._snapshot()
	if snap != null:
		_ensure_history().begin_edit(snap)
		_burst_open = true


## Close the burst; records one step iff the document actually changed
## (equal-gated in the core), marking dirty and notifying on a real change.
func commit_edit() -> void:
	var document := _document()
	if document == null:
		return
	var snap: Variant = document._snapshot()
	if snap == null:
		return
	_burst_open = false
	if _ensure_history().commit_edit(snap):
		document.mark_dirty()
		document._history_applied("commit")


## Record a caller-provided PRE-mutation snapshot as one equal-gated step —
## the shadow-step funnel (B4): the mutation already happened by the time its
## deferred change signal arrives, so the caller hands in the baseline it
## cached beforehand. Inert while a coalescing burst is open (the burst's
## commit owns that step). Returns whether a step was recorded.
func record_shadow_step(before: Variant) -> bool:
	if before == null or _burst_open:
		return false
	var document := _document()
	if document == null:
		return false
	var live: Variant = document._snapshot()
	if live == null or live == before:
		return false
	_ensure_history().push_step(before)
	document.mark_dirty()
	document._history_applied("step")
	return true


func flush_edit() -> void:
	commit_edit()


## Record the CURRENT document as one undo step before a structural mutation
## the caller applies itself (silent: the caller emits its own signals after
## mutating). Clears redo.
func record_undo_step() -> void:
	var document := _document()
	if document == null:
		return
	var snap: Variant = document._snapshot()
	if snap != null:
		_ensure_history().push_step(snap)


## Bracket a single structural mutation as one equal-gated undo step, marking
## dirty and notifying ("step") only when it really changed the document.
func push_undo_step(mutation: Callable) -> void:
	var document := _document()
	if document == null:
		mutation.call()
		return
	var before: Variant = document._snapshot()
	if before == null:
		mutation.call()
		return
	flush_edit()
	_ensure_history().begin_edit(before)
	mutation.call()
	var after: Variant = document._snapshot()
	if _ensure_history().commit_edit(after):
		document.mark_dirty()
		document._history_applied("step")


func can_undo() -> bool:
	return _history != null and _history.can_undo()


func can_redo() -> bool:
	return _history != null and _history.can_redo()


func undo() -> void:
	flush_edit()
	if _history == null:
		return
	var document := _document()
	if document == null:
		return
	var live: Variant = document._snapshot()
	if live == null:
		return
	var snap: Variant = _history.undo_swap(live)
	if snap == null:
		return
	_burst_open = false
	document._apply_snapshot(snap)
	document.mark_dirty()
	document._history_applied("undo")


func redo() -> void:
	flush_edit()
	if _history == null:
		return
	var document := _document()
	if document == null:
		return
	var live: Variant = document._snapshot()
	if live == null:
		return
	var snap: Variant = _history.redo_swap(live)
	if snap == null:
		return
	_burst_open = false
	document._apply_snapshot(snap)
	document.mark_dirty()
	document._history_applied("redo")


## Drop the whole history (open / new / save-as flows). The document's dirty flag
## is untouched; the document calls mark_clean() separately when the baseline moves.
func clear() -> void:
	_burst_open = false
	if _history != null:
		_history.clear()
