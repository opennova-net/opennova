extends "res://modtools/mission/controller/controller_section.gd"

# Waypoint paths + markers: selection, overlays, drag / add /
# reorder / delete.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# Focus a waypoint path (0..127) in the panel + overlay. Drops the marker selection (a
# different path's markers) and rebuilds the overlay so its gizmos / pickables follow.
func select_waypoint_path(index: int) -> void:
	if index == _c._selected_path_index:
		return
	_c._selected_path_index = index
	# Drop the marker selection (it belonged to the previous path) AND tell the overlay to
	# clear its highlight, so a marker index that also appears on the new path is not left
	# lit. _refresh_waypoint_overlay then rebuilds against the new active path.
	_c._selected_marker = {}
	if _c._waypoint_overlay != null and is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay.set_selected_marker(-1)
	_refresh_waypoint_overlay()
	_c._notify_changed()


func get_selected_waypoint_path_index() -> int:
	return _c._selected_path_index


# Focus the first empty waypoint path so the user can author into it. The path list only
# shows populated paths (plus the active one), so on an all-empty mission no path is
# selectable and "Add marker" would stay disabled forever; this is the "start a new route"
# entry point. Returns the chosen path index, or -1 if all 128 are full (not reachable in
# practice). Selecting it makes the (empty) path active, which the list then shows.
func select_new_waypoint_path() -> int:
	if _c._mission == null:
		return -1
	var idx := _first_empty_path()
	if idx >= 0:
		select_waypoint_path(idx)
	return idx


# Set the active path's flags from the three editor toggles (loop is the inverse of the
# stored DoesNotLoop bit). One undo step: re-pass the path's current marker order with the
# new flags through set_waypoint_path (a flag-only edit), then re-bake the overlay (team
# colour / loop segment may change). Inert without an active path / mission.
func set_waypoint_flags(loop: bool, blue: bool, red: bool) -> void:
	if _c._mission == null or _c._selected_path_index < 0:
		return
	var path = _c._mission.get_waypoint_path(_c._selected_path_index)
	if path.is_empty():
		return
	# Preserve any on-disk flag bits beyond the three the UI exposes (the event- and zone-flag paths
	# mask-merge the same way): seed from the
	# path's current flags with the known bits cleared, then set only DoesNotLoop / BlueTeam / RedTeam.
	var known_mask := NovaMissionData.WP_FLAG_DOES_NOT_LOOP | NovaMissionData.WP_FLAG_BLUE_TEAM | NovaMissionData.WP_FLAG_RED_TEAM
	var flags := int(path.get("flags", 0)) & ~known_mask
	if not loop:
		flags |= NovaMissionData.WP_FLAG_DOES_NOT_LOOP
	if blue:
		flags |= NovaMissionData.WP_FLAG_BLUE_TEAM
	if red:
		flags |= NovaMissionData.WP_FLAG_RED_TEAM
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	_c._edit_step(func(): return _c._mission.set_waypoint_path(_c._selected_path_index, indices, flags),
		"", _refresh_waypoint_overlay)


func get_waypoint_summaries() -> Array:
	return _c._mission.get_waypoint_summaries() if _c._mission != null else []


# The active path as { index, flags, marker_count, marker_indices }, or {} when none is
# chosen / no mission is loaded.
func get_active_waypoint_path() -> Dictionary:
	if _c._mission == null or _c._selected_path_index < 0:
		return {}
	return _c._mission.get_waypoint_path(_c._selected_path_index)


# The selected marker enriched with its entity position for the inspector readout, or {}.
func get_selected_marker() -> Dictionary:
	if _c._selected_marker.is_empty() or _c._mission == null:
		return {}
	var marker_index := int(_c._selected_marker["marker_index"])
	var entity = _c._mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)
	if entity.is_empty():
		return {}
	return {
		"path_index": int(_c._selected_marker["path_index"]),
		"marker_index": marker_index,
		"position": entity.get("position", Vector3.ZERO),
	}


# The first waypoint path that has at least one marker, or -1 if every path is empty.
func _first_nonempty_path() -> int:
	if _c._mission == null:
		return -1
	for s in _c._mission.get_waypoint_summaries():
		if int((s as Dictionary)["marker_count"]) > 0:
			return int((s as Dictionary)["index"])
	return -1


# The first waypoint path with no markers, or -1 if all 128 are populated. Used by
# select_new_waypoint_path to give from-scratch authoring an empty path to fill.
func _first_empty_path() -> int:
	if _c._mission == null:
		return -1
	# Reserve record index 0: the engine reads waypoint_id (byte 79) with 0 == "follow no path"
	# [orig: Entity_SpawnFromBMSRecord @0x40f02f, `if (record[79])`], so a route authored into record 0
	# can never be a follow target and get_waypoint_path_options omits it. Author new routes from
	# index 1 so they show up in the Behavior "Waypoint path" picker.
	for s in _c._mission.get_waypoint_summaries():
		var idx := int((s as Dictionary)["index"])
		if idx >= 1 and int((s as Dictionary)["marker_count"]) == 0:
			return idx
	return -1


