class_name NovaDebugPickList
extends RefCounted
## The debug pick list: the entities a developer picked in the live game
## (crosshair hotkey or overlay-open click), shared between the host, the F3
## overlay's picks section and the world highlight view. HOST-OWNED — a
## crosshair pick must work before F3 has ever been opened, and the list must
## survive overlay toggles — and cleared by the host on mission start/stop so
## stale handles never cross sessions.
##
## Capped and deduped: re-picking a listed entity refreshes that row's pick
## metadata (fresh hit position/tick) instead of appending; a full list
## rejects new picks (add returns -1) rather than silently evicting the
## oldest — the developer curates the set.

signal changed
## Emitted only for a successful add/refresh, with the accepted card. Unlike
## `changed`, consumers can use this to focus the object the developer just
## picked without treating remove/clear as another selection.
signal picked(pick: Dictionary)

const MAX_PICKS := 8

var _picks: Array[Dictionary] = []


## Add (or refresh) a pick card from NovaSimulation.debug_pick_entity.
## Returns the row index, or -1 when the pick is not an entity hit or the
## list is full.
func add(pick: Dictionary) -> int:
	if not bool(pick.get("hit", false)):
		return -1
	var handle := int(pick.get("entity_handle", -1))
	if handle < 0:
		return -1
	for i in range(_picks.size()):
		if int(_picks[i].get("entity_handle", -1)) == handle:
			_picks[i] = pick.duplicate(true)
			changed.emit()
			picked.emit(_picks[i].duplicate(true))
			return i
	if _picks.size() >= MAX_PICKS:
		return -1
	_picks.append(pick.duplicate(true))
	changed.emit()
	picked.emit(_picks.back().duplicate(true))
	return _picks.size() - 1


func remove_at(index: int) -> void:
	if index < 0 or index >= _picks.size():
		return
	_picks.remove_at(index)
	changed.emit()


func clear() -> void:
	if _picks.is_empty():
		return
	_picks.clear()
	changed.emit()


func size() -> int:
	return _picks.size()


func is_full() -> bool:
	return _picks.size() >= MAX_PICKS


## Deep copies — mutating a returned card never desyncs the list.
func get_picks() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for pick in _picks:
		out.append(pick.duplicate(true))
	return out
