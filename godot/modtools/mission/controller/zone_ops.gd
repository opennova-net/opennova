extends "res://modtools/mission/controller/controller_section.gd"

# Area triggers / zones: selection, overlay, pick / drag / commit /
# delete.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

func get_selected_zone_index() -> int:
	return _c._selected_zone_index


# The selected zone dict (NovaMissionData shape), or {} when none is selected / no mission.
func get_selected_zone() -> Dictionary:
	if _c._mission == null or _c._selected_zone_index < 0:
		return {}
	return _c._mission.get_area_trigger(_c._selected_zone_index)


# Select a zone by index (the inspector list drives this). Rebuilds the overlay so the bright
# box + grab cube follow. Inert if unchanged.
func select_area_trigger(index: int) -> void:
	if index == _c._selected_zone_index:
		return
	_c._selected_zone_index = index
	_refresh_area_trigger_overlay()
	_c._notify_changed()


# Add a new zone box centred on the placed world (the average of item positions, else origin),
# select it, and dirty. Public so it is testable without a camera. Returns the new index, or -1.
func add_area_trigger_default() -> int:
	if _c._mission == null:
		return -1
	var center = _c._world_center_mission()
	var half = _c.DEFAULT_ZONE_HALF
	_c._flush_edit()
	_c._mission.begin_edit()
	# A fresh zone is active with Z unbounded (the common out-of-bounds region); the user
	# constrains Z and resizes afterwards.
	var zone = _c._mission.add_area_trigger(center - half, center + half, true, false, 0)
	if zone.is_empty():
		_c._report("Could not add an area trigger.", true)
		return -1
	_c._mission.commit_edit()
	_c._selected_zone_index = int(zone.get("index", -1))
	_refresh_area_trigger_overlay()
	_c.mark_dirty()
	return _c._selected_zone_index


# Overwrite the selected zone's bounds (mission space) from the inspector spins. One undo step.
func set_selected_zone_bounds(mn: Vector3, mx: Vector3) -> void:
	if _c._mission == null or _c._selected_zone_index < 0:
		return
	var zone = _c._mission.get_area_trigger(_c._selected_zone_index)
	if zone.is_empty():
		return
	_c._edit_step(func(): return _c._mission.set_area_trigger(_c._selected_zone_index, mn, mx,
			bool(zone.get("active", false)), bool(zone.get("constrain_z", false)), int(zone.get("id", 0))),
		"", _refresh_area_trigger_overlay)


# Set the selected zone's two known flag bits (active / constrain-Z). One undo step.
func set_selected_zone_flags(active: bool, constrain_z: bool) -> void:
	if _c._mission == null or _c._selected_zone_index < 0:
		return
	var zone = _c._mission.get_area_trigger(_c._selected_zone_index)
	if zone.is_empty():
		return
	_c._edit_step(func(): return _c._mission.set_area_trigger(_c._selected_zone_index, zone.get("min", Vector3.ZERO),
			zone.get("max", Vector3.ZERO), active, constrain_z, int(zone.get("id", 0))),
		"", _refresh_area_trigger_overlay)


# Delete the selected zone. Structural (shifts later indices), so the overlay rebuilds and the
# selection drops. *IsWithinArea trigger param2 references are auto-repaired in the lib (Phase-5 RE
# confirmed param2 is an array index): higher refs shift down, a direct hit becomes -1 (dangling, which
# the scripting diagnostics then flag). One undo step. False if none selected.
func delete_selected_area_trigger() -> bool:
	if _c._mission == null or _c._selected_zone_index < 0:
		return false
	_c._flush_edit()
	_c._mission.begin_edit()
	if not _c._mission.remove_area_trigger(_c._selected_zone_index):
		return false
	_c._mission.commit_edit()
	_c._selected_zone_index = -1
	_refresh_area_trigger_overlay()
	_c._report("Zone deleted. Triggers that referenced a higher zone shifted down; a direct reference was unset.")
	_c.mark_dirty()
	return true


# (Re)build the in-world zone overlay from the current mission, harvesting the zone pickables.
# Creates the overlay node under the objects container on first use (and after a re-bake freed
# it). `preview` optionally overrides the dragged zone's bounds. Mirrors _refresh_waypoint_overlay.
func _refresh_area_trigger_overlay(preview := {}) -> void:
	if _c._mission == null:
		return
	var container = _c._objects_container()
	if container == null:
		return
	if _c._area_overlay == null or not is_instance_valid(_c._area_overlay):
		_c._area_overlay = _c.MissionAreaTriggerOverlay.new()
		_c._area_overlay.name = "MissionAreaTriggerOverlay"
		_c._area_overlay.visible = _c._mode == _c.Mode.AREA_TRIGGERS
		container.add_child(_c._area_overlay)
	_c._area_overlay.rebuild(_c._mission, _c._selected_zone_index, preview)
	_c._zone_pickable = _c._area_overlay.zone_pickables()


