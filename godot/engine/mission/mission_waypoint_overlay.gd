extends Node3D

# In-world overlay for waypoint authoring (editor-only). Draws the active waypoint path's
# markers as pickable gizmos and every non-empty path as connector lines (the active path
# solid in its team colour, the others dimmed for context). Markers are mesh-less
# KIND_MARKER entities, so the placer counts-and-skips them; this overlay is what makes
# them visible and selectable. It is parented under the MissionObjects container, so it
# hides / frees with the placed world, and is rebuilt by the controller (which owns the
# mission); the overlay itself only reads, it never mutates.
#
# Referenced via preload (no class_name), the same convention as the controller / placer.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const Overlay := preload("res://engine/mission/mission_overlay_util.gd")

# Half-extent (world units) of a marker gizmo cube; also its pick-target half-size.
const MARKER_HALF := Overlay.MARKER_HALF
const COLOR_NEUTRAL := Overlay.COLOR_NEUTRAL    # path with no team flag
const COLOR_BLUE := Color(0.3, 0.6, 1.0)
const COLOR_RED := Color(1.0, 0.4, 0.35)
const COLOR_SELECTED := Overlay.COLOR_SELECTED
const COLOR_LINE_DIM := Color(0.6, 0.6, 0.6, 0.35)

var _gizmos: Node3D            # parent of the active path's marker MeshInstance3D gizmos
var _lines: MeshInstance3D     # one ImmediateMesh holding every path's connector lines
var _line_mesh: ImmediateMesh
var _gizmo_mesh: BoxMesh
# One record per active-path marker gizmo: { path_index, marker_index, order, aabb (world) }.
var _marker_pickable: Array = []
var _gizmo_by_marker: Dictionary = {}  # marker_index -> MeshInstance3D (for re-tint)
var _selected_marker_index: int = -1


func _ensure_built() -> void:
	if _gizmos == null:
		_gizmos = Node3D.new()
		_gizmos.name = "WaypointGizmos"
		add_child(_gizmos)
	if _lines == null:
		_line_mesh = ImmediateMesh.new()
		_lines = MeshInstance3D.new()
		_lines.name = "WaypointLines"
		_lines.mesh = _line_mesh
		_lines.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_lines.material_override = Overlay.line_material()
		add_child(_lines)
	if _gizmo_mesh == null:
		_gizmo_mesh = BoxMesh.new()
		_gizmo_mesh.size = Vector3.ONE * (MARKER_HALF * 2.0)


# Rebuild from the mission: gizmos for the active path's markers, lines for every non-empty
# path (active solid + coloured, others dimmed). Recomputes the pickable index. An
# active_path_index < 0 draws only the dimmed context lines (no gizmos / pickables).
func rebuild(mission, active_path_index: int) -> void:
	_ensure_built()
	_marker_pickable = []
	_gizmo_by_marker = {}
	# Start every rebuild unselected; the controller re-applies its real marker selection
	# right after (mission_controller._refresh_waypoint_overlay). This guarantees a path
	# switch can never leave a stale highlight on a marker index the new path happens to share.
	_selected_marker_index = -1
	for child in _gizmos.get_children():
		child.queue_free()
	_line_mesh.clear_surfaces()
	if mission == null:
		return

	var markers: Array = mission.get_entities(NovaMissionData.KIND_MARKER)
	var paths: Array = mission.get_waypoint_paths()

	# Collect every line segment first, so the ImmediateMesh surface is only opened when
	# there is at least one segment (surface_end on an empty surface is invalid).
	var segments: Array = []  # [{ a, b, color }]
	for path in paths:
		var indices: PackedInt32Array = path.get("marker_indices", PackedInt32Array())
		if indices.size() < 2:
			continue
		var is_active := int(path.get("index", -1)) == active_path_index
		var color: Color = _path_color(int(path.get("flags", 0))) if is_active else COLOR_LINE_DIM
		_collect_path_segments(segments, markers, indices, int(path.get("flags", 0)), color)
	Overlay.emit_line_segments(_line_mesh, segments)

	# Gizmos + pickables for the active path's markers only.
	if active_path_index >= 0:
		var active := _find_path(paths, active_path_index)
		if not active.is_empty():
			var aindices: PackedInt32Array = active.get("marker_indices", PackedInt32Array())
			var acolor := _path_color(int(active.get("flags", 0)))
			for order in aindices.size():
				var marker_index: int = aindices[order]
				if marker_index < 0 or marker_index >= markers.size():
					continue
				var pos: Vector3 = MissionObjectPlacer.bms_to_godot_position(markers[marker_index]["position"])
				# Label the gizmo with its 1-based route order so the path reads in the
				# viewport (matches the inspector's "1. marker #..." list).
				var giz := Overlay.make_gizmo(_gizmo_mesh, pos, acolor, str(order + 1))
				_gizmos.add_child(giz)
				_gizmo_by_marker[marker_index] = giz
				_marker_pickable.append({
					"path_index": active_path_index, "marker_index": marker_index, "order": order,
					"aabb": AABB(pos - Vector3.ONE * MARKER_HALF, Vector3.ONE * (MARKER_HALF * 2.0)),
				})
	# The controller re-applies the marker highlight after each rebuild (it owns the
	# selection, mission_controller._refresh_waypoint_overlay). The overlay deliberately does
	# NOT re-apply its own cached _selected_marker_index here: a path switch can land on a
	# path that happens to share a marker index, and re-applying the cache would bleed a stale
	# highlight the controller already dropped.


func marker_pickables() -> Array:
	return _marker_pickable


# Highlight the gizmo for marker_index (or clear, with -1). Tolerant of a marker that is
# not on the active path (no gizmo): it just records the index for the next rebuild.
func set_selected_marker(marker_index: int) -> void:
	_selected_marker_index = marker_index
	Overlay.apply_selection(_gizmo_by_marker, marker_index)


# Move a marker gizmo's world position without a full rebuild (live drag preview). No-op if
# the marker is not on the active path. The pickable AABB is left until the next rebuild;
# a drag commits through the controller, which rebuilds.
func preview_marker_position(marker_index: int, world_pos: Vector3) -> void:
	if _gizmo_by_marker.has(marker_index):
		(_gizmo_by_marker[marker_index] as MeshInstance3D).position = world_pos


func _collect_path_segments(out: Array, markers: Array, indices: PackedInt32Array, flags: int, color: Color) -> void:
	var pts: Array = []
	for marker_index in indices:
		if marker_index >= 0 and marker_index < markers.size():
			pts.append(MissionObjectPlacer.bms_to_godot_position(markers[marker_index]["position"]))
	if pts.size() < 2:
		return
	for i in pts.size() - 1:
		out.append({ "a": pts[i], "b": pts[i + 1], "color": color })
	# A path loops back to its first marker unless flagged DoesNotLoop.
	if (flags & NovaMissionData.WP_FLAG_DOES_NOT_LOOP) == 0:
		out.append({ "a": pts[pts.size() - 1], "b": pts[0], "color": color })


func _path_color(flags: int) -> Color:
	if (flags & NovaMissionData.WP_FLAG_BLUE_TEAM) != 0:
		return COLOR_BLUE
	if (flags & NovaMissionData.WP_FLAG_RED_TEAM) != 0:
		return COLOR_RED
	return COLOR_NEUTRAL


func _find_path(paths: Array, index: int) -> Dictionary:
	for p in paths:
		if int((p as Dictionary).get("index", -1)) == index:
			return p
	return {}
