extends "res://modtools/mission/controller/controller_section.gd"

# Mission scripting (Phase 4): event / trigger / action reads and edits
# driving the scripting panel.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

func get_event_count() -> int:
	return _c._mission.get_event_count() if _c._mission != null else 0


func get_events() -> Array:
	return _c._mission.get_events() if _c._mission != null else []


# The selected event index, or -1 when nothing valid is selected. Pure read (no side effects): a
# selection that fell out of range (the event list shrank under it via a delete or an undo) reads as
# "nothing selected" rather than a stale index, and the caller re-selects from the list. The stored
# field is left alone; every read re-validates it against the current event count.
func get_selected_event_index() -> int:
	if _c._mission == null or _c._selected_event_index < 0 or _c._selected_event_index >= _c._mission.get_event_count():
		return -1
	return _c._selected_event_index


# The selected event's full chain { event, triggers, actions, references, diagnostics }, or {}.
func get_selected_event_chain() -> Dictionary:
	var index := get_selected_event_index()
	if _c._mission == null or index < 0:
		return {}
	return _c._mission.get_event_chain(index)


func get_logic_summary() -> Dictionary:
	return _c._mission.get_logic_summary() if _c._mission != null else {}


func get_trigger_main_types() -> Array:
	return _c._mission.get_trigger_main_types() if _c._mission != null else []


func get_trigger_sub_types(main_type: int) -> Array:
	return _c._mission.get_trigger_sub_types(main_type) if _c._mission != null else []


func get_action_types() -> Array:
	return _c._mission.get_action_types() if _c._mission != null else []


func get_action_sub_types(action_type: int) -> Array:
	return _c._mission.get_action_sub_types(action_type) if _c._mission != null else []


func get_event_flag_bits() -> Array:
	return _c._mission.get_event_flag_bits() if _c._mission != null else []


func get_ai_flag_bits() -> Array:
	return _c._mission.get_ai_flag_bits() if _c._mission != null else []


# Focus an event by index (the inspector list drives this). Inert if unchanged.
func select_event(index: int) -> void:
	if index == _c._selected_event_index:
		return
	_c._selected_event_index = index
	_c._notify_changed()


# Append a new empty event, select it, and dirty. One undo step. Returns the new index, or -1.
func add_event_default() -> int:
	if _c._mission == null:
		return -1
	_c._flush_edit()
	_c._mission.begin_edit()
	var event: Dictionary = _c._mission.add_event(0, 0, 0)
	if event.is_empty():
		_c._report("Could not add an event.", true)
		return -1
	_c._mission.commit_edit()
	_c._selected_event_index = int(event.get("index", -1))
	_c.mark_dirty()
	return _c._selected_event_index


# Delete the selected event (drops its triggers + actions; ResetEvent references are repaired in the
# lib). Structural, so the selection clamps to the shrunken list. One undo step. False if none selected.
func delete_selected_event() -> bool:
	if _c._mission == null or get_selected_event_index() < 0:
		return false
	_c._flush_edit()
	_c._mission.begin_edit()
	if not _c._mission.remove_event(_c._selected_event_index):
		return false
	_c._mission.commit_edit()
	# Drop the selection after a delete (like the zone panel): the row the user was on is gone, and the
	# index would otherwise point at the event that shifted into its slot.
	_c._selected_event_index = -1
	_c._report("Event deleted. A ResetEvent action that pointed past it was repaired; a direct hit was unset.")
	_c.mark_dirty()
	return true


# Overwrite the selected event's own attributes (the EventFlags bitfield + reset_after / delay). One step.
func set_selected_event(flags: int, reset_after: int, delay: int) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.set_event(_c._selected_event_index, flags, reset_after, delay))


# Append a trigger to the selected event (defaults to a Group / Null condition). One undo step.
func add_selected_event_trigger() -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.add_event_trigger(_c._selected_event_index, {}),
		"Could not add a trigger (an event chains at most 20).")


# Overwrite the trigger at `local_index` (its position in the event's chain) from an editor dict. One step.
func set_selected_event_trigger(local_index: int, trigger: Dictionary) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.set_event_trigger(_c._selected_event_index, local_index, trigger))


func remove_selected_event_trigger(local_index: int) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.remove_event_trigger(_c._selected_event_index, local_index))


func move_selected_event_trigger(local_index: int, delta: int) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.move_event_trigger(_c._selected_event_index, local_index, delta))


# Append an action to the selected event (defaults to a Null action). One undo step.
func add_selected_event_action() -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.add_event_action(_c._selected_event_index, {}),
		"Could not add an action (an event chains at most 20).")


func set_selected_event_action(local_index: int, action: Dictionary) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.set_event_action(_c._selected_event_index, local_index, action))


func remove_selected_event_action(local_index: int) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.remove_event_action(_c._selected_event_index, local_index))


func move_selected_event_action(local_index: int, delta: int) -> void:
	if _c._mission == null or get_selected_event_index() < 0:
		return
	_c._edit_step(func(): return _c._mission.move_event_action(_c._selected_event_index, local_index, delta))
