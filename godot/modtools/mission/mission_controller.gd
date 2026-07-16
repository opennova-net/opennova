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
# Parsed co-named <mission>.til. This is the editor host's copy of GameWorld's
# mission-scoped terrain override: the same resource drives tile composition
# and foliage exclusion while the Mission workspace is active.
var _mission_tile_info: NovaTerrainTileInfo
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


# --- F5 decomposition: controller sections (modtools/mission/controller/) ------
# Method bundles in the #178 inspector-split shape: ALL state stays on this
# controller (sections reach it through `_c`); public API stays here as
# delegates, so callers and the workspace contract never moved.
const ControllerIo := preload("res://modtools/mission/controller/io_ops.gd")
const ControllerReground := preload("res://modtools/mission/controller/reground_ops.gd")
const ControllerViewport := preload("res://modtools/mission/controller/viewport_ops.gd")
const ControllerPlacement := preload("res://modtools/mission/controller/placement_ops.gd")
const ControllerWaypoints := preload("res://modtools/mission/controller/waypoint_ops.gd")
const ControllerZones := preload("res://modtools/mission/controller/zone_ops.gd")
const ControllerSim := preload("res://modtools/mission/controller/sim_ops.gd")
var _io  # ControllerIo (created in _init)
var _reground  # ControllerReground (created in _init)
var _viewport  # ControllerViewport (created in _init)
var _placement  # ControllerPlacement (created in _init)
var _waypoints  # ControllerWaypoints (created in _init)
var _zones  # ControllerZones (created in _init)
var _sim  # ControllerSim (created in _init)


func _init(p_terrain_editor: Node = null) -> void:
	terrain_editor = p_terrain_editor
	_io = ControllerIo.new(self)
	_reground = ControllerReground.new(self)
	_viewport = ControllerViewport.new(self)
	_placement = ControllerPlacement.new(self)
	_waypoints = ControllerWaypoints.new(self)
	_zones = ControllerZones.new(self)
	_sim = ControllerSim.new(self)


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value


# --- State accessors ----------------------------------------------------------

func get_mission() -> NovaMissionData:
	return _mission


## The co-named mission tile array, or null when this mission has no .til.
## MissionWorkspace passes this public fact into TerrainEditor's preview
## context; callers never need to reach into controller load state.
func get_mission_tile_info() -> NovaTerrainTileInfo:
	return _mission_tile_info


## Return the loaded BMS clock as the shared HHMM preview value. NAN means no
## Mission document owns the clock; the Environment author's time then renders.
func get_mission_preview_time_of_day() -> float:
	if _mission == null:
		return NAN
	var info: Dictionary = _mission.get_info()
	return NovaEnvironment.mission_start_time_hhmm(
		int(info.get("start_time", 0)))


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
	var entity = _viewport._find_entity(kind, index)
	if entity.is_empty():
		return ""
	var db = _placement._item_db()
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
	var entity = _viewport._find_entity(int(_selected_ref["kind"]), int(_selected_ref["index"]))
	if entity.is_empty():
		return ""
	var db = _placement._item_db()
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
	_selected_user_points_visible = value and selected_has_user_points() and not _sim.is_simulating()
	_viewport._refresh_selected_user_points_overlay()
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
	if _mission == null or _viewport._find_entity(kind, index).is_empty():
		return
	_viewport._select(kind, index)
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
	var aabb = _viewport._selected_world_aabb()
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


## The mounted resource root, via the bound terrain editor — the controller's one
## VFS seam. The controller runs headless in tests (no shell), so this reads the
## editor, not the workspace; the duck-type guard for bare doubles lives here.
func _resource_root() -> NovaResourceRoot:
	if terrain_editor != null and terrain_editor.has_method("get_resource_root"):
		return terrain_editor.get_resource_root()
	return null


# Retail loads <mission>.til into one shared terrain tile array used by both
# terrain composition and foliage's radius-2 blocker. Keep the basename and
# VFS lookup identical to GameWorld._load_mission_tile_info.
# [orig: Terrain_LoadFoliageFile @ 0x60a740;
# Foliage_PathBlockedByPlacedTile @ 0x606490]
func _load_mission_tile_info(bms_name: String, resource_root: NovaResourceRoot) -> void:
	_clear_mission_tile_info()
	if resource_root == null:
		return
	var mission_name := bms_name.get_file()
	if mission_name.is_empty():
		mission_name = bms_name
	var til_name := mission_name.get_basename() + ".til"
	if not resource_root.has_file(til_name):
		return
	var til_bytes := resource_root.read_file(til_name)
	if til_bytes.is_empty():
		return
	var tile_info := NovaTerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		push_warning("MissionController: failed to parse mission tile file '%s'." % til_name)
		return
	_mission_tile_info = tile_info


func _clear_mission_tile_info() -> void:
	_mission_tile_info = null


func open_mission(bms_path: String) -> Error:
	return _io.open_mission(bms_path)


func new_mission() -> Error:
	return _io.new_mission()


func clear() -> void:
	_io.clear()


