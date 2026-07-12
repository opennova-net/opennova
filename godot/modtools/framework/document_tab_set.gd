class_name DocumentTabSet
extends RefCounted
## The model half of per-workspace document tabs: an ordered set of open
## documents plus the active index. Pure bookkeeping, no UI — a workspace owns
## one of these, forwards the EditorWorkspace document-tab hooks to it, and
## re-emits `changed` as `documents_changed` for the shell's tab strip.
## Documents are duck-typed to the EditorDocument shape (`current_path`,
## `is_dirty`), so any domain document family fits (Strings now, Mnu next).

signal changed

var _docs: Array = []
var _active := -1


func count() -> int:
	return _docs.size()


func get_active_index() -> int:
	return _active


func get_active() -> Object:
	return _docs[_active] if _active >= 0 and _active < _docs.size() else null


func get_at(index: int) -> Object:
	return _docs[index] if index >= 0 and index < _docs.size() else null


func index_of(doc: Object) -> int:
	return _docs.find(doc)


## Finds the tab already holding `path` (case-insensitive — resource names are);
## -1 on miss or empty path.
func index_of_path(path: String) -> int:
	if path.strip_edges().is_empty():
		return -1
	var want := path.strip_edges().to_lower()
	for i in _docs.size():
		var doc_path := String(_docs[i].get("current_path"))
		if not doc_path.is_empty() and doc_path.to_lower() == want:
			return i
	return -1


func add(doc: Object, make_active := true) -> int:
	_docs.append(doc)
	if make_active or _active < 0:
		_active = _docs.size() - 1
	changed.emit()
	return _docs.size() - 1


func set_active(index: int) -> Error:
	if index < 0 or index >= _docs.size():
		return ERR_INVALID_PARAMETER
	if index == _active:
		return OK
	_active = index
	changed.emit()
	return OK


## Removes and returns the document (the caller owns freeing Nodes). The active
## index clamps to the nearest surviving neighbor.
func remove_at(index: int) -> Object:
	if index < 0 or index >= _docs.size():
		return null
	var doc: Object = _docs[index]
	_docs.remove_at(index)
	if _docs.is_empty():
		_active = -1
	elif index < _active or _active >= _docs.size():
		_active = clampi(_active - 1, 0, _docs.size() - 1)
	changed.emit()
	return doc


## Re-emit for changes that do not touch membership (dirty flips, renames after
## save-as) so the strip refreshes its labels/badges.
func notify_changed() -> void:
	changed.emit()


## The get_document_tabs() rows, derived from the documents.
func tabs() -> Array[DocumentTabRow]:
	var rows: Array[DocumentTabRow] = []
	for doc in _docs:
		var path := String(doc.get("current_path"))
		var dirty := false
		if doc.has_method("is_dirty"):
			dirty = doc.is_dirty()
		else:
			dirty = bool(doc.get("is_dirty"))
		rows.append(DocumentTabRow.make(
			path.get_file() if not path.is_empty() else "Untitled",
			dirty,
			path,
			path if not path.is_empty() else "Not saved yet"))
	return rows


## Non-empty current_paths in tab order — the session-persistence payload.
func open_paths() -> PackedStringArray:
	var paths := PackedStringArray()
	for doc in _docs:
		var path := String(doc.get("current_path"))
		if not path.is_empty():
			paths.append(path)
	return paths
