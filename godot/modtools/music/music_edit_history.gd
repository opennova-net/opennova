class_name MusicEditHistory
extends RefCounted

# Generic command stack used by the music editor's per-mode undo history
# (one for the bank, one for the script). Each entry is a Callable pair
# (do, undo). push records a new command; undo / redo step through. push
# truncates any redo tail so the linear history remains coherent.

var _stack: Array = []
var _index: int = 0  # points to next slot to use


func push(do: Callable, undo: Callable) -> void:
	# Truncate any redo history past the current index so we don't keep
	# orphan commands when a new edit branches off mid-stack.
	if _index < _stack.size():
		_stack.resize(_index)
	_stack.append({"do": do, "undo": undo})
	_index = _stack.size()


func can_undo() -> bool:
	return _index > 0


func can_redo() -> bool:
	return _index < _stack.size()


func undo() -> void:
	if not can_undo():
		return
	_index -= 1
	(_stack[_index]["undo"] as Callable).call()


func redo() -> void:
	if not can_redo():
		return
	(_stack[_index]["do"] as Callable).call()
	_index += 1


func clear() -> void:
	_stack.clear()
	_index = 0
