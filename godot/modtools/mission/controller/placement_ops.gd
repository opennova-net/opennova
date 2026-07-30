extends "res://modtools/mission/controller/controller_section.gd"

# Place new objects: the armed placement tool + incremental
# render of a placed entity.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# The placeable items for the palette: every items.def entry, as { id, display_name, type },
# in the database's stable display order. Markers are included (placed as gizmo entities).
# Empty until a mission (hence a resource root + items.def) is loaded.
func get_placeable_items() -> Array:
	var db := _item_db()
	if db == null:
		return []
	var out: Array = []
	for item in db.get_items():
		var entry: Dictionary = item
		out.append({
			"id": int(entry.get("id", 0)),
			"display_name": String(entry.get("display_name", "")),
			"type": int(entry.get("type", 0)),
		})
	return out


# Arm placement for an items.def item id. A later terrain click places it. Rejects
# unknown ids. Drops any current selection so the inspector shows the placement
# affordance rather than an edit panel. Marker items arm too (placed as gizmo entities).
func arm_placement(item_id: int) -> void:
	if _c._mission == null:
		return
	var db := _item_db()
	if db == null or not db.has_item(item_id):
		return
	# Arming is a new action: close any open transform session as its own undo step first.
	_c._flush_edit()
	_c._place_item_id = item_id
	_c._viewport._deselect()
	_c._notify_changed()


func disarm_placement() -> void:
	if _c._place_item_id == 0:
		return
	_c._place_item_id = 0
	# Placement suppresses the gizmo (its want-gate excludes is_placement_armed). A just-placed
	# entity stays selected, so once disarmed re-show its gizmo (no-op when nothing is selected).
	_c._viewport._refresh_gizmo()
	_c._notify_changed()


func is_placement_armed() -> bool:
	return _c._place_item_id != 0


func get_placement_item_id() -> int:
	return _c._place_item_id


# Place a new instance of `item_id` at a world-space ground point, with zero rotation.
# Derives the entity kind from the item's type, writes the record (add_entity), renders
# it incrementally, dirties, and selects the new entity. Returns false (with a status)
# if there is no mission / container or the lib rejects the add. Public so it is
# directly testable without a camera + terrain raycast.
func place_entity_at_world(item_id: int, global_hit: Vector3) -> bool:
	if _c._mission == null:
		return false
	var container = _c._objects_container()
	if container == null:
		return false
	var db := _item_db()
	var item_type := db.get_item_type(item_id) if db != null else -1
	var kind := NovaMissionData.kind_for_item_type(item_type)
	# A readable label for the status line: the model name when resolvable, else the raw id.
	var item_name: String = db.get_display_name(item_id) if db != null and db.has_item(item_id) else ""
	if item_name.is_empty():
		item_name = "item %d" % item_id
	var local = container.global_transform.affine_inverse() * global_hit
	# The list selection + Ground-userpoint bake [orig: sub_401A90, dfx2med.exe] live in the
	# engine's authoring facade; the editor only converts the hit/anchor to mission space.
	var anchor_bms := Vector3.ZERO
	if _c._placer != null and kind != NovaMissionData.KIND_MARKER:
		anchor_bms = _c._placer.ground_anchor_bms(_c._placer.graphic_for(item_id))
	# Placing is its own undo step: close any open session, then bracket the add with
	# begin_edit/commit_edit (commit pushes one step iff the add changed the document).
	_c._flush_edit()
	_c._mission.begin_edit()
	var record = _c._mission.place_entity_grounded(item_id, item_type, _c.MissionObjectPlacer.godot_to_bms_position(local), anchor_bms)
	if record.is_empty():
		# Balance the begin_edit() bracket on the reject path (no-op step, the failed add changed
		# nothing) so the open session does not leak into the next gesture.
		_c._mission.commit_edit()
		_c._report("Could not place %s." % item_name, true)
		return false
	_c._mission.commit_edit()
	var new_index := int(record.get("index", -1))
	# The entity set grew: invalidate the inspector's cached group / waypoint-path / entity pickers.
	# (Placement renders incrementally rather than through _rebake_objects, which is the other bump site.)
	_c._membership_rev += 1
	# Markers are mesh-less: the placer skips them, so render via the marker overlay (rebuild so the
	# new gizmo + pickable exist before we select it). Mesh entities render incrementally.
	if kind == NovaMissionData.KIND_MARKER:
		_c._waypoints._refresh_marker_overlay()
	else:
		_render_placed_entity(kind, new_index)
	_c.mark_dirty()
	_c._viewport._select(kind, new_index)
	_c._report("Placed %s. Ctrl+Z to undo." % item_name)
	return true



func _item_db() -> NovaItemDatabase:
	if _c._placer == null:
		return null
	return _c._placer.get_item_db()


# Raycast the terrain under the cursor and place the armed item there. A miss (off the
# terrain) is ignored so a stray click into the sky does nothing.
func _place_armed_at(mouse_pos: Vector2) -> void:
	if not _c.terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = _c.terrain_editor.raycast_terrain_at(mouse_pos)
	if not _c.terrain_editor.is_valid_terrain_hit(hit):
		return
	place_entity_at_world(_c._place_item_id, hit)


# Render a just-added entity into the live container and fold its counts into the
# displayed stats, reusing the retained placer's caches.
func _render_placed_entity(kind: int, index: int) -> void:
	if _c._placer == null:
		return
	var container = _c._objects_container()
	if container == null:
		return
	# place_single already added this entity's pick collider node under the container.
	var delta: Dictionary = _c._placer.place_single(_c._mission, container, kind, index, _c._environment_node())
	_c._pickable = _c._placer.pickable_records
	for key in delta:
		_c._stats[key] = int(_c._stats.get(key, 0)) + int(delta[key])
	_c._viewport._refresh_pick_debug()


# --- Authoring (Phase 4): delete + structural re-bake -------------------------
# Deleting an entity is structural: the lib erases it from its kind's list, so every
# later entity of that kind shifts down one index. The pickable index and MultiMesh
# slot mapping were built from the old indices, so rather than patch them in place we
# re-bake the whole MissionObjects container from the post-delete record — correct by
# construction, and cheap because the retained placer keeps its model + batch caches.
