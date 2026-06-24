extends RefCounted

# Editor-side controller for the Mission workspace (the Phase 2 adapter).
#
# Holds the open mission (NovaMissionData) plus its document state (path /
# loaded / dirty) and drives the load: parse the mission file, resolve its referenced
# terrain + environment from the shared resource root, load them through the
# terrain editor (read-only viewport), then run the host-agnostic
# MissionObjectPlacer under the terrain editor's world root. The resolve + place
# logic is the same piece the runtime uses (GameWorld.load_mission); this is the
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
const MissionMarkerOverlay := preload("res://engine/mission/mission_marker_overlay.gd")
const ObjectUserPointOverlayScript := preload("res://engine/object/object_user_point_overlay.gd")
const MissionGizmo := preload("res://modtools/framework/transform_gizmo_3d.gd")
const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
# Must match MissionObjectPlacer.CONTAINER_NAME — that is where placed objects land.
const OBJECTS_CONTAINER := "MissionObjects"

# Deviation tolerance passed to BOTH the re-ground dry-run count and the apply, so
# the "terrain changed under N objects" prompt and the move share one policy.
const REGROUND_EPSILON := 0.01

# AI-change action family + the PLAYPARTANIM sub-type, for resolving a scripting action's target to a
# live model for in-editor preview. Mirrors the runtime present pass. [orig: Entity_ApplyCommand case 0x22]
const _ACT_CHANGE_GROUP_AI := 3
const _ACT_AREA_AI_RED := 12
const _ACT_AREA_AI_BLUE := 13
const _ACT_CHANGE_SINGLE_AI := 21
const _AI_SUB_PLAYPARTANIM := 34

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
# The terrain height revision captured when the mission loaded (or when drift was
# last acknowledged / re-grounded), so reconcile_with_terrain can detect height
# edits made under the loaded mission. -1 = no revision available (stub editors
# without get_height_revision).
var _loaded_height_revision: int = -1
# Surface height under each entity's ground point when the mission was last in a
# known-grounded state (load / new / applied re-ground), keyed by the quantized BMS
# ground point. The drift scan compares the CURRENT surface against this memo and
# only re-grounds entities whose ground actually moved — an entity authored off the
# surface on purpose (waypoint markers carry flight altitude for flying AIs, see
# docs/world/world-wac-ai-re.md §7.4; objects can be raised via the inspector) is
# never counted or touched while the terrain under it is unchanged. Keyed by ground
# POSITION, not entity index, so adds/removes/drags cannot mis-attribute a row: a
# new or moved entity simply has no row and rides through to the engine's own
# deviation check. NOT cleared on decline (acknowledge_terrain_drift), so the
# manual re-ground keeps seeing the drift after the prompt was waved away.
var _ground_baseline: Dictionary = {}
# Cached _build_reground_requests() rows plus the token they were built under, so
# one drift cycle (activate-time count -> prompt apply -> baseline re-record)
# walks + samples the world ONCE instead of three times. Validity is a token
# compare, not mutation hooks: any object edit moves object_records_revision
# (positions are record bytes), any height edit moves the terrain height
# revision, any resource rescan moves NovaResourceRoot.cache_epoch (the anchors),
# and a mission swap changes the instance id. An empty token never matches, so
# nothing is cached while a build precondition is missing. The rows are
# UNFILTERED — the _ground_baseline filter stays per-call, so the
# decline-then-manual semantics above are untouched.
var _reground_requests_cache: Array = []
var _reground_cache_token: Array = []
var _last_open_dir: String = ""
var _stats: Dictionary = {}
var _last_status: String = ""
# Memoised is_dirty() result (-1 = needs recompute, 0 = clean, 1 = dirty). _mission.is_dirty() is a
# full-document bms::equal compare and the shell polls is_dirty() every frame (Save enablement + title
# `*`), so the result is cached and invalidated on every `changed` emission (see _notify_changed).
var _dirty_cache: int = -1
# The placer that built the current world, retained so place-new can render one entity
# incrementally (reusing its model + batch caches) instead of rebuilding everything.
var _placer  # MissionObjectPlacer (preloaded, no class_name)

# --- Authoring (Phase 1) state ------------------------------------------------
# Pickable index harvested from the placer (edit_mode): one record per (entity,
# static batch) or per animated entity. See MissionObjectPlacer.pickable_records.
var _pickable: Array = []

# --- Live simulation ("Play the mission") -------------------------------------
# The shared MissionRuntime node (parented under the objects container so it self-ticks via _process)
# that promotes the loaded mission into a libs/world World + AI and presents entity state (transform +
# part anims + visibility) onto the placed nodes -- the SAME runtime + present pass the game runs. Null
# when not simulating. Mutually exclusive with editing: starting it disarms the active tool.
var _sim_driver: Node = null

# --- Exact picking via per-entity collision bodies ----------------------------
# Object picking shoots the cursor ray through the viewport world's physics space and
# reads the hit collider's "entity_ref" meta. The collision bodies are real
# StaticBody3D + CollisionShape3D nodes (convex hulls from the 3di collision volumes)
# created by the placer under the MissionObjects container, so they are freed with the
# container automatically -- no manual lifecycle here. The viewport SubViewport sets
# own_world_3d so its physics is stepped (a shared/un-stepped world makes intersect_ray
# silently miss). Markers/zones stay analytic-AABB gizmos.
const PICK_RAY_LENGTH := 100000.0
# The selected entity's pick body (for live drag) + its anchor (entity_xform * this =
# the body's container-local transform). Set in _select, moved in _apply_selected_xform.
var _selected_collider: Node3D
# The selected object's Godot model-local ground anchor (Vector3.ZERO for none / markers). Subtracted
# from a terrain-drop position so the model's ground point lands under the cursor -- the author-time
# bake the engine does (and the only place the Ground userpoint is applied; render is direct).
var _selected_ground_offset: Vector3 = Vector3.ZERO
# Debug: when on, draw the pick hulls in world (see _refresh_pick_debug).
var _pick_debug := false

# --- Transform gizmo (ImGuizmo-style translate + rotate) ----------------------
# An in-world gizmo on the selected object: world-aligned translate arrows (X/Y/Z) and three
# rotate rings (pitch / yaw / roll). It produces drag deltas; the controller applies them
# through the same _apply_selected_xform / _commit_selected_transform spine as the terrain
# drag + numeric edits, so undo + the inspector stay in sync. Objects only (markers keep
# their terrain-drag). The node lives under the MissionObjects container, so it frees with a
# re-bake; the ref is dropped in _reset_selection_state and lazily rebuilt in _refresh_gizmo.
var _gizmo  # TransformGizmo3D (framework), bms basis_builder injected
var _gizmo_enabled := true
# Active handle drag: { part, axis } while a gizmo handle is held, else empty. The selection's
# transform at grab time is snapshotted so every motion applies an absolute delta (no drift).
var _gizmo_drag: Dictionary = {}
var _gizmo_start_origin: Vector3 = Vector3.ZERO
var _gizmo_start_rot: Vector3 = Vector3.ZERO
# Last cursor position the gizmo hover-highlight ran for, to throttle the per-motion handle hit-test
# to pixel movement (mirrors the object hover's HOVER_PIXEL_EPSILON gate). Reset on (re)selection.
var _gizmo_hover_pos: Vector2 = Vector2(-1, -1)

# Hover preview: a distinct-color wire box bracketing the object under the cursor
# (objects mode, not dragging/armed), so the user sees what a click would select.
const HOVER_PIXEL_EPSILON := 3.0
var _hover_box: MeshInstance3D
var _hovered_ref: Dictionary = {}
var _hover_pos: Vector2 = Vector2(-1, -1)

# The selected entity as { kind, index }, or empty when nothing is selected.
var _selected_ref: Dictionary = {}
# The selected entity's static batch records (its MultiMesh slots), or its animated
# node, plus its tracked container-local transform and authored rotation (degrees).
var _selected_records: Array = []
var _selected_node: Node3D
var _selected_graphic := ""
# For an animated selection, the model's ground-anchor offset (Transform3D applied
# as node.transform = entity_xform * offset). Static entities bake the same offset
# into each MultiMesh instance via the pickable record, so it only needs tracking
# for the animated-node path. IDENTITY when nothing animated is selected.
var _selected_node_offset: Transform3D = Transform3D.IDENTITY
var _selected_xform: Transform3D = Transform3D.IDENTITY
var _selected_rotation_deg: Vector3 = Vector3.ZERO
var _selected_user_point_overlay: ObjectUserPointOverlay
var _selected_user_points_visible := false
# In-editor PLAYPARTANIM preview: the model node currently being previewed (or null), plus a registry
# (cached, rebuilt when the entity set changes via _membership_rev) to resolve a scripting action's
# target SSN/group/zone to its live model -- the same MissionEntityRegistry the runtime host uses.
var _preview_node: Node3D
var _preview_registry
var _preview_registry_rev: int = -1
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
# The in-world overlay drawing a gizmo for EVERY marker (any type), so markers are visible +
# selectable in Objects mode where they are placed/edited as general entities. Built under the
# objects container (frees with it); ref dropped on every re-bake, lazily rebuilt. Visible only in
# Objects mode (the waypoint overlay shows path markers in Waypoints mode), so the two never
# double-draw. Mirrors _waypoint_overlay.
var _marker_overlay  # MissionMarkerOverlay (preloaded, no class_name)
# Marker placement ("add marker" tool): while armed, a terrain click adds a marker to the
# active path instead of selecting (mirrors object placement arming, but mode-scoped).
var _marker_place_armed: bool = false
# The items.def id of the engine's canonical "waypoint" marker: BMS type_id 6005 + kItemIdOffset
# (100000). A waypoint-path member must be a waypoint-TYPE marker so the engine treats it as a
# waypoint node ([orig: Entity_SpawnFromBMSRecord @0x40f060 switches on type_id == 6005]); a
# marker's role is entirely its type_id. Real data confirms it: items.def id 106005 = "waypoint",
# while 106001 = "start, player", 106178+ = "snd:" emitters, 100396+ = vehicle-spawn markers, etc. --
# distinct marker types that must NOT be turned into waypoints. Seeding a new path marker by copying
# whatever marker happened to be placed first (a player start) was the "waypoints become player
# starts" bug. The id policy now lives in the engine's authoring facade
# (libs/mission authoring.h: marker_item_id_for_path / kWaypointMarkerItemId),
# exposed as NovaMissionData.marker_item_id_for_path / add_path_marker_grounded.
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
# Bumped whenever the entity SET or its group membership changes (object/marker add/remove via
# _rebake_objects, and group edits). The inspector caches the group / waypoint-path / entity pickers
# and rebuilds them only when this changes, instead of re-marshalling every entity (~1600 Dictionaries)
# on each `changed` -- which fires on every edit commit / drag release, not just structural changes.
var _membership_rev: int = 0
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
	# save / new), so undoing back to the saved state clears the `*`. Memoised: _mission.is_dirty()
	# is a full-document compare and the shell polls this every frame, so cache it and recompute only
	# when `changed` fires (every mutation / save / load / undo / redo re-emits it via _notify_changed).
	if _dirty_cache < 0:
		_dirty_cache = 1 if (_mission != null and _mission.is_dirty()) else 0
	return _dirty_cache == 1


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


# The items.def graphic basename for the current selection, or "" when nothing is
# selected / no item database is loaded / the item has no declared graphic.
func get_selected_graphic_name() -> String:
	if _selected_ref.is_empty():
		return ""
	var entity := _find_entity(int(_selected_ref["kind"]), int(_selected_ref["index"]))
	if entity.is_empty():
		return ""
	var db := _item_db()
	if db == null:
		return ""
	var item_id := int(entity.get("item_id", 0))
	if not db.has_item(item_id):
		return ""
	return db.get_graphic(item_id).strip_edges()


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


func selected_has_user_points() -> bool:
	var data := _selected_object_data()
	return data != null \
		and data.has_method("get_user_point_count") \
		and data.get_user_point_count() > 0


func is_selected_user_points_visible() -> bool:
	return _selected_user_points_visible and selected_has_user_points()


func set_selected_user_points_visible(value: bool) -> void:
	_selected_user_points_visible = value and selected_has_user_points() and not is_simulating()
	_refresh_selected_user_points_overlay()
	_notify_changed()


