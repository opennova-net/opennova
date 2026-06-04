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
# Transient one-line status for an action the user just took (undo, delete, place, a
# rejected edit). The workspace relays it to the shell status bar; open / save keep their
# own relay (they poll get_last_status with bespoke durations), so this is for the actions
# that fire outside a workspace hook (e.g. the viewport Ctrl+Z / Delete path).
signal status_reported(message: String, is_error: bool)

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionWaypointOverlay := preload("res://engine/mission/mission_waypoint_overlay.gd")
const MissionAreaTriggerOverlay := preload("res://engine/mission/mission_area_trigger_overlay.gd")
# Must match MissionObjectPlacer.CONTAINER_NAME — that is where placed objects land.
const OBJECTS_CONTAINER := "MissionObjects"

# Editing modes. The viewport + inspector follow the active mode; they are mutually exclusive
# (entering one drops every other's selection + armed tool). OBJECTS is the default (P1-P5
# object authoring); WAYPOINTS is P7 marker authoring; AREA_TRIGGERS is zone authoring (Phase
# 2); SCRIPTING (Phase 4) is panel-driven (the viewport is inert in that mode).
enum Mode { OBJECTS, WAYPOINTS, AREA_TRIGGERS, SCRIPTING }

var terrain_editor: Node

var _mission: NovaMissionData
var _current_path: String = ""
# The resolved .trn path the loaded mission mounted, so a later terrain swap in the
# Terrain workspace can be detected (see reconcile_with_terrain()).
var _loaded_trn_path: String = ""
var _last_open_dir: String = ""
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
# For an animated selection, the model's ground-anchor offset (Transform3D applied
# as node.transform = entity_xform * offset). Static entities bake the same offset
# into each MultiMesh instance via the pickable record, so it only needs tracking
# for the animated-node path. IDENTITY when nothing animated is selected.
var _selected_node_offset: Transform3D = Transform3D.IDENTITY
var _selected_xform: Transform3D = Transform3D.IDENTITY
var _selected_rotation_deg: Vector3 = Vector3.ZERO
# Drag session: _drag_active spans press..release; _drag_moved gates the commit so a
# plain click only selects.
var _drag_active: bool = false
var _drag_moved: bool = false
# True when the most recent drag sample fell off the terrain. A drag that never lands a
# valid hit moves nothing (and commits nothing); this lets the release explain why.
var _drag_off_terrain: bool = false
# Place-new ("placement mode"): the armed items.def item id (0 = not armed). While
# armed, a left-click on the terrain places a new instance of this item instead of
# selecting / dragging; right-click or Escape disarms. See arm_placement().
var _place_item_id: int = 0
# Translucent box marking the selection in the viewport (lazily built under the
# objects container; freed with the container).
var _selection_box: MeshInstance3D

# --- Authoring (Phase 5): undo / redo -----------------------------------------
# The undo history + dirty flag live on the NovaMissionData document (in-memory bms::File
# snapshots, never serialized bytes); the controller is a thin driver. A continuous gesture (a
# terrain drag, a run of inspector SpinBox edits) is bracketed by begin_edit/commit_edit and
# becomes one step; one-shot mutations bracket the same way. undo/redo swap the document in
# memory and the controller re-bakes the world to match.
# Guards undo/redo against re-entrancy (a restore -> rebake -> changed -> inspector
# refresh must never re-enter another restore).
var _restoring: bool = false

# --- Authoring (P7): waypoints ------------------------------------------------
# Active editing mode (Mode.*). The viewport + inspector follow it; modes are exclusive, so
# switching clears the others' selection + any armed tool. Object editing (P1-P5) runs in
# Mode.OBJECTS, waypoint marker authoring in Mode.WAYPOINTS, zone authoring in
# Mode.AREA_TRIGGERS. Replaces the old `_waypoint_mode` bool.
var _mode: int = Mode.OBJECTS
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

# --- Authoring (Phase 2): area triggers / zones -------------------------------
# The in-world overlay drawing zone wire boxes + the selected zone's grab cube. Built under
# the objects container (frees with it); ref dropped on every re-bake, lazily rebuilt. Mirrors
# _waypoint_overlay.
var _area_overlay  # MissionAreaTriggerOverlay (preloaded, no class_name)
# The selected zone's index into the area-trigger list, or -1 when none is selected. Unlike
# the waypoint path this does NOT persist across re-bakes (a zone delete shifts indices), so it
# resets with the selection state.
var _selected_zone_index: int = -1
# Zone body pickables harvested from the overlay: one per zone, { zone_index, handle, aabb }.
var _zone_pickable: Array = []
# Whole-zone translate drag: the terrain hit where the drag began plus the zone's bounds at
# that moment (mission space). The drag previews the box; the record commits once on release.
var _zone_drag_start_hit: Vector3 = Vector3.ZERO
var _zone_drag_min: Vector3 = Vector3.ZERO
var _zone_drag_max: Vector3 = Vector3.ZERO
# The previewed (mission-space) bounds during a live zone drag, committed on release.
var _zone_preview_min: Vector3 = Vector3.ZERO
var _zone_preview_max: Vector3 = Vector3.ZERO
# Default half-extents (mission units) of a freshly added zone box, before the user resizes.
const DEFAULT_ZONE_HALF := Vector3(64.0, 64.0, 32.0)

# --- Authoring (Phase 4): mission scripting (events / triggers / actions) ------
# Scripting is panel-driven: the viewport is inert in Mode.SCRIPTING, so there is no overlay or
# pickable list, only a selected event. The selected event index persists across re-bakes (like the
# waypoint path, unlike the zone selection) so a logic edit elsewhere keeps the user on their event;
# it is clamped back into range whenever the event list shrinks (see get_selected_event_index). A
# structural undo/redo can REORDER events, which the in-range clamp cannot detect, so _restore drops
# the selection rather than risk binding it to a different event.
var _selected_event_index: int = -1


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
	# Exact, owned by the document: true iff it differs from the clean baseline (set at open /
	# save / new), so undoing back to the saved state clears the `*`.
	return _mission != null and _mission.is_dirty()


