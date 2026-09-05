class_name DebugPickList
extends RefCounted
## The debug pick list: the entities a developer picked in the live game
## (crosshair hotkey or tools-open click), shared between the shell, the world
## highlight view and, through the pick session, the F3 Entities window (every
## landed pick selects its row there). SHELL-owned: a crosshair pick works
## before F3 has ever been opened and the list survives tools toggles; the
## shell clears it on mission start/stop so stale handles never cross sessions.
##
## Capped and deduped: re-picking a listed entity refreshes that row's pick
## metadata (fresh hit position/tick) instead of appending; a full list evicts
## its oldest row so picking never wedges (the highlight shows the newest eight).

## A pick landed (appended or refreshed): the packed engine handle it names.
signal picked(handle: int)

const MAX_PICKS := 8

var _picks: Array[DebugPickCard] = []


## Add (or refresh) a pick card from Simulation.debug_pick_entity.
## Returns the row index, or -1 when the pick is not an entity hit.
func add(pick: DebugPickCard) -> int:
	if pick == null or not pick.hit:
		return -1
	var handle := pick.entity_handle
	if handle < 0:
		return -1
	for i in range(_picks.size()):
		if _picks[i].entity_handle == handle:
			_picks[i] = pick.duplicate()
			picked.emit(handle)
			return i
	if _picks.size() >= MAX_PICKS:
		_picks.pop_front()
	_picks.append(pick.duplicate())
	picked.emit(handle)
	return _picks.size() - 1


func clear() -> void:
	_picks.clear()


## Value copies — mutating a returned card never desyncs the list.
func get_picks() -> Array[DebugPickCard]:
	var out: Array[DebugPickCard] = []
	for pick in _picks:
		out.append(pick.duplicate())
	return out
