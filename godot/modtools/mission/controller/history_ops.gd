extends "res://modtools/mission/controller/controller_section.gd"

# Undo / redo edit-step machinery (Phase 5): the begin/commit edit-session
# bracket, the one-shot _edit_step recipe, and the undo/redo restore spine.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

func can_undo() -> bool:
	return _c._mission != null and _c._mission.can_undo()


func can_redo() -> bool:
	return _c._mission != null and _c._mission.can_redo()


# The number of undo steps on the stack (for tests / a future history readout).
func undo_depth() -> int:
	return _c._mission.undo_depth() if _c._mission != null else 0


# Open an edit session, capturing the pre-edit document once. Inert if a session is already open
# (so a run of axis edits coalesces) or no mission is loaded. Delegates to the document.
func begin_edit() -> void:
	if _c._mission != null:
		_c._mission.begin_edit()


# Close an edit session, pushing one undo step only if the document actually changed (a plain
# click, a same-value edit, or a programmatic refresh push nothing). Delegates to the document.
func commit_edit() -> void:
	if _c._mission != null:
		_c._mission.commit_edit()


func _flush_edit() -> void:
	commit_edit()


# One-shot mutation recipe shared by the simple setters. Flush any open edit session as its own step,
# open a fresh edit, run `do` (which performs exactly one NovaMissionData mutation and returns its
# result), and on success commit a single undo step, run `on_success` (e.g. an overlay refresh), and
# mark the document dirty. A bool result commits when true; a Dictionary / Array result (the chain
# mutators return the edited record / chain) commits when non-empty. On a rejected edit, surface
# `err` when it is non-empty. Returns whether the edit applied. Callers keep their own pre-guards (a
# valid selection, a fetched record) before calling -- this owns only the begin/commit/dirty bracket.
func _edit_step(do: Callable, err := "", on_success := Callable()) -> bool:
	if _c._mission == null:
		return false
	_flush_edit()
	_c._mission.begin_edit()
	var result: Variant = do.call()
	var ok := false
	if result is Dictionary:
		ok = not (result as Dictionary).is_empty()
	elif result is Array:
		ok = not (result as Array).is_empty()
	else:
		ok = bool(result)
	if ok:
		_c._mission.commit_edit()
		if on_success.is_valid():
			on_success.call()
		_c.mark_dirty()
	elif not err.is_empty():
		_c._report(err, true)
	return ok


func _clear_history() -> void:
	if _c._mission != null:
		_c._mission.clear_history()


func undo() -> void:
	_restore_step(true)


func redo() -> void:
	_restore_step(false)


# Shared undo/redo spine (the two differ only in direction + the status line). A keyboard undo/redo
# can arrive mid-drag; cancel_drag abandons the visual gesture (so the re-bake does not free nodes a
# continuing drag still references) and commits any open edit session as its step before we rewind.
# _restoring guards re-entrancy: a restore -> rebake -> `changed` -> inspector roundtrip must not recurse.
func _restore_step(is_undo: bool) -> void:
	if _c._restoring:
		return
	_c._viewport.cancel_drag()
	if _c._mission == null:
		return
	if is_undo and not _c._mission.can_undo():
		_c._report("Nothing to undo.")
		return
	if not is_undo and not _c._mission.can_redo():
		_c._report("Nothing to redo.")
		return
	_c._restoring = true
	var before: Dictionary = _c._mission.structure_fingerprint()
	if is_undo:
		_c._mission.undo()
	else:
		_c._mission.redo()
	_after_restore(before)
	_c._restoring = false
	_c._report("Undid the last change." if is_undo else "Redid the last change.")


# Sync the world + selection to the document after an in-memory undo/redo swap, then re-bake and
# notify once so the inspector refreshes against the restored world in a single pass. `before` is the
# pre-restore NovaMissionData.structure_fingerprint() -- { events, zones, object_rev }.
#
# Adding / deleting an event shifts later event indices, so a kept _selected_event_index could bind to
# a DIFFERENT event (the in-range clamp can't see a shift); drop the selection only when the event set
# actually changed (event add/delete are the only ops that change the count -- there is no event-reorder
# op -- so a count change is exactly the structural case). An attribute / trigger / action undo leaves
# the list intact and keeps the user on their event.
func _after_restore(before: Dictionary) -> void:
	var after: Dictionary = _c._mission.structure_fingerprint()
	if int(after["events"]) != int(before["events"]):
		_c._selected_event_index = -1
	# Same reasoning for the zone selection: area triggers are NOT part of object_rev (it covers only
	# placed objects), so an undo/redo of a zone add/delete takes the lightweight overlay-only path
	# below and would otherwise rebuild the overlay against a stale _selected_zone_index that now points
	# at a different (reindexed) zone. Add/delete are the only ops that change the zone count (no
	# reorder), so a count change is exactly the structural case; drop the selection then.
	if int(after["zones"]) != int(before["zones"]):
		_c._selected_zone_index = -1
	# Defensive clamp: never leave the index past the end of the restored zone list.
	if _c._selected_zone_index >= _c._mission.get_area_trigger_count():
		_c._selected_zone_index = -1
	# Skip the full object re-place when the undo/redo changed only non-object data (events / triggers /
	# actions / zones / header / loadout / groups): every placed object is byte-identical, so re-baking
	# ~all MultiMesh instances is pure waste. The placed nodes + pickable index + stats + object selection
	# all stay valid; only the active mode's overlay (which reads events / zones / paths from the document)
	# needs a refresh. Any object change moves object_rev -> full re-bake. object_rev is computed in C++
	# (NovaMissionData.object_records_revision) over the raw record bytes, so this no longer marshals the
	# placed-object set into ~1600 entity dictionaries twice per undo.
	if int(after["object_rev"]) == int(before["object_rev"]):
		_c._refresh_active_overlay()
	else:
		_c._rebake_objects()
	_c.mark_dirty()


# Mark the current input event handled so a consumed Ctrl+Z / Ctrl+Y does not propagate
# further (mirrors terrain_editor). No-op without a live viewport (headless tests).
func _consume_viewport_key() -> void:
	if _c.terrain_editor == null or not _c.terrain_editor.is_inside_tree():
		return
	var vp: Viewport = _c.terrain_editor.get_viewport()
	if vp != null:
		vp.set_input_as_handled()
