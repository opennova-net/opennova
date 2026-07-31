extends "res://modtools/mission/controller/controller_section.gd"

# Viewport authoring: select + terrain-plane drag, the transform
# gizmo, pick bodies + pick debug, hover, the selection box, and
# the selection geometry helpers.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

func handle_viewport_input(event: InputEvent) -> void:
	if _c._mission == null or _c.terrain_editor == null:
		return
	# Scripting mode is panel-driven and the viewport is inert in it (see Mode docs): swallow all
	# pointer events so a stray click cannot select, drag, or hover-pick an object, while still
	# letting the keyboard shortcuts (undo / redo / delete) below run.
	if _c.is_scripting_mode() and not (event is InputEventKey):
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		# A non-left button pressed mid gizmo-drag (e.g. right-click to look / middle to orbit) ends
		# the drag and restores the grab-time pose. Otherwise FlyCamera (which also sees the event)
		# would move the camera under the gizmo's frozen drag plane, flinging the selection across the
		# map as the same cursor pixel reprojects. cancel_drag() rolls back to the snapshot.
		if mb.pressed and mb.button_index != MOUSE_BUTTON_LEFT and not _c._gizmo_drag.is_empty():
			cancel_drag()
		# Right-click while armed cancels the placement tool (a familiar "drop the tool"
		# gesture) and does not fall through to selection -- objects in objects mode, the
		# add-marker tool in waypoints mode.
		if mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
			if _c._placement.is_placement_armed():
				_c._placement.disarm_placement()
				return
			if _c._marker_place_armed:
				_c._waypoints.disarm_marker_placement()
				return
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if mb.pressed:
			# Waypoints mode: left-click selects (or, when armed, adds) the active path's
			# markers, and starts a marker drag -- never objects (the modes are exclusive).
			if _c._mode == _c.Mode.WAYPOINTS:
				_c._waypoints._on_marker_left_press(mb.position)
			# Area-trigger mode: left-click selects a zone and starts a translate drag.
			elif _c._mode == _c.Mode.AREA_TRIGGERS:
				_c._zones._on_zone_left_press(mb.position)
			# Armed: left-click places a new instance at the cursor instead of selecting.
			# Stay armed so the user can place several; right-click / Escape / the Stop
			# button disarms.
			elif _c._placement.is_placement_armed():
				_c._placement._place_armed_at(mb.position)
			else:
				_on_left_press(mb.position)
		else:
			if _c._mode == _c.Mode.WAYPOINTS:
				_c._waypoints._on_marker_left_release()
			elif _c._mode == _c.Mode.AREA_TRIGGERS:
				_c._zones._on_zone_left_release()
			elif not _c._gizmo_drag.is_empty():
				_on_gizmo_release()
			else:
				_on_left_release()
	elif event is InputEventKey:
		var key := event as InputEventKey
		# Ignore key-up and auto-repeat echoes (holding the key must not chain actions).
		if not key.pressed or key.echo:
			return
		# Undo / redo keyboard shortcuts live in the shell's _shortcut_input
		# (B6); the controller's undo() itself keeps the sim gate.
		if key.keycode == KEY_ESCAPE:
			# Escape drops whichever placement tool is armed (object or add-marker).
			if _c._placement.is_placement_armed():
				_c._placement.disarm_placement()
			elif _c._marker_place_armed:
				_c._waypoints.disarm_marker_placement()
		elif (key.keycode == KEY_DELETE or key.keycode == KEY_BACKSPACE) and not key.ctrl_pressed:
			# Delete the current selection, unless a GUI control owns the keyboard. The router
			# feeds us via _unhandled_input, which only withholds keys a focused control
			# actually consumes -- a SpinBox holding focus via its arrows, an ItemList, or a
			# Button do NOT consume Delete/Backspace, so without this guard a stray Backspace
			# while editing a field would silently delete. Mirrors the focus-owner guard in
			# credits_editor / fnt_editor / terrain_editor. Mode-scoped: a marker in waypoints
			# mode, an object otherwise.
			if not _c._gui_focus_blocks_shortcut():
				if _c._mode == _c.Mode.WAYPOINTS and not _c._selected_marker.is_empty():
					_c._waypoints.delete_selected_marker()
				elif _c._mode == _c.Mode.AREA_TRIGGERS and _c._selected_zone_index >= 0:
					_c._zones.delete_selected_area_trigger()
				elif _c._mode == _c.Mode.OBJECTS and not _c._selected_ref.is_empty():
					_c.delete_selected()
	elif event is InputEventMouseMotion and _c._drag_active:
		var motion := event as InputEventMouseMotion
		# Defend against a missed button-up (e.g. the release landed on a different
		# control while switching workspaces): if the left button is no longer held,
		# the gesture was abandoned, not continuing, so end it without committing.
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) == 0:
			cancel_drag()
			return
		# Drag the active mode's selection: a marker in waypoints mode, a zone in area-trigger
		# mode, an object otherwise.
		if _c._mode == _c.Mode.WAYPOINTS:
			_c._waypoints._on_marker_drag(motion.position)
		elif _c._mode == _c.Mode.AREA_TRIGGERS:
			_c._zones._on_zone_drag(motion.position)
		elif not _c._gizmo_drag.is_empty():
			_on_gizmo_drag(motion.position)
		else:
			_on_drag(motion.position)
	elif event is InputEventMouseMotion:
		# Bare hover (not dragging): highlight the gizmo handle under the cursor + preview the
		# object that a click would select (objects mode).
		var hover_pos := (event as InputEventMouseMotion).position
		_update_gizmo_hover(hover_pos)
		_on_hover(hover_pos)