func get_current_path() -> String:
	return _current_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_stats() -> Dictionary:
	return _stats


func get_last_status() -> String:
	return _last_status


# Set the transient status line and notify the workspace so it surfaces in the shell
# status bar. `is_error` widens the on-screen duration. Distinct from open / save, which
# set _last_status directly and are surfaced by the workspace's own poll after the call
# returns; _report is for actions that also fire outside a workspace hook (the viewport
# Ctrl+Z / Delete path), so they need to push their own status.
func _report(message: String, is_error: bool = false) -> void:
	_last_status = message
	status_reported.emit(message, is_error)


# The items.def display name for an entity, or "" when it can't be resolved (no item
# database, unknown id, or blank name). Drives the inspector identity line and the
# delete / place status messages so the user reads a model name, not just an index.
func entity_display_name(kind: int, index: int) -> String:
	var entity := _find_entity(kind, index)
	if entity.is_empty():
		return ""
	var db := _item_db()
	if db == null:
		return ""
	var item_id := int(entity.get("item_id", 0))
	if not db.has_item(item_id):
		return ""
	return db.get_display_name(item_id).strip_edges()


# The resolved model name of the current selection, or "" when nothing is selected /
# unresolvable. The inspector pairs this with the kind + index for the identity line.
func get_selected_display_name() -> String:
	if _selected_ref.is_empty():
		return ""
	return entity_display_name(int(_selected_ref["kind"]), int(_selected_ref["index"]))


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
	return "%s%s" % [mission_name, "*" if is_dirty() else ""]


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
	# A fresh undo history for this document, and a clean baseline so the freshly-opened mission
	# is not dirty (and undoing back to it later clears the `*`).
	_clear_history()
	_mission.mark_clean()
	# Fresh document: drop any prior marker selection and focus a populated path (so the
	# waypoint panel is not empty) only if the user is already in waypoints mode.
	_selected_marker = {}
	_marker_place_armed = false
	_selected_path_index = _first_nonempty_path() if _mode == Mode.WAYPOINTS else -1
	# Focus the first zone when reopening already in area-trigger mode, mirroring set_mode (and the
	# waypoint branch above), so the Triggers panel is not empty after an open.
	_selected_zone_index = 0 if (_mode == Mode.AREA_TRIGGERS and mission.get_area_trigger_count() > 0) else -1
	if _mode == Mode.WAYPOINTS:
		_refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_refresh_area_trigger_overlay()
	_last_status = _describe_load(mission, bms_path, env_note)
	changed.emit()
	return OK


## Create a brand-new empty mission on the currently-loaded terrain. A mission needs a terrain
## both to place objects onto and to reference in its header, so this requires one to be loaded
## already (open or create a terrain first); it adopts that terrain's basename as the mission's
## terrain ref. The (empty) objects are placed so the placer + palette + picking are live, exactly
## as after an open. The mission has no file yet (Save routes to Save As) and is clean until the
## first edit. Returns OK, or an error; get_last_status() carries a human-facing reason.
func new_mission() -> Error:
	_last_status = ""
	if terrain_editor == null or not terrain_editor.has_method("get_resource_root"):
		_last_status = "No terrain editor is bound."
		return ERR_UNAVAILABLE
	var resource_root: NovaResourceRoot = terrain_editor.get_resource_root()
	if resource_root == null:
		_last_status = "Set a resource directory before creating a mission."
		return ERR_UNCONFIGURED
	var trn_path := String(terrain_editor.get_current_trn_path()) if terrain_editor.has_method("get_current_trn_path") else ""
	var world_root: Node3D = terrain_editor.get_terrain_world_root() if terrain_editor.has_method("get_terrain_world_root") else null
	if trn_path.is_empty() or world_root == null:
		_last_status = "Open or create a terrain first, then start a new mission on it."
		return ERR_UNCONFIGURED

	var mission := NovaMissionData.new()
	if mission.create_default() != OK:
		_last_status = "Could not create a new mission: %s" % mission.get_last_error()
		return FAILED
	# Self-describe: adopt the loaded terrain's basename so a later reopen resolves the same world.
	var terrain_ref := trn_path.get_file().get_basename()
	mission.set_header_string("terrain", terrain_ref)

	# Reset to a neutral environment (a fresh mission carries no env ref), then build the empty
	# world so the placer + palette + picking are live, exactly as after an open.
	var env_note := _load_environment(mission, resource_root)
	_place_objects(mission, resource_root)

	_mission = mission
	_current_path = ""          # no file yet; Save routes through Save As
	_loaded_trn_path = trn_path
	# Empty undo history and a clean baseline: the new mission is not dirty until the first edit
	# (Save As is always available regardless). mark_clean must follow create_default so the
	# baseline is the empty mission.
	_clear_history()
	_mission.mark_clean()
	_reset_selection_state()
	_selected_marker = {}
	_marker_place_armed = false
	_selected_path_index = -1
	_selected_zone_index = -1
	_selected_event_index = -1
	if _mode == Mode.WAYPOINTS:
		_refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_refresh_area_trigger_overlay()
	if not env_note.is_empty():
		_last_status = "New mission on %s (%s)." % [terrain_ref, env_note]
	else:
		_last_status = "New mission on %s." % terrain_ref
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
	# Dropping the document drops its undo history + clean baseline with it (they live on the
	# NovaMissionData), so there is nothing else to reset; is_dirty() reads false once _mission is null.
	_mission = null
	_current_path = ""
	_loaded_trn_path = ""
	_stats = {}
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


# Notify listeners that the document may have changed. The dirty flag itself is owned by the
# document (is_dirty -> _mission.is_dirty(), an exact compare against the clean baseline), so this
# just re-emits `changed` to refresh the title (`*`) + Save enablement + inspector.
func mark_dirty() -> void:
	changed.emit()


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
		# The saved state is the new clean baseline; the undo history is kept so the user can
		# still undo across the save.
		_mission.mark_clean()
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
		_mission.mark_clean()
		_last_status = "Saved %s." % filename
		changed.emit()
	else:
		_last_status = "Could not save %s: %s" % [filename, _mission.get_last_error()]
	return err as Error