func reconcile_with_terrain() -> int:
	return _reground.reconcile_with_terrain()


# Quantize a BMS ground point to centimetres for the baseline memo. The same
# unmoved entity reproduces the same key bit-for-bit (the builder is
# deterministic); colliding keys are harmless because the value only depends on
# the position being sampled.
static func _ground_key(hit_bms: Vector3) -> Vector2i:
	return Vector2i(roundi(hit_bms.x * 100.0), roundi(hit_bms.y * 100.0))


func reground_drifted() -> int:
	return _reground.reground_drifted()


func reground_all() -> Dictionary:
	return _reground.reground_all()


func acknowledge_terrain_drift() -> void:
	_reground.acknowledge_terrain_drift()


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
	return _io.save_current()


func save_as(dir_path: String) -> Error:
	return _io.save_as(dir_path)


func save_as_file(path: String) -> Error:
	return _io.save_as_file(path)


func save_as_path(path: String) -> Error:
	return _io.save_as_path(path)


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
	if _sim._reject_edit_while_simulating():
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
	if _sim._reject_edit_while_simulating():
		return
	_viewport.cancel_drag()
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
	_viewport.handle_viewport_input(event)


func cancel_drag() -> void:
	_viewport.cancel_drag()


func is_gizmo_enabled() -> bool:
	return _viewport.is_gizmo_enabled()


func set_gizmo_enabled(value: bool) -> void:
	_viewport.set_gizmo_enabled(value)


func is_pick_debug() -> bool:
	return _viewport.is_pick_debug()


func set_pick_debug(value: bool) -> void:
	_viewport.set_pick_debug(value)


func can_preview_part_anim(action: Dictionary) -> bool:
	return _sim.can_preview_part_anim(action)


func preview_part_anim(action: Dictionary) -> bool:
	return _sim.preview_part_anim(action)


func stop_preview() -> void:
	_sim.stop_preview()


func move_selected_to_world_grounded(global_hit: Vector3) -> bool:
	return _viewport.move_selected_to_world_grounded(global_hit)


# The full editable dictionary for the selected entity (see NovaMissionData entity
# fields: position is mission-space, rotation_deg is authored degrees, plus team /
# group), or {} when nothing is selected.
func get_selected_entity() -> Dictionary:
	if _selected_ref.is_empty():
		return {}
	return _viewport._find_entity(int(_selected_ref["kind"]), int(_selected_ref["index"]))


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
	_viewport._apply_selected_xform(Transform3D(_selected_xform.basis, MissionObjectPlacer.bms_to_godot_position(bms_pos)))
	_viewport._commit_selected_transform()


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
	_viewport._apply_selected_xform(Transform3D(basis, _selected_xform.origin))
	_viewport._commit_selected_transform()


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

func get_placeable_items() -> Array:
	return _placement.get_placeable_items()


func arm_placement(item_id: int) -> void:
	_placement.arm_placement(item_id)


func disarm_placement() -> void:
	_placement.disarm_placement()


func is_placement_armed() -> bool:
	return _placement.is_placement_armed()


func get_placement_item_id() -> int:
	return _placement.get_placement_item_id()


func place_entity_at_world(item_id: int, global_hit: Vector3) -> bool:
	return _placement.place_entity_at_world(item_id, global_hit)


# Remove the currently-selected entity, then re-bake the world so it matches the new
# record. A selected marker (mesh-less) skips the object re-bake (its delete shifts no object
# MultiMesh indices) and just rebuilds the marker overlay. Returns false (a no-op) when nothing is
# selected or the lib rejects the removal; clears the selection on success. Public so the
# inspector's Delete button and the viewport Delete key share one path.
func delete_selected() -> bool:
	if _selected_ref.is_empty() or _mission == null:
		return false
	if _sim._reject_edit_while_simulating():
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
		_viewport._deselect()
		_waypoints._refresh_marker_overlay()
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
	_viewport._reset_selection_state()
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
	_viewport._refresh_pick_debug()
	# The re-bake replaced the container (and the old overlay with it); rebuild the active
	# mode's overlay against the new world.
	_refresh_active_overlay()


