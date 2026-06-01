extends RefCounted

# Editor-side controller for the Mission workspace (the Phase 2 adapter).
#
# Holds the open mission (NovaMissionData) plus its document state (path /
# loaded / dirty) and drives the load: parse the .bms, resolve its referenced
# terrain + environment from the shared resource root, load them through the
# terrain editor (read-only viewport), then run the host-agnostic
# MissionObjectPlacer under the terrain editor's world root. The resolve + place
# logic is the same piece the runtime uses (NovaWorld.load_mission); this is the
# thin editor binding around it.
#
# Read-only for now: mission authoring (place / move / save entities) is deferred,
# so the document never goes dirty. The dirty/save hooks exist for when authoring
# lands. Referenced via preload (no class_name) so it resolves without an editor
# re-import, the same convention as the placer and veg_assets.gd.

signal changed  # Mission loaded or cleared; the inspector rebuilds on this.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionWaypointOverlay := preload("res://engine/mission/mission_waypoint_overlay.gd")
# Must match MissionObjectPlacer.CONTAINER_NAME — that is where placed objects land.
const OBJECTS_CONTAINER := "MissionObjects"

var terrain_editor: Node

var _mission: NovaMissionData
var _current_path: String = ""
# The resolved .trn path the loaded mission mounted, so a later terrain swap in the
# Terrain workspace can be detected (see reconcile_with_terrain()).
var _loaded_trn_path: String = ""
var _last_open_dir: String = ""
var _is_dirty: bool = false
var _stats: Dictionary = {}
var _last_status: String = ""
# The placer that built the current world, retained so place-new can render one entity
# incrementally (reusing its model + batch caches) instead of rebuilding everything.
var _placer  # MissionObjectPlacer (preloaded, no class_name)

# --- Authoring (Phase 1) state ------------------------------------------------
# Pickable index harvested from the placer (edit_mode): one record per (entity,
# static batch) or per animated entity. See MissionObjectPlacer.pickable_records.
var _pickable: Array = []
# The selected entity as { kind, index }, or empty when nothing is selected.
var _selected_ref: Dictionary = {}
# The selected entity's static batch records (its MultiMesh slots), or its animated
# node, plus its tracked container-local transform and authored rotation (degrees).
var _selected_records: Array = []
var _selected_node: Node3D
var _selected_xform: Transform3D = Transform3D.IDENTITY
var _selected_rotation_deg: Vector3 = Vector3.ZERO
# Drag session: _drag_active spans press..release; _drag_moved gates the commit so a
# plain click only selects.
var _drag_active: bool = false
var _drag_moved: bool = false
# Place-new ("placement mode"): the armed items.def item id (0 = not armed). While
# armed, a left-click on the terrain places a new instance of this item instead of
# selecting / dragging; right-click or Escape disarms. See arm_placement().
var _place_item_id: int = 0
# Translucent box marking the selection in the viewport (lazily built under the
# objects container; freed with the container).
var _selection_box: MeshInstance3D

# --- Authoring (Phase 5): undo / redo -----------------------------------------
# Whole-document byte snapshots taken through the real serializer
# (NovaMissionData.snapshot = write_bms_bytes) and restored via restore_snapshot
# (load_bms_bytes). Each stack entry is a re-serialized, valid .bms — never the raw
# bytes opened from disk — so the same machinery rewinds a from-scratch mission too.
# Mirrors strings_editor.gd. UNDO_LIMIT caps memory; the oldest step is dropped first.
const UNDO_LIMIT := 100
var _undo_stack: Array[PackedByteArray] = []
var _redo_stack: Array[PackedByteArray] = []
# An open edit session (begin_edit .. commit_edit) coalesces a continuous gesture (a
# terrain drag, or a run of inspector SpinBox edits) into one undo step: the pre-edit
# snapshot is held here and pushed only if the bytes actually changed.
var _pending_snapshot: PackedByteArray = PackedByteArray()
var _editing: bool = false
# The document bytes as opened / last saved. The dirty flag is exact: true iff the
# current document differs from this, so undoing back to the original drops the `*`.
var _clean_snapshot: PackedByteArray = PackedByteArray()
# Guards undo/redo against re-entrancy (a restore -> rebake -> changed -> inspector
# refresh must never re-enter another restore).
var _restoring: bool = false

# --- Authoring (P7): waypoints ------------------------------------------------
# Waypoints mode: while true the viewport selects / drags the active path's markers
# instead of objects, and the inspector shows the waypoint panel. The two modes are
# exclusive; switching clears the other's selection + any armed tool. Object editing
# (P1-P5) is untouched in objects mode.
var _waypoint_mode: bool = false
# The waypoint path (0..127) the panel is focused on, or -1 when none is chosen. Persists
# across re-bakes (undo/redo/edit), unlike the selection, so the user stays on their path.
var _selected_path_index: int = -1
# The selected marker as { path_index, marker_index }, or empty when none is selected.
var _selected_marker: Dictionary = {}
# Marker pickables harvested from the overlay (active path only): one per marker gizmo,
# { path_index, marker_index, order, aabb (world) }. Parallels _pickable for objects.
var _marker_pickable: Array = []
# The in-world overlay drawing marker gizmos + path lines. Built under the objects
# container (so it frees with it); the ref is dropped on every re-bake and lazily rebuilt.
var _waypoint_overlay  # MissionWaypointOverlay (preloaded, no class_name)
# Marker placement ("add marker" tool): while armed, a terrain click adds a marker to the
# active path instead of selecting (mirrors object placement arming, but mode-scoped).
var _marker_place_armed: bool = false
# The active path marker id seeded into add_waypoint_marker when the mission carries no
# marker to copy from (authoring waypoints from scratch). Reused from an existing marker
# when one is present, so shipped data round-trips with its own id.
const DEFAULT_MARKER_ITEM_ID := 100001
# The live (container-local) position of a marker being dragged, written to the record on
# release (the drag previews the gizmo only; the record is committed once, as one step).
var _marker_drag_local: Vector3 = Vector3.ZERO


func _init(p_terrain_editor: Node = null) -> void:
	terrain_editor = p_terrain_editor


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value


# --- State accessors ----------------------------------------------------------

func get_mission() -> NovaMissionData:
	return _mission


func is_loaded() -> bool:
	return _mission != null


func is_dirty() -> bool:
	return _is_dirty


func get_current_path() -> String:
	return _current_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_stats() -> Dictionary:
	return _stats


func get_last_status() -> String:
	return _last_status


# { kind, index, position (mission-space Vector3), animated } for the selected
# entity, or empty when nothing is selected. Drives the inspector's selection line.
func get_selection_summary() -> Dictionary:
	if _selected_ref.is_empty():
		return {}
	return {
		"kind": int(_selected_ref["kind"]),
		"index": int(_selected_ref["index"]),
		"position": MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin),
		"animated": _selected_node != null,
	}


func get_mission_title() -> String:
	if _mission == null:
		return "Mission"
	var mission_name := _mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = _current_path.get_file().get_basename()
	if mission_name.is_empty():
		mission_name = "untitled"
	return "%s%s" % [mission_name, "*" if _is_dirty else ""]