# --- Authoring (Phase 5): undo / redo -----------------------------------------
# The history + dirty flag live on the document (NovaMissionData): in-memory bms::File snapshots,
# never serialized bytes. The controller drives them. A continuous gesture (a drag, a run of
# inspector edits) is bracketed by begin_edit/commit_edit so it becomes one step; one-shot
# mutations bracket the same way (commit pushes a step only if the document actually changed, so a
# plain click / same-value / failed edit pushes nothing). undo/redo swap the document in memory
# (O(1), cannot fail) and the controller re-bakes the world to match. Triggered by the viewport
# Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y; also exposed for the workspace's framework hooks. Selection is
# dropped on restore: structural edits reindex entities, and the re-bake resets selection anyway.

func can_undo() -> bool:
	return _mission != null and _mission.can_undo()


func can_redo() -> bool:
	return _mission != null and _mission.can_redo()


# The number of undo steps on the stack (for tests / a future history readout).
func undo_depth() -> int:
	return _mission.undo_depth() if _mission != null else 0


# Open an edit session, capturing the pre-edit document once. Inert if a session is already open
# (so a run of axis edits coalesces) or no mission is loaded. Delegates to the document.
func begin_edit() -> void:
	if _mission != null:
		_mission.begin_edit()


# Close an edit session, pushing one undo step only if the document actually changed (a plain
# click, a same-value edit, or a programmatic refresh push nothing). Delegates to the document.
func commit_edit() -> void:
	if _mission != null:
		_mission.commit_edit()


func _flush_edit() -> void:
	commit_edit()


func _clear_history() -> void:
	if _mission != null:
		_mission.clear_history()


func undo() -> void:
	if _restoring:
		return
	# A keyboard undo can arrive mid-drag; cancel_drag abandons the visual gesture (so the
	# re-bake does not free nodes a continuing drag still references) and commits any open
	# edit session as its step before we rewind.
	cancel_drag()
	if _mission == null:
		return
	if not _mission.can_undo():
		_report("Nothing to undo.")
		return
	_restoring = true
	var prev_event_count := _mission.get_event_count()
	_mission.undo()
	_after_restore(prev_event_count)
	_restoring = false
	_report("Undid the last change.")


func redo() -> void:
	if _restoring:
		return
	cancel_drag()
	if _mission == null:
		return
	if not _mission.can_redo():
		_report("Nothing to redo.")
		return
	_restoring = true
	var prev_event_count := _mission.get_event_count()
	_mission.redo()
	_after_restore(prev_event_count)
	_restoring = false
	_report("Redid the last change.")


# Sync the world + selection to the document after an in-memory undo/redo swap, then re-bake and
# notify once so the inspector refreshes against the restored world in a single pass. Adding /
# deleting an event shifts later event indices, so a kept _selected_event_index could bind to a
# DIFFERENT event (the in-range clamp can't see a shift); drop the selection only when the event
# set actually changed (event add/delete are the only ops that change the count -- there is no
# event-reorder op -- so a count change is exactly the structural case). An attribute / trigger /
# action undo leaves the list intact and keeps the user on their event.
func _after_restore(prev_event_count: int) -> void:
	if _mission.get_event_count() != prev_event_count:
		_selected_event_index = -1
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
			if _mode == Mode.WAYPOINTS:
				_on_marker_left_press(mb.position)
			# Area-trigger mode: left-click selects a zone and starts a translate drag.
			elif _mode == Mode.AREA_TRIGGERS:
				_on_zone_left_press(mb.position)
			# Armed: left-click places a new instance at the cursor instead of selecting.
			# Stay armed so the user can place several; right-click / Escape / the Stop
			# button disarms.
			elif is_placement_armed():
				_place_armed_at(mb.position)
			else:
				_on_left_press(mb.position)
		else:
			if _mode == Mode.WAYPOINTS:
				_on_marker_left_release()
			elif _mode == Mode.AREA_TRIGGERS:
				_on_zone_left_release()
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
				if _mode == Mode.WAYPOINTS and not _selected_marker.is_empty():
					delete_selected_marker()
				elif _mode == Mode.AREA_TRIGGERS and _selected_zone_index >= 0:
					delete_selected_area_trigger()
				elif _mode == Mode.OBJECTS and not _selected_ref.is_empty():
					delete_selected()
	elif event is InputEventMouseMotion and _drag_active:
		var motion := event as InputEventMouseMotion
		# Defend against a missed button-up (e.g. the release landed on a different
		# control while switching workspaces): if the left button is no longer held,
		# the gesture was abandoned, not continuing, so end it without committing.
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) == 0:
			cancel_drag()
			return
		# Drag the active mode's selection: a marker in waypoints mode, a zone in area-trigger
		# mode, an object otherwise.
		if _mode == Mode.WAYPOINTS:
			_on_marker_drag(motion.position)
		elif _mode == Mode.AREA_TRIGGERS:
			_on_zone_drag(motion.position)
		else:
			_on_drag(motion.position)


# End an in-progress drag without committing. The workspace calls this when it
# deactivates / unmounts so a half-finished gesture cannot silently resume (and
# relocate + dirty the selection) on a later bare hover after the user returns.
func cancel_drag() -> void:
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	# Close any open edit session. A drag is visual-only until release commits it, so a
	# cancelled drag leaves the document unchanged and this pushes nothing; an inspector edit
	# session that happens to be open keeps its undo step (commit, not discard, so a workspace
	# switch mid-edit does not silently drop the step).
	commit_edit()
	# A cancelled marker / zone drag previewed the gizmo but wrote no record; snap it back to
	# the stored position.
	if _mode == Mode.WAYPOINTS:
		_refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_refresh_area_trigger_overlay()


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
	_drag_off_terrain = false
	# Snapshot the pre-drag state; _on_left_release commits it as one step iff the entity
	# actually moved.
	begin_edit()


