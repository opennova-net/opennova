extends Node3D

# In-world overlay for area-trigger / restriction-zone authoring (editor-only). Draws every
# zone as an axis-aligned wireframe box (the selected one bright, the others dimmed for
# context; inactive zones are drawn fainter), plus a pickable body handle per zone and a
# grab cube on the selected zone's centre. Area triggers are 32-byte box records the engine
# reads as interleaved per-axis bounds + a flags dword (Entity_IsTeamInTriggerBounds
# @0x43c75c); the lib (libs/mission) round-trips them byte-faithfully and the binding
# (NovaMissionData.get_area_triggers) hands them here as { index, id, min, max, active,
# constrain_z, raw_flags } in mission space. This overlay only reads; the controller mutates.
#
# Parented under the MissionObjects container (so it hides / frees with the placed world),
# rebuilt by the controller. Referenced via preload (no class_name), same convention as the
# waypoint overlay.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const Overlay := preload("res://engine/mission/mission_overlay_util.gd")

# Half-extent (world units) of the centre grab cube; also its pick-target half-size.
const HANDLE_HALF := 1.5
const COLOR_SELECTED := Color(0.25, 1.0, 0.55)
const COLOR_ACTIVE := Color(0.95, 0.7, 0.2, 0.85)      # an active zone, not selected
const COLOR_INACTIVE := Color(0.55, 0.55, 0.6, 0.35)   # flags&0x01 clear
const COLOR_HANDLE := Color(0.25, 1.0, 0.55)
# A zero-extent zone box would be unpickable; pad the body pick AABB by this on each axis.
const PICK_PAD := 1.0

var _lines: MeshInstance3D
var _line_mesh: ImmediateMesh
var _handle: MeshInstance3D            # centre grab cube for the selected zone (or hidden)
var _handle_mesh: BoxMesh
# One record per zone: { zone_index, handle (-1 = body), aabb (world) }.
var _zone_pickable: Array = []


func _ensure_built() -> void:
	if _lines == null:
		_line_mesh = ImmediateMesh.new()
		_lines = MeshInstance3D.new()
		_lines.name = "AreaTriggerLines"
		_lines.mesh = _line_mesh
		_lines.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_lines.material_override = Overlay.line_material()
		add_child(_lines)
	if _handle_mesh == null:
		_handle_mesh = BoxMesh.new()
		_handle_mesh.size = Vector3.ONE * (HANDLE_HALF * 2.0)
	if _handle == null:
		_handle = MeshInstance3D.new()
		_handle.name = "AreaTriggerHandle"
		_handle.mesh = _handle_mesh
		_handle.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.albedo_color = COLOR_HANDLE
		_handle.material_override = mat
		_handle.visible = false
		add_child(_handle)


# Rebuild every zone's wire box + the pickable index. `selected_index` is drawn bright with a
# centre grab cube. `preview` optionally overrides one zone's bounds during a live drag:
# { "index": int, "min": Vector3 (mission), "max": Vector3 (mission) }.
func rebuild(mission, selected_index: int, preview: Dictionary = {}) -> void:
	_ensure_built()
	_zone_pickable = []
	_line_mesh.clear_surfaces()
	_handle.visible = false
	if mission == null:
		return

	var zones: Array = mission.get_area_triggers()
	var segments: Array = []  # [{ a, b, color }]
	for zone in zones:
		var index := int(zone.get("index", -1))
		var mn: Vector3 = zone.get("min", Vector3.ZERO)
		var mx: Vector3 = zone.get("max", Vector3.ZERO)
		if not preview.is_empty() and int(preview.get("index", -1)) == index:
			mn = preview.get("min", mn)
			mx = preview.get("max", mx)
		var is_selected := index == selected_index
		var color: Color
		if is_selected:
			color = COLOR_SELECTED
		elif bool(zone.get("active", false)):
			color = COLOR_ACTIVE
		else:
			color = COLOR_INACTIVE
		_collect_box_segments(segments, mn, mx, color)
		# Pickable body AABB (world space). The mission->godot map is an axis permutation, so the
		# box stays axis-aligned: see mission_object_placer.bms_to_godot_position.
		var pa := MissionObjectPlacer.bms_to_godot_position(mn)
		var pb := MissionObjectPlacer.bms_to_godot_position(mx)
		var lo := Vector3(min(pa.x, pb.x), min(pa.y, pb.y), min(pa.z, pb.z))
		var hi := Vector3(max(pa.x, pb.x), max(pa.y, pb.y), max(pa.z, pb.z))
		var aabb := AABB(lo - Vector3.ONE * PICK_PAD, (hi - lo) + Vector3.ONE * (PICK_PAD * 2.0))
		_zone_pickable.append({ "zone_index": index, "handle": -1, "aabb": aabb })
		# Centre grab cube on the selected zone.
		if is_selected:
			_handle.position = (lo + hi) * 0.5
			_handle.visible = true

	Overlay.emit_line_segments(_line_mesh, segments)


func zone_pickables() -> Array:
	return _zone_pickable


func _collect_box_segments(out: Array, mn: Vector3, mx: Vector3, color: Color) -> void:
	# Eight mission-space corners -> godot, then the 12 edges of the axis-aligned box.
	var c := []
	for cx in [mn.x, mx.x]:
		for cy in [mn.y, mx.y]:
			for cz in [mn.z, mx.z]:
				c.append(MissionObjectPlacer.bms_to_godot_position(Vector3(cx, cy, cz)))
	# Corner index = (xbit<<2)|(ybit<<1)|zbit. Edges connect corners differing in one bit.
	var edges := [
		[0, 1], [2, 3], [4, 5], [6, 7],   # z-varying
		[0, 2], [1, 3], [4, 6], [5, 7],   # y-varying
		[0, 4], [1, 5], [2, 6], [3, 7],   # x-varying
	]
	for e in edges:
		out.append({ "a": c[e[0]], "b": c[e[1]], "color": color })
