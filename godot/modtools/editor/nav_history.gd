class_name EditorNavHistory
extends RefCounted

## Browser-style navigation history over shell locations. The shell records a
## lazy departure snapshot — an EditorNavLocation — each time the user leaves
## a location through a navigation entry point, and Back/Forward walk those
## snapshots as two stacks. The current location is never stored:
## go_back/go_forward pass it in at commit time (peek → navigate → commit or
## drop), so a failed restore can never corrupt the stacks.

const MAX_ENTRIES := 50

var _back: Array[EditorNavLocation] = []
var _forward: Array[EditorNavLocation] = []


## Push a departure snapshot. Consecutive duplicates collapse, and any recorded
## navigation invalidates the forward stack (browser semantics).
func record(entry: EditorNavLocation) -> void:
	_forward.clear()
	if not _back.is_empty() and EditorNavLocation.same(_back.back(), entry):
		return
	_back.append(entry)
	while _back.size() > MAX_ENTRIES:
		_back.remove_at(0)


func can_go_back() -> bool:
	return not _back.is_empty()


func can_go_forward() -> bool:
	return not _forward.is_empty()


func peek_back() -> EditorNavLocation:
	return _back.back() if not _back.is_empty() else null


func peek_forward() -> EditorNavLocation:
	return _forward.back() if not _forward.is_empty() else null


## Commit a successful Back: the peeked entry leaves the back stack and the
## pre-navigation current location becomes the Forward destination.
func commit_back(current: EditorNavLocation) -> EditorNavLocation:
	var entry: EditorNavLocation = _back.pop_back()
	_forward.append(current)
	return entry


func commit_forward(current: EditorNavLocation) -> EditorNavLocation:
	var entry: EditorNavLocation = _forward.pop_back()
	_back.append(current)
	return entry


## Discard a dead destination (e.g. its file no longer opens).
func drop_back() -> void:
	if not _back.is_empty():
		_back.remove_at(_back.size() - 1)


func drop_forward() -> void:
	if not _forward.is_empty():
		_forward.remove_at(_forward.size() - 1)


func back_count() -> int:
	return _back.size()


func forward_count() -> int:
	return _forward.size()