# (Re)build the in-world overlay from the current mission + active path, and re-harvest the
# marker pickable index. Creates the overlay node under the objects container on first use
# (and after a re-bake freed it). The overlay reflects the controller's marker selection.
func _refresh_waypoint_overlay() -> void:
	if _c._mission == null:
		return
	var container = _c._objects_container()
	if container == null:
		return
	if _c._waypoint_overlay == null or not is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay = _c.MissionWaypointOverlay.new()
		_c._waypoint_overlay.name = "MissionWaypointOverlay"
		_c._waypoint_overlay.visible = _c._mode == _c.Mode.WAYPOINTS
		container.add_child(_c._waypoint_overlay)
	_c._waypoint_overlay.rebuild(_c._mission, _c._selected_path_index)
	_c._marker_pickable = _c._waypoint_overlay.marker_pickables()
	if not _c._selected_marker.is_empty():
		_c._waypoint_overlay.set_selected_marker(int(_c._selected_marker["marker_index"]))


# (Re)build the always-on marker overlay (a gizmo per marker, labelled with its items.def display
# name) and re-apply the gizmo highlight for a selected marker. Creates the node under the objects
# container on first use (and after a re-bake freed it); visible only in Objects mode. Markers are
# placed + edited as general entities there, so this is the Objects-mode counterpart of the
# waypoint overlay.
func _refresh_marker_overlay() -> void:
	if _c._mission == null:
		return
	var container = _c._objects_container()
	if container == null:
		return
	if _c._marker_overlay == null or not is_instance_valid(_c._marker_overlay):
		_c._marker_overlay = _c.MissionMarkerOverlay.new()
		_c._marker_overlay.name = "MissionMarkerOverlay"
		_c._marker_overlay.visible = _c._mode == _c.Mode.OBJECTS
		container.add_child(_c._marker_overlay)
	_c._marker_overlay.rebuild(_c._mission, _marker_labels())
	# Re-apply the highlight for a selected marker (the object-selection path holds it in _selected_ref).
	if not _c._selected_ref.is_empty() and int(_c._selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		_c._marker_overlay.set_selected_marker(int(_c._selected_ref["index"]))


# The display name for every marker (aligned to KIND_MARKER index), so the overlay can label each
# gizmo and the user can tell a player start from a waypoint node. Falls back to "" (no label) when
# the name can't be resolved.
func _marker_labels() -> Array:
	var labels: Array = []
	if _c._mission == null:
		return labels
	var count = _c._mission.get_entity_count(NovaMissionData.KIND_MARKER)
	for i in count:
		labels.append(_c.entity_display_name(NovaMissionData.KIND_MARKER, i))
	return labels


# Select a marker on the active path by its KIND_MARKER entity index (the inspector's
# ordered marker list drives this). Inert without an active path.
func select_waypoint_marker(marker_index: int) -> void:
	if _c._selected_path_index < 0:
		return
	_select_marker(_c._selected_path_index, marker_index)


func _on_marker_left_press(mouse_pos: Vector2) -> void:
	# Armed: a click adds a marker to the active path at the cursor instead of selecting.
	if _c._marker_place_armed:
		_place_marker_armed_at(mouse_pos)
		return
	# Close any open edit session as its own step before a new gesture (mirrors _on_left_press).
	_c._flush_edit()
	var ref := _pick_marker(mouse_pos)
	if ref.is_empty():
		_deselect_marker()
		return
	_select_marker(int(ref["path_index"]), int(ref["marker_index"]))
	# Begin a drag: motion re-grounds the marker on the terrain, release writes the record as
	# one undo step (a plain click selects without moving, like an object click).
	_c._drag_active = true
	_c._drag_moved = false
	_c._drag_off_terrain = false
	_c.begin_edit()


# Pick the nearest active-path marker under the cursor (ray-vs-AABB over the overlay's
# gizmo AABBs), or {} on a miss. Mirrors _pick_entity but over _marker_pickable.
func _pick_marker(mouse_pos: Vector2) -> Dictionary:
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("get_editor_camera"):
		return {}
	var camera: Camera3D = _c.terrain_editor.get_editor_camera()
	if camera == null:
		return {}
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best: Dictionary = {}
	for rec in _c._marker_pickable:
		var aabb: AABB = rec["aabb"]
		if aabb.size == Vector3.ZERO:
			continue
		var t = _c._viewport._ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = { "path_index": int(rec["path_index"]), "marker_index": int(rec["marker_index"]) }
	return best


func _select_marker(path_index: int, marker_index: int) -> void:
	_c._selected_marker = { "path_index": path_index, "marker_index": marker_index }
	if _c._waypoint_overlay != null and is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay.set_selected_marker(marker_index)
	_c._notify_changed()


func _deselect_marker() -> void:
	if _c._selected_marker.is_empty():
		return
	_c._selected_marker = {}
	if _c._waypoint_overlay != null and is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay.set_selected_marker(-1)
	_c._notify_changed()


# --- Authoring (P7d): marker drag / add / reorder / delete --------------------
# Marker editing reuses the object authoring spine: the terrain-regrounding drag and the
# begin_edit/commit_edit undo bracketing (each gesture is one step). A
# drag previews the gizmo and commits the record once on release; add / delete are
# structural (they change the marker list), so they re-bake; reorder / flags rewrite only
# the path's reference list.

# Re-ground the dragged marker on the terrain each motion: preview the gizmo only (the
# record is written once, on release), so a drag is one undo step.
func _on_marker_drag(mouse_pos: Vector2) -> void:
	if _c._selected_marker.is_empty() or _c.terrain_editor == null or not _c.terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = _c.terrain_editor.raycast_terrain_at(mouse_pos)
	if not _c.terrain_editor.is_valid_terrain_hit(hit):
		_c._drag_off_terrain = true
		return
	var container = _c._objects_container()
	if container == null:
		return
	_c._drag_off_terrain = false
	_c._drag_moved = true
	_c._marker_drag_local = container.global_transform.affine_inverse() * hit
	if _c._waypoint_overlay != null and is_instance_valid(_c._waypoint_overlay):
		_c._waypoint_overlay.preview_marker_position(int(_c._selected_marker["marker_index"]), _c._marker_drag_local)


func _on_marker_left_release() -> void:
	if _c._drag_active and _c._drag_moved and not _c._selected_marker.is_empty():
		_commit_marker_drag()
	elif _c._drag_active and _c._drag_off_terrain and not _c._drag_moved:
		# A marker dragged only over off-terrain space moved nothing; snap the previewed gizmo
		# back to its stored position and say why.
		_c._report("Drag ended off the terrain; the marker was not moved.")
		_refresh_waypoint_overlay()
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# Push the drag as one step (no-op for a plain click: nothing was written).
	_c.commit_edit()


# Write the dragged marker's new position back to its KIND_MARKER record (keeping its
# rotation), then re-snap the overlay to the committed value.
func _commit_marker_drag() -> void:
	if _c._selected_marker.is_empty() or _c._mission == null:
		return
	var marker_index := int(_c._selected_marker["marker_index"])
	var bms_pos = _c.MissionObjectPlacer.godot_to_bms_position(_c._marker_drag_local)
	var entity = _c._mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)
	var rot: Vector3 = entity.get("rotation_deg", Vector3.ZERO)
	if _c._mission.set_entity_transform(NovaMissionData.KIND_MARKER, marker_index, bms_pos, rot):
		_refresh_waypoint_overlay()
		_c.mark_dirty()