# End an in-progress drag without committing. The workspace calls this when it
# deactivates / unmounts so a half-finished gesture cannot silently resume (and
# relocate + dirty the selection) on a later bare hover after the user returns.
func cancel_drag() -> void:
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# Close any open edit session. A drag is visual-only until release commits it, so a
	# cancelled drag leaves the document unchanged and this pushes nothing; an inspector edit
	# session that happens to be open keeps its undo step (commit, not discard, so a workspace
	# switch mid-edit does not silently drop the step).
	_c.commit_edit()
	# A cancelled transform-gizmo drag previewed a move/rotate but wrote no record; restore the
	# selection to the snapshot taken at grab time, then drop the gizmo drag + highlight.
	if not _c._gizmo_drag.is_empty():
		_c._gizmo_drag = {}
		if not _c._selected_ref.is_empty():
			_c._selected_rotation_deg = _c._gizmo_start_rot
			_apply_selected_xform(Transform3D(_c.MissionObjectPlacer.bms_to_godot_basis(_c._gizmo_start_rot), _c._gizmo_start_origin))
		if _c._gizmo != null and is_instance_valid(_c._gizmo):
			_c._gizmo.end_drag()
			_c._gizmo.set_highlight({})
	# A cancelled marker / zone drag previewed the gizmo but wrote no record; snap it back to
	# the stored position by rebuilding the active mode's overlay.
	_c._refresh_active_overlay()


func _on_left_press(mouse_pos: Vector2) -> void:
	# Close any open inspector edit session as its own step before starting a new gesture,
	# so SpinBox edits and a following drag never coalesce.
	_c._flush_edit()
	# Drop the hover highlight so it does not linger over the entity we are selecting.
	_clear_hover()
	# A grab on the transform gizmo's handle takes priority over (re)selection / free-drag: it
	# manipulates the already-selected object along that axis / ring. Only when the cursor misses
	# every handle does a left-press fall through to picking + the terrain free-drag below.
	if _begin_gizmo_drag(mouse_pos):
		return
	var ref := _pick_entity(mouse_pos)
	if ref.is_empty():
		_deselect()
		return
	_select(int(ref["kind"]), int(ref["index"]))
	_c._drag_active = true
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# Snapshot the pre-drag state; _on_left_release commits it as one step iff the entity
	# actually moved.
	_c.begin_edit()


func _on_drag(mouse_pos: Vector2) -> void:
	if _c._selected_ref.is_empty() or _c.terrain_editor == null or not _c.terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = _c.terrain_editor.raycast_terrain_at(mouse_pos)
	if not _c.terrain_editor.is_valid_terrain_hit(hit):
		# Off the terrain: leave the object at its last valid spot and remember the miss so
		# the release can explain a drag that never landed anywhere.
		_c._drag_off_terrain = true
		return
	_c._drag_off_terrain = false
	_c._drag_moved = true
	_move_selected_to_world(hit)


func _on_left_release() -> void:
	if _c._drag_active and _c._drag_moved:
		_commit_selected_transform()
	elif _c._drag_active and _c._drag_off_terrain and not _c._drag_moved:
		# A drag that only ever sampled off-terrain moved nothing; say so rather than leaving
		# the user wondering why the object stayed put.
		_c._report("Drag ended off the terrain; the object was not moved.")
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# Push the drag as one undo step (no-op for a plain click: the bytes are unchanged).
	_c.commit_edit()


# --- Transform gizmo ----------------------------------------------------------
# The in-world gizmo (framework TransformGizmo3D, bms basis_builder injected) draws translate
# arrows + rotate rings on the selected object and returns drag deltas; the controller applies
# them through the same _apply_selected_xform / _commit_selected_transform spine as the terrain
# drag + numeric edits, so undo + the inspector stay in lockstep. Objects only (markers keep
# their terrain-drag). The node is a child of the MissionObjects container so it frees with a
# re-bake.

func is_gizmo_enabled() -> bool:
	return _c._gizmo_enabled


func set_gizmo_enabled(value: bool) -> void:
	_c._gizmo_enabled = value
	_refresh_gizmo()


# The editor camera, or null (headless tests / no terrain editor bound).
func _editor_camera() -> Camera3D:
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("get_editor_camera"):
		return null
	return _c.terrain_editor.get_editor_camera()