func _on_drag(mouse_pos: Vector2) -> void:
	if _selected_ref.is_empty() or terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		# Off the terrain: leave the object at its last valid spot and remember the miss so
		# the release can explain a drag that never landed anywhere.
		_drag_off_terrain = true
		return
	_drag_off_terrain = false
	_drag_moved = true
	_move_selected_to_world(hit)


func _on_left_release() -> void:
	if _drag_active and _drag_moved:
		_commit_selected_transform()
	elif _drag_active and _drag_off_terrain and not _drag_moved:
		# A drag that only ever sampled off-terrain moved nothing; say so rather than leaving
		# the user wondering why the object stayed put.
		_report("Drag ended off the terrain; the object was not moved.")
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
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
	_selected_node_offset = Transform3D.IDENTITY
	for rec in _pickable:
		if int(rec["kind"]) == kind and int(rec["index"]) == index:
			# Skip records whose backing node was freed (e.g. a re-bake mid-flight): a stale
			# ref would dangle through _apply_selected_xform. A dropped record just means no
			# box / no drag handle for that slot, not a crash.
			if bool(rec.get("animated", false)):
				var node = rec.get("node")
				if node != null and is_instance_valid(node):
					_selected_node = node
					_selected_node_offset = rec.get("offset", Transform3D.IDENTITY)
			else:
				var mmi = rec.get("mmi")
				if mmi != null and is_instance_valid(mmi):
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
	_selected_node_offset = Transform3D.IDENTITY
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
		# The anchor offset rides the node so the dragged model keeps its ground point
		# under the cursor, matching how it was first placed.
		_selected_node.transform = _selected_xform * _selected_node_offset
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
	# these methods are public). Surface the clamp so a corrected value is not a surprise.
	var clamped := clampi(value, 0, 255)
	if clamped != value:
		_report("Team clamped to the 0 to 255 range.")
	set_selected_property("team", clamped)


func set_selected_group(value: int) -> void:
	var clamped := clampi(value, 0, 255)
	if clamped != value:
		_report("Group clamped to the 0 to 255 range.")
	set_selected_property("group", clamped)


# Generic per-entity scalar property edit from the inspector: team / group plus the
# AI + waypoint fields the format carries (waypoint_id, wp_number, perception, accuracy,
# alert_state, the engagement / attack distances, spawn_count, max_simultaneous,
# ai_flags). `property` is the entity-dictionary key it edits. A property change is its
# own undo step: close any open transform session first, then bracket the write with
# begin_edit/commit_edit so it records one step only if the write actually changed the document
# (a same-value write is a no-op). The value range is governed by the inspector's SpinBoxes and the format's field
# widths, so this does not clamp; team / group clamp through their wrappers above.
func set_selected_property(property: String, value: int) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	# set_entity_property_int returns false only on rejection (bad index, unknown property,
	# failed write) -- never on a benign same-value write -- so a false return is a real
	# error worth surfacing rather than swallowing.
	if _mission.set_entity_property_int(int(_selected_ref["kind"]), int(_selected_ref["index"]), property, value):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not set %s on the selected object." % property, true)


# String counterpart of set_selected_property, for the fixed-string entity fields
# "name1" (AI class) and "name2" (AI script). Same snapshot / one-undo-step model.
func set_selected_string_property(property: String, value: String) -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_entity_property_string(int(_selected_ref["kind"]), int(_selected_ref["index"]), property, value):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not set %s on the selected object." % property, true)


# --- Authoring: mission-header editing ----------------------------------------
# Each setter snapshots, writes one header field through NovaMissionData, then pushes a
# single undo step. Field names match NovaMissionData::set_header_* and the inspector form.
func set_header_string(field: String, value: String) -> void:
	if _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_header_string(field, value):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not set mission %s." % field, true)


func set_header_int(field: String, value: int) -> void:
	if _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_header_int(field, value):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not set mission %s." % field, true)


func set_header_flag(bit: int, on: bool) -> void:
	if _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_header_flag(bit, on):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not set mission flag.", true)


# --- Weapon loadout + groups (mission-global) ---------------------------------
# Loadout entries are dictionaries { index, name, value1, value2 }; groups are
# { index, field0, field8, field12 }. Both edit through the one-step snapshot/undo recipe.

func get_weapon_loadout() -> Array:
	if _mission == null:
		return []
	return _mission.get_weapon_loadout()


func set_weapon_loadout(entries: Array) -> void:
	if _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_weapon_loadout(entries):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not update the weapon loadout.", true)


func get_group_count() -> int:
	if _mission == null:
		return 0
	return _mission.get_group_count()


func get_groups() -> Array:
	if _mission == null:
		return []
	return _mission.get_groups()


func get_group(index: int) -> Dictionary:
	if _mission == null:
		return {}
	return _mission.get_group(index)


func set_group(index: int, field0: int, field8: int, field12: int) -> void:
	if _mission == null:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_group(index, field0, field8, field12):
		_mission.commit_edit()
		mark_dirty()
	else:
		_report("Could not update group %d." % index, true)


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
	# A readable label for the status line: the model name when resolvable, else the raw id.
	var item_name: String = db.get_display_name(item_id) if db != null and db.has_item(item_id) else ""
	if item_name.is_empty():
		item_name = "item %d" % item_id
	var local := container.global_transform.affine_inverse() * global_hit
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(local)
	# Placing is its own undo step: close any open session, then bracket the add with
	# begin_edit/commit_edit (commit pushes one step iff the add changed the document).
	_flush_edit()
	_mission.begin_edit()
	var record := _mission.add_entity(kind, item_id, bms_pos, Vector3.ZERO)
	if record.is_empty():
		_report("Could not place %s." % item_name, true)
		return false
	_mission.commit_edit()
	var new_index := int(record.get("index", -1))
	_render_placed_entity(kind, new_index)
	mark_dirty()
	_select(kind, new_index)
	_report("Placed %s. Ctrl+Z to undo." % item_name)
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
	# Capture a readable label before the removal: after the re-bake the selection (and its
	# resolvable name) is gone.
	var label := get_selected_display_name()
	# Deleting is its own undo step: close any open session, then bracket the removal with
	# begin_edit/commit_edit (a successful removal always changes the document, so this is never a
	# no-op step).
	_flush_edit()
	_mission.begin_edit()
	if not _mission.remove_entity(kind, index):
		_report("Could not delete the selected object.", true)
		return false
	_mission.commit_edit()
	# Re-bake first (it resets the selection state and rebuilds stats), then dirty +
	# emit once so the inspector refreshes against the post-delete world in a single pass.
	_rebake_objects()
	mark_dirty()
	_report("Deleted %s. Ctrl+Z to undo." % (label if not label.is_empty() else "object"))
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
	# The re-bake replaced the container (and the old overlay with it); rebuild the active
	# mode's overlay against the new world.
	if _mode == Mode.WAYPOINTS:
		_refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_refresh_area_trigger_overlay()