# --- Add marker (placement tool) ---------------------------------------------

# Arm the "add marker" tool: a terrain click then adds a marker to the active path. Needs
# an active path; drops any marker selection so the inspector shows the placement state.
func arm_marker_placement() -> void:
	if _c._mission == null or _c._selected_path_index < 0:
		return
	_c._flush_edit()
	_c._marker_place_armed = true
	_deselect_marker()
	_c._notify_changed()


func disarm_marker_placement() -> void:
	if not _c._marker_place_armed:
		return
	_c._marker_place_armed = false
	_c._notify_changed()


func is_marker_placement_armed() -> bool:
	return _c._marker_place_armed


# Add a marker to the active path at a world-space ground point (append). One call both
# creates the KIND_MARKER entity and links it into the path (the lib's add_waypoint_marker).
# A new marker entity does not shift any object indices, so only the overlay is rebuilt.
# Selects the new marker and dirties. Public so it is testable without a camera. Returns
# false if there is no active path / container or the lib rejects the add.
func add_marker_to_active_path_at_world(global_hit: Vector3) -> bool:
	if _c._mission == null or _c._selected_path_index < 0:
		return false
	var container = _c._objects_container()
	if container == null:
		return false
	var local = container.global_transform.affine_inverse() * global_hit
	var bms_pos = _c.MissionObjectPlacer.godot_to_bms_position(local)
	_c._flush_edit()
	_c._mission.begin_edit()
	var result = _c._mission.add_path_marker_grounded(_c._selected_path_index, bms_pos)
	if result.is_empty():
		_c._report("Could not add a waypoint marker.", true)
		return false
	_c._mission.commit_edit()
	# A new marker entity grew the entity set: invalidate the cached pickers (added incrementally via
	# the overlay rather than _rebake_objects).
	_c._membership_rev += 1
	_refresh_waypoint_overlay()
	var marker_index := int((result.get("marker", {}) as Dictionary).get("index", -1))
	if marker_index >= 0:
		_select_marker(_c._selected_path_index, marker_index)
	_c.mark_dirty()
	return true