func _selected_object_data() -> NovaObjectData:
	if _selected_ref.is_empty() or _selected_graphic.is_empty() or _placer == null:
		return null
	if int(_selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		return null
	if not _placer.has_method("object_data_for"):
		return null
	return _placer.object_data_for(_selected_graphic)


# Select an object from the inspector's "Placed objects" browser by kind + array index,
# then frame the editor camera on it so it is found in the viewport. This is the whole
# point of the list: on a large map a named unit can be located without hunting the world.
# Public (the viewport pick path uses the private _select); a missing entity is a no-op.
func select_object(kind: int, index: int) -> void:
	if _mission == null or _find_entity(kind, index).is_empty():
		return
	_select(kind, index)
	focus_selection_in_view()


# Orbit the editor camera onto the current selection's world AABB (falling back to its
# authored origin when the selection has no baked mesh). Keeps the current heading so the
# view does not spin. Returns false with no camera / nothing selected (e.g. headless tests).
func focus_selection_in_view() -> bool:
	if _selected_ref.is_empty() or terrain_editor == null or not terrain_editor.has_method("get_editor_camera"):
		return false
	var camera: Camera3D = terrain_editor.get_editor_camera()
	if camera == null or not camera.has_method("frame_bounds_custom"):
		return false
	var aabb := _selected_world_aabb()
	var center: Vector3
	var radius: float
	if aabb.size != Vector3.ZERO:
		center = aabb.position + aabb.size * 0.5
		radius = maxf(aabb.size.length() * 0.5, 8.0)
	else:
		# Mesh-less / not-yet-baked: frame the authored origin, converted to world space.
		var container := _objects_container()
		center = (container.global_transform * _selected_xform.origin) if container != null else _selected_xform.origin
		radius = 16.0
	camera.frame_bounds_custom(center, radius, 2.5, 1200.0, camera.rotation.y, -0.55)
	return true


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

# True when `trn_path` already IS the mounted terrain (case-insensitive,
# slash-normalized — resolve_file and a user's own open can disagree on form)
# and that terrain has no unsaved edits. Dirty never matches, so the reload
# there preserves today's semantics; the dirty read is duck-typed because the
# headless test stub carries no is_dirty.
func _is_same_clean_terrain(trn_path: String) -> bool:
	if not terrain_editor.has_method("get_current_trn_path"):
		return false
	if bool(terrain_editor.get("is_dirty")):
		return false
	var current := String(terrain_editor.get_current_trn_path())
	if current.is_empty():
		return false
	return current.replace("\\", "/").to_lower() == trn_path.replace("\\", "/").to_lower()

## Open a .bms/.mis: parse it, resolve + load its referenced terrain and environment
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

	# Wall-clock attribution per load stage; the abandoned timeline of a failed
	# open never reaches the ring (only finish() retains it).
	var timeline := PerfTimeline.begin("Mission load %s" % bms_path.get_file())

	timeline.span("parse")
	var mission := NovaMissionData.new()
	if mission.open_file(bms_path) != OK:
		_last_status = "Could not read %s: %s" % [bms_path.get_file(), mission.get_last_error()]
		return ERR_CANT_OPEN
	timeline.end_span()

	# The mission header selects the world: resolve its terrain (required) and
	# environment (optional) from the user's resource directory, case-insensitive.
	var terrain_ref := mission.get_terrain_ref()
	var trn_path := resource_root.resolve_file(terrain_ref + ".trn")
	if trn_path.is_empty():
		_last_status = "%s.trn (referenced by the mission) was not found in the resource directory." % terrain_ref
		return ERR_FILE_NOT_FOUND

	# Loading the referenced terrain is an atomic dependency of opening the mission,
	# not a separate user action, so it goes straight to open_trn rather than the
	# terrain editor's dirty-guarded request_open_trn. When the resolved .trn is
	# already the mounted terrain and it carries no unsaved edits, the remount is
	# skipped — the dominant browse-missions-on-one-map flow pays the terrain build
	# once. A dirty terrain always reloads (predictable authoring semantics).
	timeline.span("terrain")
	if _is_same_clean_terrain(trn_path):
		# Adopt the editor's own path form so reconcile_with_terrain's exact
		# compare cannot mistake a case/slash difference for a terrain swap.
		trn_path = String(terrain_editor.get_current_trn_path())
	else:
		var trn_err := int(terrain_editor.open_trn(trn_path, timeline))
		if trn_err != OK:
			# open_trn already replaced the editor's terrain with an empty one, so any
			# previously-loaded mission now describes a world that is gone. Drop it
			# rather than leaving stale objects / metadata over a blanked terrain.
			clear()
			_last_status = "Could not load %s.trn (error %d)." % [terrain_ref, trn_err]
			return trn_err as Error
	timeline.end_span()

	timeline.span("environment")
	var env_note := _load_environment(mission, resource_root)
	timeline.end_span()
	timeline.span("objects")
	_place_objects(mission, resource_root, timeline)
	timeline.end_span()

	_mission = mission
	_current_path = bms_path
	_loaded_trn_path = trn_path
	_record_ground_state()
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
	timeline.span("overlays")
	_refresh_active_overlay()
	timeline.end_span()
	timeline.finish()
	_last_status = "%s (%s)" % [_describe_load(mission, bms_path, env_note), timeline.brief(3)]
	_notify_changed()
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
	_record_ground_state()
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
	_refresh_active_overlay()
	if not env_note.is_empty():
		_last_status = "New mission on %s (%s)." % [terrain_ref, env_note]
	else:
		_last_status = "New mission on %s." % terrain_ref
	_notify_changed()
	return OK


func clear() -> void:
	sim_stop() # tear down a running simulation before the placed world it drives is cleared
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
	_loaded_height_revision = -1
	_ground_baseline = {}
	_reground_requests_cache = []
	_reground_cache_token = []
	_stats = {}
	_notify_changed()


# If the terrain underneath was swapped out from under a loaded mission (the user
# opened a different terrain in the Terrain workspace), the placed objects no
# longer belong to the mounted world. Drop the mission so its objects / metadata
# stop describing a world that is no longer there; re-opening shows it on its own
# terrain again. Called when the Mission workspace regains focus.
#
# When the SAME terrain is still mounted but its heights were edited since the
# mission loaded (or since drift was last resolved), returns how many entities'
# ground points no longer sit on the surface — the workspace prompts to re-ground
# on a non-zero count. Returns 0 on the swap/clear and no-drift paths.
func reconcile_with_terrain() -> int:
	if _mission == null or terrain_editor == null or not terrain_editor.has_method("get_current_trn_path"):
		return 0
	if String(terrain_editor.get_current_trn_path()) != _loaded_trn_path:
		clear()
		return 0
	# Same terrain file: detect height edits made under the loaded mission. The
	# revision gate keeps the common no-edit activate at zero cost (no request
	# build, no sampling).
	if _loaded_height_revision < 0 or not terrain_editor.has_method("get_height_revision"):
		return 0
	if int(terrain_editor.get_height_revision()) == _loaded_height_revision:
		return 0
	# Wall-clock attribution for the post-edit drift scan (the workspace-switch
	# hitch): created only past the revision gate, so the common no-edit activate
	# stays unmeasured and free. finish() retains it in the PerfTimeline ring for
	# the debug overlay's perf pane; the scan has no status line of its own.
	var timeline := PerfTimeline.begin("Mission drift scan")
	var count := _count_terrain_drift(timeline)
	if count == 0:
		# The height edits missed every object; settle on the new revision so
		# later activates skip the scan.
		_record_height_revision()
	timeline.finish()
	return count


func _record_height_revision() -> void:
	if terrain_editor != null and terrain_editor.has_method("get_height_revision"):
		_loaded_height_revision = int(terrain_editor.get_height_revision())
	else:
		_loaded_height_revision = -1


# The validity token for _reground_requests_cache (see the cache comment at the
# declaration). Empty when a build precondition is missing — never cached, every
# call rebuilds, exactly the pre-cache behavior.
func _reground_token() -> Array:
	if _mission == null or _placer == null or terrain_editor == null \
			or not terrain_editor.has_method("sample_height_world") \
			or not terrain_editor.has_method("get_height_revision"):
		return []
	return [
		_mission.get_instance_id(),
		_mission.object_records_revision(),
		int(terrain_editor.get_height_revision()),
		NovaResourceRoot.cache_epoch(),
	]


# The full (unfiltered) re-ground request set, built at most once per token.
func _reground_requests_cached() -> Array:
	var token := _reground_token()
	if token.is_empty():
		return _build_reground_requests()
	if token != _reground_cache_token:
		_reground_requests_cache = _build_reground_requests()
		_reground_cache_token = token
	return _reground_requests_cache


# Adopt the current terrain + entity layout as the known-grounded reference: the
# height revision plus the surface memo under every entity's ground point.
func _record_ground_state() -> void:
	_record_height_revision()
	_ground_baseline = {}
	for r in _reground_requests_cached():
		var request: Dictionary = r
		var hit: Vector3 = request["ground_hit_bms"]
		_ground_baseline[_ground_key(hit)] = hit.z


# Quantize a BMS ground point to centimetres for the baseline memo. The same
# unmoved entity reproduces the same key bit-for-bit (the builder is
# deterministic); colliding keys are harmless because the value only depends on
# the position being sampled.
static func _ground_key(hit_bms: Vector3) -> Vector2i:
	return Vector2i(roundi(hit_bms.x * 100.0), roundi(hit_bms.y * 100.0))


# The re-ground request set restricted to entities whose ground SURFACE moved
# since the baseline. A row whose sampled height still matches its memo is an
# entity sitting at an author-chosen offset over unchanged terrain — excluded, so
# a bulk re-ground can never flatten it. Rows without a memo (placed or dragged
# since the baseline) ride through to the engine's own deviation check, which
# skips them unless they are genuinely off the surface.
func _drifted_requests() -> Array:
	var requests: Array = []
	for r in _reground_requests_cached():
		var request: Dictionary = r
		var hit: Vector3 = request["ground_hit_bms"]
		var key := _ground_key(hit)
		if _ground_baseline.has(key) and absf(hit.z - float(_ground_baseline[key])) <= REGROUND_EPSILON:
			continue
		requests.append(request)
	return requests


# Dry-run count through the engine's re-ground policy: the prompt count and the
# apply share one request set and one epsilon (literally one token-keyed cached
# build, see _reground_requests_cached), so the count can never lie.
func _count_terrain_drift(timeline: PerfTimeline = null) -> int:
	PerfTimeline.span_on(timeline, "requests")
	var requests := _drifted_requests()
	PerfTimeline.end_on(timeline)
	if requests.is_empty():
		return 0
	PerfTimeline.span_on(timeline, "count")
	var count: int = _mission.reground_entities(requests, REGROUND_EPSILON, false)
	PerfTimeline.end_on(timeline)
	return count


## Snap every entity whose ground moved back onto the terrain surface as ONE undo
## step, then update the moved entities' placed world in place (a re-ground is a
## pure-z move of existing entities — no membership change — so re-baking every
## placed object would be waste; an unmappable record falls back to the full
## re-bake). Returns how many entities moved. Adopts the new ground state
## afterwards (the drift is resolved). Serves both the activate-time prompt and
## the inspector's manual button; entities authored off the surface on purpose
## are protected by the baseline filter (see _ground_baseline).
func reground_drifted() -> int:
	if _mission == null:
		return 0
	if _reject_edit_while_simulating():
		return 0
	var timeline := PerfTimeline.begin("Mission re-ground")
	timeline.span("requests")
	var requests := _drifted_requests()
	timeline.end_span()
	_flush_edit()
	_mission.begin_edit()
	timeline.span("apply")
	var result: Dictionary = _mission.reground_entities_apply(requests, REGROUND_EPSILON)
	var moved := int(result.get("moved", 0))
	timeline.end_span()
	_mission.commit_edit() # pushes one step only if something actually moved
	# The apply is pure-z (x/y preserved exactly by the conjugate bake, see
	# _build_reground_requests) and the surface did not change, so the cached rows
	# are byte-valid for the post-apply document; re-key the token so the baseline
	# re-record below reuses them instead of re-marshalling + resampling the world.
	_reground_cache_token = _reground_token()
	timeline.span("baseline")
	_record_ground_state()
	timeline.end_span()
	if moved > 0:
		timeline.span("update")
		if not _apply_reground_world_update(requests,
				result.get("rows", PackedInt32Array()),
				result.get("positions", PackedVector3Array())):
			_rebake_objects()
		timeline.end_span()
		mark_dirty()
		timeline.finish()
		_report("Re-grounded %d object%s (%s)." % [moved, "" if moved == 1 else "s", timeline.brief(3)])
	else:
		timeline.finish()
		_report("No objects needed re-grounding.")
	return moved


## Snap EVERY entity onto the current surface as one undo step — the bulk-repair
## seam (MCP reground_mission). Unlike reground_drifted there is NO baseline
## filter: rows whose terrain never moved are re-grounded too, which is exactly
## the mis-grounded-mission repair the drift path is designed to skip (its
## filter protects deliberate off-surface authoring; this seam plants those
## too, so callers must warn). Returns { checked, moved }.
func reground_all() -> Dictionary:
	if _mission == null or _reject_edit_while_simulating():
		return { "checked": 0, "moved": 0 }
	var requests := _reground_requests_cached()
	_flush_edit()
	_mission.begin_edit()
	var result: Dictionary = _mission.reground_entities_apply(requests, REGROUND_EPSILON)
	var moved := int(result.get("moved", 0))
	_mission.commit_edit() # pushes one step only if something actually moved
	# Same pure-z reasoning as reground_drifted: the cached rows stay byte-valid
	# for the post-apply document, so re-key the token before re-recording.
	_reground_cache_token = _reground_token()
	_record_ground_state()
	if moved > 0:
		if not _apply_reground_world_update(requests,
				result.get("rows", PackedInt32Array()),
				result.get("positions", PackedVector3Array())):
			_rebake_objects()
		mark_dirty()
	_report("Re-grounded %d of %d entities." % [moved, requests.size()])
	return { "checked": requests.size(), "moved": moved }


# Post-apply world sync for a bulk re-ground: rewrite only the moved entities'
# MultiMesh slots / animated nodes / pick bodies in place — the same absolute
# writes _apply_selected_xform does for the selection — instead of re-baking
# every placed object. A re-ground changes no membership, so _stats,
# _membership_rev, and the inspector's option caches all stay valid (none hold
# positions); the preview registry's entity_ref positions go stale exactly as
# they do after a drag and refresh on the next re-bake. A moved entity with
# zero pickable records is skipped, not a failure: the placer found it
# unresolved at place time, so nothing is rendered for it (this also keeps
# headless hosts on the targeted path). Returns false when a matched record's
# backing node was freed underneath us — the caller falls back to the full
# re-bake, which rebuilds everything from the document.
func _apply_reground_world_update(requests: Array, moved_rows: PackedInt32Array,
		new_positions_bms: PackedVector3Array) -> bool:
	# Container-local transform per moved entity, keyed "kind:index". Rotation is
	# unchanged by a re-ground; the row carried it so nothing is re-marshalled.
	var moved: Dictionary = {}
	var any_marker := false
	for n in moved_rows.size():
		var request: Dictionary = requests[moved_rows[n]]
		var kind := int(request.get("kind", -1))
		if kind == NovaMissionData.KIND_MARKER:
			any_marker = true
		moved["%d:%d" % [kind, int(request.get("index", -1))]] = MissionObjectPlacer.entity_transform(
				new_positions_bms[n], request.get("rotation_deg", Vector3.ZERO))
	for r in _pickable:
		var rec: Dictionary = r
		var key := "%d:%d" % [int(rec["kind"]), int(rec["index"])]
		if not moved.has(key):
			continue
		var xform: Transform3D = moved[key]
		if bool(rec.get("animated", false)):
			var node = rec.get("node")
			if node == null or not is_instance_valid(node):
				return false
			node.transform = xform * (rec.get("offset", Transform3D.IDENTITY) as Transform3D)
		else:
			var mmi = rec.get("mmi")
			if mmi == null or not is_instance_valid(mmi):
				return false
			var mm: MultiMesh = rec["mm"]
			mm.set_instance_transform(int(rec["slot"]), xform * (rec["offset"] as Transform3D))
	# Pick bodies sit at the entity transform directly (the drag path's lockstep
	# write); a missing body is normal (markers, shapeless or unresolved models).
	var container := _objects_container()
	if container != null:
		for n in moved_rows.size():
			var request: Dictionary = requests[moved_rows[n]]
			var kind := int(request.get("kind", -1))
			if kind == NovaMissionData.KIND_MARKER:
				continue
			var index := int(request.get("index", -1))
			var body := container.get_node_or_null(NodePath("Pick_%d_%d" % [kind, index])) as Node3D
			if body != null:
				body.transform = moved["%d:%d" % [kind, index]]
	# A selected entity re-syncs its box / gizmo / bound collider through the one
	# shared writer (rotation unchanged: pure-z). The selection SURVIVES a
	# targeted re-ground — only the re-bake fallback still drops it.
	if not _selected_ref.is_empty():
		var sel_key := "%d:%d" % [int(_selected_ref.get("kind", -1)), int(_selected_ref.get("index", -1))]
		if moved.has(sel_key):
			_apply_selected_xform(moved[sel_key])
	# Markers are mesh-less; their gizmos live in the active mode's overlay.
	if any_marker:
		_refresh_active_overlay()
	if _pick_debug:
		_refresh_pick_debug()
	return true


## Decline path for the activate-time prompt: adopt the current height revision so
## the prompt stays quiet until the NEXT height edit. The surface memo is kept, so
## the drift stays visible to the manual re-ground button (declining the question
## is not the same as calling the layout grounded). NOTE an undo of an APPLIED
## re-ground re-arms neither path today: the apply adopted the new surface as the
## baseline, so the restored pre-apply positions read as authored offsets over
## unchanged terrain until the next height edit. Re-arming after undo (tagging the
## re-ground's undo step and dropping its baseline rows on restore) is an open
## follow-up.
func acknowledge_terrain_drift() -> void:
	_record_height_revision()


# One re-ground request per entity, sampling the terrain under each entity's ROTATED
# ground anchor. The engine bakes position = hit - R*anchor (authoring.cpp
# bake_ground_transform, with R including the Rz(90) model-forward correction even at
# zero rotation), and bms_to_godot_basis is the exact conjugate of that R (parity
# pinned by mission_object_placer_test.gd) — so adding the rotated anchor back on
# first, mirroring the drop bake in _move_selected_to_world (basis * ground offset),
# makes the apply a pure z re-ground: x/y are preserved exactly and an entity
# already on the surface never counts as drifted. Unresolved items (no items.def
# row -> unknown anchor) and off-terrain samples (NAN) are skipped; markers ground
# their own origin (anchor ZERO, the engine stores the hit directly).
func _build_reground_requests() -> Array:
	var requests: Array = []
	if _mission == null or _placer == null:
		return requests
	if terrain_editor == null or not terrain_editor.has_method("sample_height_world"):
		return requests
	# Pass 1: walk the entities, resolving each one's rotated ground point (and
	# skipping unresolved graphics); the sample points land in a parallel packed
	# array so the surface query is ONE batched call, not ~6 boundary crossings
	# per entity.
	var rows: Array = []
	var points := PackedVector2Array()
	for e in _mission.get_all_entities():
		var entity: Dictionary = e
		var kind := int(entity.get("kind", -1))
		var ground_godot: Vector3 = MissionObjectPlacer.bms_to_godot_position(entity.get("position", Vector3.ZERO))
		var anchor_bms := Vector3.ZERO
		if kind != NovaMissionData.KIND_MARKER:
			var graphic: String = _placer.graphic_for(int(entity.get("item_id", 0)))
			if graphic.is_empty():
				continue
			var anchor_godot: Vector3 = _placer.ground_anchor_godot(graphic)
			anchor_bms = MissionObjectPlacer.godot_to_bms_position(anchor_godot)
			ground_godot += MissionObjectPlacer.bms_to_godot_basis(entity.get("rotation_deg", Vector3.ZERO)) * anchor_godot
		rows.append({
			"kind": kind,
			"index": int(entity.get("index", -1)),
			"ground_godot": ground_godot,
			"anchor_bms": anchor_bms,
			"rotation_deg": entity.get("rotation_deg", Vector3.ZERO),
		})
		points.append(Vector2(ground_godot.x, ground_godot.z))
	# Pass 2: sample — batched when the editor offers it, else the scalar loop so
	# any duck-typed host (a headless stub faking the surface) keeps working.
	var heights: PackedFloat32Array
	if terrain_editor.has_method("sample_heights_world"):
		heights = terrain_editor.sample_heights_world(points)
	else:
		heights = PackedFloat32Array()
		heights.resize(points.size())
		for i in points.size():
			heights[i] = terrain_editor.sample_height_world(points[i].x, points[i].y)
	# Pass 3: assemble, dropping off-terrain rows (NAN) exactly as the per-point
	# builder did.
	for i in rows.size():
		var height := heights[i]
		if is_nan(height):
			continue
		var row: Dictionary = rows[i]
		var ground_godot: Vector3 = row["ground_godot"]
		requests.append({
			"kind": row["kind"],
			"index": row["index"],
			"ground_hit_bms": MissionObjectPlacer.godot_to_bms_position(Vector3(ground_godot.x, height, ground_godot.z)),
			"ground_anchor_bms": row["anchor_bms"],
			# Not read by the engine (its parser ignores unknown keys); carried for
			# the targeted world update, which rebuilds the moved entities'
			# container-local transforms without re-marshalling them.
			"rotation_deg": row["rotation_deg"],
		})
	return requests


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
	_notify_changed()


# Invalidate the cached dirty state and notify listeners. Every site that previously called
# changed.emit() now routes through here, so is_dirty()'s memo is dropped in lockstep with the
# title `*` / Save enablement / inspector refresh that `changed` already drives (a mutation, save,
# load, clear, undo or redo). Uses emit_signal so the global changed.emit() -> _notify_changed()
# rewrite does not recurse into this helper.
func _notify_changed() -> void:
	_dirty_cache = -1
	emit_signal("changed")


# --- Save ---------------------------------------------------------------------
# Mirrors the editor save contract (see strings_editor.gd): save_current() writes
# back to the opened path and returns ERR_INVALID_PARAMETER when there is none (the
# shell then offers Save As); save_as() takes a directory and composes the filename.

func save_current() -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	var ext := _current_path.get_extension().to_lower()
	if _current_path.is_empty() or (ext != "bms" and ext != "mis"):
		return ERR_INVALID_PARAMETER
	var err := int(_mission.save_file())
	if err == OK:
		# The saved state is the new clean baseline; the undo history is kept so the user can
		# still undo across the save.
		_mission.mark_clean()
		_last_status = "Saved %s." % _current_path.get_file()
		_notify_changed()
	else:
		_last_status = "Could not save %s: %s" % [_current_path.get_file(), _mission.get_last_error()]
	return err as Error


func save_as(dir_path: String) -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	var filename := _current_path.get_file()
	if filename.is_empty():
		filename = "mission.bms"
	return save_as_file(dir_path.path_join(filename))


func save_as_file(path: String) -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if path.is_empty():
		return ERR_INVALID_PARAMETER
	var ext := path.get_extension().to_lower()
	if ext != "bms" and ext != "mis":
		_last_status = "Mission files must be saved as .bms or .mis."
		return ERR_INVALID_PARAMETER
	var dir_path := path.get_base_dir()
	if not dir_path.is_empty():
		var mkdir := DirAccess.make_dir_recursive_absolute(dir_path)
		if mkdir != OK:
			return mkdir
	var filename := path.get_file()
	var err := int(_mission.save_as(path))
	if err == OK:
		_current_path = path
		_last_open_dir = dir_path
		_mission.mark_clean()
		_last_status = "Saved %s." % filename
		_notify_changed()
	else:
		_last_status = "Could not save %s: %s" % [filename, _mission.get_last_error()]
	return err as Error


## Save to an explicit .bms file path (the MCP save seam). Mirrors save_as(),
## which takes a directory and composes the filename from the current path;
## this takes the full destination and adopts it as the current path.
func save_as_path(path: String) -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if path.is_empty() or path.get_extension().to_lower() != "bms":
		return ERR_INVALID_PARAMETER
	var mkdir := DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	if mkdir != OK:
		return mkdir
	var err := int(_mission.save_as(path))
	if err == OK:
		_current_path = path
		_last_open_dir = path.get_base_dir()
		_mission.mark_clean()
		_last_status = "Saved %s." % path.get_file()
		_notify_changed()
	else:
		_last_status = "Could not save %s: %s" % [path.get_file(), _mission.get_last_error()]
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


# One-shot mutation recipe shared by the simple setters. Flush any open edit session as its own step,
# open a fresh edit, run `do` (which performs exactly one NovaMissionData mutation and returns its
# result), and on success commit a single undo step, run `on_success` (e.g. an overlay refresh), and
# mark the document dirty. A bool result commits when true; a Dictionary / Array result (the chain
# mutators return the edited record / chain) commits when non-empty. On a rejected edit, surface
# `err` when it is non-empty. Returns whether the edit applied. Callers keep their own pre-guards (a
# valid selection, a fetched record) before calling -- this owns only the begin/commit/dirty bracket.
func _edit_step(do: Callable, err := "", on_success := Callable()) -> bool:
	if _mission == null:
		return false
	if _reject_edit_while_simulating():
		return false
	_flush_edit()
	_mission.begin_edit()
	var result: Variant = do.call()
	var ok := false
	if result is Dictionary:
		ok = not (result as Dictionary).is_empty()
	elif result is Array:
		ok = not (result as Array).is_empty()
	else:
		ok = bool(result)
	if ok:
		_mission.commit_edit()
		if on_success.is_valid():
			on_success.call()
		mark_dirty()
	elif not err.is_empty():
		_report(err, true)
	return ok


func _clear_history() -> void:
	if _mission != null:
		_mission.clear_history()


func undo() -> void:
	_restore_step(true)


func redo() -> void:
	_restore_step(false)


# Shared undo/redo spine (the two differ only in direction + the status line). A keyboard undo/redo
# can arrive mid-drag; cancel_drag abandons the visual gesture (so the re-bake does not free nodes a
# continuing drag still references) and commits any open edit session as its step before we rewind.
# _restoring guards re-entrancy: a restore -> rebake -> `changed` -> inspector roundtrip must not recurse.
func _restore_step(is_undo: bool) -> void:
	if _restoring:
		return
	if _reject_edit_while_simulating():
		return
	cancel_drag()
	if _mission == null:
		return
	if is_undo and not _mission.can_undo():
		_report("Nothing to undo.")
		return
	if not is_undo and not _mission.can_redo():
		_report("Nothing to redo.")
		return
	_restoring = true
	var before := _mission.structure_fingerprint()
	if is_undo:
		_mission.undo()
	else:
		_mission.redo()
	_after_restore(before)
	_restoring = false
	_report("Undid the last change." if is_undo else "Redid the last change.")


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
	var after := _mission.structure_fingerprint()
	if int(after["events"]) != int(before["events"]):
		_selected_event_index = -1
	# Same reasoning for the zone selection: area triggers are NOT part of object_rev (it covers only
	# placed objects), so an undo/redo of a zone add/delete takes the lightweight overlay-only path
	# below and would otherwise rebuild the overlay against a stale _selected_zone_index that now points
	# at a different (reindexed) zone. Add/delete are the only ops that change the zone count (no
	# reorder), so a count change is exactly the structural case; drop the selection then.
	if int(after["zones"]) != int(before["zones"]):
		_selected_zone_index = -1
	# Defensive clamp: never leave the index past the end of the restored zone list.
	if _selected_zone_index >= _mission.get_area_trigger_count():
		_selected_zone_index = -1
	# Skip the full object re-place when the undo/redo changed only non-object data (events / triggers /
	# actions / zones / header / loadout / groups): every placed object is byte-identical, so re-baking
	# ~all MultiMesh instances is pure waste. The placed nodes + pickable index + stats + object selection
	# all stay valid; only the active mode's overlay (which reads events / zones / paths from the document)
	# needs a refresh. Any object change moves object_rev -> full re-bake. object_rev is computed in C++
	# (NovaMissionData.object_records_revision) over the raw record bytes, so this no longer marshals the
	# placed-object set into ~1600 entity dictionaries twice per undo.
	if int(after["object_rev"]) == int(before["object_rev"]):
		_refresh_active_overlay()
	else:
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
	# Live simulation: the present pass writes the placed nodes' transforms every tick, so no
	# pick / drag / gizmo / place / hover gesture may start, and the keyboard mutators (undo /
	# redo / delete) are locked out too. Report on a discrete attempt (a click or a mutating
	# key), stay silent on hover/motion, and let everything else fall to the camera.
	if is_simulating():
		if event is InputEventMouseButton and (event as InputEventMouseButton).pressed:
			_reject_edit_while_simulating()
		elif event is InputEventKey:
			var sim_key := event as InputEventKey
			var is_mutator := (sim_key.ctrl_pressed and sim_key.keycode in [KEY_Z, KEY_Y]) \
				or sim_key.keycode in [KEY_DELETE, KEY_BACKSPACE]
			if sim_key.pressed and not sim_key.echo and is_mutator and not _gui_focus_blocks_shortcut():
				_reject_edit_while_simulating()
				_consume_viewport_key()
		return
	# Scripting mode is panel-driven and the viewport is inert in it (see Mode docs): swallow all
	# pointer events so a stray click cannot select, drag, or hover-pick an object, while still
	# letting the keyboard shortcuts (undo / redo / delete) below run.
	if is_scripting_mode() and not (event is InputEventKey):
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		# A non-left button pressed mid gizmo-drag (e.g. right-click to look / middle to orbit) ends
		# the drag and restores the grab-time pose. Otherwise FlyCamera (which also sees the event)
		# would move the camera under the gizmo's frozen drag plane, flinging the selection across the
		# map as the same cursor pixel reprojects. cancel_drag() rolls back to the snapshot.
		if mb.pressed and mb.button_index != MOUSE_BUTTON_LEFT and not _gizmo_drag.is_empty():
			cancel_drag()
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
			elif not _gizmo_drag.is_empty():
				_on_gizmo_release()
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
		elif not _gizmo_drag.is_empty():
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
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	# Close any open edit session. A drag is visual-only until release commits it, so a
	# cancelled drag leaves the document unchanged and this pushes nothing; an inspector edit
	# session that happens to be open keeps its undo step (commit, not discard, so a workspace
	# switch mid-edit does not silently drop the step).
	commit_edit()
	# A cancelled transform-gizmo drag previewed a move/rotate but wrote no record; restore the
	# selection to the snapshot taken at grab time, then drop the gizmo drag + highlight.
	if not _gizmo_drag.is_empty():
		_gizmo_drag = {}
		if not _selected_ref.is_empty():
			_selected_rotation_deg = _gizmo_start_rot
			_apply_selected_xform(Transform3D(MissionObjectPlacer.bms_to_godot_basis(_gizmo_start_rot), _gizmo_start_origin))
		if _gizmo != null and is_instance_valid(_gizmo):
			_gizmo.end_drag()
			_gizmo.set_highlight({})
	# A cancelled marker / zone drag previewed the gizmo but wrote no record; snap it back to
	# the stored position by rebuilding the active mode's overlay.
	_refresh_active_overlay()


func _on_left_press(mouse_pos: Vector2) -> void:
	# Close any open inspector edit session as its own step before starting a new gesture,
	# so SpinBox edits and a following drag never coalesce.
	_flush_edit()
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


# --- Transform gizmo ----------------------------------------------------------
# The in-world gizmo (framework TransformGizmo3D, bms basis_builder injected) draws translate
# arrows + rotate rings on the selected object and returns drag deltas; the controller applies
# them through the same _apply_selected_xform / _commit_selected_transform spine as the terrain
# drag + numeric edits, so undo + the inspector stay in lockstep. Objects only (markers keep
# their terrain-drag). The node is a child of the MissionObjects container so it frees with a
# re-bake.

func is_gizmo_enabled() -> bool:
	return _gizmo_enabled


func set_gizmo_enabled(value: bool) -> void:
	_gizmo_enabled = value
	_refresh_gizmo()


# The editor camera, or null (headless tests / no terrain editor bound).
func _editor_camera() -> Camera3D:
	if terrain_editor == null or not terrain_editor.has_method("get_editor_camera"):
		return null
	return terrain_editor.get_editor_camera()


# Try to start a gizmo handle drag at `mouse_pos`. Returns true (and arms the drag) when the cursor
# is over an arrow / ring of the visible gizmo; false otherwise so the caller falls through to
# picking + the terrain free-drag. Snapshots the selection transform so each motion applies an
# absolute delta from the grab (no drift), and opens the same begin_edit/commit_edit undo bracket.
func _begin_gizmo_drag(mouse_pos: Vector2) -> bool:
	if _gizmo == null or not is_instance_valid(_gizmo) or not _gizmo.visible or _selected_ref.is_empty():
		return false
	var camera := _editor_camera()
	if camera == null:
		return false
	var handle: Dictionary = _gizmo.pick_handle(camera, mouse_pos)
	if handle.is_empty():
		return false
	_gizmo_drag = handle
	_gizmo_start_origin = _selected_xform.origin
	_gizmo_start_rot = _selected_rotation_deg
	_gizmo.begin(handle, camera, mouse_pos)
	_gizmo.set_highlight(handle)
	_drag_active = true
	_drag_moved = false
	_drag_off_terrain = false
	begin_edit()
	return true


# Apply the gizmo's drag delta to the snapshotted start transform: translate slides the origin
# along a world axis; rotate spins one authored angle (pitch/yaw/roll). Previews via
# _apply_selected_xform (which moves the mesh, pick body, selection box, and the gizmo); the record
# commits once on release.
func _on_gizmo_drag(mouse_pos: Vector2) -> void:
	if _gizmo_drag.is_empty() or _selected_ref.is_empty() or _gizmo == null or not is_instance_valid(_gizmo):
		return
	var camera := _editor_camera()
	if camera == null:
		return
	var d: Dictionary = _gizmo.update(camera, mouse_pos)
	if d.has("translate"):
		var world_delta: Vector3 = d["translate"]
		var local_delta := world_delta
		var container := _objects_container()
		if container != null:
			local_delta = container.global_transform.basis.inverse() * world_delta
		_drag_moved = true
		_apply_selected_xform(Transform3D(_selected_xform.basis, _gizmo_start_origin + local_delta))
	elif d.has("rotate_deg"):
		var axis := int(_gizmo_drag.get("axis", 1))
		var nv := roundf(_gizmo_start_rot[axis] + float(d["rotate_deg"]))
		var r := _gizmo_start_rot
		if axis == 0:
			r.x = nv
		elif axis == 1:
			r.y = nv
		else:
			r.z = nv
		_selected_rotation_deg = r
		_drag_moved = true
		_apply_selected_xform(Transform3D(MissionObjectPlacer.bms_to_godot_basis(r), _selected_xform.origin))


func _on_gizmo_release() -> void:
	if _drag_moved:
		# _commit_selected_transform writes BOTH position and _selected_rotation_deg, so a rotate
		# gesture commits with no extra code.
		_commit_selected_transform()
	_gizmo_drag = {}
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	if _gizmo != null and is_instance_valid(_gizmo):
		_gizmo.end_drag()
		_gizmo.set_highlight({})
	# Push the gesture as one undo step (no-op when nothing moved), then re-orient the rings to the
	# committed rotation.
	commit_edit()
	_refresh_gizmo()


# Highlight the gizmo handle under the cursor on a bare hover (no drag), for grab feedback.
# Throttled to pixel movement so the (fixed-cost) handle hit-test does not re-run on sub-pixel jitter.
func _update_gizmo_hover(mouse_pos: Vector2) -> void:
	if _gizmo == null or not is_instance_valid(_gizmo) or not _gizmo.visible or not _gizmo_drag.is_empty():
		return
	if _gizmo_hover_pos.distance_to(mouse_pos) < HOVER_PIXEL_EPSILON:
		return
	_gizmo_hover_pos = mouse_pos
	var camera := _editor_camera()
	if camera == null:
		return
	_gizmo.set_highlight(_gizmo.pick_handle(camera, mouse_pos))


# (Re)build / place / hide the transform gizmo for the current selection. Shown only in Objects
# mode, gizmo enabled, with a non-marker object selected and no placement tool armed. Created lazily
# under the objects container (freed with it on a re-bake; the ref is dropped in
# _reset_selection_state). Re-orients the rings to the selection's current degrees.
func _refresh_gizmo() -> void:
	if _mission == null:
		return
	var container := _objects_container()
	if container == null:
		return
	var want := _gizmo_enabled and _mode == Mode.OBJECTS and not is_placement_armed() \
		and not is_simulating() \
		and not _selected_ref.is_empty() and int(_selected_ref.get("kind", -1)) != NovaMissionData.KIND_MARKER
	if not want:
		if _gizmo != null and is_instance_valid(_gizmo):
			_gizmo.visible = false
		return
	if _gizmo == null or not is_instance_valid(_gizmo):
		_gizmo = MissionGizmo.new()
		_gizmo.name = "MissionTransformGizmo"
		# Mission's authored angles are nested BMS euler, not plain euler: the rings must
		# derive their axes through the same basis the placer renders with.
		_gizmo.basis_builder = MissionObjectPlacer.bms_to_godot_basis
		container.add_child(_gizmo)
	_gizmo.visible = true
	_gizmo.show_for(_selected_xform.origin, _selected_rotation_deg)


# --- Pick bodies --------------------------------------------------------------
# The pick bodies (StaticBody3D + CollisionShape3D, "entity_ref" meta) are created by
# MissionObjectPlacer.add_pick_collider under the MissionObjects container, so they are
# built and freed with the visual world -- nothing to manage here. The selected entity's
# body is looked up by name ("Pick_<kind>_<index>") in _select for live drag.

func _selected_pick_collider() -> Node3D:
	if _selected_ref.is_empty():
		return null
	var container := _objects_container()
	if container == null:
		return null
	return container.get_node_or_null(NodePath("Pick_%d_%d" % [int(_selected_ref["kind"]), int(_selected_ref["index"])])) as Node3D


# World-space AABB of a placed entity (union over its pickable records / animated
# node). Used to bracket the hover highlight, mirroring _selected_world_aabb.
func _entity_world_aabb(kind: int, index: int) -> AABB:
	var result := AABB()
	var have := false
	for rec in _pickable:
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
	return _pick_debug


func set_pick_debug(value: bool) -> void:
	_pick_debug = value
	_refresh_pick_debug()


func _refresh_pick_debug() -> void:
	var container := _objects_container()
	if container == null:
		return
	var existing := container.get_node_or_null("MissionPickDebug")
	if existing != null:
		container.remove_child(existing)
		existing.queue_free()
	if not _pick_debug or _mission == null or _placer == null:
		return
	var root := Node3D.new()
	root.name = "MissionPickDebug"
	container.add_child(root)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(0.30, 1.0, 0.45, 0.9)
	mat.no_depth_test = true
	var seen: Dictionary = {}
	for rec in _pickable:
		var kind := int(rec["kind"])
		var index := int(rec["index"])
		var key := "%d:%d" % [kind, index]
		if seen.has(key):
			continue
		seen[key] = true
		var graphic := String(rec.get("graphic", ""))
		if graphic.is_empty():
			continue
		var shapes: Array = _placer.collision_shapes_for(graphic)
		if shapes.is_empty():
			continue
		# Container-local transform of the body (= world / container.global_transform).
		var entity := _find_entity(kind, index)
		var local: Transform3D = MissionObjectPlacer.entity_transform(
			entity.get("position", Vector3.ZERO), entity.get("rotation_deg", Vector3.ZERO))
		for shape in shapes:
			var mi := MeshInstance3D.new()
			mi.mesh = (shape as Shape3D).get_debug_mesh()
			mi.material_override = mat
			mi.transform = local
			mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			root.add_child(mi)


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
	# Objects: exact ray-vs-convex-hull via the viewport world's stepped physics space
	# (BVH broadphase, nearest hit). The hit StaticBody3D carries its (kind,index) in its
	# "entity_ref" meta.
	var container := _objects_container()
	if container != null and container.is_inside_tree():
		var world := container.get_world_3d()
		if world != null:
			var ss := world.direct_space_state
			if ss != null:
				var q := PhysicsRayQueryParameters3D.create(from, from + dir * PICK_RAY_LENGTH)
				var hit := ss.intersect_ray(q)
				if not hit.is_empty():
					var collider = hit.get("collider")
					if collider != null and (collider as Object).has_meta("entity_ref"):
						var ref: Dictionary = (collider as Object).get_meta("entity_ref")
						best = { "kind": int(ref["kind"]), "index": int(ref["index"]) }
						best_t = from.distance_to(hit["position"])
	# Markers are mesh-less, so they are not bodies; their gizmo AABBs come from the marker
	# overlay. The container sits at the world origin, so the overlay's AABBs are world-space (same
	# assumption as _pick_marker). The nearest of {hull hit, marker gizmo} wins.
	if _marker_overlay != null and is_instance_valid(_marker_overlay):
		for rec in _marker_overlay.marker_pickables():
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
	_flush_edit()
	stop_preview()
	_clear_selected_user_points()
	_selected_ref = { "kind": kind, "index": index }
	_selected_records = []
	_selected_node = null
	_selected_graphic = ""
	_selected_node_offset = Transform3D.IDENTITY
	var graphic := ""
	for rec in _pickable:
		if int(rec["kind"]) == kind and int(rec["index"]) == index:
			if graphic.is_empty():
				graphic = String(rec.get("graphic", ""))
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
	# Bind the entity's pick body node + its anchor so a drag can move the body live
	# (keeps a mid-drag re-pick exact). Markers have no body, so this resolves to null.
	_selected_collider = _selected_pick_collider()
	_selected_ground_offset = Vector3.ZERO
	if _placer != null and not graphic.is_empty():
		_selected_graphic = graphic
		_selected_ground_offset = _placer.ground_anchor_godot(graphic)
	var entity := _find_entity(kind, index)
	_selected_rotation_deg = entity.get("rotation_deg", Vector3.ZERO)
	_selected_xform = MissionObjectPlacer.entity_transform(
		entity.get("position", Vector3.ZERO), _selected_rotation_deg)
	# A marker has no mesh records, so the selection box stays hidden; highlight its gizmo instead.
	if kind == NovaMissionData.KIND_MARKER and _marker_overlay != null and is_instance_valid(_marker_overlay):
		_marker_overlay.set_selected_marker(index)
	_update_selection_box()
	# Show the transform gizmo on this selection (hidden for markers / non-objects modes). Reset the
	# hover throttle so the first motion over the rebuilt gizmo re-highlights.
	_refresh_gizmo()
	_gizmo_hover_pos = Vector2(-1, -1)
	_notify_changed()


func _deselect() -> void:
	stop_preview()
	_clear_selected_user_points()
	if _selected_ref.is_empty():
		return
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_graphic = ""
	_selected_node_offset = Transform3D.IDENTITY
	_selected_collider = null
	_selected_ground_offset = Vector3.ZERO
	_hide_selection_box()
	if _gizmo != null and is_instance_valid(_gizmo):
		_gizmo.visible = false
	if _marker_overlay != null and is_instance_valid(_marker_overlay):
		_marker_overlay.set_selected_marker(-1)
	_notify_changed()


func _refresh_selected_user_points_overlay() -> void:
	if not _selected_user_points_visible or not selected_has_user_points():
		_free_selected_user_points_overlay()
		return
	var container := _objects_container()
	if container == null:
		_free_selected_user_points_overlay()
		return
	var data := _selected_object_data()
	if data == null:
		_free_selected_user_points_overlay()
		return
	if _selected_user_point_overlay == null or not is_instance_valid(_selected_user_point_overlay):
		_selected_user_point_overlay = ObjectUserPointOverlayScript.new()
		_selected_user_point_overlay.name = "MissionSelectedUserPoints"
		container.add_child(_selected_user_point_overlay)
	_selected_user_point_overlay.set_object_data(data)
	if _selected_node != null and is_instance_valid(_selected_node):
		_selected_user_point_overlay.set_source_model(_selected_node)
		_selected_user_point_overlay.set_entity_transform(Transform3D.IDENTITY)
	else:
		_selected_user_point_overlay.set_source_model(null)
		_selected_user_point_overlay.set_entity_transform(_selected_xform)
	_selected_user_point_overlay.refresh_points()
	_selected_user_point_overlay.set_points_visible(true)


func _clear_selected_user_points() -> void:
	_selected_user_points_visible = false
	_free_selected_user_points_overlay()


func _free_selected_user_points_overlay() -> void:
	if _selected_user_point_overlay != null and is_instance_valid(_selected_user_point_overlay):
		_selected_user_point_overlay.queue_free()
	_selected_user_point_overlay = null


# --- In-editor PLAYPARTANIM preview -------------------------------------------
# Play a scripting PLAYPARTANIM action's part animation on its target model in the editor viewport so an
# author can see the motion without launching the game. Reuses the runtime path: it resolves the action's
# target (SSN / group / zone) through the same MissionEntityRegistry the host uses, then drives
# NovaObjectModel.restart_part_anim (a clean-from-rest variant of the runtime play_part_anim). The placed
# model already _process-ticks in the viewport, so the sweep animates live.

# Registry over the placed (edit-mode) container, rebuilt only when the entity set changes.
func _get_preview_registry():
	if _preview_registry == null or _preview_registry_rev != _membership_rev:
		_preview_registry = MissionEntityRegistry.new()
		_preview_registry.build(_objects_container(), _mission)
		_preview_registry_rev = _membership_rev
	return _preview_registry


# Resolve a PLAYPARTANIM action to one live animatable model: its explicit target (param1) by
# action_type, else an animated current selection, else null.
func _resolve_part_anim_node(action: Dictionary) -> Node3D:
	var registry = _get_preview_registry()
	var target := int(action.get("param1", 0))
	var nodes: Array = []
	match int(action.get("action_type", -1)):
		_ACT_CHANGE_SINGLE_AI:
			var hit = registry.resolve_single(target)  # registry is untyped here; no := inference
			if hit != null:
				nodes = [hit]
		_ACT_CHANGE_GROUP_AI:
			nodes = registry.resolve_group(target)
		_ACT_AREA_AI_RED, _ACT_AREA_AI_BLUE:
			nodes = registry.resolve_zone(target)
	for n in nodes:
		if n != null and is_instance_valid(n) and n.has_method("play_part_anim"):
			return n
	# Fallback: an animated current selection (e.g. previewing while an object is selected).
	if _selected_node != null and is_instance_valid(_selected_node) and _selected_node.has_method("play_part_anim"):
		return _selected_node
	return null


## True when the given scripting action can be previewed (a target model resolves).
func can_preview_part_anim(action: Dictionary) -> bool:
	return not action.is_empty() and _resolve_part_anim_node(action) != null


## Play the action's part animation on its target model (clean restart from rest). Returns false when no
## target resolves. channel = param2, play_type = param3, time = param4 (16.16 seconds -> seconds).
func preview_part_anim(action: Dictionary) -> bool:
	var node := _resolve_part_anim_node(action)
	if node == null:
		return false
	stop_preview()
	_preview_node = node
	var channel := int(action.get("param2", 0))
	var play_type := int(action.get("param3", 0))
	var time_s := float(int(action.get("param4", 0))) / 65536.0
	if node.has_method("set_playing"):
		node.set_playing(true)
	if node.has_method("reset_animation_time"):
		node.reset_animation_time()
	if node.has_method("restart_part_anim"):
		node.restart_part_anim(channel, play_type, time_s)
	elif node.has_method("play_part_anim"):
		node.play_part_anim(channel, play_type, time_s)
	return true


## Stop any running preview and return the previewed model to rest.
func stop_preview() -> void:
	if _preview_node != null and is_instance_valid(_preview_node):
		if _preview_node.has_method("clear_part_anims"):
			_preview_node.clear_part_anims()
		if _preview_node.has_method("clear_ctrl_values"):
			_preview_node.clear_ctrl_values()
		if _preview_node.has_method("reset_animation_time"):
			_preview_node.reset_animation_time()
	_preview_node = null


# Move the selected entity so its origin sits at a world-space ground point: keep the
# current rotation, only the origin tracks the cursor.
func _move_selected_to_world(global_hit: Vector3) -> void:
	var container := _objects_container()
	if container == null:
		return
	var local := container.global_transform.affine_inverse() * global_hit
	# Bake the Ground userpoint: the dropped model's origin sits at the terrain hit minus its rotated
	# model-local ground anchor, so its ground point lands under the cursor (render is direct).
	# [orig: sub_401A90, dfx2med.exe]
	_apply_selected_xform(Transform3D(_selected_xform.basis, local - (_selected_xform.basis * _selected_ground_offset)))


# Write a new container-local transform onto the selection: rewrite every static
# MultiMesh instance (slot) of the entity, or the animated node, plus the selection
# box. Shared by the viewport drag and the inspector's numeric pos/rot edits so both
# move the in-world object identically.
func _apply_selected_xform(xform: Transform3D) -> void:
	_selected_xform = xform
	if not _selected_ref.is_empty() and int(_selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		_clear_selected_user_points()
		# A marker is mesh-less: preview its gizmo (container-local origin) via the overlay. No mesh
		# records / node to move, and the selection box stays hidden.
		if _marker_overlay != null and is_instance_valid(_marker_overlay):
			_marker_overlay.preview_marker_position(int(_selected_ref["index"]), _selected_xform.origin)
		return
	if _selected_node != null:
		# The anchor offset rides the node so the dragged model keeps its ground point
		# under the cursor, matching how it was first placed.
		_selected_node.transform = _selected_xform * _selected_node_offset
	else:
		for rec in _selected_records:
			var mm: MultiMesh = rec["mm"]
			mm.set_instance_transform(int(rec["slot"]), _selected_xform * (rec["offset"] as Transform3D))
	# Move the pick body node in lockstep so a re-pick mid/after-drag stays exact. The body sits at
	# the entity transform directly (render is direct; the ground anchor is baked into the stored
	# position, not applied here). No-op for a marker (no body).
	if _selected_collider != null and is_instance_valid(_selected_collider):
		_selected_collider.transform = _selected_xform
	_refresh_selected_user_points_overlay()
	_update_selection_box()
	# Keep the transform gizmo on the selection. During a gizmo drag, only reposition (keep the
	# captured drag plane + ring orientation frozen); otherwise re-orient the rings to the new
	# rotation (numeric edits, fresh selection).
	if _gizmo != null and is_instance_valid(_gizmo) and _gizmo.visible:
		if _gizmo_drag.is_empty():
			_gizmo.show_for(_selected_xform.origin, _selected_rotation_deg)
		else:
			_gizmo.set_origin(_selected_xform.origin)


func _commit_selected_transform() -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	if _reject_edit_while_simulating():
		return
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin)
	if _mission.set_entity_transform(int(_selected_ref["kind"]), int(_selected_ref["index"]), bms_pos, _selected_rotation_deg):
		# A marker's gizmo was preview-moved; rebuild the overlay so its pickable AABB tracks the
		# committed position (re-applies the selection highlight).
		if int(_selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
			_refresh_marker_overlay()
		mark_dirty()


## Programmatic grounded move (the MCP edit seam): drop the SELECTED entity so
## its ground point sits at a world-space terrain hit — the viewport drag's
## anchor bake (origin = hit − rotated ground anchor; hit stored directly for
## markers) — as one closed undo step. Returns false with no selection, no
## mission, or while simulating.
func move_selected_to_world_grounded(global_hit: Vector3) -> bool:
	if _selected_ref.is_empty() or _mission == null:
		return false
	if _reject_edit_while_simulating():
		return false
	_flush_edit()
	begin_edit()
	_move_selected_to_world(global_hit)
	_commit_selected_transform()
	commit_edit()
	return true


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
	var basis := MissionObjectPlacer.bms_to_godot_basis(_selected_rotation_deg)
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
	if _selected_ref.is_empty():
		return
	# set_entity_property_int returns false only on rejection (bad index, unknown property, failed
	# write) -- never on a benign same-value write -- so a false return is a real error worth surfacing.
	_edit_step(func(): return _mission.set_entity_property_int(
			int(_selected_ref["kind"]), int(_selected_ref["index"]), property, value),
		"Could not set %s on the selected object." % property)
	# A group change moves which groups are "in use" (and which "New group N" the picker offers), so the
	# cached group dropdown must rebuild. Entity-set changes are covered by _rebake_objects; other
	# per-entity fields (waypoint_id, team, AI) don't affect any cached option list, so don't bump here.
	if property == "group":
		_membership_rev += 1


# Revision of the entity set + group membership; see _membership_rev. The inspector gates its
# (otherwise per-`changed`, ~1600-entity) rebuild of the group / waypoint-path / entity pickers on this.
func get_membership_revision() -> int:
	return _membership_rev


# String counterpart of set_selected_property, for the fixed-string entity fields
# "name1" (AI class) and "name2" (AI script). Same snapshot / one-undo-step model.
func set_selected_string_property(property: String, value: String) -> void:
	if _selected_ref.is_empty():
		return
	_edit_step(func(): return _mission.set_entity_property_string(
			int(_selected_ref["kind"]), int(_selected_ref["index"]), property, value),
		"Could not set %s on the selected object." % property)


# --- Authoring: mission-header editing ----------------------------------------
# Each setter snapshots, writes one header field through NovaMissionData, then pushes a
# single undo step. Field names match NovaMissionData::set_header_* and the inspector form.
func set_header_string(field: String, value: String) -> void:
	_edit_step(func(): return _mission.set_header_string(field, value),
		"Could not set mission %s." % field)


func set_header_int(field: String, value: int) -> void:
	_edit_step(func(): return _mission.set_header_int(field, value),
		"Could not set mission %s." % field)


func set_header_flag(bit: int, on: bool) -> void:
	_edit_step(func(): return _mission.set_header_flag(bit, on),
		"Could not set mission flag.")


# Single-select game mode (one attrib_flags mode bit, or 0 = Single Player). Mirrors set_header_*:
# one undo step + dirty. NovaMissionData.set_game_mode clears the other mode bits.
func set_game_mode(bit: int) -> void:
	_edit_step(func(): return _mission.set_game_mode(bit),
		"Could not set the game mode.")


# --- Weapon loadout + groups (mission-global) ---------------------------------
# Loadout entries are dictionaries { index, name, value1, value2 }; groups are
# { index, field0(flags), field8(value), field12(constant 10) }. Edits use the one-step snapshot/undo recipe.

func get_weapon_loadout() -> Array:
	if _mission == null:
		return []
	return _mission.get_weapon_loadout()


func set_weapon_loadout(entries: Array) -> void:
	_edit_step(func(): return _mission.set_weapon_loadout(entries),
		"Could not update the weapon loadout.")


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
	_edit_step(func(): return _mission.set_group(index, field0, field8, field12),
		"Could not update group %d." % index)


# --- Authoring (Phase 3): place new objects -----------------------------------
# The inspector's palette arms an items.def item; a left-click on the terrain then
# places a new instance there and selects it, while staying armed so several can be
# placed. Markers (player start, insertion, waypoint, ...) are placed the same way --
# they are mesh-less general entities shown as gizmos by the marker overlay (the engine
# spawns markers through the same path as every other entity; a marker's role is its
# items.def item). Waypoint *paths* (sequencing waypoint markers) are a separate concern
# handled in Waypoints mode.

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
	if _mission == null:
		return
	var db := _item_db()
	if db == null or not db.has_item(item_id):
		return
	# Arming is a new action: close any open transform session as its own undo step first.
	_flush_edit()
	_place_item_id = item_id
	_deselect()
	_notify_changed()


func disarm_placement() -> void:
	if _place_item_id == 0:
		return
	_place_item_id = 0
	# Placement suppresses the gizmo (its want-gate excludes is_placement_armed). A just-placed
	# entity stays selected, so once disarmed re-show its gizmo (no-op when nothing is selected).
	_refresh_gizmo()
	_notify_changed()


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
	if _reject_edit_while_simulating():
		return false
	var container := _objects_container()
	if container == null:
		return false
	var db := _item_db()
	var item_type := db.get_item_type(item_id) if db != null else -1
	var kind := NovaMissionData.kind_for_item_type(item_type)
	# A readable label for the status line: the model name when resolvable, else the raw id.
	var item_name: String = db.get_display_name(item_id) if db != null and db.has_item(item_id) else ""
	if item_name.is_empty():
		item_name = "item %d" % item_id
	var local := container.global_transform.affine_inverse() * global_hit
	# The list selection + Ground-userpoint bake [orig: sub_401A90, dfx2med.exe] live in the
	# engine's authoring facade; the editor only converts the hit/anchor to mission space.
	var anchor_bms := Vector3.ZERO
	if _placer != null and kind != NovaMissionData.KIND_MARKER:
		anchor_bms = _placer.ground_anchor_bms(_placer.graphic_for(item_id))
	# Placing is its own undo step: close any open session, then bracket the add with
	# begin_edit/commit_edit (commit pushes one step iff the add changed the document).
	_flush_edit()
	_mission.begin_edit()
	var record := _mission.place_entity_grounded(item_id, item_type, MissionObjectPlacer.godot_to_bms_position(local), anchor_bms)
	if record.is_empty():
		# Balance the begin_edit() bracket on the reject path (no-op step, the failed add changed
		# nothing) so the open session does not leak into the next gesture.
		_mission.commit_edit()
		_report("Could not place %s." % item_name, true)
		return false
	_mission.commit_edit()
	var new_index := int(record.get("index", -1))
	# The entity set grew: invalidate the inspector's cached group / waypoint-path / entity pickers.
	# (Placement renders incrementally rather than through _rebake_objects, which is the other bump site.)
	_membership_rev += 1
	# Markers are mesh-less: the placer skips them, so render via the marker overlay (rebuild so the
	# new gizmo + pickable exist before we select it). Mesh entities render incrementally.
	if kind == NovaMissionData.KIND_MARKER:
		_refresh_marker_overlay()
	else:
		_render_placed_entity(kind, new_index)
	mark_dirty()
	_select(kind, new_index)
	_report("Placed %s. Ctrl+Z to undo." % item_name)
	return true



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
	# place_single already added this entity's pick collider node under the container.
	var delta: Dictionary = _placer.place_single(_mission, container, kind, index, _environment_node())
	_pickable = _placer.pickable_records
	for key in delta:
		_stats[key] = int(_stats.get(key, 0)) + int(delta[key])
	_refresh_pick_debug()


# --- Authoring (Phase 4): delete + structural re-bake -------------------------
# Deleting an entity is structural: the lib erases it from its kind's list, so every
# later entity of that kind shifts down one index. The pickable index and MultiMesh
# slot mapping were built from the old indices, so rather than patch them in place we
# re-bake the whole MissionObjects container from the post-delete record — correct by
# construction, and cheap because the retained placer keeps its model + batch caches.

# Remove the currently-selected entity, then re-bake the world so it matches the new
# record. A selected marker (mesh-less) skips the object re-bake (its delete shifts no object
# MultiMesh indices) and just rebuilds the marker overlay. Returns false (a no-op) when nothing is
# selected or the lib rejects the removal; clears the selection on success. Public so the
# inspector's Delete button and the viewport Delete key share one path.
func delete_selected() -> bool:
	if _selected_ref.is_empty() or _mission == null:
		return false
	if _reject_edit_while_simulating():
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
		# Balance the begin_edit() bracket on the reject path (no-op step, the failed remove changed
		# nothing) so the open session does not leak into the next gesture.
		_mission.commit_edit()
		_report("Could not delete the selected object.", true)
		return false
	_mission.commit_edit()
	if kind == NovaMissionData.KIND_MARKER:
		# Objects are untouched by a marker delete; drop the selection and rebuild only the marker
		# overlay against the post-delete list (cheaper than re-placing every object). The marker is
		# still part of the entity set the inspector's cached pickers marshal, so bump the membership
		# revision (the non-marker branch gets this from _rebake_objects).
		_membership_rev += 1
		_deselect()
		_refresh_marker_overlay()
	else:
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
	# A re-bake is the universal choke point for entity-set changes (add / remove / place / delete /
	# marker edits, and undo/redo whose object signature differs), so bump the membership revision here
	# to invalidate the inspector's cached group / waypoint-path / entity pickers. (A bulk re-ground is
	# NOT a membership change — it moves existing entities in place and skips both the re-bake and this
	# bump; see _apply_reground_world_update.)
	_membership_rev += 1
	var options: Dictionary = {}
	var env_node := _environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	_stats = _placer.place(_mission, world_root, options)
	_pickable = _placer.pickable_records
	# The placer (re)created the pick colliders with the world; just refresh the debug overlay.
	_refresh_pick_debug()
	# The re-bake replaced the container (and the old overlay with it); rebuild the active
	# mode's overlay against the new world.
	_refresh_active_overlay()


# Rebuild only the overlay for the current mode (each mode owns exactly one). Shared by the
# re-bake and the lightweight undo path so the mode -> overlay dispatch lives in one place.
func _refresh_active_overlay() -> void:
	if _mode == Mode.WAYPOINTS:
		_refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_refresh_area_trigger_overlay()
	elif _mode == Mode.OBJECTS:
		_refresh_marker_overlay()


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
	# A mode switch is a fresh context: stop any running part-animation preview.
	stop_preview()
	_clear_selected_user_points()
	# Exclusive selection: clear the object selection refs + its box, the marker selection,
	# the zone selection, and any armed placement tool.
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_graphic = ""
	_selected_node_offset = Transform3D.IDENTITY
	_selected_collider = null
	_selected_ground_offset = Vector3.ZERO
	_hide_selection_box()
	# Hover only lives in objects mode; drop it on any mode switch.
	_clear_hover()
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
	_refresh_marker_overlay()
	# Each overlay is visible only in its own mode (markers are placed/edited in Objects mode; the
	# waypoint overlay shows path markers in Waypoints mode), so the two marker-gizmo overlays never
	# double-draw.
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.visible = mode == Mode.WAYPOINTS
	if _area_overlay != null and is_instance_valid(_area_overlay):
		_area_overlay.visible = mode == Mode.AREA_TRIGGERS
	if _marker_overlay != null and is_instance_valid(_marker_overlay):
		_marker_overlay.visible = mode == Mode.OBJECTS
	# The transform gizmo lives only in Objects mode and only with a selection; a mode switch
	# clears the selection above, so just hide it here (rebuilt on the next object select).
	if _gizmo != null and is_instance_valid(_gizmo):
		_gizmo.visible = false
	_notify_changed()


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
	_notify_changed()


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
	_edit_step(func(): return _mission.set_waypoint_path(_selected_path_index, indices, flags),
		"", _refresh_waypoint_overlay)


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
	# Reserve record index 0: the engine reads waypoint_id (byte 79) with 0 == "follow no path"
	# [orig: Entity_SpawnFromBMSRecord @0x40f02f, `if (record[79])`], so a route authored into record 0
	# can never be a follow target and get_waypoint_path_options omits it. Author new routes from
	# index 1 so they show up in the Behavior "Waypoint path" picker.
	for s in _mission.get_waypoint_summaries():
		var idx := int((s as Dictionary)["index"])
		if idx >= 1 and int((s as Dictionary)["marker_count"]) == 0:
			return idx
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


# (Re)build the always-on marker overlay (a gizmo per marker, labelled with its items.def display
# name) and re-apply the gizmo highlight for a selected marker. Creates the node under the objects
# container on first use (and after a re-bake freed it); visible only in Objects mode. Markers are
# placed + edited as general entities there, so this is the Objects-mode counterpart of the
# waypoint overlay.
func _refresh_marker_overlay() -> void:
	if _mission == null:
		return
	var container := _objects_container()
	if container == null:
		return
	if _marker_overlay == null or not is_instance_valid(_marker_overlay):
		_marker_overlay = MissionMarkerOverlay.new()
		_marker_overlay.name = "MissionMarkerOverlay"
		_marker_overlay.visible = _mode == Mode.OBJECTS
		container.add_child(_marker_overlay)
	_marker_overlay.rebuild(_mission, _marker_labels())
	# Re-apply the highlight for a selected marker (the object-selection path holds it in _selected_ref).
	if not _selected_ref.is_empty() and int(_selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		_marker_overlay.set_selected_marker(int(_selected_ref["index"]))


# The display name for every marker (aligned to KIND_MARKER index), so the overlay can label each
# gizmo and the user can tell a player start from a waypoint node. Falls back to "" (no label) when
# the name can't be resolved.
func _marker_labels() -> Array:
	var labels: Array = []
	if _mission == null:
		return labels
	var count := _mission.get_entity_count(NovaMissionData.KIND_MARKER)
	for i in count:
		labels.append(entity_display_name(NovaMissionData.KIND_MARKER, i))
	return labels


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
	_notify_changed()


func _deselect_marker() -> void:
	if _selected_marker.is_empty():
		return
	_selected_marker = {}
	if _waypoint_overlay != null and is_instance_valid(_waypoint_overlay):
		_waypoint_overlay.set_selected_marker(-1)
	_notify_changed()


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
	_notify_changed()


func disarm_marker_placement() -> void:
	if not _marker_place_armed:
		return
	_marker_place_armed = false
	_notify_changed()


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
	var result := _mission.add_path_marker_grounded(_selected_path_index, bms_pos)
	if result.is_empty():
		_report("Could not add a waypoint marker.", true)
		return false
	_mission.commit_edit()
	# A new marker entity grew the entity set: invalidate the cached pickers (added incrementally via
	# the overlay rather than _rebake_objects).
	_membership_rev += 1
	_refresh_waypoint_overlay()
	var marker_index := int((result.get("marker", {}) as Dictionary).get("index", -1))
	if marker_index >= 0:
		_select_marker(_selected_path_index, marker_index)
	mark_dirty()
	return true



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
	_edit_step(func(): return _mission.set_waypoint_path(_selected_path_index, indices, int(path.get("flags", 0))),
		"", _refresh_waypoint_overlay)


# Delete the selected marker entirely: remove_entity drops the KIND_MARKER entity and
# repairs every path that referenced it (drops the index, decrements higher ones). Markers
# reindex, so re-bake from the post-delete record. One undo step. Returns false if nothing
# is selected or the lib rejects it; clears the marker selection on success.
func delete_selected_marker() -> bool:
	if _mission == null or _selected_marker.is_empty():
		return false
	if _reject_edit_while_simulating():
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
	var id_counts: Dictionary = {}
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		for e in _mission.get_entities(kind):
			var ed := e as Dictionary
			var bms_id := int(ed.get("bms_id", 0))
			var display := entity_display_name(kind, int(ed.get("index", 0)))
			var label := ("%s #%d" % [display, bms_id]) if display != "" else ("Unit #%d" % bms_id)
			out.append({ "value": bms_id, "label": label })
			id_counts[bms_id] = int(id_counts.get(bms_id, 0)) + 1
	# bms_id is not guaranteed unique on disk (many records default to 0), and the picker keys its
	# OptionButton items by value, so rows that share an id would be visually indistinguishable. Append a
	# 1-based ordinal to each member of a colliding id so the user can tell them apart. The committed
	# value stays the bms_id -- the engine resolves units by that id (FindByNetId), so same-id rows are
	# genuinely equivalent on disk; this only disambiguates the display.
	var seen: Dictionary = {}
	for row in out:
		var v := int(row["value"])
		if int(id_counts.get(v, 0)) > 1:
			var n := int(seen.get(v, 0)) + 1
			seen[v] = n
			row["label"] = "%s  (%d)" % [String(row["label"]), n]
	return out


# Human kind label for a "Placed objects" browser row. The list covers every entity kind the
# Objects mode renders + picks, markers included (they are placed / edited as general entities
# there, gizmo-picked via the always-on marker overlay; Waypoints mode is just a second view of them).
func _object_kind_label(kind: int) -> String:
	match kind:
		NovaMissionData.KIND_BUILDING:
			return "Building"
		NovaMissionData.KIND_ORGANIC:
			return "Person"
		NovaMissionData.KIND_MARKER:
			return "Marker"
		_:
			return "Item"


# Flat, ordered list of every placed object (items / buildings / people / markers) for the
# inspector's left-pane browser. One row per entity: { kind, index, item_id, name, category }.
# `name` is the resolved model name (or "" -> the inspector falls back to the item id); the row
# order is the on-disk array order, stable across edits, so duplicate-name ordinals stay put.
func get_object_list() -> Array:
	var out: Array = []
	if _mission == null:
		return out
	for kind in [NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC, NovaMissionData.KIND_MARKER]:
		var category := _object_kind_label(kind)
		for e in _mission.get_entities(kind):
			var ed := e as Dictionary
			var index := int(ed.get("index", 0))
			out.append({
				"kind": kind,
				"index": index,
				"item_id": int(ed.get("item_id", 0)),
				"name": entity_display_name(kind, index),
				"category": category,
			})
	return out


# Number of placed objects across every Objects-mode kind (markers included). Cheap (count fields,
# no record walk); the inspector gates its (potentially 1000+ row) list rebuild on this changing.
func get_object_count() -> int:
	if _mission == null:
		return 0
	return _mission.get_entity_count(NovaMissionData.KIND_ITEM) \
		+ _mission.get_entity_count(NovaMissionData.KIND_BUILDING) \
		+ _mission.get_entity_count(NovaMissionData.KIND_ORGANIC) \
		+ _mission.get_entity_count(NovaMissionData.KIND_MARKER)


# Whether item names are resolvable yet. A mission can open before its items.def is reachable
# (the resource directory is repointed afterwards); the inspector rebuilds its row labels once
# this flips true so the browser does not stay stuck on "Item <id>" placeholders.
func has_item_database() -> bool:
	return _item_db() != null


# Options for the inspector's "Waypoint path" picker -- which path a unit follows (the waypoint_id /
# byte-79 field, [orig: Entity_SpawnFromBMSRecord @0x40f02f `if (record[79]) follow path record[79]`]).
# Shaped { id, label } for ObjectUiHelpers.populate_id_option. id 0 = "None": byte 79 == 0 means the
# unit follows no path, so path index 0 is unreachable as a follow target and is not offered. The
# inspector adds the unit's current value if it is not in this set, so an odd value still round-trips.
func get_waypoint_path_options() -> Array:
	var out: Array = [{ "id": 0, "label": "None" }]
	if _mission == null:
		return out
	for s in _mission.get_waypoint_summaries():
		var d := s as Dictionary
		var idx := int(d.get("index", 0))
		var count := int(d.get("marker_count", 0))
		if idx <= 0 or count <= 0:
			continue
		out.append({ "id": idx, "label": "Path %d  -  %d markers" % [idx, count] })
	return out


# Options for the inspector's "Group" picker -- which squad a unit belongs to (the group_id / byte-78
# field, [orig: Entity_SpawnFromBMSRecord @0x40ebb7]; the format carries 64 groups, 0..63). Shaped
# { id, label }: "Ungrouped" (0), every group already in use (annotated with its unit count so the
# user joins an existing squad), and a "New group N" entry for the first free group so a fresh squad
# can be started. The inspector adds the unit's current group if it is not already listed.
func get_group_options() -> Array:
	var out: Array = [{ "id": 0, "label": "Ungrouped" }]
	if _mission == null:
		return out
	var counts: Dictionary = {}
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		for e in _mission.get_entities(kind):
			var g := int((e as Dictionary).get("group", 0))
			if g > 0:
				counts[g] = int(counts.get(g, 0)) + 1
	var used: Array = counts.keys()
	used.sort()
	for g in used:
		out.append({ "id": int(g), "label": "Group %d  (%d units)" % [int(g), int(counts[g])] })
	for candidate in range(1, 64):
		if not counts.has(candidate):
			out.append({ "id": candidate, "label": "New group %d" % candidate })
			break
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
	_notify_changed()


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
	_edit_step(func(): return _mission.set_area_trigger(_selected_zone_index, mn, mx,
			bool(zone.get("active", false)), bool(zone.get("constrain_z", false)), int(zone.get("id", 0))),
		"", _refresh_area_trigger_overlay)


# Set the selected zone's two known flag bits (active / constrain-Z). One undo step.
func set_selected_zone_flags(active: bool, constrain_z: bool) -> void:
	if _mission == null or _selected_zone_index < 0:
		return
	var zone := _mission.get_area_trigger(_selected_zone_index)
	if zone.is_empty():
		return
	_edit_step(func(): return _mission.set_area_trigger(_selected_zone_index, zone.get("min", Vector3.ZERO),
			zone.get("max", Vector3.ZERO), active, constrain_z, int(zone.get("id", 0))),
		"", _refresh_area_trigger_overlay)


# Delete the selected zone. Structural (shifts later indices), so the overlay rebuilds and the
# selection drops. *IsWithinArea trigger param2 references are auto-repaired in the lib (Phase-5 RE
# confirmed param2 is an array index): higher refs shift down, a direct hit becomes -1 (dangling, which
# the scripting diagnostics then flag). One undo step. False if none selected.
func delete_selected_area_trigger() -> bool:
	if _mission == null or _selected_zone_index < 0:
		return false
	if _reject_edit_while_simulating():
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
		_notify_changed()
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
	_notify_changed()


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


func get_ai_flag_bits() -> Array:
	return _mission.get_ai_flag_bits() if _mission != null else []


# Focus an event by index (the inspector list drives this). Inert if unchanged.
func select_event(index: int) -> void:
	if index == _selected_event_index:
		return
	_selected_event_index = index
	_notify_changed()


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
	if _reject_edit_while_simulating():
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
	_edit_step(func(): return _mission.set_event(_selected_event_index, flags, reset_after, delay))


# Append a trigger to the selected event (defaults to a Group / Null condition). One undo step.
func add_selected_event_trigger() -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.add_event_trigger(_selected_event_index, {}),
		"Could not add a trigger (an event chains at most 20).")


# Overwrite the trigger at `local_index` (its position in the event's chain) from an editor dict. One step.
func set_selected_event_trigger(local_index: int, trigger: Dictionary) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.set_event_trigger(_selected_event_index, local_index, trigger))


func remove_selected_event_trigger(local_index: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.remove_event_trigger(_selected_event_index, local_index))


func move_selected_event_trigger(local_index: int, delta: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.move_event_trigger(_selected_event_index, local_index, delta))


# Append an action to the selected event (defaults to a Null action). One undo step.
func add_selected_event_action() -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.add_event_action(_selected_event_index, {}),
		"Could not add an action (an event chains at most 20).")


