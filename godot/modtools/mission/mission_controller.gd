extends RefCounted

# Editor-side controller for the Mission workspace (the Phase 2 adapter).
#
# Holds the open mission (NovaMissionData) plus its document state (path /
# loaded / dirty) and drives the load: parse the mission file, resolve its referenced
# terrain + environment from the shared resource root, load them through the
# terrain editor (read-only viewport), then run the shell-agnostic
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
const MissionWaypointOverlay := preload("res://modtools/mission/mission_waypoint_overlay.gd")
const MissionAreaTriggerOverlay := preload("res://modtools/mission/mission_area_trigger_overlay.gd")
const MissionMarkerOverlay := preload("res://modtools/mission/mission_marker_overlay.gd")
const ObjectUserPointOverlayScript := preload("res://engine/object/object_user_point_overlay.gd")
const MissionGizmo := preload("res://modtools/framework/transform_gizmo_3d.gd")
const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
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
# Parsed co-named <mission>.til. This is the editor mount's copy of GameWorld's
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
# target SSN/group/zone to its live model -- the same MissionEntityRegistry the runtime mount uses.
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
const ControllerPreview := preload("res://modtools/mission/controller/preview_ops.gd")
const ControllerHistory := preload("res://modtools/mission/controller/history_ops.gd")
const ControllerSelection := preload("res://modtools/mission/controller/selection_ops.gd")
const ControllerScripting := preload("res://modtools/mission/controller/scripting_ops.gd")
const ControllerEdit := preload("res://modtools/mission/controller/edit_ops.gd")
var _io  # ControllerIo (created in _init)
var _reground  # ControllerReground (created in _init)
var _viewport  # ControllerViewport (created in _init)
var _placement  # ControllerPlacement (created in _init)
var _waypoints  # ControllerWaypoints (created in _init)
var _zones  # ControllerZones (created in _init)
var _preview  # ControllerPreview (created in _init)
var _history  # ControllerHistory (created in _init)
var _selection  # ControllerSelection (created in _init)
var _scripting  # ControllerScripting (created in _init)
var _edit  # ControllerEdit (created in _init)


func _init(p_terrain_editor: Node = null) -> void:
	terrain_editor = p_terrain_editor
	_io = ControllerIo.new(self)
	_reground = ControllerReground.new(self)
	_viewport = ControllerViewport.new(self)
	_placement = ControllerPlacement.new(self)
	_waypoints = ControllerWaypoints.new(self)
	_zones = ControllerZones.new(self)
	_preview = ControllerPreview.new(self)
	_history = ControllerHistory.new(self)
	_selection = ControllerSelection.new(self)
	_scripting = ControllerScripting.new(self)
	_edit = ControllerEdit.new(self)


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


func entity_display_name(kind: int, index: int) -> String:
	return _selection.entity_display_name(kind, index)


func get_selected_display_name() -> String:
	return _selection.get_selected_display_name()


func get_selected_graphic_name() -> String:
	return _selection.get_selected_graphic_name()


func get_selection_summary() -> Dictionary:
	return _selection.get_selection_summary()


func selected_has_user_points() -> bool:
	return _selection.selected_has_user_points()


func is_selected_user_points_visible() -> bool:
	return _selection.is_selected_user_points_visible()


func set_selected_user_points_visible(value: bool) -> void:
	_selection.set_selected_user_points_visible(value)


func _selected_object_data() -> NovaObjectData:
	return _selection._selected_object_data()


func select_object(kind: int, index: int) -> void:
	_selection.select_object(kind, index)


func focus_selection_in_view() -> bool:
	return _selection.focus_selection_in_view()


func get_mission_title() -> String:
	return _selection.get_mission_title()


func _resource_root() -> NovaResourceRoot:
	return _selection._resource_root()


func _load_mission_tile_info(bms_name: String, resource_root: NovaResourceRoot) -> void:
	_selection._load_mission_tile_info(bms_name, resource_root)


func _clear_mission_tile_info() -> void:
	_selection._clear_mission_tile_info()


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
	return _history.can_undo()


func can_redo() -> bool:
	return _history.can_redo()


func undo_depth() -> int:
	return _history.undo_depth()


func begin_edit() -> void:
	_history.begin_edit()


func commit_edit() -> void:
	_history.commit_edit()


func _flush_edit() -> void:
	_history._flush_edit()


func _edit_step(do: Callable, err := "", on_success := Callable()) -> bool:
	return _history._edit_step(do, err, on_success)


func _clear_history() -> void:
	_history._clear_history()


func undo() -> void:
	_history.undo()


func redo() -> void:
	_history.redo()


func _consume_viewport_key() -> void:
	_history._consume_viewport_key()


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
	return _preview.can_preview_part_anim(action)


func preview_part_anim(action: Dictionary) -> bool:
	return _preview.preview_part_anim(action)


func stop_preview() -> void:
	_preview.stop_preview()


func move_selected_to_world_grounded(global_hit: Vector3) -> bool:
	return _viewport.move_selected_to_world_grounded(global_hit)


func get_selected_entity() -> Dictionary:
	return _selection.get_selected_entity()


func get_selected_position() -> Vector3:
	return _selection.get_selected_position()


func get_selected_rotation() -> Vector3:
	return _selection.get_selected_rotation()