# --- Authoring (P7): waypoint mode + marker selection -------------------------
# Waypoints mode switches the viewport from object editing to authoring the active path's
# markers, and the inspector to the waypoint panel. The two modes are exclusive: entering
# either drops the other's selection and any armed placement tool. The chosen path persists
# across re-bakes; the marker selection (like the object selection) does not. Marker
# picking reuses the same analytic ray-vs-AABB as objects, over the overlay's gizmo AABBs.

# Switch the active editing mode (Mode.*). A mode switch is a fresh context: it closes any
# open edit session, then drops EVERY mode's selection + armed tool so only the new mode's
# clicks are live. Each mode focuses a sensible default on entry (a populated waypoint path /
# the first zone) and toggles its overlay's visibility. Inert if already in `mode`.
func set_mode(mode: int) -> void:
	if _mode == mode:
		return
	_flush_edit()
	_mode = mode
	# Exclusive selection: clear the object selection refs + its box, the marker selection,
	# the zone selection, and any armed placement tool.
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_node_offset = Transform3D.IDENTITY
	_hide_selection_box()
	_selected_marker = {}
	_selected_zone_index = -1
	_place_item_id = 0
	_marker_place_armed = false
	if mode == Mode.WAYPOINTS and _selected_path_index < 0:
		# Focus a populated path on entry so the panel is not empty.
		_selected_path_index = _first_nonempty_path()
	if mode == Mode.AREA_TRIGGERS and _mission != null and _mission.get_area_trigger_count() > 0:
		# Focus the first zone on entry so the panel is not empty.
		_selected_zone_index = 0
	if mode == Mode.SCRIPTING and _mission != null and get_selected_event_index() < 0 and _mission.get_event_count() > 0:
		# Focus the first event on entry so the scripting panel is not empty (get_selected_event_index
		# reads a stale-but-out-of-range selection as -1, so a shrunken list re-focuses event 0).
		_selected_event_index = 0
	_refresh_waypoint_overlay()
	_refresh_area_trigger_overlay()
	# Each overlay is visible only in its own mode.
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.visible = mode == Mode.WAYPOINTS
	if _area_overlay != null and is_instance_valid(_area_overlay):
		_area_overlay.visible = mode == Mode.AREA_TRIGGERS
	changed.emit()


func get_mode() -> int:
	return _mode


# Backward-compatible wrapper: waypoints mode is Mode.WAYPOINTS, otherwise Mode.OBJECTS.
func set_waypoint_mode(enabled: bool) -> void:
	set_mode(Mode.WAYPOINTS if enabled else Mode.OBJECTS)


func is_waypoint_mode() -> bool:
	return _mode == Mode.WAYPOINTS


func is_area_trigger_mode() -> bool:
	return _mode == Mode.AREA_TRIGGERS


func is_objects_mode() -> bool:
	return _mode == Mode.OBJECTS


func is_scripting_mode() -> bool:
	return _mode == Mode.SCRIPTING


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
	_mission.begin_edit()
	if _mission.set_waypoint_path(_selected_path_index, indices, flags):
		_mission.commit_edit()
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
		_waypoint_overlay.visible = _mode == Mode.WAYPOINTS
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
	_drag_off_terrain = false
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
# Marker editing reuses the object authoring spine: the terrain-regrounding drag and the
# begin_edit/commit_edit undo bracketing (each gesture is one step). A
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
		_drag_off_terrain = true
		return
	var container := _objects_container()
	if container == null:
		return
	_drag_off_terrain = false
	_drag_moved = true
	_marker_drag_local = container.global_transform.affine_inverse() * hit
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.preview_marker_position(int(_selected_marker["marker_index"]), _marker_drag_local)


func _on_marker_left_release() -> void:
	if _drag_active and _drag_moved and not _selected_marker.is_empty():
		_commit_marker_drag()
	elif _drag_active and _drag_off_terrain and not _drag_moved:
		# A marker dragged only over off-terrain space moved nothing; snap the previewed gizmo
		# back to its stored position and say why.
		_report("Drag ended off the terrain; the marker was not moved.")
		_refresh_waypoint_overlay()
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
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
	_mission.begin_edit()
	var result := _mission.add_waypoint_marker(_selected_path_index, _default_marker_item_id(), bms_pos, Vector3.ZERO, -1)
	if result.is_empty():
		_report("Could not add a waypoint marker.", true)
		return false
	_mission.commit_edit()
	_refresh_waypoint_overlay()
	var marker_index := int((result.get("marker", {}) as Dictionary).get("index", -1))
	if marker_index >= 0:
		_select_marker(_selected_path_index, marker_index)
	mark_dirty()
	return true