func set_selected_event_action(local_index: int, action: Dictionary) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.set_event_action(_selected_event_index, local_index, action))


func remove_selected_event_action(local_index: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.remove_event_action(_selected_event_index, local_index))


func move_selected_event_action(local_index: int, delta: int) -> void:
	if _mission == null or get_selected_event_index() < 0:
		return
	_edit_step(func(): return _mission.move_event_action(_selected_event_index, local_index, delta))


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


# --- Hover preview ------------------------------------------------------------
# Re-pick on bare mouse motion (objects mode only) and bracket the object under the
# cursor with an amber wire box, distinct from the cyan selection box. Throttled to
# pixel movement; never mutates selection or opens an edit session.
func _on_hover(mouse_pos: Vector2) -> void:
	if _mode != Mode.OBJECTS or _drag_active or is_placement_armed():
		_clear_hover()
		return
	if _hover_pos.distance_to(mouse_pos) < HOVER_PIXEL_EPSILON:
		return
	_hover_pos = mouse_pos
	var ref := _pick_entity(mouse_pos)
	var kind := int(ref.get("kind", -1))
	var index := int(ref.get("index", -1))
	# Skip empties, markers (their own gizmo highlights), and the current selection.
	if ref.is_empty() or kind == NovaMissionData.KIND_MARKER \
			or (not _selected_ref.is_empty() \
				and int(_selected_ref.get("kind", -2)) == kind \
				and int(_selected_ref.get("index", -2)) == index):
		_clear_hover()
		return
	_hovered_ref = { "kind": kind, "index": index }
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
	if _hover_box != null and is_instance_valid(_hover_box):
		return _hover_box
	var container := _objects_container()
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
	_hover_box = mi
	return mi


