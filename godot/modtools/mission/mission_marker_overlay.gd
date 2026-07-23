extends Node3D

# In-world overlay for ALL markers (editor-only). Markers (player start, insertion, waypoint,
# location, ...) are mesh-less KIND_MARKER entities, so the placer counts-and-skips them; this
# overlay is what makes every marker visible and selectable in Objects mode, where markers are
# placed and edited like any other entity. It draws one cube gizmo per marker (optionally labelled
# with the marker's display name) and exposes ray-pick AABBs. It is parented under the
# MissionObjects container, so it hides / frees with the placed world and is rebuilt by the
# controller; the overlay only reads, never mutates.
#
# This is the general-marker counterpart to mission_waypoint_overlay.gd (which decorates the active
# waypoint *path* with order labels + connector lines in Waypoints mode). The two are never visible
# at once: this one shows in Objects mode, the waypoint overlay in Waypoints mode.
#
# Referenced via preload (no class_name), the same convention as the controller / placer.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const Overlay := preload("res://engine/mission/mission_overlay_util.gd")

# Half-extent (world units) of a marker gizmo cube; also its pick-target half-size. Sourced from the
# shared overlay util so gizmos read at a consistent size across modes.
const MARKER_HALF := Overlay.MARKER_HALF
const COLOR_NEUTRAL := Overlay.COLOR_NEUTRAL
const COLOR_SELECTED := Overlay.COLOR_SELECTED

var _gizmos: Node3D
var _gizmo_mesh: BoxMesh
# One record per marker gizmo: { marker_index, aabb (world) }.
var _marker_pickable: Array = []
var _gizmo_by_marker: Dictionary = {}  # marker_index -> MeshInstance3D (for re-tint / drag preview)
var _selected_marker_index: int = -1


func _ensure_built() -> void:
	if _gizmos == null:
		_gizmos = Node3D.new()
		_gizmos.name = "MarkerGizmos"
		add_child(_gizmos)
	if _gizmo_mesh == null:
		_gizmo_mesh = BoxMesh.new()
		_gizmo_mesh.size = Vector3.ONE * (MARKER_HALF * 2.0)


# Rebuild from the mission: a gizmo per KIND_MARKER entity, labelled with `labels[i]` when that
# entry is a non-empty string (the controller passes the marker's items.def display name so the
# user can tell a player start from a waypoint node). Recomputes the pickable index. The controller
# re-applies its real marker selection right after (so a rebuild starts unselected).
func rebuild(mission, labels: Array = []) -> void:
	_ensure_built()
	_marker_pickable = []
	_gizmo_by_marker = {}
	_selected_marker_index = -1
	for child in _gizmos.get_children():
		child.queue_free()
	if mission == null:
		return
	var markers: Array = mission.get_entities(NovaMissionData.KIND_MARKER)
	for marker_index in markers.size():
		var pos: Vector3 = MissionObjectPlacer.bms_to_godot_position(markers[marker_index]["position"])
		var label: String = String(labels[marker_index]) if marker_index < labels.size() else ""
		var giz := Overlay.make_gizmo(_gizmo_mesh, pos, COLOR_NEUTRAL, label)
		_gizmos.add_child(giz)
		_gizmo_by_marker[marker_index] = giz
		_marker_pickable.append({
			"marker_index": marker_index,
			"aabb": AABB(pos - Vector3.ONE * MARKER_HALF, Vector3.ONE * (MARKER_HALF * 2.0)),
		})


func marker_pickables() -> Array:
	return _marker_pickable


# Highlight the gizmo for marker_index (or clear, with -1). Tolerant of an index with no gizmo
# (e.g. just after a structural change): it only records the index for the next rebuild.
func set_selected_marker(marker_index: int) -> void:
	_selected_marker_index = marker_index
	Overlay.apply_selection(_gizmo_by_marker, marker_index)


# Move a marker gizmo's world position without a full rebuild (live drag preview). No-op if the
# marker has no gizmo. The pickable AABB is left until the next rebuild; a drag commits through the
# controller, which rebuilds.
func preview_marker_position(marker_index: int, world_pos: Vector3) -> void:
	if _gizmo_by_marker.has(marker_index):
		(_gizmo_by_marker[marker_index] as MeshInstance3D).position = world_pos