# --- Open ---------------------------------------------------------------------

## Open a .bms: parse it, resolve + load its referenced terrain and environment
## through the terrain editor, then place its objects under the shared world root.
## Returns OK, or an error code; get_last_status() carries a human-facing reason.
func open_mission(bms_path: String) -> Error:
	_last_status = ""
	if terrain_editor == null or not terrain_editor.has_method("get_resource_root"):
		_last_status = "No terrain editor is bound."
		return ERR_UNAVAILABLE
	var resource_root: NovaResourceRoot = terrain_editor.get_resource_root()
	if resource_root == null:
		_last_status = "Set a resource directory before opening a mission."
		return ERR_UNCONFIGURED

	var mission := NovaMissionData.new()
	if mission.open_file(bms_path) != OK:
		_last_status = "Could not read %s: %s" % [bms_path.get_file(), mission.get_last_error()]
		return ERR_CANT_OPEN

	# The mission header selects the world: resolve its terrain (required) and
	# environment (optional) from the user's resource directory, case-insensitive.
	var terrain_ref := mission.get_terrain_ref()
	var trn_path := resource_root.resolve_file(terrain_ref + ".trn")
	if trn_path.is_empty():
		_last_status = "%s.trn (referenced by the mission) was not found in the resource directory." % terrain_ref
		return ERR_FILE_NOT_FOUND

	# Loading the referenced terrain is an atomic dependency of opening the mission,
	# not a separate user action, so it goes straight to open_trn rather than the
	# terrain editor's dirty-guarded request_open_trn.
	var trn_err := int(terrain_editor.open_trn(trn_path))
	if trn_err != OK:
		# open_trn already replaced the editor's terrain with an empty one, so any
		# previously-loaded mission now describes a world that is gone. Drop it
		# rather than leaving stale objects / metadata over a blanked terrain.
		clear()
		_last_status = "Could not load %s.trn (error %d)." % [terrain_ref, trn_err]
		return trn_err as Error

	var env_note := _load_environment(mission, resource_root)
	_place_objects(mission, resource_root)

	_mission = mission
	_current_path = bms_path
	_loaded_trn_path = trn_path
	_last_open_dir = bms_path.get_base_dir()
	_is_dirty = false
	# Baseline for the exact dirty flag, and a fresh undo history for this document.
	# The baseline is a re-serialization (not the raw file bytes) so it compares
	# apples-to-apples with later snapshot()s.
	_clean_snapshot = mission.snapshot()
	_clear_history()
	# Fresh document: drop any prior marker selection and focus a populated path (so the
	# waypoint panel is not empty) only if the user is already in waypoints mode.
	_selected_marker = {}
	_marker_place_armed = false
	_selected_path_index = _first_nonempty_path() if _waypoint_mode else -1
	if _waypoint_mode:
		_refresh_waypoint_overlay()
	_last_status = _describe_load(mission, bms_path, env_note)
	changed.emit()
	return OK


func clear() -> void:
	_reset_selection_state()
	_pickable = []
	_place_item_id = 0
	_marker_place_armed = false
	_selected_path_index = -1
	_placer = null
	_clear_objects()
	_mission = null
	_current_path = ""
	_loaded_trn_path = ""
	_stats = {}
	_is_dirty = false
	# Drop the undo history and baseline: they describe a document that is no longer
	# loaded, and restoring into a missing mission is meaningless.
	_clean_snapshot = PackedByteArray()
	_clear_history()
	changed.emit()


# If the terrain underneath was swapped out from under a loaded mission (the user
# opened a different terrain in the Terrain workspace), the placed objects no
# longer belong to the mounted world. Drop the mission so its objects / metadata
# stop describing a world that is no longer there; re-opening shows it on its own
# terrain again. Called when the Mission workspace regains focus.
func reconcile_with_terrain() -> void:
	if _mission == null or terrain_editor == null or not terrain_editor.has_method("get_current_trn_path"):
		return
	if String(terrain_editor.get_current_trn_path()) != _loaded_trn_path:
		clear()


# The MissionObjects container hangs under the shared terrain world root, so it is
# also under the terrain workspace's view. Hide it there, show it in the mission
# workspace (toggled from the workspace's activate / deactivate).
func set_objects_visible(value: bool) -> void:
	var container := _objects_container()
	if container != null:
		container.visible = value


# Recompute the exact dirty flag and notify. Called after every mutation, undo, and
# redo. Dirty is exact: true iff the current document differs from the opened / last-
# saved bytes, so undoing all the way back to the original clears the `*`. The compare
# is cheap (the document is already serialized for the undo snapshots).
func mark_dirty() -> void:
	_recompute_dirty()
	changed.emit()


func _recompute_dirty() -> void:
	if _mission == null:
		_is_dirty = false
		return
	# Before a clean baseline exists (e.g. mid-open), fall back to the binding's coarse
	# "modified since load" flag rather than reporting spuriously clean.
	if _clean_snapshot.is_empty():
		_is_dirty = _mission.is_modified()
		return
	_is_dirty = _mission.snapshot() != _clean_snapshot


# --- Save ---------------------------------------------------------------------
# Mirrors the editor save contract (see strings_editor.gd): save_current() writes
# back to the opened path and returns ERR_INVALID_PARAMETER when there is none (the
# shell then offers Save As); save_as() takes a directory and composes the filename.

func save_current() -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if _current_path.is_empty() or _current_path.get_extension().to_lower() != "bms":
		return ERR_INVALID_PARAMETER
	var err := int(_mission.save_file())
	if err == OK:
		# The saved bytes are the new clean baseline; the undo history is kept so the user
		# can still undo across the save.
		_clean_snapshot = _mission.snapshot()
		_is_dirty = false
		_last_status = "Saved %s." % _current_path.get_file()
		changed.emit()
	else:
		_last_status = "Could not save %s: %s" % [_current_path.get_file(), _mission.get_last_error()]
	return err as Error


func save_as(dir_path: String) -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	var mkdir := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir != OK:
		return mkdir
	var filename := _current_path.get_file()
	if filename.is_empty():
		filename = "mission.bms"
	var path := dir_path.path_join(filename)
	var err := int(_mission.save_as(path))
	if err == OK:
		_current_path = path
		_last_open_dir = dir_path
		_clean_snapshot = _mission.snapshot()
		_is_dirty = false
		_last_status = "Saved %s." % filename
		changed.emit()
	else:
		_last_status = "Could not save %s: %s" % [filename, _mission.get_last_error()]
	return err as Error


# --- Authoring (Phase 5): undo / redo -----------------------------------------
# Whole-document byte snapshots, mirroring strings_editor.gd. A mutation snapshots the
# pre-edit document, pushes it on the undo stack, and clears redo; undo/redo swap the
# current state onto the opposite stack and restore the popped snapshot, then re-bake
# the world to match. Continuous gestures (a drag, a run of inspector edits) are
# bracketed by begin_edit/commit_edit so each becomes one step. Triggered by the
# viewport Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y; also exposed for the workspace's framework
# hooks. Selection is dropped on restore: structural edits reindex entities, so a stored
# {kind, index} could bind to a different entity, and the re-bake resets selection anyway.