func _clear_hover() -> void:
	_hovered_ref = {}
	_hover_pos = Vector2(-1, -1)
	if _hover_box != null and is_instance_valid(_hover_box):
		_hover_box.visible = false


# Clear selection refs without touching the scene. The selection box is a child of the
# objects container, so it is freed when the container is (re)built; here we only drop
# the dangling ref.
func _reset_selection_state() -> void:
	stop_preview()
	_clear_selected_user_points()
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_graphic = ""
	_selected_node_offset = Transform3D.IDENTITY
	_selected_xform = Transform3D.IDENTITY
	_selected_rotation_deg = Vector3.ZERO
	# Drop the selection's pick-body ref (the body node is freed/rebuilt with the
	# container, not here).
	_selected_collider = null
	_selected_ground_offset = Vector3.ZERO
	_drag_active = false
	_drag_moved = false
	_drag_off_terrain = false
	# The transform gizmo is a container child too, so a re-bake freed it; drop the dangling ref
	# (and any in-flight gizmo drag) so the next _refresh_gizmo rebuilds it.
	_gizmo = null
	_gizmo_drag = {}
	_gizmo_hover_pos = Vector2(-1, -1)
	_selection_box = null
	# The hover box is a container child too, so the re-bake freed it; drop the dangling ref.
	_hover_box = null
	_hovered_ref = {}
	_hover_pos = Vector2(-1, -1)
	# Waypoint marker selection + overlay are tied to the container contents, so they reset
	# with it; the chosen path (_selected_path_index) persists across re-bakes by design.
	_selected_marker = {}
	_marker_pickable = []
	_waypoint_overlay = null
	# The marker overlay is a container child too, so the re-bake freed it; drop the dangling ref so
	# the next _refresh_marker_overlay rebuilds it rather than orphaning a freed node.
	_marker_overlay = null
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
			# Game parity: the runtime layers the mission's attrib-gated fog/water
			# overrides on top of the .env (get_environment_overrides builds exactly
			# the apply_mission_overrides payload). Apply them to the preview too,
			# then re-fan-out — open_env already emitted with the bare .env values.
			var env_file: Variant = env_editor.get("env_file")
			var overrides: Dictionary = mission.get_environment_overrides()
			if env_file != null and not overrides.is_empty() and env_file.has_method("apply_mission_overrides"):
				env_file.apply_mission_overrides(overrides)
				if env_editor.has_method("_emit_all_changed"):
					env_editor._emit_all_changed()
			return ""  # loaded the mission's own environment; nothing to reset or note
		else:
			note = "environment %s could not be loaded" % env_ref

	# Blank, unresolved, or unreadable reference: reset to a neutral default so the
	# atmosphere matches the inspector instead of lingering from a prior mission.
	if env_editor.has_method("create_default_environment"):
		env_editor.create_default_environment(false)
	return note