# Raycast the terrain under the cursor and add a marker there; a miss (off the terrain) is
# ignored. Stays armed so several can be placed.
func _place_marker_armed_at(mouse_pos: Vector2) -> void:
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = _c.terrain_editor.raycast_terrain_at(mouse_pos)
	if not _c.terrain_editor.is_valid_terrain_hit(hit):
		return
	add_marker_to_active_path_at_world(hit)


# --- Reorder / delete / clear -------------------------------------------------

# Move the selected marker one step earlier (-1) or later (+1) along the active path. This
# rewrites only the path's reference order (no marker entity changes), so it rebuilds just
# the overlay. One undo step. Inert at the ends or without a marker selection.
func move_selected_marker(delta: int) -> void:
	if _c._mission == null or _c._selected_marker.is_empty() or _c._selected_path_index < 0:
		return
	var path = _c._mission.get_waypoint_path(_c._selected_path_index)
	if path.is_empty():
		return
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	var marker_index := int(_c._selected_marker["marker_index"])
	var pos := indices.find(marker_index)
	if pos < 0:
		return
	var target := pos + delta
	if target < 0 or target >= indices.size():
		return
	var tmp := indices[pos]
	indices[pos] = indices[target]
	indices[target] = tmp
	_c._edit_step(func(): return _c._mission.set_waypoint_path(_c._selected_path_index, indices, int(path.get("flags", 0))),
		"", _refresh_waypoint_overlay)


# Delete the selected marker entirely: remove_entity drops the KIND_MARKER entity and
# repairs every path that referenced it (drops the index, decrements higher ones). Markers
# reindex, so re-bake from the post-delete record. One undo step. Returns false if nothing
# is selected or the lib rejects it; clears the marker selection on success.
func delete_selected_marker() -> bool:
	if _c._mission == null or _c._selected_marker.is_empty():
		return false
	var marker_index := int(_c._selected_marker["marker_index"])
	_c._flush_edit()
	_c._mission.begin_edit()
	if not _c._mission.remove_entity(NovaMissionData.KIND_MARKER, marker_index):
		return false
	_c._mission.commit_edit()
	_c._rebake_objects()
	_c.mark_dirty()
	return true


# Empty the active path AND delete its marker entities, so no orphaned markers are left
# behind (the path's references alone would orphan the nodes). Removes markers in descending
# index order so each removal stays valid; remove_entity repairs the path as it goes. One
# undo step. Returns false when the path is already empty.
func clear_active_path() -> bool:
	if _c._mission == null or _c._selected_path_index < 0:
		return false
	var path = _c._mission.get_waypoint_path(_c._selected_path_index)
	if path.is_empty():
		return false
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	if indices.is_empty():
		return false
	# Dedup before removing: a (corrupt/hand-edited) path can list the same marker index twice, and
	# removing descending would delete the duplicate's now-shifted neighbour on the second pass.
	var descending: Array = []
	for mi in indices:
		var idx := int(mi)
		if not descending.has(idx):
			descending.append(idx)
	descending.sort()
	descending.reverse()
	_c._flush_edit()
	_c._mission.begin_edit()
	var removed := false
	for mi in descending:
		if _c._mission.remove_entity(NovaMissionData.KIND_MARKER, mi):
			removed = true
	if not removed:
		return false
	_c._mission.commit_edit()
	_c._selected_marker = {}
	_c._rebake_objects()
	_c.mark_dirty()
	return true


# --- Authoring (Phase 2): area triggers / zones -------------------------------
# Zone authoring mirrors the marker spine: ray-vs-AABB picking over the overlay's zone body
# AABBs, a terrain-projected translate drag that previews the box and commits the record once
# on release (the begin_edit/commit_edit bracket makes it one undo step), and the same
# begin_edit/commit_edit bracket for one-shot mutations (add / set / flags / delete). Resize is
# precise through the inspector spins (set_selected_zone_bounds); the in-world drag translates
# the whole box. The engine does not auto-swap area-trigger bounds, so the binding normalizes
# min<=max on every write (NovaMissionData.add/set_area_trigger).
