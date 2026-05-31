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
	_last_status = _describe_load(mission, bms_path, env_note)
	changed.emit()
	return OK


func clear() -> void:
	_reset_selection_state()
	_pickable = []
	_place_item_id = 0
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
		# Right-click while armed cancels placement (a familiar "drop the tool" gesture)
		# and does not fall through to selection.
		if mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed and is_placement_armed():
			disarm_placement()
			return
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if mb.pressed:
			# Armed: left-click places a new instance at the cursor instead of selecting.
			# Stay armed so the user can place several; right-click / Escape / the Stop
			# button disarms.
			if is_placement_armed():
				_place_armed_at(mb.position)
			else:
				_on_left_press(mb.position)
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
		if key.keycode == KEY_ESCAPE and is_placement_armed():
			disarm_placement()
		elif (key.keycode == KEY_DELETE or key.keycode == KEY_BACKSPACE) and not key.ctrl_pressed and not _selected_ref.is_empty():
			# Delete the selected entity, unless a GUI control owns the keyboard. The router
			# feeds us via _unhandled_input, which only withholds keys a focused control
			# actually consumes -- a SpinBox holding focus via its arrows, an ItemList, or a
			# Button do NOT consume Delete/Backspace, so without this guard a stray Backspace
			# while editing a coordinate field would silently delete the object. Mirrors the
			# focus-owner guard in credits_editor / fnt_editor / terrain_editor. (Placement
			# arming also clears the selection, so this and an armed tool stay exclusive.)
			if not _gui_focus_blocks_shortcut():
				delete_selected()
	elif event is InputEventMouseMotion and _drag_active:
		var motion := event as InputEventMouseMotion
		# Defend against a missed button-up (e.g. the release landed on a different
		# control while switching workspaces): if the left button is no longer held,
		# the gesture was abandoned, not continuing, so end it without committing.
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) == 0:
			cancel_drag()
			return
		_on_drag(motion.position)


# End an in-progress drag without committing. The workspace calls this when it
# deactivates / unmounts so a half-finished gesture cannot silently resume (and
# relocate + dirty the selection) on a later bare hover after the user returns.
func cancel_drag() -> void:
	_drag_active = false
	_drag_moved = false
	# Close any open edit session. A drag is visual-only until _on_left_release commits
	# it, so a cancelled drag leaves the document unchanged and this pushes nothing; an
	# inspector edit session that happens to be open keeps its undo step (commit, not
	# discard, so a workspace switch mid-edit does not silently drop the step).
	commit_edit()


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
	_set_selected_property("team", value)


func set_selected_group(value: int) -> void:
	_set_selected_property("group", value)


func _set_selected_property(property: String, value: int) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	# team / group are stored as uint8 by the format; clamp at this API boundary so an
	# out-of-range value cannot silently wrap (the SpinBoxes already cap 0..255, but
	# this method is public).
	value = clampi(value, 0, 255)
	# A property change is its own undo step: close any open transform session first, then
	# capture the pre-edit state and record it only if the write actually changed the bytes
	# (a same-value write is a no-op).
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