func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


# Open an edit session, capturing the pre-edit snapshot once. Inert if a session is
# already open (so a run of axis edits coalesces) or no mission is loaded.
func begin_edit() -> void:
	if _editing or _mission == null:
		return
	_pending_snapshot = _mission.snapshot()
	_editing = true


# Close an edit session, pushing the held snapshot as one undo step only if the document
# actually changed (a plain click, a same-value edit, or a programmatic refresh push
# nothing). Clears redo on a real change.
func commit_edit() -> void:
	if not _editing:
		return
	_editing = false
	var pending := _pending_snapshot
	_pending_snapshot = PackedByteArray()
	if pending.is_empty() or _mission == null:
		return
	if _mission.snapshot() != pending:
		_undo_stack.append(pending)
		_trim_undo()
		_redo_stack.clear()


func _flush_edit() -> void:
	commit_edit()


# Record one undo step from a snapshot captured BEFORE a mutation, clearing redo. Skips
# the push when the document did not actually change, so a no-op edit adds no step.
func _push_undo_step(before: PackedByteArray) -> void:
	if before.is_empty() or _mission == null:
		return
	if _mission.snapshot() == before:
		return
	_undo_stack.append(before)
	_trim_undo()
	_redo_stack.clear()


func _trim_undo() -> void:
	while _undo_stack.size() > UNDO_LIMIT:
		_undo_stack.pop_front()


func _clear_history() -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_pending_snapshot = PackedByteArray()
	_editing = false


func undo() -> void:
	if _restoring:
		return
	# A keyboard undo can arrive mid-drag; cancel_drag abandons the visual gesture (so the
	# re-bake does not free nodes a continuing drag still references) and commits any open
	# edit session as its step before we rewind.
	cancel_drag()
	if _undo_stack.is_empty() or _mission == null:
		return
	_restoring = true
	_redo_stack.append(_mission.snapshot())
	_restore(_undo_stack.pop_back())
	_restoring = false


func redo() -> void:
	if _restoring:
		return
	cancel_drag()
	if _redo_stack.is_empty() or _mission == null:
		return
	_restoring = true
	_undo_stack.append(_mission.snapshot())
	_restore(_redo_stack.pop_back())
	_restoring = false


# Replace the document from a snapshot, then re-bake the world to match and recompute the
# exact dirty flag. Emits changed once (via mark_dirty after the re-bake) so the inspector
# refreshes against the restored world in a single pass.
func _restore(snapshot: PackedByteArray) -> void:
	if not _mission.restore_snapshot(snapshot):
		# Self-produced snapshots always parse, so this is a defensive path: load_bms_bytes
		# leaves the document empty on failure, so clear rather than re-bake against nothing.
		_last_status = "Could not restore the previous mission state."
		clear()
		return
	_rebake_objects()
	mark_dirty()


# Mark the current input event handled so a consumed Ctrl+Z / Ctrl+Y does not propagate
# further (mirrors terrain_editor). No-op without a live viewport (headless tests).
func _consume_viewport_key() -> void:
	if terrain_editor == null or not terrain_editor.is_inside_tree():
		return
	var vp := terrain_editor.get_viewport()
	if vp != null:
		vp.set_input_as_handled()


# --- Viewport authoring: select + terrain-plane drag --------------------------
# Driven by the viewport input router (set as its input_target by the workspace).
# Left-click picks an entity (analytic ray-vs-AABB over the pickable index); dragging
# re-grounds it on the terrain each motion; release writes the new position back to
# the mission record. Rotation is preserved (numeric/rotate editing is a later phase).

func handle_viewport_input(event: InputEvent) -> void:
	if _mission == null or terrain_editor == null:
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		# Right-click while armed cancels the placement tool (a familiar "drop the tool"
		# gesture) and does not fall through to selection -- objects in objects mode, the
		# add-marker tool in waypoints mode.
		if mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
			if is_placement_armed():
				disarm_placement()
				return
			if _marker_place_armed:
				disarm_marker_placement()
				return
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if mb.pressed:
			# Waypoints mode: left-click selects (or, when armed, adds) the active path's
			# markers, and starts a marker drag -- never objects (the modes are exclusive).
			if _waypoint_mode:
				_on_marker_left_press(mb.position)
			# Armed: left-click places a new instance at the cursor instead of selecting.
			# Stay armed so the user can place several; right-click / Escape / the Stop
			# button disarms.
			elif is_placement_armed():
				_place_armed_at(mb.position)
			else:
				_on_left_press(mb.position)
		else:
			if _waypoint_mode:
				_on_marker_left_release()
			else:
				_on_left_release()
	elif event is InputEventKey:
		var key := event as InputEventKey
		# Ignore key-up and auto-repeat echoes (holding the key must not chain actions).
		if not key.pressed or key.echo:
			return
		# Undo / redo: Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y. Claimed before the other shortcuts
		# and gated by the same focus guard as Delete, so a focused SpinBox / LineEdit keeps
		# its own text undo. Marked handled so the key does not propagate further. (Mirrors
		# fnt_editor / terrain_editor, which also key off ctrl_pressed, not Cmd, on macOS.)
		if key.ctrl_pressed and not _gui_focus_blocks_shortcut():
			if key.keycode == KEY_Z and not key.shift_pressed:
				undo()
				_consume_viewport_key()
				return
			if (key.keycode == KEY_Z and key.shift_pressed) or key.keycode == KEY_Y:
				redo()
				_consume_viewport_key()
				return
		if key.keycode == KEY_ESCAPE:
			# Escape drops whichever placement tool is armed (object or add-marker).
			if is_placement_armed():
				disarm_placement()
			elif _marker_place_armed:
				disarm_marker_placement()
		elif (key.keycode == KEY_DELETE or key.keycode == KEY_BACKSPACE) and not key.ctrl_pressed:
			# Delete the current selection, unless a GUI control owns the keyboard. The router
			# feeds us via _unhandled_input, which only withholds keys a focused control
			# actually consumes -- a SpinBox holding focus via its arrows, an ItemList, or a
			# Button do NOT consume Delete/Backspace, so without this guard a stray Backspace
			# while editing a field would silently delete. Mirrors the focus-owner guard in
			# credits_editor / fnt_editor / terrain_editor. Mode-scoped: a marker in waypoints
			# mode, an object otherwise.
			if not _gui_focus_blocks_shortcut():
				if _waypoint_mode and not _selected_marker.is_empty():
					delete_selected_marker()
				elif not _waypoint_mode and not _selected_ref.is_empty():
					delete_selected()
	elif event is InputEventMouseMotion and _drag_active:
		var motion := event as InputEventMouseMotion
		# Defend against a missed button-up (e.g. the release landed on a different
		# control while switching workspaces): if the left button is no longer held,
		# the gesture was abandoned, not continuing, so end it without committing.
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) == 0:
			cancel_drag()
			return
		# Drag the active mode's selection: a marker in waypoints mode, an object otherwise.
		if _waypoint_mode:
			_on_marker_drag(motion.position)
		else:
			_on_drag(motion.position)