# The item id to seed a new marker with. Prefer copying an existing marker's id so shipped
# data keeps its own marker type. When authoring from scratch, use a marker-type id the
# loaded database actually carries (so the marker resolves to a real model) rather than a
# hardcoded id the database might not have; only then fall back to the plausible default.
func _default_marker_item_id() -> int:
	if _mission != null:
		var markers := _mission.get_entities(NovaMissionData.KIND_MARKER)
		if not markers.is_empty():
			return int((markers[0] as Dictionary).get("item_id", DEFAULT_MARKER_ITEM_ID))
	var db := _item_db()
	if db != null:
		for item in db.get_items():
			var entry: Dictionary = item
			if int(entry.get("type", -1)) == NovaItemDatabase.TYPE_MARKER:
				return int(entry.get("id", DEFAULT_MARKER_ITEM_ID))
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
	_mission.begin_edit()
	if _mission.set_waypoint_path(_selected_path_index, indices, int(path.get("flags", 0))):
		_mission.commit_edit()
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
	_mission.begin_edit()
	if not _mission.remove_entity(NovaMissionData.KIND_MARKER, marker_index):
		return false
	_mission.commit_edit()
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
	# Dedup before removing: a (corrupt/hand-edited) path can list the same marker index twice, and
	# removing descending would delete the duplicate's now-shifted neighbour on the second pass.
	var descending: Array = []
	for mi in indices:
		var idx := int(mi)
		if not descending.has(idx):
			descending.append(idx)
	descending.sort()
	descending.reverse()
	_flush_edit()
	_mission.begin_edit()
	var removed := false
	for mi in descending:
		if _mission.remove_entity(NovaMissionData.KIND_MARKER, mi):
			removed = true
	if not removed:
		return false
	_mission.commit_edit()
	_selected_marker = {}
	_rebake_objects()
	mark_dirty()
	return true


# --- Authoring (Phase 2): area triggers / zones -------------------------------
# Zone authoring mirrors the marker spine: ray-vs-AABB picking over the overlay's zone body
# AABBs, a terrain-projected translate drag that previews the box and commits the record once
# on release (the begin_edit/commit_edit bracket makes it one undo step), and the same
# begin_edit/commit_edit bracket for one-shot mutations (add / set / flags / delete). Resize is
# precise through the inspector spins (set_selected_zone_bounds); the in-world drag translates
# the whole box. The engine does not auto-swap area-trigger bounds, so the binding normalizes
# min<=max on every write (NovaMissionData.add/set_area_trigger).

func get_area_triggers() -> Array:
	return _mission.get_area_triggers() if _mission != null else []


# Flat list of every entity (all kinds), shaped for a scripting param picker: { value: bms_id, label }.
# Single/Player triggers and Single actions reference a unit by its BMS/net id, not an array index
# ([orig: EntityPool_FindByNetId @0x4f0a20]); an unmatched value still round-trips as a raw row.
func get_all_entities() -> Array:
	if _mission == null:
		return []
	var out: Array = []
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		for e in _mission.get_entities(kind):
			var ed := e as Dictionary
			var bms_id := int(ed.get("bms_id", 0))
			var display := entity_display_name(kind, int(ed.get("index", 0)))
			var label := ("%s #%d" % [display, bms_id]) if display != "" else ("Unit #%d" % bms_id)
			out.append({ "value": bms_id, "label": label })
	return out


func get_selected_zone_index() -> int:
	return _selected_zone_index


# The selected zone dict (NovaMissionData shape), or {} when none is selected / no mission.
func get_selected_zone() -> Dictionary:
	if _mission == null or _selected_zone_index < 0:
		return {}
	return _mission.get_area_trigger(_selected_zone_index)


# Select a zone by index (the inspector list drives this). Rebuilds the overlay so the bright
# box + grab cube follow. Inert if unchanged.
func select_area_trigger(index: int) -> void:
	if index == _selected_zone_index:
		return
	_selected_zone_index = index
	_refresh_area_trigger_overlay()
	changed.emit()


# Add a new zone box centred on the placed world (the average of item positions, else origin),
# select it, and dirty. Public so it is testable without a camera. Returns the new index, or -1.
func add_area_trigger_default() -> int:
	if _mission == null:
		return -1
	var center := _world_center_mission()
	var half := DEFAULT_ZONE_HALF
	_flush_edit()
	_mission.begin_edit()
	# A fresh zone is active with Z unbounded (the common out-of-bounds region); the user
	# constrains Z and resizes afterwards.
	var zone := _mission.add_area_trigger(center - half, center + half, true, false, 0)
	if zone.is_empty():
		_report("Could not add an area trigger.", true)
		return -1
	_mission.commit_edit()
	_selected_zone_index = int(zone.get("index", -1))
	_refresh_area_trigger_overlay()
	mark_dirty()
	return _selected_zone_index


# Overwrite the selected zone's bounds (mission space) from the inspector spins. One undo step.
func set_selected_zone_bounds(mn: Vector3, mx: Vector3) -> void:
	if _mission == null or _selected_zone_index < 0:
		return
	var zone := _mission.get_area_trigger(_selected_zone_index)
	if zone.is_empty():
		return
	_flush_edit()
	_mission.begin_edit()
	var updated := _mission.set_area_trigger(_selected_zone_index, mn, mx,
		bool(zone.get("active", false)), bool(zone.get("constrain_z", false)), int(zone.get("id", 0)))
	if not updated.is_empty():
		_mission.commit_edit()
		_refresh_area_trigger_overlay()
		mark_dirty()


# Set the selected zone's two known flag bits (active / constrain-Z). One undo step.
func set_selected_zone_flags(active: bool, constrain_z: bool) -> void:
	if _mission == null or _selected_zone_index < 0:
		return
	var zone := _mission.get_area_trigger(_selected_zone_index)
	if zone.is_empty():
		return
	_flush_edit()
	_mission.begin_edit()
	var updated := _mission.set_area_trigger(_selected_zone_index, zone.get("min", Vector3.ZERO),
		zone.get("max", Vector3.ZERO), active, constrain_z, int(zone.get("id", 0)))
	if not updated.is_empty():
		_mission.commit_edit()
		_refresh_area_trigger_overlay()
		mark_dirty()


