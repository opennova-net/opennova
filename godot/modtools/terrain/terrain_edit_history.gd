class_name TerrainEditHistory
extends RefCounted

const MAX_STEPS := 32

enum Kind { HEIGHTMAP, BLENDMAP, COLORMAP, TILEINFO, SURFACEMAP, FOLIAGEMAP, FOLIAGE_DEFS }

var _undo_stack: Array = []
var _redo_stack: Array = []

var _pending_kind: int = -1
var _pending_rect: Rect2i = Rect2i(0, 0, 0, 0)
var _pending_before_full: Image = null


func clear() -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_pending_kind = -1
	_pending_rect = Rect2i(0, 0, 0, 0)
	_pending_before_full = null


func begin_stroke(kind: int, source_image: Image) -> void:
	cancel_stroke()
	_pending_kind = kind
	_pending_rect = Rect2i(0, 0, 0, 0)
	_pending_before_full = Image.new()
	_pending_before_full.copy_from(source_image)


func cancel_stroke() -> void:
	_pending_kind = -1
	_pending_rect = Rect2i(0, 0, 0, 0)
	_pending_before_full = null


func has_pending() -> bool:
	return _pending_kind != -1


func expand_rect(dab_rect: Rect2i, atlas_size: int) -> void:
	if _pending_kind == -1:
		return
	var bounds := Rect2i(0, 0, atlas_size, atlas_size)
	var clipped := dab_rect.intersection(bounds)
	if clipped.size.x <= 0 or clipped.size.y <= 0:
		return
	if _pending_rect.size.x == 0 or _pending_rect.size.y == 0:
		_pending_rect = clipped
	else:
		_pending_rect = _pending_rect.merge(clipped)


func end_stroke(current_image: Image) -> void:
	if _pending_kind == -1:
		return
	if _pending_rect.size.x <= 0 or _pending_rect.size.y <= 0:
		cancel_stroke()
		return

	var before := _pending_before_full.get_region(_pending_rect)
	var after := current_image.get_region(_pending_rect)

	var snapshot := {
		"kind": _pending_kind,
		"rect": _pending_rect,
		"before": before,
		"after": after,
	}
	_undo_stack.push_back(snapshot)
	if _undo_stack.size() > MAX_STEPS:
		_undo_stack.pop_front()
	_redo_stack.clear()

	cancel_stroke()


func push_custom_snapshot(kind: int, before_value: Variant, after_value: Variant) -> void:
	var snapshot := {
		"kind": kind,
		"before_value": _duplicate_value(before_value),
		"after_value": _duplicate_value(after_value),
	}
	_undo_stack.push_back(snapshot)
	if _undo_stack.size() > MAX_STEPS:
		_undo_stack.pop_front()
	_redo_stack.clear()


func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func pop_undo() -> Dictionary:
	if _undo_stack.is_empty():
		return {}
	var snapshot: Dictionary = _undo_stack.pop_back()
	_redo_stack.push_back(snapshot)
	return snapshot


func pop_redo() -> Dictionary:
	if _redo_stack.is_empty():
		return {}
	var snapshot: Dictionary = _redo_stack.pop_back()
	_undo_stack.push_back(snapshot)
	return snapshot


func _duplicate_value(value: Variant) -> Variant:
	if value is Array:
		return (value as Array).duplicate(true)
	if value is Dictionary:
		return (value as Dictionary).duplicate(true)
	return value
