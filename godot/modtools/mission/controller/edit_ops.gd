extends "res://modtools/mission/controller/controller_section.gd"

# Edit operations: delete-selected, the world re-bake (the universal choke
# point for entity-set changes), the per-mode overlay refresh it dispatches,
# and the editing-mode switch.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# Remove the currently-selected entity, then re-bake the world so it matches the new
# record. A selected marker (mesh-less) skips the object re-bake (its delete shifts no object
# MultiMesh indices) and just rebuilds the marker overlay. Returns false (a no-op) when nothing is
# selected or the lib rejects the removal; clears the selection on success. Public so the
# inspector's Delete button and the viewport Delete key share one path.
func delete_selected() -> bool:
	if _c._selected_ref.is_empty() or _c._mission == null:
		return false
	var kind := int(_c._selected_ref["kind"])
	var index := int(_c._selected_ref["index"])
	# Capture a readable label before the removal: after the re-bake the selection (and its
	# resolvable name) is gone.
	var label: String = _c.get_selected_display_name()
	# Deleting is its own undo step: close any open session, then bracket the removal with
	# begin_edit/commit_edit (a successful removal always changes the document, so this is never a
	# no-op step).
	_c._flush_edit()
	_c._mission.begin_edit()
	if not _c._mission.remove_entity(kind, index):
		# Balance the begin_edit() bracket on the reject path (no-op step, the failed remove changed
		# nothing) so the open session does not leak into the next gesture.
		_c._mission.commit_edit()
		_c._report("Could not delete the selected object.", true)
		return false
	_c._mission.commit_edit()
	if kind == NovaMissionData.KIND_MARKER:
		# Objects are untouched by a marker delete; drop the selection and rebuild only the marker
		# overlay against the post-delete list (cheaper than re-placing every object). The marker is
		# still part of the entity set the inspector's cached pickers marshal, so bump the membership
		# revision (the non-marker branch gets this from _rebake_objects).
		_c._membership_rev += 1
		_c._viewport._deselect()
		_c._waypoints._refresh_marker_overlay()
	else:
		# Re-bake first (it resets the selection state and rebuilds stats), then dirty +
		# emit once so the inspector refreshes against the post-delete world in a single pass.
		_rebake_objects()
	_c.mark_dirty()
	_c._report("Deleted %s. Ctrl+Z to undo." % (label if not label.is_empty() else "object"))
	return true


# Rebuild the entire MissionObjects container from the current mission state, reusing
# the retained placer so its (expensive) model + batch caches survive the rebuild. The
# container node identity is kept (place() clears and refills it), so _objects_container
# still resolves. Drops the selection: its box and pickable records are freed with the
# old container contents and the indices they carried may no longer be valid.
func _rebake_objects() -> void:
	if _c._placer == null or _c._mission == null:
		return
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node3D = _c.terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	_c._viewport._reset_selection_state()
	# A re-bake is the universal choke point for entity-set changes (add / remove / place / delete /
	# marker edits, and undo/redo whose object signature differs), so bump the membership revision here
	# to invalidate the inspector's cached group / waypoint-path / entity pickers. (A bulk re-ground is
	# NOT a membership change — it moves existing entities in place and skips both the re-bake and this
	# bump; see _apply_reground_world_update.)
	_c._membership_rev += 1
	var options: Dictionary = {}
	var env_node: Node = _c._environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	_c._stats = _c._placer.place(_c._mission, world_root, options)
	_c._pickable = _c._placer.pickable_records
	# The placer (re)created the pick colliders with the world; just refresh the debug overlay.
	_c._viewport._refresh_pick_debug()
	# The re-bake replaced the container (and the old overlay with it); rebuild the active
	# mode's overlay against the new world.
	_refresh_active_overlay()


# Rebuild only the overlay for the current mode (each mode owns exactly one). Shared by the
# re-bake and the lightweight undo path so the mode -> overlay dispatch lives in one place.
func _refresh_active_overlay() -> void:
	if _c._mode == _c.Mode.WAYPOINTS:
		_c._waypoints._refresh_waypoint_overlay()
	elif _c._mode == _c.Mode.AREA_TRIGGERS:
		_c._zones._refresh_area_trigger_overlay()
	elif _c._mode == _c.Mode.OBJECTS:
		_c._waypoints._refresh_marker_overlay()


# Switch the active editing mode (Mode.*). A mode switch is a fresh context: it closes any
# open edit session, then drops EVERY mode's selection + armed tool so only the new mode's
# clicks are live. Each mode focuses a sensible default on entry (a populated waypoint path /
# the first zone) and toggles its overlay's visibility. Inert if already in `mode`.
func set_mode(mode: int) -> void:
	if _c._mode == mode:
		return
	_c._flush_edit()
	_c._mode = mode
	# A mode switch is a fresh context: stop any running part-animation preview.
	_c._preview.stop_preview()
	_c._viewport._clear_selected_user_points()
	# Exclusive selection: clear the object selection refs + its box, the marker selection,
	# the zone selection, and any armed placement tool.
	_c._selected_ref = {}
	_c._selected_records = []
	_c._selected_node = null
	_c._selected_graphic = ""
	_c._selected_node_offset = Transform3D.IDENTITY
	_c._selected_collider = null
	_c._selected_ground_offset = Vector3.ZERO
	_c._viewport._hide_selection_box()
	# Hover only lives in objects mode; drop it on any mode switch.
	_c._viewport._clear_hover()
	_c._selected_marker = {}
	_c._selected_zone_index = -1
	_c._place_item_id = 0
	_c._marker_place_armed = false
	if mode == _c.Mode.WAYPOINTS and _c._selected_path_index < 0:
		# Focus a populated path on entry so the panel is not empty.
		_c._selected_path_index = _c._waypoints._first_nonempty_path()
	if mode == _c.Mode.AREA_TRIGGERS and _c._mission != null and _c._mission.get_area_trigger_count() > 0:
		# Focus the first zone on entry so the panel is not empty.
		_c._selected_zone_index = 0
	if mode == _c.Mode.SCRIPTING and _c._mission != null and _c.get_selected_event_index() < 0 and _c._mission.get_event_count() > 0:
		# Focus the first event on entry so the scripting panel is not empty (get_selected_event_index
		# reads a stale-but-out-of-range selection as -1, so a shrunken list re-focuses event 0).
		_c._selected_event_index = 0
	_c._waypoints._refresh_waypoint_overlay()
	_c._zones._refresh_area_trigger_overlay()
	_c._waypoints._refresh_marker_overlay()
	# Each overlay is visible only in its own mode (markers are placed/edited in Objects mode; the
	# waypoint overlay shows path markers in Waypoints mode), so the two marker-gizmo overlays never
	# double-draw.
	if _c._waypoint_overlay != null and is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay.visible = mode == _c.Mode.WAYPOINTS
	if _c._area_overlay != null and is_instance_valid(_c._area_overlay):
		_c._area_overlay.visible = mode == _c.Mode.AREA_TRIGGERS
	if _c._marker_overlay != null and is_instance_valid(_c._marker_overlay):
		_c._marker_overlay.visible = mode == _c.Mode.OBJECTS
	# The transform gizmo lives only in Objects mode and only with a selection; a mode switch
	# clears the selection above, so just hide it here (rebuilt on the next object select).
	if _c._gizmo != null and is_instance_valid(_c._gizmo):
		_c._gizmo.visible = false
	_c._notify_changed()