# Try to start a gizmo handle drag at `mouse_pos`. Returns true (and arms the drag) when the cursor
# is over an arrow / ring of the visible gizmo; false otherwise so the caller falls through to
# picking + the terrain free-drag. Snapshots the selection transform so each motion applies an
# absolute delta from the grab (no drift), and opens the same begin_edit/commit_edit undo bracket.
func _begin_gizmo_drag(mouse_pos: Vector2) -> bool:
	if _c._gizmo == null or not is_instance_valid(_c._gizmo) or not _c._gizmo.visible or _c._selected_ref.is_empty():
		return false
	var camera := _editor_camera()
	if camera == null:
		return false
	var handle: Dictionary = _c._gizmo.pick_handle(camera, mouse_pos)
	if handle.is_empty():
		return false
	_c._gizmo_drag = handle
	_c._gizmo_start_origin = _c._selected_xform.origin
	_c._gizmo_start_rot = _c._selected_rotation_deg
	_c._gizmo.begin(handle, camera, mouse_pos)
	_c._gizmo.set_highlight(handle)
	_c._drag_active = true
	_c._drag_moved = false
	_c._drag_off_terrain = false
	_c.begin_edit()
	return true


# Apply the gizmo's drag delta to the snapshotted start transform: translate slides the origin
# along a world axis; rotate spins one authored angle (pitch/yaw/roll). Previews via
# _apply_selected_xform (which moves the mesh, pick body, selection box, and the gizmo); the record
# commits once on release.
func _on_gizmo_drag(mouse_pos: Vector2) -> void:
	if _c._gizmo_drag.is_empty() or _c._selected_ref.is_empty() or _c._gizmo == null or not is_instance_valid(_c._gizmo):
		return
	var camera := _editor_camera()
	if camera == null:
		return
	var d: Dictionary = _c._gizmo.update(camera, mouse_pos)
	if d.has("translate"):
		var world_delta: Vector3 = d["translate"]
		var local_delta := world_delta
		var container = _c._objects_container()
		if container != null:
			local_delta = container.global_transform.basis.inverse() * world_delta
		_c._drag_moved = true
		_apply_selected_xform(Transform3D(_c._selected_xform.basis, _c._gizmo_start_origin + local_delta))
	elif d.has("rotate_deg"):
		var axis := int(_c._gizmo_drag.get("axis", 1))
		var nv := roundf(_c._gizmo_start_rot[axis] + float(d["rotate_deg"]))
		var r = _c._gizmo_start_rot
		if axis == 0:
			r.x = nv
		elif axis == 1:
			r.y = nv
		else:
			r.z = nv
		_c._selected_rotation_deg = r
		_c._drag_moved = true
		_apply_selected_xform(Transform3D(_c.MissionObjectPlacer.bms_to_godot_basis(r), _c._selected_xform.origin))


func _on_gizmo_release() -> void:
	if _c._drag_moved:
		# _commit_selected_transform writes BOTH position and _selected_rotation_deg, so a rotate
		# gesture commits with no extra code.
		_commit_selected_transform()
	_c._gizmo_drag = {}
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	if _c._gizmo != null and is_instance_valid(_c._gizmo):
		_c._gizmo.end_drag()
		_c._gizmo.set_highlight({})
	# Push the gesture as one undo step (no-op when nothing moved), then re-orient the rings to the
	# committed rotation.
	_c.commit_edit()
	_refresh_gizmo()


# Highlight the gizmo handle under the cursor on a bare hover (no drag), for grab feedback.
# Throttled to pixel movement so the (fixed-cost) handle hit-test does not re-run on sub-pixel jitter.
func _update_gizmo_hover(mouse_pos: Vector2) -> void:
	if _c._gizmo == null or not is_instance_valid(_c._gizmo) or not _c._gizmo.visible or not _c._gizmo_drag.is_empty():
		return
	if _c._gizmo_hover_pos.distance_to(mouse_pos) < _c.HOVER_PIXEL_EPSILON:
		return
	_c._gizmo_hover_pos = mouse_pos
	var camera := _editor_camera()
	if camera == null:
		return
	_c._gizmo.set_highlight(_c._gizmo.pick_handle(camera, mouse_pos))


# (Re)build / place / hide the transform gizmo for the current selection. Shown only in Objects
# mode, gizmo enabled, with a non-marker object selected and no placement tool armed. Created lazily
# under the objects container (freed with it on a re-bake; the ref is dropped in
# _reset_selection_state). Re-orients the rings to the selection's current degrees.
func _refresh_gizmo() -> void:
	if _c._mission == null:
		return
	var container = _c._objects_container()
	if container == null:
		return
	var want = _c._gizmo_enabled and _c._mode == _c.Mode.OBJECTS and not _c._placement.is_placement_armed() \
		and not _c._selected_ref.is_empty() and int(_c._selected_ref.get("kind", -1)) != NovaMissionData.KIND_MARKER
	if not want:
		if _c._gizmo != null and is_instance_valid(_c._gizmo):
			_c._gizmo.visible = false
		return
	if _c._gizmo == null or not is_instance_valid(_c._gizmo):
		_c._gizmo = _c.MissionGizmo.new()
		_c._gizmo.name = "MissionTransformGizmo"
		# Mission's authored angles are nested BMS euler, not plain euler: the rings must
		# derive their axes through the same basis the placer renders with.
		_c._gizmo.basis_builder = _c.MissionObjectPlacer.bms_to_godot_basis
		container.add_child(_c._gizmo)
	_c._gizmo.visible = true
	_c._gizmo.show_for(_c._selected_xform.origin, _c._selected_rotation_deg)