# Delete the selected zone. Structural (shifts later indices), so the overlay rebuilds and the
# selection drops. *IsWithinArea trigger param2 references are auto-repaired in the lib (Phase-5 RE
# confirmed param2 is an array index): higher refs shift down, a direct hit becomes -1 (dangling, which
# the scripting diagnostics then flag). One undo step. False if none selected.
func delete_selected_area_trigger() -> bool:
	if _mission == null or _selected_zone_index < 0:
		return false
	_flush_edit()
	_mission.begin_edit()
	if not _mission.remove_area_trigger(_selected_zone_index):
		return false
	_mission.commit_edit()
	_selected_zone_index = -1
	_refresh_area_trigger_overlay()
	_report("Zone deleted. Triggers that referenced a higher zone shifted down; a direct reference was unset.")
	mark_dirty()
	return true


# (Re)build the in-world zone overlay from the current mission, harvesting the zone pickables.
# Creates the overlay node under the objects container on first use (and after a re-bake freed
# it). `preview` optionally overrides the dragged zone's bounds. Mirrors _refresh_waypoint_overlay.
func _refresh_area_trigger_overlay(preview := {}) -> void:
	if _mission == null:
		return
	var container := _objects_container()
	if container == null:
		return
	if _area_overlay == null or not is_instance_valid(_area_overlay):
		_area_overlay = MissionAreaTriggerOverlay.new()
		_area_overlay.name = "MissionAreaTriggerOverlay"
		_area_overlay.visible = _mode == Mode.AREA_TRIGGERS
		container.add_child(_area_overlay)
	_area_overlay.rebuild(_mission, _selected_zone_index, preview)
	_zone_pickable = _area_overlay.zone_pickables()


func _on_zone_left_press(mouse_pos: Vector2) -> void:
	_flush_edit()
	var index := _pick_zone(mouse_pos)
	if index < 0:
		_deselect_zone()
		return
	if index != _selected_zone_index:
		_selected_zone_index = index
		_refresh_area_trigger_overlay()
		changed.emit()
	# Begin a translate drag: motion re-grounds the box centre on the terrain, release writes
	# the record once (a plain click just selects). The bracket makes the drag one undo step.
	var zone := _mission.get_area_trigger(index)
	_zone_drag_min = zone.get("min", Vector3.ZERO)
	_zone_drag_max = zone.get("max", Vector3.ZERO)
	_zone_preview_min = _zone_drag_min
	_zone_preview_max = _zone_drag_max
	_drag_active = true
	_drag_moved = false
	_drag_off_terrain = false
	begin_edit()


# Pick the nearest zone under the cursor (ray-vs-AABB over the overlay's body AABBs), or -1 on
# a miss. Mirrors _pick_marker.
func _pick_zone(mouse_pos: Vector2) -> int:
	if terrain_editor == null or not terrain_editor.has_method("get_editor_camera"):
		return -1
	var camera: Camera3D = terrain_editor.get_editor_camera()
	if camera == null:
		return -1
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best := -1
	for rec in _zone_pickable:
		var aabb: AABB = rec["aabb"]
		if aabb.size == Vector3.ZERO:
			continue
		var t := _ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = int(rec["zone_index"])
	return best


# Translate the selected box horizontally to follow the terrain hit (the vertical extent is
# left unchanged). Previews the overlay box only; the record commits once on release.
func _on_zone_drag(mouse_pos: Vector2) -> void:
	if _selected_zone_index < 0 or terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		_drag_off_terrain = true
		return
	if not _drag_moved:
		# First valid sample anchors the drag so the box does not jump to the cursor.
		_zone_drag_start_hit = hit
	_drag_off_terrain = false
	_drag_moved = true
	# Godot (x, z) map to mission (x, -y); the vertical (godot y / mission z) extent is kept.
	var d := hit - _zone_drag_start_hit
	var mission_delta := Vector3(d.x, -d.z, 0.0)
	_zone_preview_min = _zone_drag_min + mission_delta
	_zone_preview_max = _zone_drag_max + mission_delta
	_refresh_area_trigger_overlay({ "index": _selected_zone_index, "min": _zone_preview_min, "max": _zone_preview_max })


func _on_zone_left_release() -> void:
	if _drag_active and _drag_moved and _selected_zone_index >= 0:
		_commit_zone_drag()
	elif _drag_active and _drag_off_terrain and not _drag_moved:
		_report("Drag ended off the terrain; the zone was not moved.")
		_refresh_area_trigger_overlay()
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	# Push the drag as one step (no-op for a plain click: nothing was written).
	commit_edit()


# Write the dragged box's previewed bounds back to the record, keeping its flags + id.
func _commit_zone_drag() -> void:
	if _mission == null or _selected_zone_index < 0:
		return
	var zone := _mission.get_area_trigger(_selected_zone_index)
	if zone.is_empty():
		return
	var updated := _mission.set_area_trigger(_selected_zone_index, _zone_preview_min, _zone_preview_max,
		bool(zone.get("active", false)), bool(zone.get("constrain_z", false)), int(zone.get("id", 0)))
	if not updated.is_empty():
		_refresh_area_trigger_overlay()
		mark_dirty()


func _deselect_zone() -> void:
	if _selected_zone_index < 0:
		return
	_selected_zone_index = -1
	_refresh_area_trigger_overlay()
	changed.emit()


# --- Authoring (Phase 4): mission scripting forwarders ------------------------
# Panel-driven (no viewport interaction): the inspector's Scripting tab calls these, each on the same
# begin_edit -> mutate -> commit_edit -> mark_dirty undo recipe as the other modes. Reads pass through
# to the binding; every read tolerates "no mission" by returning an empty value.

func get_event_count() -> int:
	return _mission.get_event_count() if _mission != null else 0


func get_events() -> Array:
	return _mission.get_events() if _mission != null else []


# The selected event index, or -1 when nothing valid is selected. Pure read (no side effects): a
# selection that fell out of range (the event list shrank under it via a delete or an undo) reads as
# "nothing selected" rather than a stale index, and the caller re-selects from the list. The stored
# field is left alone; every read re-validates it against the current event count.
func get_selected_event_index() -> int:
	if _mission == null or _selected_event_index < 0 or _selected_event_index >= _mission.get_event_count():
		return -1
	return _selected_event_index


