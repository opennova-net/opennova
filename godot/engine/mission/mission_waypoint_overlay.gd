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

# Half-extent (world units) of a marker gizmo cube; also its pick-target half-size.
const MARKER_HALF := 1.5
const COLOR_NEUTRAL := Color(0.95, 0.85, 0.2)    # path with no team flag
const COLOR_BLUE := Color(0.3, 0.6, 1.0)
const COLOR_RED := Color(1.0, 0.4, 0.35)
const COLOR_SELECTED := Color(0.2, 1.0, 0.5)
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
		_lines.material_override = _line_material()
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
	if not segments.is_empty():
		_line_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
		for seg in segments:
			_line_mesh.surface_set_color(seg["color"])
			_line_mesh.surface_add_vertex(seg["a"])
			_line_mesh.surface_set_color(seg["color"])
			_line_mesh.surface_add_vertex(seg["b"])
		_line_mesh.surface_end()

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
				var giz := _make_gizmo(pos, acolor, order + 1)
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
	for mi in _gizmo_by_marker:
		var giz: MeshInstance3D = _gizmo_by_marker[mi]
		var mat := giz.material_override as StandardMaterial3D
		if mat != null:
			mat.albedo_color = giz.get_meta("base_color", COLOR_NEUTRAL)
	if marker_index >= 0 and _gizmo_by_marker.has(marker_index):
		var sel: MeshInstance3D = _gizmo_by_marker[marker_index]
		var sel_mat := sel.material_override as StandardMaterial3D
		if sel_mat != null:
			sel_mat.albedo_color = COLOR_SELECTED


# Move a marker gizmo's world position without a full rebuild (live drag preview). No-op if
# the marker is not on the active path. The pickable AABB is left until the next rebuild;
# a drag commits through the controller, which rebuilds.
func preview_marker_position(marker_index: int, world_pos: Vector3) -> void:
	if _gizmo_by_marker.has(marker_index):
		(_gizmo_by_marker[marker_index] as MeshInstance3D).position = world_pos


func _make_gizmo(pos: Vector3, color: Color, order: int) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = _gizmo_mesh
	mi.position = pos
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = color
	mi.material_override = mat
	mi.set_meta("base_color", color)
	# A constant-size, always-on-top route-order number floating above the cube. It is its
	# own node (not the cube's material), so the selection re-tint never disturbs it.
	var label := Label3D.new()
	label.text = str(order)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.fixed_size = true
	label.no_depth_test = true
	label.font_size = 28
	label.outline_size = 10
	label.modulate = Color.WHITE
	label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
	label.position = Vector3(0.0, MARKER_HALF + 0.6, 0.0)
	mi.add_child(label)
	return mi


func _line_material() -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	# Dim context lines carry alpha < 1, so the material must blend.
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	return mat


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