# End an in-progress drag without committing. The workspace calls this when it
# deactivates / unmounts so a half-finished gesture cannot silently resume (and
# relocate + dirty the selection) on a later bare hover after the user returns.
func cancel_drag() -> void:
	_drag_active = false
	_drag_moved = false
	# Close any open edit session. A drag is visual-only until release commits it, so a
	# cancelled drag leaves the document unchanged and this pushes nothing; an inspector edit
	# session that happens to be open keeps its undo step (commit, not discard, so a workspace
	# switch mid-edit does not silently drop the step).
	commit_edit()
	# A cancelled marker drag previewed the gizmo but wrote no record; snap it back to the
	# stored position.
	if _waypoint_mode:
		_refresh_waypoint_overlay()


func _on_left_press(mouse_pos: Vector2) -> void:
	# Close any open inspector edit session as its own step before starting a new gesture,
	# so SpinBox edits and a following drag never coalesce.
	_flush_edit()
	var ref := _pick_entity(mouse_pos)
	if ref.is_empty():
		_deselect()
		return
	_select(int(ref["kind"]), int(ref["index"]))
	_drag_active = true
	_drag_moved = false
	# Snapshot the pre-drag state; _on_left_release commits it as one step iff the entity
	# actually moved.
	begin_edit()


func _on_drag(mouse_pos: Vector2) -> void:
	if _selected_ref.is_empty() or terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		return
	_drag_moved = true
	_move_selected_to_world(hit)


func _on_left_release() -> void:
	if _drag_active and _drag_moved:
		_commit_selected_transform()
	_drag_active = false
	_drag_moved = false
	# Push the drag as one undo step (no-op for a plain click: the bytes are unchanged).
	commit_edit()


func _pick_entity(mouse_pos: Vector2) -> Dictionary:
	if not terrain_editor.has_method("get_editor_camera"):
		return {}
	var camera: Camera3D = terrain_editor.get_editor_camera()
	if camera == null:
		return {}
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best: Dictionary = {}
	for rec in _pickable:
		var aabb := _record_world_aabb(rec)
		if aabb.size == Vector3.ZERO:
			continue
		var t := _ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = { "kind": int(rec["kind"]), "index": int(rec["index"]) }
	return best


func _select(kind: int, index: int) -> void:
	_selected_ref = { "kind": kind, "index": index }
	_selected_records = []
	_selected_node = null
	for rec in _pickable:
		if int(rec["kind"]) == kind and int(rec["index"]) == index:
			if bool(rec.get("animated", false)):
				_selected_node = rec.get("node")
			else:
				_selected_records.append(rec)
	var entity := _find_entity(kind, index)
	_selected_rotation_deg = entity.get("rotation_deg", Vector3.ZERO)
	_selected_xform = MissionObjectPlacer.entity_transform(
		entity.get("position", Vector3.ZERO), _selected_rotation_deg)
	_update_selection_box()
	changed.emit()


func _deselect() -> void:
	if _selected_ref.is_empty():
		return
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_hide_selection_box()
	changed.emit()


# Move the selected entity so its origin sits at a world-space ground point: keep the
# current rotation, only the origin tracks the cursor.
func _move_selected_to_world(global_hit: Vector3) -> void:
	var container := _objects_container()
	if container == null:
		return
	var local := container.global_transform.affine_inverse() * global_hit
	_apply_selected_xform(Transform3D(_selected_xform.basis, local))


# Write a new container-local transform onto the selection: rewrite every static
# MultiMesh instance (slot) of the entity, or the animated node, plus the selection
# box. Shared by the viewport drag and the inspector's numeric pos/rot edits so both
# move the in-world object identically.
func _apply_selected_xform(xform: Transform3D) -> void:
	_selected_xform = xform
	if _selected_node != null:
		_selected_node.transform = _selected_xform
	else:
		for rec in _selected_records:
			var mm: MultiMesh = rec["mm"]
			mm.set_instance_transform(int(rec["slot"]), _selected_xform * (rec["offset"] as Transform3D))
	_update_selection_box()


func _commit_selected_transform() -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin)
	if _mission.set_entity_transform(int(_selected_ref["kind"]), int(_selected_ref["index"]), bms_pos, _selected_rotation_deg):
		mark_dirty()


# --- Authoring (Phase 2): numeric / property edits from the inspector ---------
# The inspector reads the selected entity's current values and pushes edits back
# through these. Position / rotation reuse the Phase 1 render path (the object moves
# in the viewport exactly as a drag would) then commit + dirty; team / group are not
# visual, so they only write the record + dirty.

# The full editable dictionary for the selected entity (see NovaMissionData entity
# fields: position is mission-space, rotation_deg is authored degrees, plus team /
# group), or {} when nothing is selected.
func get_selected_entity() -> Dictionary:
	if _selected_ref.is_empty():
		return {}
	return _find_entity(int(_selected_ref["kind"]), int(_selected_ref["index"]))


# The live selected position in mission (BMS) space, tracking any uncommitted drag.
func get_selected_position() -> Vector3:
	if _selected_ref.is_empty():
		return Vector3.ZERO
	return MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin)


# The live selected rotation as authored (pitch, yaw, roll) degrees.
func get_selected_rotation() -> Vector3:
	if _selected_ref.is_empty():
		return Vector3.ZERO
	return _selected_rotation_deg


func set_selected_position(bms_pos: Vector3) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	# Open (or continue) one edit session so a run of axis edits on this entity coalesces
	# into a single undo step; it is pushed by the next action's flush. begin_edit is inert
	# if a session is already open, so X / Y / Z / pitch / yaw / roll share one step.
	begin_edit()
	# entity_transform places objects at bms_to_godot_position(pos) in container-local
	# space (the drag path and get_selected_position both invert exactly that), so set
	# the local origin directly. Routing through the container's world transform would
	# double-apply it and shift the object whenever the container is not at the origin.
	_apply_selected_xform(Transform3D(_selected_xform.basis, MissionObjectPlacer.bms_to_godot_position(bms_pos)))
	_commit_selected_transform()


func set_selected_rotation(rot_deg: Vector3) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	begin_edit()
	# Unlike a drag (position only), this rebuilds the basis from the authored degrees
	# and re-applies the full transform so the in-world object actually rotates. Round
	# to whole degrees first: the format (and set_entity_transform) stores integer
	# degrees, so keeping a fractional value would leave get_selected_rotation out of
	# step with the persisted record on the next axis edit.
	_selected_rotation_deg = rot_deg.round()
	var basis := Basis.from_euler(MissionObjectPlacer.bms_to_godot_rotation(_selected_rotation_deg))
	_apply_selected_xform(Transform3D(basis, _selected_xform.origin))
	_commit_selected_transform()


func set_selected_team(value: int) -> void:
	# team / group are stored as uint8 by the format; clamp at this API boundary so an
	# out-of-range value cannot silently wrap (the SpinBoxes already cap 0..255, but
	# these methods are public).
	set_selected_property("team", clampi(value, 0, 255))


func set_selected_group(value: int) -> void:
	set_selected_property("group", clampi(value, 0, 255))