func set_selected_position(bms_pos: Vector3) -> void:
	_selection.set_selected_position(bms_pos)


func set_selected_rotation(rot_deg: Vector3) -> void:
	_selection.set_selected_rotation(rot_deg)


func set_selected_team(value: int) -> void:
	_selection.set_selected_team(value)


func set_selected_group(value: int) -> void:
	_selection.set_selected_group(value)


func set_selected_property(property: String, value: int) -> void:
	_selection.set_selected_property(property, value)


func get_membership_revision() -> int:
	return _selection.get_membership_revision()


func set_selected_string_property(property: String, value: String) -> void:
	_selection.set_selected_string_property(property, value)


func set_header_string(field: String, value: String) -> void:
	_selection.set_header_string(field, value)


func set_header_int(field: String, value: int) -> void:
	_selection.set_header_int(field, value)


func set_header_flag(bit: int, on: bool) -> void:
	_selection.set_header_flag(bit, on)


func set_game_mode(bit: int) -> void:
	_selection.set_game_mode(bit)


func get_weapon_loadout() -> Array:
	return _selection.get_weapon_loadout()


func set_weapon_loadout(entries: Array) -> void:
	_selection.set_weapon_loadout(entries)


func get_group_count() -> int:
	return _selection.get_group_count()


func get_groups() -> Array:
	return _selection.get_groups()


func get_group(index: int) -> Dictionary:
	return _selection.get_group(index)


func set_group(index: int, field0: int, field8: int, field12: int) -> void:
	_selection.set_group(index, field0, field8, field12)


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


func delete_selected() -> bool:
	return _edit.delete_selected()


func _rebake_objects() -> void:
	_edit._rebake_objects()


func _refresh_active_overlay() -> void:
	_edit._refresh_active_overlay()


# --- Authoring (P7): waypoint mode + marker selection -------------------------
# Waypoints mode switches the viewport from object editing to authoring the active path's
# markers, and the inspector to the waypoint panel. The two modes are exclusive: entering
# either drops the other's selection and any armed placement tool. The chosen path persists
# across re-bakes; the marker selection (like the object selection) does not. Marker
# picking reuses the same analytic ray-vs-AABB as objects, over the overlay's gizmo AABBs.

func set_mode(mode: int) -> void:
	_edit.set_mode(mode)


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

func move_selected_marker(delta: int) -> void:
	_waypoints.move_selected_marker(delta)


func delete_selected_marker() -> bool:
	return _waypoints.delete_selected_marker()


func clear_active_path() -> bool:
	return _waypoints.clear_active_path()


func get_area_triggers() -> Array:
	return _mission.get_area_triggers() if _mission != null else []


func get_all_entities() -> Array:
	return _selection.get_all_entities()


func get_object_list() -> Array:
	return _selection.get_object_list()


func get_object_count() -> int:
	return _selection.get_object_count()


func has_item_database() -> bool:
	return _selection.has_item_database()


func get_waypoint_path_options() -> Array:
	return _selection.get_waypoint_path_options()


func get_group_options() -> Array:
	return _selection.get_group_options()


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
	return _scripting.get_event_count()


func get_events() -> Array:
	return _scripting.get_events()


func get_selected_event_index() -> int:
	return _scripting.get_selected_event_index()


func get_selected_event_chain() -> Dictionary:
	return _scripting.get_selected_event_chain()


func get_logic_summary() -> Dictionary:
	return _scripting.get_logic_summary()


func get_trigger_main_types() -> Array:
	return _scripting.get_trigger_main_types()


func get_trigger_sub_types(main_type: int) -> Array:
	return _scripting.get_trigger_sub_types(main_type)


func get_action_types() -> Array:
	return _scripting.get_action_types()


func get_action_sub_types(action_type: int) -> Array:
	return _scripting.get_action_sub_types(action_type)


func get_event_flag_bits() -> Array:
	return _scripting.get_event_flag_bits()


func get_ai_flag_bits() -> Array:
	return _scripting.get_ai_flag_bits()


func select_event(index: int) -> void:
	_scripting.select_event(index)


func add_event_default() -> int:
	return _scripting.add_event_default()


func delete_selected_event() -> bool:
	return _scripting.delete_selected_event()


func set_selected_event(flags: int, reset_after: int, delay: int) -> void:
	_scripting.set_selected_event(flags, reset_after, delay)


func add_selected_event_trigger() -> void:
	_scripting.add_selected_event_trigger()


func set_selected_event_trigger(local_index: int, trigger: Dictionary) -> void:
	_scripting.set_selected_event_trigger(local_index, trigger)


func remove_selected_event_trigger(local_index: int) -> void:
	_scripting.remove_selected_event_trigger(local_index)


func move_selected_event_trigger(local_index: int, delta: int) -> void:
	_scripting.move_selected_event_trigger(local_index, delta)


func add_selected_event_action() -> void:
	_scripting.add_selected_event_action()


func set_selected_event_action(local_index: int, action: Dictionary) -> void:
	_scripting.set_selected_event_action(local_index, action)


func remove_selected_event_action(local_index: int) -> void:
	_scripting.remove_selected_event_action(local_index)


func move_selected_event_action(local_index: int, delta: int) -> void:
	_scripting.move_selected_event_action(local_index, delta)


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