# --- Pick bodies --------------------------------------------------------------
# The pick bodies (StaticBody3D + CollisionShape3D, "entity_ref" meta) are created by
# MissionObjectPlacer.add_pick_collider under the MissionObjects container, so they are
# built and freed with the visual world -- nothing to manage here. The selected entity's
# body is looked up by name ("Pick_<kind>_<index>") in _select for live drag.

func _selected_pick_collider() -> Node3D:
	if _c._selected_ref.is_empty():
		return null
	var container = _c._objects_container()
	if container == null:
		return null
	return container.get_node_or_null(NodePath("Pick_%d_%d" % [int(_c._selected_ref["kind"]), int(_c._selected_ref["index"])])) as Node3D


# World-space AABB of a placed entity (union over its pickable records / animated
# node). Used to bracket the hover highlight, mirroring _selected_world_aabb.
func _entity_world_aabb(kind: int, index: int) -> AABB:
	var result := AABB()
	var have := false
	for rec in _c._pickable:
		if int(rec["kind"]) != kind or int(rec["index"]) != index:
			continue
		var a := _record_world_aabb(rec)
		if a.size == Vector3.ZERO:
			continue
		if not have:
			result = a
			have = true
		else:
			result = result.merge(a)
	return result


# --- Pick debug overlay -------------------------------------------------------
# A diagnostic the user can toggle from the object browser: draws every pick body's
# convex collision hull(s) in world (the exact geometry intersect_ray tests), so it
# is obvious whether bodies exist and sit on their objects.

func is_pick_debug() -> bool:
	return _c._pick_debug


func set_pick_debug(value: bool) -> void:
	_c._pick_debug = value
	_refresh_pick_debug()


func _refresh_pick_debug() -> void:
	var container = _c._objects_container()
	if container == null:
		return
	var existing = container.get_node_or_null("MissionPickDebug")
	if existing != null:
		container.remove_child(existing)
		existing.queue_free()
	if not _c._pick_debug or _c._mission == null or _c._placer == null:
		return
	var root := Node3D.new()
	root.name = "MissionPickDebug"
	container.add_child(root)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(0.30, 1.0, 0.45, 0.9)
	mat.no_depth_test = true
	var seen: Dictionary = {}
	for rec in _c._pickable:
		var kind := int(rec["kind"])
		var index := int(rec["index"])
		var key := "%d:%d" % [kind, index]
		if seen.has(key):
			continue
		seen[key] = true
		var graphic := String(rec.get("graphic", ""))
		if graphic.is_empty():
			continue
		var shapes: Array = _c._placer.collision_shapes_for(graphic)
		if shapes.is_empty():
			continue
		# Container-local transform of the body (= world / container.global_transform).
		var entity := _find_entity(kind, index)
		var local: Transform3D = _c.MissionObjectPlacer.entity_transform(
			entity.get("position", Vector3.ZERO), entity.get("rotation_deg", Vector3.ZERO))
		for shape in shapes:
			var mi := MeshInstance3D.new()
			mi.mesh = (shape as Shape3D).get_debug_mesh()
			mi.material_override = mat
			mi.transform = local
			mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			root.add_child(mi)


func _pick_entity(mouse_pos: Vector2) -> Dictionary:
	if not _c.terrain_editor.has_method("get_editor_camera"):
		return {}
	var camera: Camera3D = _c.terrain_editor.get_editor_camera()
	if camera == null:
		return {}
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best: Dictionary = {}
	# Objects: exact ray-vs-convex-hull via the viewport world's stepped physics space
	# (BVH broadphase, nearest hit). The hit StaticBody3D carries its (kind,index) in its
	# "entity_ref" meta.
	var container = _c._objects_container()
	if container != null and container.is_inside_tree():
		var world = container.get_world_3d()
		if world != null:
			var ss = world.direct_space_state
			if ss != null:
				var q := PhysicsRayQueryParameters3D.create(from, from + dir * _c.PICK_RAY_LENGTH)
				var hit = ss.intersect_ray(q)
				if not hit.is_empty():
					var collider = hit.get("collider")
					if collider != null and (collider as Object).has_meta("entity_ref"):
						var ref: Dictionary = (collider as Object).get_meta("entity_ref")
						best = { "kind": int(ref["kind"]), "index": int(ref["index"]) }
						best_t = from.distance_to(hit["position"])
	# Markers are mesh-less, so they are not bodies; their gizmo AABBs come from the marker
	# overlay. The container sits at the world origin, so the overlay's AABBs are world-space (same
	# assumption as _pick_marker). The nearest of {hull hit, marker gizmo} wins.
	if _c._marker_overlay != null and is_instance_valid(_c._marker_overlay):
		for rec in _c._marker_overlay.marker_pickables():
			var maabb: AABB = rec["aabb"]
			if maabb.size == Vector3.ZERO:
				continue
			var mt := _ray_aabb_entry(maabb, from, dir)
			if mt >= 0.0 and mt < best_t:
				best_t = mt
				best = { "kind": NovaMissionData.KIND_MARKER, "index": int(rec["marker_index"]) }
	return best