# Generic per-entity scalar property edit from the inspector: team / group plus the
# AI + waypoint fields the format carries (waypoint_id, wp_number, perception, accuracy,
# alert_state, the engagement / attack distances, spawn_count, max_simultaneous,
# ai_flags). `property` is the entity-dictionary key it edits. A property change is its
# own undo step: close any open transform session first, then capture the pre-edit state
# and record it only if the write actually changed the bytes (a same-value write is a
# no-op). The value range is governed by the inspector's SpinBoxes and the format's field
# widths, so this does not clamp; team / group clamp through their wrappers above.
func set_selected_property(property: String, value: int) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	_flush_edit()
	var before := _mission.snapshot()
	if _mission.set_entity_property_int(int(_selected_ref["kind"]), int(_selected_ref["index"]), property, value):
		_push_undo_step(before)
		mark_dirty()


# --- Authoring (Phase 3): place new objects -----------------------------------
# The inspector's palette arms an items.def item; a left-click on the terrain then
# places a new instance there (add_entity + incremental render) and selects it, while
# staying armed so several can be placed. Markers are excluded (no mesh; they belong
# to the deferred waypoint editing).

# The placeable items for the palette: every items.def entry that maps to a renderable
# entity kind (markers excluded), as { id, display_name, type }, in the database's
# stable display order. Empty until a mission (hence a resource root + items.def) is
# loaded.
func get_placeable_items() -> Array:
	var db := _item_db()
	if db == null:
		return []
	var out: Array = []
	for item in db.get_items():
		var entry: Dictionary = item
		var type := int(entry.get("type", 0))
		if _kind_for_item_type(type) == NovaMissionData.KIND_MARKER:
			continue
		out.append({
			"id": int(entry.get("id", 0)),
			"display_name": String(entry.get("display_name", "")),
			"type": type,
		})
	return out


# Arm placement for an items.def item id. A later terrain click places it. Rejects
# unknown ids and marker-kind items (mesh-less). Drops any current selection so the
# inspector shows the placement affordance rather than an edit panel.
func arm_placement(item_id: int) -> void:
	if _mission == null:
		return
	var db := _item_db()
	if db == null or not db.has_item(item_id):
		return
	if _kind_for_item_type(db.get_item_type(item_id)) == NovaMissionData.KIND_MARKER:
		return
	# Arming is a new action: close any open transform session as its own undo step first.
	_flush_edit()
	_place_item_id = item_id
	_deselect()
	changed.emit()


func disarm_placement() -> void:
	if _place_item_id == 0:
		return
	_place_item_id = 0
	changed.emit()


func is_placement_armed() -> bool:
	return _place_item_id != 0


func get_placement_item_id() -> int:
	return _place_item_id


# Place a new instance of `item_id` at a world-space ground point, with zero rotation.
# Derives the entity kind from the item's type, writes the record (add_entity), renders
# it incrementally, dirties, and selects the new entity. Returns false (with a status)
# if there is no mission / container or the lib rejects the add. Public so it is
# directly testable without a camera + terrain raycast.
func place_entity_at_world(item_id: int, global_hit: Vector3) -> bool:
	if _mission == null:
		return false
	var container := _objects_container()
	if container == null:
		return false
	var db := _item_db()
	var kind := _kind_for_item_type(db.get_item_type(item_id)) if db != null else NovaMissionData.KIND_ITEM
	var local := container.global_transform.affine_inverse() * global_hit
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(local)
	# Placing is its own undo step: close any open session, snapshot the pre-place state,
	# then record it after the add succeeds.
	_flush_edit()
	var before := _mission.snapshot()
	var record := _mission.add_entity(kind, item_id, bms_pos, Vector3.ZERO)
	if record.is_empty():
		_last_status = "Could not place item %d." % item_id
		return false
	_push_undo_step(before)
	var new_index := int(record.get("index", -1))
	_render_placed_entity(kind, new_index)
	mark_dirty()
	_select(kind, new_index)
	return true


# Map an items.def item type to the BMS entity-list kind a new placement lands in.
# Empirically 1:1 and deterministic across 185k entities in 114 shipping JO missions:
#   Person                        -> Organic
#   Building / Decoration / Foliage -> Building   (all three share the Building list)
#   Marker                        -> Marker       (mesh-less; excluded from the palette)
#   Vehicle / Object / Powerup / Unknown -> Item
# Decoration and Foliage going to the Building list (not Item) is the non-obvious part
# and is the dominant case in real data (foliage + decoration are ~55% of all entities).
func _kind_for_item_type(type: int) -> int:
	match type:
		NovaItemDatabase.TYPE_PERSON:
			return NovaMissionData.KIND_ORGANIC
		NovaItemDatabase.TYPE_BUILDING, NovaItemDatabase.TYPE_DECORATION, NovaItemDatabase.TYPE_FOLIAGE:
			return NovaMissionData.KIND_BUILDING
		NovaItemDatabase.TYPE_MARKER:
			return NovaMissionData.KIND_MARKER
		_:
			return NovaMissionData.KIND_ITEM


func _item_db() -> NovaItemDatabase:
	if _placer == null:
		return null
	return _placer.get_item_db()


# Raycast the terrain under the cursor and place the armed item there. A miss (off the
# terrain) is ignored so a stray click into the sky does nothing.
func _place_armed_at(mouse_pos: Vector2) -> void:
	if not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		return
	place_entity_at_world(_place_item_id, hit)


# Render a just-added entity into the live container and fold its counts into the
# displayed stats, reusing the retained placer's caches.
func _render_placed_entity(kind: int, index: int) -> void:
	if _placer == null:
		return
	var container := _objects_container()
	if container == null:
		return
	var delta: Dictionary = _placer.place_single(_mission, container, kind, index, _environment_node())
	_pickable = _placer.pickable_records
	for key in delta:
		_stats[key] = int(_stats.get(key, 0)) + int(delta[key])


# --- Authoring (Phase 4): delete + structural re-bake -------------------------
# Deleting an entity is structural: the lib erases it from its kind's list, so every
# later entity of that kind shifts down one index. The pickable index and MultiMesh
# slot mapping were built from the old indices, so rather than patch them in place we
# re-bake the whole MissionObjects container from the post-delete record — correct by
# construction, and cheap because the retained placer keeps its model + batch caches.

# Remove the currently-selected entity, then re-bake the world so it matches the new
# record. Markers are never selectable (mesh-less), so this only ever deletes a
# mesh-having entity. Returns false (a no-op) when nothing is selected or the lib
# rejects the removal; clears the selection on success. Public so the inspector's
# Delete button and the viewport Delete key share one path.
func delete_selected() -> bool:
	if _selected_ref.is_empty() or _mission == null:
		return false
	var kind := int(_selected_ref["kind"])
	var index := int(_selected_ref["index"])
	# Deleting is its own undo step: close any open session, snapshot the pre-delete state,
	# then record it after the removal succeeds (a successful removal always changes the
	# document, so this is never a no-op step).
	_flush_edit()
	var before := _mission.snapshot()
	if not _mission.remove_entity(kind, index):
		return false
	_push_undo_step(before)
	# Re-bake first (it resets the selection state and rebuilds stats), then dirty +
	# emit once so the inspector refreshes against the post-delete world in a single pass.
	_rebake_objects()
	mark_dirty()
	return true


