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

# Half-extent (world units) of a marker gizmo cube; also its pick-target half-size. Matches the
# waypoint overlay so gizmos read at a consistent size across modes.
const MARKER_HALF := 1.5
const COLOR_NEUTRAL := Color(0.95, 0.85, 0.2)
const COLOR_SELECTED := Color(0.2, 1.0, 0.5)

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
		var giz := _make_gizmo(pos, COLOR_NEUTRAL, label)
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


# Move a marker gizmo's world position without a full rebuild (live drag preview). No-op if the
# marker has no gizmo. The pickable AABB is left until the next rebuild; a drag commits through the
# controller, which rebuilds.
func preview_marker_position(marker_index: int, world_pos: Vector3) -> void:
	if _gizmo_by_marker.has(marker_index):
		(_gizmo_by_marker[marker_index] as MeshInstance3D).position = world_pos


func _make_gizmo(pos: Vector3, color: Color, label_text: String) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = _gizmo_mesh
	mi.position = pos
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = color
	mi.material_override = mat
	mi.set_meta("base_color", color)
	if not label_text.is_empty():
		# A constant-size, always-on-top name floating above the cube, so the user can tell marker
		# types apart. Its own node (not the cube's material), so the selection re-tint never
		# disturbs it.
		var label := Label3D.new()
		label.text = label_text
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.fixed_size = true
		label.no_depth_test = true
		label.font_size = 22
		label.outline_size = 8
		label.modulate = Color.WHITE
		label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		label.position = Vector3(0.0, MARKER_HALF + 0.6, 0.0)
		mi.add_child(label)
	return mi