func _select(kind: int, index: int) -> void:
	# End any open edit-coalescing window before the selection changes. set_selected_position /
	# set_selected_rotation leave a begin_edit() session open (committed by the next gesture's flush),
	# so an inspector-driven re-select must flush it here too (the viewport's _on_left_press already
	# does), otherwise SpinBox edits to two different objects fold into a single undo step.
	_c._flush_edit()
	_c._preview.stop_preview()
	_clear_selected_user_points()
	_c._selected_ref = { "kind": kind, "index": index }
	_c._selected_records = []
	_c._selected_node = null
	_c._selected_graphic = ""
	_c._selected_node_offset = Transform3D.IDENTITY
	var graphic := ""
	for rec in _c._pickable:
		if int(rec["kind"]) == kind and int(rec["index"]) == index:
			if graphic.is_empty():
				graphic = String(rec.get("graphic", ""))
			# Skip records whose backing node was freed (e.g. a re-bake mid-flight): a stale
			# ref would dangle through _apply_selected_xform. A dropped record just means no
			# box / no drag handle for that slot, not a crash.
			if bool(rec.get("animated", false)):
				var node = rec.get("node")
				if node != null and is_instance_valid(node):
					_c._selected_node = node
					_c._selected_node_offset = rec.get("offset", Transform3D.IDENTITY)
			else:
				var mmi = rec.get("mmi")
				if mmi != null and is_instance_valid(mmi):
					_c._selected_records.append(rec)
	# Bind the entity's pick body node + its anchor so a drag can move the body live
	# (keeps a mid-drag re-pick exact). Markers have no body, so this resolves to null.
	_c._selected_collider = _selected_pick_collider()
	_c._selected_ground_offset = Vector3.ZERO
	if _c._placer != null and not graphic.is_empty():
		_c._selected_graphic = graphic
		_c._selected_ground_offset = _c._placer.ground_anchor_godot(graphic)
	var entity := _find_entity(kind, index)
	_c._selected_rotation_deg = entity.get("rotation_deg", Vector3.ZERO)
	_c._selected_xform = _c.MissionObjectPlacer.entity_transform(
		entity.get("position", Vector3.ZERO), _c._selected_rotation_deg)
	# A marker has no mesh records, so the selection box stays hidden; highlight its gizmo instead.
	if kind == NovaMissionData.KIND_MARKER and _c._marker_overlay != null and is_instance_valid(_c._marker_overlay):
		_c._marker_overlay.set_selected_marker(index)
	_update_selection_box()
	# Show the transform gizmo on this selection (hidden for markers / non-objects modes). Reset the
	# hover throttle so the first motion over the rebuilt gizmo re-highlights.
	_refresh_gizmo()
	_c._gizmo_hover_pos = Vector2(-1, -1)
	_c._notify_changed()


func _deselect() -> void:
	_c._preview.stop_preview()
	_clear_selected_user_points()
	if _c._selected_ref.is_empty():
		return
	_c._selected_ref = {}
	_c._selected_records = []
	_c._selected_node = null
	_c._selected_graphic = ""
	_c._selected_node_offset = Transform3D.IDENTITY
	_c._selected_collider = null
	_c._selected_ground_offset = Vector3.ZERO
	_hide_selection_box()
	if _c._gizmo != null and is_instance_valid(_c._gizmo):
		_c._gizmo.visible = false
	if _c._marker_overlay != null and is_instance_valid(_c._marker_overlay):
		_c._marker_overlay.set_selected_marker(-1)
	_c._notify_changed()


func _refresh_selected_user_points_overlay() -> void:
	if not _c._selected_user_points_visible or not _c.selected_has_user_points():
		_free_selected_user_points_overlay()
		return
	var container = _c._objects_container()
	if container == null:
		_free_selected_user_points_overlay()
		return
	var data = _c._selected_object_data()
	if data == null:
		_free_selected_user_points_overlay()
		return
	if _c._selected_user_point_overlay == null or not is_instance_valid(_c._selected_user_point_overlay):
		_c._selected_user_point_overlay = _c.ObjectUserPointOverlayScript.new()
		_c._selected_user_point_overlay.name = "MissionSelectedUserPoints"
		container.add_child(_c._selected_user_point_overlay)
	_c._selected_user_point_overlay.set_object_data(data)
	if _c._selected_node != null and is_instance_valid(_c._selected_node):
		_c._selected_user_point_overlay.set_source_model(_c._selected_node)
		_c._selected_user_point_overlay.set_entity_transform(Transform3D.IDENTITY)
	else:
		_c._selected_user_point_overlay.set_source_model(null)
		_c._selected_user_point_overlay.set_entity_transform(_c._selected_xform)
	_c._selected_user_point_overlay.refresh_points()
	_c._selected_user_point_overlay.set_points_visible(true)


func _clear_selected_user_points() -> void:
	_c._selected_user_points_visible = false
	_free_selected_user_points_overlay()


func _free_selected_user_points_overlay() -> void:
	if _c._selected_user_point_overlay != null and is_instance_valid(_c._selected_user_point_overlay):
		_c._selected_user_point_overlay.queue_free()
	_c._selected_user_point_overlay = null