# Rebuild the entire MissionObjects container from the current mission state, reusing
# the retained placer so its (expensive) model + batch caches survive the rebuild. The
# container node identity is kept (place() clears and refills it), so _objects_container
# still resolves. Drops the selection: its box and pickable records are freed with the
# old container contents and the indices they carried may no longer be valid.
func _rebake_objects() -> void:
	if _placer == null or _mission == null:
		return
	if terrain_editor == null or not terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node3D = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	_reset_selection_state()
	var options: Dictionary = {}
	var env_node := _environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	_stats = _placer.place(_mission, world_root, options)
	_pickable = _placer.pickable_records
	# The re-bake replaced the container (and the old overlay with it); rebuild the waypoint
	# overlay against the new world when waypoints mode is active.
	if _waypoint_mode:
		_refresh_waypoint_overlay()


# --- Authoring (P7): waypoint mode + marker selection -------------------------
# Waypoints mode switches the viewport from object editing to authoring the active path's
# markers, and the inspector to the waypoint panel. The two modes are exclusive: entering
# either drops the other's selection and any armed placement tool. The chosen path persists
# across re-bakes; the marker selection (like the object selection) does not. Marker
# picking reuses the same analytic ray-vs-AABB as objects, over the overlay's gizmo AABBs.

func set_waypoint_mode(enabled: bool) -> void:
	if _waypoint_mode == enabled:
		return
	# A mode switch is a fresh context: close any open edit session as its own step first.
	_flush_edit()
	_waypoint_mode = enabled
	# Exclusive selection: clear the object selection refs + its box, the marker selection,
	# and any armed object placement, so only the active mode's clicks are live.
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_hide_selection_box()
	_selected_marker = {}
	_place_item_id = 0
	_marker_place_armed = false
	if enabled and _selected_path_index < 0:
		# Focus a populated path on entry so the panel is not empty.
		_selected_path_index = _first_nonempty_path()
	_refresh_waypoint_overlay()
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.visible = enabled
	changed.emit()


func is_waypoint_mode() -> bool:
	return _waypoint_mode


# Focus a waypoint path (0..127) in the panel + overlay. Drops the marker selection (a
# different path's markers) and rebuilds the overlay so its gizmos / pickables follow.
func select_waypoint_path(index: int) -> void:
	if index == _selected_path_index:
		return
	_selected_path_index = index
	# Drop the marker selection (it belonged to the previous path) AND tell the overlay to
	# clear its highlight, so a marker index that also appears on the new path is not left
	# lit. _refresh_waypoint_overlay then rebuilds against the new active path.
	_selected_marker = {}
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.set_selected_marker(-1)
	_refresh_waypoint_overlay()
	changed.emit()


func get_selected_waypoint_path_index() -> int:
	return _selected_path_index


# Focus the first empty waypoint path so the user can author into it. The path list only
# shows populated paths (plus the active one), so on an all-empty mission no path is
# selectable and "Add marker" would stay disabled forever; this is the "start a new route"
# entry point. Returns the chosen path index, or -1 if all 128 are full (not reachable in
# practice). Selecting it makes the (empty) path active, which the list then shows.
func select_new_waypoint_path() -> int:
	if _mission == null:
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
	if _mission == null or _selected_path_index < 0:
		return
	var path := _mission.get_waypoint_path(_selected_path_index)
	if path.is_empty():
		return
	var flags := 0
	if not loop:
		flags |= NovaMissionData.WP_FLAG_DOES_NOT_LOOP
	if blue:
		flags |= NovaMissionData.WP_FLAG_BLUE_TEAM
	if red:
		flags |= NovaMissionData.WP_FLAG_RED_TEAM
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	_flush_edit()
	var before := _mission.snapshot()
	if _mission.set_waypoint_path(_selected_path_index, indices, flags):
		_push_undo_step(before)
		_refresh_waypoint_overlay()
		mark_dirty()


func get_waypoint_summaries() -> Array:
	return _mission.get_waypoint_summaries() if _mission != null else []


# The active path as { index, flags, marker_count, marker_indices }, or {} when none is
# chosen / no mission is loaded.
func get_active_waypoint_path() -> Dictionary:
	if _mission == null or _selected_path_index < 0:
		return {}
	return _mission.get_waypoint_path(_selected_path_index)


# The selected marker enriched with its entity position for the inspector readout, or {}.
func get_selected_marker() -> Dictionary:
	if _selected_marker.is_empty() or _mission == null:
		return {}
	var marker_index := int(_selected_marker["marker_index"])
	var entity := _mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)
	if entity.is_empty():
		return {}
	return {
		"path_index": int(_selected_marker["path_index"]),
		"marker_index": marker_index,
		"position": entity.get("position", Vector3.ZERO),
	}


# The first waypoint path that has at least one marker, or -1 if every path is empty.
func _first_nonempty_path() -> int:
	if _mission == null:
		return -1
	for s in _mission.get_waypoint_summaries():
		if int((s as Dictionary)["marker_count"]) > 0:
			return int((s as Dictionary)["index"])
	return -1


# The first waypoint path with no markers, or -1 if all 128 are populated. Used by
# select_new_waypoint_path to give from-scratch authoring an empty path to fill.
func _first_empty_path() -> int:
	if _mission == null:
		return -1
	for s in _mission.get_waypoint_summaries():
		if int((s as Dictionary)["marker_count"]) == 0:
			return int((s as Dictionary)["index"])
	return -1


# (Re)build the in-world overlay from the current mission + active path, and re-harvest the
# marker pickable index. Creates the overlay node under the objects container on first use
# (and after a re-bake freed it). The overlay reflects the controller's marker selection.
func _refresh_waypoint_overlay() -> void:
	if _mission == null:
		return
	var container := _objects_container()
	if container == null:
		return
	if _waypoint_overlay == null or not is_instance_valid(_waypoint_overlay):
		_waypoint_overlay = MissionWaypointOverlay.new()
		_waypoint_overlay.name = "MissionWaypointOverlay"
		_waypoint_overlay.visible = _waypoint_mode
		container.add_child(_waypoint_overlay)
	_waypoint_overlay.rebuild(_mission, _selected_path_index)
	_marker_pickable = _waypoint_overlay.marker_pickables()
	if not _selected_marker.is_empty():
		_waypoint_overlay.set_selected_marker(int(_selected_marker["marker_index"]))


# Select a marker on the active path by its KIND_MARKER entity index (the inspector's
# ordered marker list drives this). Inert without an active path.
func select_waypoint_marker(marker_index: int) -> void:
	if _selected_path_index < 0:
		return
	_select_marker(_selected_path_index, marker_index)


func _on_marker_left_press(mouse_pos: Vector2) -> void:
	# Armed: a click adds a marker to the active path at the cursor instead of selecting.
	if _marker_place_armed:
		_place_marker_armed_at(mouse_pos)
		return
	# Close any open edit session as its own step before a new gesture (mirrors _on_left_press).
	_flush_edit()
	var ref := _pick_marker(mouse_pos)
	if ref.is_empty():
		_deselect_marker()
		return
	_select_marker(int(ref["path_index"]), int(ref["marker_index"]))
	# Begin a drag: motion re-grounds the marker on the terrain, release writes the record as
	# one undo step (a plain click selects without moving, like an object click).
	_drag_active = true
	_drag_moved = false
	begin_edit()