## Re-apply the open mission's environment (ref + fog/water overrides) to the
## editor preview — the seam the MCP set_mission_header tool calls after the
## `environment` header changes, since open/new are otherwise the only times
## the preview tracks the mission. Returns the load note ("" on success).
func reload_environment() -> String:
	if _mission == null:
		return "no mission open"
	var resource_root: NovaResourceRoot = terrain_editor.get_resource_root() \
			if terrain_editor != null and terrain_editor.has_method("get_resource_root") else null
	if resource_root == null:
		return "no resource root mounted"
	return _load_environment(_mission, resource_root)


func _place_objects(mission: NovaMissionData, resource_root: NovaResourceRoot, timeline: PerfTimeline = null) -> void:
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
	if timeline != null:
		options["timeline"] = timeline
	_stats = _placer.place(mission, world_root, options)
	_pickable = _placer.pickable_records
	# The placer created the pick colliders with the world; refresh the debug overlay if on.
	_refresh_pick_debug()


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


# --- Live simulation ("Play the mission") -------------------------------------
# Promote the loaded mission into a libs/world World + AI through the shared MissionRuntime (the same
# driver + present pass the game runs), at the same DIVIDED cadence the game runs, so the preview IS
# the game's pacing. Read-only over the mission data: Stop rewinds the world and restores the
# authored node transforms. While simulating, editing is locked out (see
# _reject_edit_while_simulating): the present pass owns the placed nodes' transforms every tick, so
# letting a gizmo drag or inspector write race it would leave two writers fighting over one node.