# --- In-editor PLAYPARTANIM preview -------------------------------------------
# Play a scripting PLAYPARTANIM action's part animation on its target model in the editor viewport so an
# author can see the motion without launching the game. Reuses the runtime path: it resolves the action's
# target (SSN / group / zone) through the same MissionEntityRegistry the mount uses, then drives
# NovaObjectModel.restart_part_anim (a clean-from-rest variant of the runtime play_part_anim). The placed
# model already _process-ticks in the viewport, so the sweep animates live.

# Move the selected entity so its origin sits at a world-space ground point: keep the
# current rotation, only the origin tracks the cursor.
func _move_selected_to_world(global_hit: Vector3) -> void:
	var container = _c._objects_container()
	if container == null:
		return
	var local = container.global_transform.affine_inverse() * global_hit
	# Bake the Ground userpoint: the dropped model's origin sits at the terrain hit minus its rotated
	# model-local ground anchor, so its ground point lands under the cursor (render is direct).
	# [orig: sub_401A90, dfx2med.exe]
	_apply_selected_xform(Transform3D(_c._selected_xform.basis, local - (_c._selected_xform.basis * _c._selected_ground_offset)))


# Write a new container-local transform onto the selection: rewrite every static
# MultiMesh instance (slot) of the entity, or the animated node, plus the selection
# box. Shared by the viewport drag and the inspector's numeric pos/rot edits so both
# move the in-world object identically.
func _apply_selected_xform(xform: Transform3D) -> void:
	_c._selected_xform = xform
	if not _c._selected_ref.is_empty() and int(_c._selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		_clear_selected_user_points()
		# A marker is mesh-less: preview its gizmo (container-local origin) via the overlay. No mesh
		# records / node to move, and the selection box stays hidden.
		if _c._marker_overlay != null and is_instance_valid(_c._marker_overlay):
			_c._marker_overlay.preview_marker_position(int(_c._selected_ref["index"]), _c._selected_xform.origin)
		return
	if _c._selected_node != null:
		# The anchor offset rides the node so the dragged model keeps its ground point
		# under the cursor, matching how it was first placed.
		_c._selected_node.transform = _c._selected_xform * _c._selected_node_offset
	else:
		for rec_v in _c._selected_records:
			var rec: Dictionary = rec_v
			var mm: MultiMesh = rec["mm"] as MultiMesh
			var slot: int = int(rec["slot"])
			var moved: Transform3D = \
					_c._selected_xform * (rec["offset"] as Transform3D)
			mm.set_instance_transform(slot, moved)
			# Keep the parallel static-caster batch aligned during editor drags.
			var shadow_mm: MultiMesh = rec.get("shadow_mm") as MultiMesh
			if shadow_mm != null and slot >= 0 \
					and slot < shadow_mm.instance_count:
				var shadow_moved: Transform3D = moved
				if not bool(rec.get("casts_static_shadow", false)):
					shadow_moved.basis = \
							shadow_moved.basis.scaled(Vector3.ZERO)
				shadow_mm.set_instance_transform(slot, shadow_moved)
	# Move the pick body node in lockstep so a re-pick mid/after-drag stays exact. The body sits at
	# the entity transform directly (render is direct; the ground anchor is baked into the stored
	# position, not applied here). No-op for a marker (no body).
	if _c._selected_collider != null and is_instance_valid(_c._selected_collider):
		_c._selected_collider.transform = _c._selected_xform
	_refresh_selected_user_points_overlay()
	_update_selection_box()
	# Keep the transform gizmo on the selection. During a gizmo drag, only reposition (keep the
	# captured drag plane + ring orientation frozen); otherwise re-orient the rings to the new
	# rotation (numeric edits, fresh selection).
	if _c._gizmo != null and is_instance_valid(_c._gizmo) and _c._gizmo.visible:
		if _c._gizmo_drag.is_empty():
			_c._gizmo.show_for(_c._selected_xform.origin, _c._selected_rotation_deg)
		else:
			_c._gizmo.set_origin(_c._selected_xform.origin)


func _commit_selected_transform() -> void:
	if _c._selected_ref.is_empty() or _c._mission == null:
		return
	var bms_pos = _c.MissionObjectPlacer.godot_to_bms_position(_c._selected_xform.origin)
	if _c._mission.set_entity_transform(int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]), bms_pos, _c._selected_rotation_deg):
		# A marker's gizmo was preview-moved; rebuild the overlay so its pickable AABB tracks the
		# committed position (re-applies the selection highlight).
		if int(_c._selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
			_c._waypoints._refresh_marker_overlay()
		_c.mark_dirty()


## Programmatic grounded move (the MCP edit seam): drop the SELECTED entity so
## its ground point sits at a world-space terrain hit — the viewport drag's
## anchor bake (origin = hit − rotated ground anchor; hit stored directly for
## markers) — as one closed undo step. Returns false with no selection, no
## mission.
func move_selected_to_world_grounded(global_hit: Vector3) -> bool:
	if _c._selected_ref.is_empty() or _c._mission == null:
		return false
	_c._flush_edit()
	_c.begin_edit()
	_move_selected_to_world(global_hit)
	_commit_selected_transform()
	_c.commit_edit()
	return true


# --- Authoring (Phase 2): numeric / property edits from the inspector ---------
# The inspector reads the selected entity's current values and pushes edits back
# through these. Position / rotation reuse the Phase 1 render path (the object moves
# in the viewport exactly as a drag would) then commit + dirty; team / group are not
# visual, so they only write the record + dirty.

# --- Selection geometry helpers -----------------------------------------------

func _find_entity(kind: int, index: int) -> Dictionary:
	if _c._mission == null:
		return {}
	# The entity dict's "index" equals its array position (to_record sets record.index =
	# i), so a direct get_entity is equivalent to scanning get_entities, and O(1).
	return _c._mission.get_entity(kind, index)


func _record_world_aabb(rec: Dictionary) -> AABB:
	# Hover (_entity_world_aabb) walks every _pickable record, which can transiently hold a freed
	# node after a mid-flight re-bake. Guard with is_instance_valid (not just != null) so a
	# freed-but-non-null node/mmi does not error on is_inside_tree(), matching _select's staleness guard.
	if bool(rec.get("animated", false)):
		var node: Node3D = rec.get("node")
		return _node_world_aabb(node) if is_instance_valid(node) and node.is_inside_tree() else AABB()
	var mmi: MultiMeshInstance3D = rec.get("mmi")
	var mm: MultiMesh = rec.get("mm")
	if not is_instance_valid(mmi) or not is_instance_valid(mm) or not mmi.is_inside_tree():
		return AABB()
	var inst := mmi.global_transform * mm.get_instance_transform(int(rec["slot"]))
	return inst * (rec["mesh_aabb"] as AABB)


func _node_world_aabb(node: Node3D) -> AABB:
	var result := AABB()
	var have := false
	for vi in _visual_instances(node):
		var world: AABB = (vi as VisualInstance3D).global_transform * (vi as VisualInstance3D).get_aabb()
		if not have:
			result = world
			have = true
		else:
			result = result.merge(world)
	return result


func _visual_instances(node: Node) -> Array:
	var out: Array = []
	if node == null:
		return out
	if node is VisualInstance3D:
		out.append(node)
	for child in node.get_children():
		out.append_array(_visual_instances(child))
	return out


# Slab-method ray/AABB: returns the entry distance along `dir` (>= 0), or -1 on a miss.
func _ray_aabb_entry(aabb: AABB, from: Vector3, dir: Vector3) -> float:
	var tmin := -INF
	var tmax := INF
	var mn := aabb.position
	var mx := aabb.position + aabb.size
	for axis in 3:
		var o: float = from[axis]
		var d: float = dir[axis]
		var lo: float = mn[axis]
		var hi: float = mx[axis]
		if absf(d) < 1e-8:
			if o < lo or o > hi:
				return -1.0
		else:
			var t1 := (lo - o) / d
			var t2 := (hi - o) / d
			if t1 > t2:
				var tmp := t1
				t1 = t2
				t2 = tmp
			tmin = maxf(tmin, t1)
			tmax = minf(tmax, t2)
			if tmin > tmax:
				return -1.0
	if tmax < 0.0:
		return -1.0
	return maxf(tmin, 0.0)


func _selected_world_aabb() -> AABB:
	if _c._selected_node != null:
		return _node_world_aabb(_c._selected_node)
	var result := AABB()
	var have := false
	for rec in _c._selected_records:
		var a := _record_world_aabb(rec)
		if a.size == Vector3.ZERO:
			continue
		if not have:
			result = a
			have = true
		else:
			result = result.merge(a)
	return result


func _update_selection_box() -> void:
	var aabb := _selected_world_aabb()
	if aabb.size == Vector3.ZERO:
		_hide_selection_box()
		return
	var box := _ensure_selection_box()
	if box == null:
		return
	# Pad slightly so the outline reads around the object rather than z-fighting it.
	var pad := Vector3.ONE * 0.25
	var size := aabb.size + pad * 2.0
	var center := aabb.position + aabb.size * 0.5
	box.global_transform = Transform3D(Basis().scaled(size), center)
	box.visible = true


func _ensure_selection_box() -> MeshInstance3D:
	if _c._selection_box != null and is_instance_valid(_c._selection_box):
		return _c._selection_box
	var container = _c._objects_container()
	if container == null:
		return null
	var mi := MeshInstance3D.new()
	mi.name = "MissionSelectionBox"
	# A crisp wire-cube outline rather than a translucent filled box: it reads strongly at
	# any object size and never obscures the object it brackets. Scaled to the selection's
	# AABB by _update_selection_box.
	mi.mesh = _build_selection_wire_mesh()
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(0.3, 0.95, 1.0)
	# Draw on top so a selected object behind terrain or another object is still findable.
	mat.no_depth_test = true
	mi.material_override = mat
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	container.add_child(mi)
	_c._selection_box = mi
	return mi


# A unit wire cube (edges only, centred at the origin, spanning -0.5..0.5) as an
# ImmediateMesh. _update_selection_box scales it to the selection's padded AABB; scaling a
# line mesh keeps the edges crisp at any size.
func _build_selection_wire_mesh() -> ImmediateMesh:
	var mesh := ImmediateMesh.new()
	var c := 0.5
	var corners := [
		Vector3(-c, -c, -c), Vector3(c, -c, -c), Vector3(c, -c, c), Vector3(-c, -c, c),
		Vector3(-c, c, -c), Vector3(c, c, -c), Vector3(c, c, c), Vector3(-c, c, c),
	]
	var edges := [
		[0, 1], [1, 2], [2, 3], [3, 0],  # bottom ring
		[4, 5], [5, 6], [6, 7], [7, 4],  # top ring
		[0, 4], [1, 5], [2, 6], [3, 7],  # verticals
	]
	mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for e in edges:
		mesh.surface_add_vertex(corners[e[0]])
		mesh.surface_add_vertex(corners[e[1]])
	mesh.surface_end()
	return mesh


func _hide_selection_box() -> void:
	if _c._selection_box != null and is_instance_valid(_c._selection_box):
		_c._selection_box.visible = false


# --- Hover preview ------------------------------------------------------------
# Re-pick on bare mouse motion (objects mode only) and bracket the object under the
# cursor with an amber wire box, distinct from the cyan selection box. Throttled to
# pixel movement; never mutates selection or opens an edit session.
func _on_hover(mouse_pos: Vector2) -> void:
	if _c._mode != _c.Mode.OBJECTS or _c._drag_active or _c._placement.is_placement_armed():
		_clear_hover()
		return
	if _c._hover_pos.distance_to(mouse_pos) < _c.HOVER_PIXEL_EPSILON:
		return
	_c._hover_pos = mouse_pos
	var ref := _pick_entity(mouse_pos)
	var kind := int(ref.get("kind", -1))
	var index := int(ref.get("index", -1))
	# Skip empties, markers (their own gizmo highlights), and the current selection.
	if ref.is_empty() or kind == NovaMissionData.KIND_MARKER \
			or (not _c._selected_ref.is_empty() \
				and int(_c._selected_ref.get("kind", -2)) == kind \
				and int(_c._selected_ref.get("index", -2)) == index):
		_clear_hover()
		return
	_c._hovered_ref = { "kind": kind, "index": index }
	var aabb := _entity_world_aabb(kind, index)
	if aabb.size == Vector3.ZERO:
		_clear_hover()
		return
	var box := _ensure_hover_box()
	if box == null:
		return
	var pad := Vector3.ONE * 0.2
	box.global_transform = Transform3D(Basis().scaled(aabb.size + pad * 2.0), aabb.position + aabb.size * 0.5)
	box.visible = true


func _ensure_hover_box() -> MeshInstance3D:
	if _c._hover_box != null and is_instance_valid(_c._hover_box):
		return _c._hover_box
	var container = _c._objects_container()
	if container == null:
		return null
	var mi := MeshInstance3D.new()
	mi.name = "MissionHoverBox"
	mi.mesh = _build_selection_wire_mesh()
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(1.0, 0.85, 0.2)
	mat.no_depth_test = true
	mi.material_override = mat
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	container.add_child(mi)
	_c._hover_box = mi
	return mi


func _clear_hover() -> void:
	_c._hovered_ref = {}
	_c._hover_pos = Vector2(-1, -1)
	if _c._hover_box != null and is_instance_valid(_c._hover_box):
		_c._hover_box.visible = false


# Clear selection refs without touching the scene. The selection box is a child of the
# objects container, so it is freed when the container is (re)built; here we only drop
# the dangling ref.
func _reset_selection_state() -> void:
	_c._preview.stop_preview()
	_clear_selected_user_points()
	_c._selected_ref = {}
	_c._selected_records = []
	_c._selected_node = null
	_c._selected_graphic = ""
	_c._selected_node_offset = Transform3D.IDENTITY
	_c._selected_xform = Transform3D.IDENTITY
	_c._selected_rotation_deg = Vector3.ZERO
	# Drop the selection's pick-body ref (the body node is freed/rebuilt with the
	# container, not here).
	_c._selected_collider = null
	_c._selected_ground_offset = Vector3.ZERO
	_c._drag_active = false
	_c._drag_moved = false
	_c._drag_off_terrain = false
	# The transform gizmo is a container child too, so a re-bake freed it; drop the dangling ref
	# (and any in-flight gizmo drag) so the next _refresh_gizmo rebuilds it.
	_c._gizmo = null
	_c._gizmo_drag = {}
	_c._gizmo_hover_pos = Vector2(-1, -1)
	_c._selection_box = null
	# The hover box is a container child too, so the re-bake freed it; drop the dangling ref.
	_c._hover_box = null
	_c._hovered_ref = {}
	_c._hover_pos = Vector2(-1, -1)
	# Waypoint marker selection + overlay are tied to the container contents, so they reset
	# with it; the chosen path (_selected_path_index) persists across re-bakes by design.
	_c._selected_marker = {}
	_c._marker_pickable = []
	_c._waypoint_overlay = null
	# The marker overlay is a container child too, so the re-bake freed it; drop the dangling ref so
	# the next _refresh_marker_overlay rebuilds it rather than orphaning a freed node.
	_c._marker_overlay = null
	# Zone selection + overlay are likewise container-tied. Unlike the waypoint path, the zone
	# selection does NOT survive a re-bake (a delete shifts indices), so it resets here too.
	_c._selected_zone_index = -1
	_c._zone_pickable = []
	_c._area_overlay = null