# The selected event's full chain { event, triggers, actions, references, diagnostics }, or {}.
func get_selected_event_chain() -> Dictionary:
	var index := get_selected_event_index()
	if _mission == null or index < 0:
		return {}
	return _mission.get_event_chain(index)


func get_logic_summary() -> Dictionary:
	return _mission.get_logic_summary() if _mission != null else {}


func get_trigger_main_types() -> Array:
	return _mission.get_trigger_main_types() if _mission != null else []


func get_trigger_sub_types(main_type: int) -> Array:
	return _mission.get_trigger_sub_types(main_type) if _mission != null else []


func get_action_types() -> Array:
	return _mission.get_action_types() if _mission != null else []


func get_action_sub_types(action_type: int) -> Array:
	return _mission.get_action_sub_types(action_type) if _mission != null else []


func get_event_flag_bits() -> Array:
	return _mission.get_event_flag_bits() if _mission != null else []


# Focus an event by index (the inspector list drives this). Inert if unchanged.
func select_event(index: int) -> void:
	if index == _selected_event_index:
		return
	_selected_event_index = index
	changed.emit()


# Append a new empty event, select it, and dirty. One undo step. Returns the new index, or -1.
func add_event_default() -> int:
	if _mission == null:
		return -1
	_flush_edit()
	_mission.begin_edit()
	var event := _mission.add_event(0, 0, 0)
	if event.is_empty():
		_report("Could not add an event.", true)
		return -1
	_mission.commit_edit()
	_selected_event_index = int(event.get("index", -1))
	mark_dirty()
	return _selected_event_index


# Delete the selected event (drops its triggers + actions; ResetEvent references are repaired in the
# lib). Structural, so the selection clamps to the shrunken list. One undo step. False if none selected.
func delete_selected_event() -> bool:
	if _mission == null or get_selected_event_index() < 0:
		return false
	_flush_edit()
	_mission.begin_edit()
	if not _mission.remove_event(_selected_event_index):
		return false
	_mission.commit_edit()
	# Drop the selection after a delete (like the zone panel): the row the user was on is gone, and the
	# index would otherwise point at the event that shifted into its slot.
	_selected_event_index = -1
	_report("Event deleted. A ResetEvent action that pointed past it was repaired; a direct hit was unset.")
	mark_dirty()
	return true


# Overwrite the selected event's own attributes (the EventFlags bitfield + reset_after / delay). One step.
func set_selected_event(flags: int, reset_after: int, delay: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.set_event(_selected_event_index, flags, reset_after, delay):
		_mission.commit_edit()
		mark_dirty()


# Append a trigger to the selected event (defaults to a Group / Null condition). One undo step.
func add_selected_event_trigger() -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	var chain := _mission.add_event_trigger(_selected_event_index, {})
	if chain.is_empty():
		_report("Could not add a trigger (an event chains at most 20).", true)
		return
	_mission.commit_edit()
	mark_dirty()


# Overwrite the trigger at `local_index` (its position in the event's chain) from an editor dict. One step.
func set_selected_event_trigger(local_index: int, trigger: Dictionary) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	var chain := _mission.set_event_trigger(_selected_event_index, local_index, trigger)
	if not chain.is_empty():
		_mission.commit_edit()
		mark_dirty()


func remove_selected_event_trigger(local_index: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.remove_event_trigger(_selected_event_index, local_index):
		_mission.commit_edit()
		mark_dirty()


func move_selected_event_trigger(local_index: int, delta: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.move_event_trigger(_selected_event_index, local_index, delta):
		_mission.commit_edit()
		mark_dirty()


# Append an action to the selected event (defaults to a Null action). One undo step.
func add_selected_event_action() -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	var chain := _mission.add_event_action(_selected_event_index, {})
	if chain.is_empty():
		_report("Could not add an action (an event chains at most 20).", true)
		return
	_mission.commit_edit()
	mark_dirty()


func set_selected_event_action(local_index: int, action: Dictionary) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	var chain := _mission.set_event_action(_selected_event_index, local_index, action)
	if not chain.is_empty():
		_mission.commit_edit()
		mark_dirty()


func remove_selected_event_action(local_index: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.remove_event_action(_selected_event_index, local_index):
		_mission.commit_edit()
		mark_dirty()


func move_selected_event_action(local_index: int, delta: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_flush_edit()
	_mission.begin_edit()
	if _mission.move_event_action(_selected_event_index, local_index, delta):
		_mission.commit_edit()
		mark_dirty()


# Mission-space centre of the placed world: the average of item positions, else origin. Used
# to drop a new zone somewhere visible rather than at (0,0,0) off in a corner.
func _world_center_mission() -> Vector3:
	if _mission == null:
		return Vector3.ZERO
	var items: Array = _mission.get_entities(NovaMissionData.KIND_ITEM)
	if items.is_empty():
		return Vector3.ZERO
	var sum := Vector3.ZERO
	for it in items:
		sum += (it as Dictionary).get("position", Vector3.ZERO)
	return sum / float(items.size())


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
	_selection_box = mi
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
	if _selection_box != null and is_instance_valid(_selection_box):
		_selection_box.visible = false


# Clear selection refs without touching the scene. The selection box is a child of the
# objects container, so it is freed when the container is (re)built; here we only drop
# the dangling ref.
func _reset_selection_state() -> void:
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_node_offset = Transform3D.IDENTITY
	_selected_xform = Transform3D.IDENTITY
	_selected_rotation_deg = Vector3.ZERO
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	_selection_box = null
	# Waypoint marker selection + overlay are tied to the container contents, so they reset
	# with it; the chosen path (_selected_path_index) persists across re-bakes by design.
	_selected_marker = {}
	_marker_pickable = []
	_waypoint_overlay = null
	# Zone selection + overlay are likewise container-tied. Unlike the waypoint path, the zone
	# selection does NOT survive a re-bake (a delete shifts indices), so it resets here too.
	_selected_zone_index = -1
	_zone_pickable = []
	_area_overlay = null


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