func can_simulate() -> bool:
	return is_loaded() and _objects_container() != null

func is_simulating() -> bool:
	return _sim_driver != null and is_instance_valid(_sim_driver)

# One gate for every mutating entry point (viewport gestures, inspector setters, undo/redo,
# delete/place): while the sim runs, reject the edit with a status line instead of racing the
# present pass. Returns true when the caller must bail.
func _reject_edit_while_simulating() -> bool:
	if not is_simulating():
		return false
	_report("Stop the simulation to edit.", true)
	return true

func is_sim_playing() -> bool:
	return is_simulating() and _sim_driver.is_playing()


# The live MissionRuntime while simulating (null otherwise) — the seam the
# debug overlay's runtime source resolves through, same shape as
# NovaWorld.get_runtime() on the game side.
func get_sim_runtime() -> Node:
	return _sim_driver if is_simulating() else null

func _ensure_sim_driver() -> bool:
	if is_simulating():
		return true
	if not is_loaded():
		return false
	var container := _objects_container()
	if container == null:
		_report("Load a mission on a terrain before simulating.", true)
		return false
	# Entering sim mode ends any half-finished edit gesture / armed tool, and hides the
	# hover box + transform gizmo (the present pass owns the nodes now).
	cancel_drag()
	disarm_placement()
	_clear_hover()
	_clear_selected_user_points()
	_sim_driver = MissionRuntime.new()
	_sim_driver.name = "MissionRuntime"
	container.add_child(_sim_driver)
	# TICK_DIVIDED + self_tick + the sim's default loco_scale: the exact options the game's
	# GameWorld path runs, so the preview IS the game's pacing. The old EVERY_PROCESS +
	# loco_scale 4096 combo (32768/8, a slowed compensation for uncapped editor fps) was an
	# editor-only divergence. The driver builds its present index over `container`. Pass the
	# editor's loaded terrain so the preview grounds AI exactly like the game runtime, and the
	# resource root so soldiers get their .adm/.bad root-motion clips (without them they stand
	# still).
	var terrain_data = terrain_editor.get_data() if terrain_editor != null and terrain_editor.has_method("get_data") else null
	var sim_root = terrain_editor.get_resource_root() if terrain_editor != null and terrain_editor.has_method("get_resource_root") else null
	if int(_sim_driver.setup(_mission, container, {
			"tick_mode": NovaSimulation.TICK_DIVIDED,
			"self_tick": true,
			"playable": false,
			"terrain": terrain_data,
			"resource_root": sim_root,
			"item_db": _item_db(),
		})) <= 0:
		sim_stop()
		_report("No AI entities to simulate in this mission.", false)
		return false
	_refresh_gizmo()
	return true

func sim_play() -> void:
	if not _ensure_sim_driver():
		return
	_sim_driver.play()
	_report("Simulating mission (%d AI)." % int(_sim_driver.entity_count()), false)
	changed.emit()

func sim_pause() -> void:
	if is_simulating():
		_sim_driver.pause()
		changed.emit()

func sim_step() -> void:
	if not _ensure_sim_driver():
		return
	_sim_driver.step_once()
	changed.emit()

func sim_stop() -> void:
	if not is_simulating():
		return
	_sim_driver.stop()
	_sim_driver.queue_free()
	_sim_driver = null
	# Editing is unlocked again: bring the transform gizmo back for the surviving selection.
	_refresh_gizmo()
	changed.emit()


## The debug overlay (C12) drives the live sim driver directly — it is
## host-neutral and bypasses the sim_* methods above. The workspace relays its
## play/pause/step presses here so `changed` still fires and the sim bar
## re-reads the driver state. (Overlay Stop relays to sim_stop() instead: it
## must also free the driver to unlock editing.)
func notify_sim_transport_changed() -> void:
	if is_simulating():
		changed.emit()


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