# Pick the nearest active-path marker under the cursor (ray-vs-AABB over the overlay's
# gizmo AABBs), or {} on a miss. Mirrors _pick_entity but over _marker_pickable.
func _pick_marker(mouse_pos: Vector2) -> Dictionary:
	if terrain_editor == null or not terrain_editor.has_method("get_editor_camera"):
		return {}
	var camera: Camera3D = terrain_editor.get_editor_camera()
	if camera == null:
		return {}
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best: Dictionary = {}
	for rec in _marker_pickable:
		var aabb: AABB = rec["aabb"]
		if aabb.size == Vector3.ZERO:
			continue
		var t := _ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = { "path_index": int(rec["path_index"]), "marker_index": int(rec["marker_index"]) }
	return best


func _select_marker(path_index: int, marker_index: int) -> void:
	_selected_marker = { "path_index": path_index, "marker_index": marker_index }
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.set_selected_marker(marker_index)
	changed.emit()


func _deselect_marker() -> void:
	if _selected_marker.is_empty():
		return
	_selected_marker = {}
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.set_selected_marker(-1)
	changed.emit()


# --- Authoring (P7d): marker drag / add / reorder / delete --------------------
# Marker editing reuses the object authoring spine: the terrain-regrounding drag, the
# begin_edit/commit_edit undo bracketing, and the snapshot/_push_undo_step step model. A
# drag previews the gizmo and commits the record once on release; add / delete are
# structural (they change the marker list), so they re-bake; reorder / flags rewrite only
# the path's reference list.

# Re-ground the dragged marker on the terrain each motion: preview the gizmo only (the
# record is written once, on release), so a drag is one undo step.
func _on_marker_drag(mouse_pos: Vector2) -> void:
	if _selected_marker.is_empty() or terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		return
	var container := _objects_container()
	if container == null:
		return
	_drag_moved = true
	_marker_drag_local = container.global_transform.affine_inverse() * hit
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.preview_marker_position(int(_selected_marker["marker_index"]), _marker_drag_local)


func _on_marker_left_release() -> void:
	if _drag_active and _drag_moved and not _selected_marker.is_empty():
		_commit_marker_drag()
	_drag_active = false
	_drag_moved = false
	# Push the drag as one step (no-op for a plain click: nothing was written).
	commit_edit()


# Write the dragged marker's new position back to its KIND_MARKER record (keeping its
# rotation), then re-snap the overlay to the committed value.
func _commit_marker_drag() -> void:
	if _selected_marker.is_empty() or _mission == null:
		return
	var marker_index := int(_selected_marker["marker_index"])
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(_marker_drag_local)
	var entity := _mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)
	var rot: Vector3 = entity.get("rotation_deg", Vector3.ZERO)
	if _mission.set_entity_transform(NovaMissionData.KIND_MARKER, marker_index, bms_pos, rot):
		_refresh_waypoint_overlay()
		mark_dirty()


# --- Add marker (placement tool) ---------------------------------------------

# Arm the "add marker" tool: a terrain click then adds a marker to the active path. Needs
# an active path; drops any marker selection so the inspector shows the placement state.
func arm_marker_placement() -> void:
	if _mission == null or _selected_path_index < 0:
		return
	_flush_edit()
	_marker_place_armed = true
	_deselect_marker()
	changed.emit()


func disarm_marker_placement() -> void:
	if not _marker_place_armed:
		return
	_marker_place_armed = false
	changed.emit()


func is_marker_placement_armed() -> bool:
	return _marker_place_armed


# Add a marker to the active path at a world-space ground point (append). One call both
# creates the KIND_MARKER entity and links it into the path (the lib's add_waypoint_marker).
# A new marker entity does not shift any object indices, so only the overlay is rebuilt.
# Selects the new marker and dirties. Public so it is testable without a camera. Returns
# false if there is no active path / container or the lib rejects the add.
func add_marker_to_active_path_at_world(global_hit: Vector3) -> bool:
	if _mission == null or _selected_path_index < 0:
		return false
	var container := _objects_container()
	if container == null:
		return false
	var local := container.global_transform.affine_inverse() * global_hit
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(local)
	_flush_edit()
	var before := _mission.snapshot()
	var result := _mission.add_waypoint_marker(_selected_path_index, _default_marker_item_id(), bms_pos, Vector3.ZERO, -1)
	if result.is_empty():
		_last_status = "Could not add a waypoint marker."
		return false
	_push_undo_step(before)
	_refresh_waypoint_overlay()
	var marker_index := int((result.get("marker", {}) as Dictionary).get("index", -1))
	if marker_index >= 0:
		_select_marker(_selected_path_index, marker_index)
	mark_dirty()
	return true


# The item id to seed a new marker with: copy an existing marker's id so shipped data keeps
# its own marker type; fall back to a plausible default when authoring from scratch.
func _default_marker_item_id() -> int:
	if _mission != null:
		var markers := _mission.get_entities(NovaMissionData.KIND_MARKER)
		if not markers.is_empty():
			return int((markers[0] as Dictionary).get("item_id", DEFAULT_MARKER_ITEM_ID))
	return DEFAULT_MARKER_ITEM_ID


# Raycast the terrain under the cursor and add a marker there; a miss (off the terrain) is
# ignored. Stays armed so several can be placed.
func _place_marker_armed_at(mouse_pos: Vector2) -> void:
	if terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		return
	add_marker_to_active_path_at_world(hit)


# --- Reorder / delete / clear -------------------------------------------------

# Move the selected marker one step earlier (-1) or later (+1) along the active path. This
# rewrites only the path's reference order (no marker entity changes), so it rebuilds just
# the overlay. One undo step. Inert at the ends or without a marker selection.
func move_selected_marker(delta: int) -> void:
	if _mission == null or _selected_marker.is_empty() or _selected_path_index < 0:
		return
	var path := _mission.get_waypoint_path(_selected_path_index)
	if path.is_empty():
		return
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	var marker_index := int(_selected_marker["marker_index"])
	var pos := indices.find(marker_index)
	if pos < 0:
		return
	var target := pos + delta
	if target < 0 or target >= indices.size():
		return
	var tmp := indices[pos]
	indices[pos] = indices[target]
	indices[target] = tmp
	_flush_edit()
	var before := _mission.snapshot()
	if _mission.set_waypoint_path(_selected_path_index, indices, int(path.get("flags", 0))):
		_push_undo_step(before)
		_refresh_waypoint_overlay()
		mark_dirty()


# Delete the selected marker entirely: remove_entity drops the KIND_MARKER entity and
# repairs every path that referenced it (drops the index, decrements higher ones). Markers
# reindex, so re-bake from the post-delete record. One undo step. Returns false if nothing
# is selected or the lib rejects it; clears the marker selection on success.
func delete_selected_marker() -> bool:
	if _mission == null or _selected_marker.is_empty():
		return false
	var marker_index := int(_selected_marker["marker_index"])
	_flush_edit()
	var before := _mission.snapshot()
	if not _mission.remove_entity(NovaMissionData.KIND_MARKER, marker_index):
		return false
	_push_undo_step(before)
	_rebake_objects()
	mark_dirty()
	return true