# Rebuild only the overlay for the current mode (each mode owns exactly one). Shared by the
# re-bake and the lightweight undo path so the mode -> overlay dispatch lives in one place.
func _refresh_active_overlay() -> void:
	if _mode == Mode.WAYPOINTS:
		_waypoints._refresh_waypoint_overlay()
	elif _mode == Mode.AREA_TRIGGERS:
		_zones._refresh_area_trigger_overlay()
	elif _mode == Mode.OBJECTS:
		_waypoints._refresh_marker_overlay()


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
	_sim.stop_preview()
	_viewport._clear_selected_user_points()
	# Exclusive selection: clear the object selection refs + its box, the marker selection,
	# the zone selection, and any armed placement tool.
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_graphic = ""
	_selected_node_offset = Transform3D.IDENTITY
	_selected_collider = null
	_selected_ground_offset = Vector3.ZERO
	_viewport._hide_selection_box()
	# Hover only lives in objects mode; drop it on any mode switch.
	_viewport._clear_hover()
	_selected_marker = {}
	_selected_zone_index = -1
	_place_item_id = 0
	_marker_place_armed = false
	if mode == Mode.WAYPOINTS and _selected_path_index < 0:
		# Focus a populated path on entry so the panel is not empty.
		_selected_path_index = _waypoints._first_nonempty_path()
	if mode == Mode.AREA_TRIGGERS and _mission != null and _mission.get_area_trigger_count() > 0:
		# Focus the first zone on entry so the panel is not empty.
		_selected_zone_index = 0
	if mode == Mode.SCRIPTING and _mission != null and get_selected_event_index() < 0 and _mission.get_event_count() > 0:
		# Focus the first event on entry so the scripting panel is not empty (get_selected_event_index
		# reads a stale-but-out-of-range selection as -1, so a shrunken list re-focuses event 0).
		_selected_event_index = 0
	_waypoints._refresh_waypoint_overlay()
	_zones._refresh_area_trigger_overlay()
	_waypoints._refresh_marker_overlay()
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


func select_waypoint_path(index: int) -> void:
	_waypoints.select_waypoint_path(index)


func get_selected_waypoint_path_index() -> int:
	return _waypoints.get_selected_waypoint_path_index()


func select_new_waypoint_path() -> int:
	return _waypoints.select_new_waypoint_path()


func set_waypoint_flags(loop: bool, blue: bool, red: bool) -> void:
	_waypoints.set_waypoint_flags(loop, blue, red)


func get_waypoint_summaries() -> Array:
	return _waypoints.get_waypoint_summaries()


func get_active_waypoint_path() -> Dictionary:
	return _waypoints.get_active_waypoint_path()


func get_selected_marker() -> Dictionary:
	return _waypoints.get_selected_marker()


func select_waypoint_marker(marker_index: int) -> void:
	_waypoints.select_waypoint_marker(marker_index)


# --- Add marker (placement tool) ---------------------------------------------

# Arm the "add marker" tool: a terrain click then adds a marker to the active path. Needs
# an active path; drops any marker selection so the inspector shows the placement state.
# --- Add marker (placement tool) ---------------------------------------------

func arm_marker_placement() -> void:
	_waypoints.arm_marker_placement()


func disarm_marker_placement() -> void:
	_waypoints.disarm_marker_placement()


func is_marker_placement_armed() -> bool:
	return _waypoints.is_marker_placement_armed()


func add_marker_to_active_path_at_world(global_hit: Vector3) -> bool:
	return _waypoints.add_marker_to_active_path_at_world(global_hit)


# --- Reorder / delete / clear -------------------------------------------------

# Move the selected marker one step earlier (-1) or later (+1) along the active path. This
# rewrites only the path's reference order (no marker entity changes), so it rebuilds just
# the overlay. One undo step. Inert at the ends or without a marker selection.
# --- Reorder / delete / clear -------------------------------------------------

func move_selected_marker(delta: int) -> void:
	_waypoints.move_selected_marker(delta)


func delete_selected_marker() -> bool:
	return _waypoints.delete_selected_marker()


func clear_active_path() -> bool:
	return _waypoints.clear_active_path()


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
	return _placement._item_db() != null


# Options for the inspector's "Waypoint path" picker -- which path a unit follows (the waypoint_id /
# byte-79 field, [orig: Entity_SpawnFromBMSRecord @0x40f02f `if (record[79]) follow path record[79]`]).
# Shaped { id, label } for InspectorForms.populate_id_option. id 0 = "None": byte 79 == 0 means the
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
	return _zones.get_selected_zone_index()


func get_selected_zone() -> Dictionary:
	return _zones.get_selected_zone()


func select_area_trigger(index: int) -> void:
	_zones.select_area_trigger(index)


func add_area_trigger_default() -> int:
	return _zones.add_area_trigger_default()


func set_selected_zone_bounds(mn: Vector3, mx: Vector3) -> void:
	_zones.set_selected_zone_bounds(mn, mx)


func set_selected_zone_flags(active: bool, constrain_z: bool) -> void:
	_zones.set_selected_zone_flags(active, constrain_z)


func delete_selected_area_trigger() -> bool:
	return _zones.delete_selected_area_trigger()


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
	if _sim._reject_edit_while_simulating():
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


func reload_environment() -> String:
	return _io.reload_environment()


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
	return _sim.can_simulate()


func is_simulating() -> bool:
	return _sim.is_simulating()


func is_sim_playing() -> bool:
	return _sim.is_sim_playing()


func get_sim_runtime() -> Node:
	return _sim.get_sim_runtime()


func sim_play() -> void:
	_sim.sim_play()


func sim_pause() -> void:
	_sim.sim_pause()


func sim_step() -> void:
	_sim.sim_step()


func sim_stop() -> void:
	_sim.sim_stop()


func notify_sim_transport_changed() -> void:
	_sim.notify_sim_transport_changed()