func _on_zone_left_press(mouse_pos: Vector2) -> void:
	_c._flush_edit()
	var index := _pick_zone(mouse_pos)
	if index < 0:
		_deselect_zone()
		return
	if index != _c._selected_zone_index:
		_c._selected_zone_index = index
		_refresh_area_trigger_overlay()
		_c._notify_changed()
	# Begin a translate drag: motion re-grounds the box centre on the terrain, release writes
	# the record once (a plain click just selects). The bracket makes the drag one undo step.
	var zone = _c._mission.get_area_trigger(index)
	_c._zone_drag_min = zone.get("min", Vector3.ZERO)
	_c._zone_drag_max = zone.get("max", Vector3.ZERO)
	_c._zone_preview_min = _c._zone_drag_min
	_c._zone_preview_max = _c._zone_drag_max
	_c._drag_active = true
	_c._drag_moved = false
	_c._drag_off_terrain = false
	_c.begin_edit()


# Pick the nearest zone under the cursor (ray-vs-AABB over the overlay's body AABBs), or -1 on
# a miss. Mirrors _pick_marker.
func _pick_zone(mouse_pos: Vector2) -> int:
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("get_editor_camera"):
		return -1
	var camera: Camera3D = _c.terrain_editor.get_editor_camera()
	if camera == null:
		return -1
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best := -1
	for rec in _c._zone_pickable:
		var aabb: AABB = rec["aabb"]
		if aabb.size == Vector3.ZERO:
			continue
		var t = _c._viewport._ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = int(rec["zone_index"])
	return best


# Translate the selected box horizontally to follow the terrain hit (the vertical extent is
# left unchanged). Previews the overlay box only; the record commits once on release.
func _on_zone_drag(mouse_pos: Vector2) -> void:
	if _c._selected_zone_index < 0 or _c.terrain_editor == null or not _c.terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = _c.terrain_editor.raycast_terrain_at(mouse_pos)
	if not _c.terrain_editor.is_valid_terrain_hit(hit):
		_c._drag_off_terrain = true
		return
	if not _c._drag_moved:
		# First valid sample anchors the drag so the box does not jump to the cursor.
		_c._zone_drag_start_hit = hit
	_c._drag_off_terrain = false
	_c._drag_moved = true
	# Godot (x, z) map to mission (x, -y); the vertical (godot y / mission z) extent is kept.
	var d = hit - _c._zone_drag_start_hit
	var mission_delta := Vector3(d.x, -d.z, 0.0)
	_c._zone_preview_min = _c._zone_drag_min + mission_delta
	_c._zone_preview_max = _c._zone_drag_max + mission_delta
	_refresh_area_trigger_overlay({ "index": _c._selected_zone_index, "min": _c._zone_preview_min, "max": _c._zone_preview_max })


func _on_zone_left_release() -> void:
	if _c._drag_active and _c._drag_moved and _c._selected_zone_index >= 0:
		_commit_zone_drag()
	elif _c._drag_active and _c._drag_off_terrain and not _c._drag_moved:
		_c._report("Drag ended off the terrain; the zone was not moved.")
		_refresh_area_trigger_overlay()
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# Push the drag as one step (no-op for a plain click: nothing was written).
	_c.commit_edit()


# Write the dragged box's previewed bounds back to the record, keeping its flags + id.
func _commit_zone_drag() -> void:
	if _c._mission == null or _c._selected_zone_index < 0:
		return
	var zone = _c._mission.get_area_trigger(_c._selected_zone_index)
	if zone.is_empty():
		return
	var updated = _c._mission.set_area_trigger(_c._selected_zone_index, _c._zone_preview_min, _c._zone_preview_max,
		bool(zone.get("active", false)), bool(zone.get("constrain_z", false)), int(zone.get("id", 0)))
	if not updated.is_empty():
		_refresh_area_trigger_overlay()
		_c.mark_dirty()


func _deselect_zone() -> void:
	if _c._selected_zone_index < 0:
		return
	_c._selected_zone_index = -1
	_refresh_area_trigger_overlay()
	_c._notify_changed()


# --- Authoring (Phase 4): mission scripting forwarders ------------------------
# Panel-driven (no viewport interaction): the inspector's Scripting tab calls these, each on the same
# begin_edit -> mutate -> commit_edit -> mark_dirty undo recipe as the other modes. Reads pass through
# to the binding; every read tolerates "no mission" by returning an empty value.