# Empty the active path AND delete its marker entities, so no orphaned markers are left
# behind (the path's references alone would orphan the nodes). Removes markers in descending
# index order so each removal stays valid; remove_entity repairs the path as it goes. One
# undo step. Returns false when the path is already empty.
func clear_active_path() -> bool:
	if _mission == null or _selected_path_index < 0:
		return false
	var path := _mission.get_waypoint_path(_selected_path_index)
	if path.is_empty():
		return false
	var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
	if indices.is_empty():
		return false
	var descending: Array = []
	for mi in indices:
		descending.append(int(mi))
	descending.sort()
	descending.reverse()
	_flush_edit()
	var before := _mission.snapshot()
	var removed := false
	for mi in descending:
		if _mission.remove_entity(NovaMissionData.KIND_MARKER, mi):
			removed = true
	if not removed:
		return false
	_push_undo_step(before)
	_selected_marker = {}
	_rebake_objects()
	mark_dirty()
	return true


# True when a GUI control that owns the keyboard currently has focus, so the viewport
# Delete/Backspace shortcut must stay inert (the user is typing in / interacting with a
# panel, not the 3D scene). Reaches the editor viewport through the bound terrain editor;
# returns false when there is no live viewport (e.g. a headless test driving synthetic
# events with no focused control), so the shortcut still fires there.
func _gui_focus_blocks_shortcut() -> bool:
	if terrain_editor == null or not terrain_editor.is_inside_tree():
		return false
	var vp := terrain_editor.get_viewport()
	if vp == null:
		return false
	var fo := vp.gui_get_focus_owner()
	return fo is LineEdit or fo is TextEdit or fo is SpinBox or fo is ItemList


# --- Selection geometry helpers -----------------------------------------------

func _find_entity(kind: int, index: int) -> Dictionary:
	if _mission == null:
		return {}
	# The entity dict's "index" equals its array position (to_record sets record.index =
	# i), so a direct get_entity is equivalent to scanning get_entities, and O(1).
	return _mission.get_entity(kind, index)


func _record_world_aabb(rec: Dictionary) -> AABB:
	if bool(rec.get("animated", false)):
		var node: Node3D = rec.get("node")
		return _node_world_aabb(node) if node != null and node.is_inside_tree() else AABB()
	var mmi: MultiMeshInstance3D = rec.get("mmi")
	var mm: MultiMesh = rec.get("mm")
	if mmi == null or mm == null or not mmi.is_inside_tree():
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
	if _selected_node != null:
		return _node_world_aabb(_selected_node)
	var result := AABB()
	var have := false
	for rec in _selected_records:
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
	if _selection_box != null and is_instance_valid(_selection_box):
		return _selection_box
	var container := _objects_container()
	if container == null:
		return null
	var mi := MeshInstance3D.new()
	mi.name = "MissionSelectionBox"
	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3.ONE
	mi.mesh = box_mesh
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = Color(0.25, 0.9, 1.0, 0.18)
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.material_override = mat
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	container.add_child(mi)
	_selection_box = mi
	return mi


func _hide_selection_box() -> void:
	if _selection_box != null and is_instance_valid(_selection_box):
		_selection_box.visible = false


# Clear selection refs without touching the scene. The selection box is a child of the
# objects container, so it is freed when the container is (re)built; here we only drop
# the dangling ref.
func _reset_selection_state() -> void:
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_xform = Transform3D.IDENTITY
	_selected_rotation_deg = Vector3.ZERO
	_drag_active = false
	_drag_moved = false
	_selection_box = null
	# Waypoint marker selection + overlay are tied to the container contents, so they reset
	# with it; the chosen path (_selected_path_index) persists across re-bakes by design.
	_selected_marker = {}
	_marker_pickable = []
	_waypoint_overlay = null


# --- Internals ----------------------------------------------------------------

# Load the mission's environment into the shared editor environment. Returns a
# short note ("" when clean) describing any problem, so the open status can be
# honest about a partial load. Environment is best-effort: the open does not fail
# just because the atmosphere is missing, but the rendered world is always made to
# match the mission rather than carrying over the previously-open mission's
# environment — when the mission brings no usable env, reset to a neutral default.
func _load_environment(mission: NovaMissionData, resource_root: NovaResourceRoot) -> String:
	if not terrain_editor.has_method("get_environment_editor"):
		return ""
	var env_editor = terrain_editor.get_environment_editor()
	if env_editor == null:
		return ""

	var note := ""
	var env_ref := mission.get_environment_ref().strip_edges()
	if not env_ref.is_empty():
		var env_path := resource_root.resolve_file(env_ref + ".env")
		if env_path.is_empty():
			note = "environment %s was not found" % env_ref
		elif env_editor.has_method("open_env") and int(env_editor.open_env(env_path)) == OK:
			return ""  # loaded the mission's own environment; nothing to reset or note
		else:
			note = "environment %s could not be loaded" % env_ref

	# Blank, unresolved, or unreadable reference: reset to a neutral default so the
	# atmosphere matches the inspector instead of lingering from a prior mission.
	if env_editor.has_method("create_default_environment"):
		env_editor.create_default_environment(false)
	return note


func _place_objects(mission: NovaMissionData, resource_root: NovaResourceRoot) -> void:
	_stats = {}
	# A fresh placement replaces the container (and the old selection box with it), so
	# drop any stale selection refs before re-harvesting the pickable index.
	_reset_selection_state()
	_pickable = []
	_place_item_id = 0
	_placer = null
	if not terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node3D = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	_placer = MissionObjectPlacer.new(resource_root)
	_placer.edit_mode = true
	var options: Dictionary = {}
	var env_node := _environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	_stats = _placer.place(mission, world_root, options)
	_pickable = _placer.pickable_records


func _environment_node() -> Node:
	if terrain_editor != null and terrain_editor.has_method("get_environment_node"):
		return terrain_editor.get_environment_node()
	return null


func _objects_container() -> Node3D:
	if terrain_editor == null or not terrain_editor.has_method("get_terrain_world_root"):
		return null
	var world_root: Node3D = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return null
	return world_root.get_node_or_null(NodePath(OBJECTS_CONTAINER)) as Node3D


func _clear_objects() -> void:
	var container := _objects_container()
	if container != null and container.get_parent() != null:
		container.get_parent().remove_child(container)
		container.queue_free()


func _describe_load(mission: NovaMissionData, bms_path: String, env_note: String = "") -> String:
	var mission_name := mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = bms_path.get_file()
	var placed := int(_stats.get("placed", 0))
	var unresolved := int(_stats.get("unresolved", 0))
	var text := "Loaded %s: %d objects placed" % [mission_name, placed]
	if unresolved > 0:
		text += ", %d unresolved" % unresolved
	if not env_note.is_empty():
		text += " (%s)" % env_note
	return text + "."
