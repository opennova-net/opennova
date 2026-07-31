extends SimDebugView

# Draws the render-occlusion portal data over the world: every nearby
# portal-carrying building's occlusion faces as type-colored outlines, with a
# section label on each portal-type record. The 3D face of the F3 overlay's
# "Show portal faces" toggle -- a developer tool for eyeballing our port of the
# section-mask/portal engine (docs/render/render-occlusion-re.md), not
# engine-witnessed behavior.
#
# Geometry comes from NovaSimulation.get_occlusion_portal_debug(): each record's
# boundary outline (interior shared edges cancelled on the OFAC low-15-bit
# identity) is transformed in C++ through the SAME render_matrix_from_pose path
# the engine's occlusion frame runs, so what is drawn IS what the portal
# traversal and occluder culling test. The mesh + labels rebuild only when the
# payload changes (static per mission apart from the weld retypes at load).
# Built / freed by GameWorld on the overlay's toggle, like the collision view.

# The engine's own portal-slot collection radius [orig: the 250 u range in
# collect_visible_sector_userpoints @ 0x5c6b60] -- the drawn sweep matches the
# buildings whose portals can hold live slots around the camera.
const RANGE_UNITS := 250.0
const LABEL_CAP := 200

# Record type -> outline color (the 60 B record type byte,
# libs/world/include/world/occlusion.h).
static func type_color(record_type: int) -> Color:
	match record_type:
		0:
			return Color(0.55, 0.55, 0.55)  # occluder face - gray
		1:
			return Color(1.0, 0.6, 0.15)    # open occluder slot (doors) - orange
		2:
			return Color(0.25, 0.9, 1.0)    # exterior window portal - cyan
		3:
			return Color(0.35, 1.0, 0.45)   # interior room-to-room portal - green
		5:
			return Color(1.0, 0.4, 1.0)     # welded cross-building link - magenta
		_:
			return COLOR_UNKNOWN
const COLOR_UNKNOWN := Color(1.0, 0.9, 0.3)


static func type_name(record_type: int) -> String:
	match record_type:
		0:
			return "occluder"
		1:
			return "open"
		2:
			return "window"
		3:
			return "portal"
		5:
			return "link"
		_:
			return "type %d" % record_type


static func section_name(section: int) -> String:
	return "ext" if section == 0 else "s%d" % section


var _mesh: ImmediateMesh
var _labels: Node3D
var _signature := 0             # hash of building poses + record geometry
var _has_surface := false
var _drawable_count := 0        # portal/occluder records with outline segments


func _sim_debug_method() -> String:
	return "get_occlusion_portal_debug"


func _build_view() -> void:
	_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("OcclusionPortalLines", _mesh))
	_labels = Node3D.new()
	_labels.name = "OcclusionPortalLabels"
	add_child(_labels)


func _refresh_from_sim(sim: Object) -> void:
	var anchor := Vector3.ZERO
	if is_inside_tree():
		var camera := get_viewport().get_camera_3d()
		if camera != null:
			anchor = camera.global_position
	var debug: Dictionary = sim.get_occlusion_portal_debug(anchor, RANGE_UNITS)
	_update_geometry(debug.get("buildings", []))


func _clear_all() -> void:
	_drawable_count = 0
	if _has_surface:
		_mesh.clear_surfaces()
		_has_surface = false
		_signature = 0
	_clear_labels()


## Number of portal/occluder records currently contributing outline geometry.
func get_debug_drawable_count() -> int:
	return _drawable_count


func _clear_labels() -> void:
	for child in _labels.get_children():
		_labels.remove_child(child)
		child.queue_free()


func _update_geometry(buildings: Array) -> void:
	# Rebuild only when a building's pose or its record geometry changed; the
	# per-frame visible flag is deliberately left OUT of the key so the batch
	# flicking on and off never forces a rebuild.
	var sig_parts := []
	for b_v in buildings:
		var b: Dictionary = b_v
		sig_parts.append(b.get("bms_id", 0))
		sig_parts.append(b.get("pos", Vector3.ZERO))
		sig_parts.append(hash(b.get("records", [])))
	var sig := hash(sig_parts)
	if sig == _signature and _has_surface == (not buildings.is_empty()):
		return
	_signature = sig
	_mesh.clear_surfaces()
	_has_surface = false
	_drawable_count = 0
	_clear_labels()

	var segments: Array = []
	var label_count := 0
	for b_v in buildings:
		var b: Dictionary = b_v
		for rec_v in b.get("records", []):
			var rec: Dictionary = rec_v
			var rtype := int(rec.get("type", 0))
			var color := type_color(rtype)
			var pts: PackedVector3Array = rec.get("segments", PackedVector3Array())
			if pts.size() >= 2:
				_drawable_count += 1
			for i in range(0, pts.size() - 1, 2):
				segments.append({ "a": pts[i], "b": pts[i + 1], "color": color })
			# Portal-type records carry a section label (record normal points
			# a -> b; section 0 = exterior). Plain occluder faces are numerous
			# and stay label-free.
			if rtype != 0 and label_count < LABEL_CAP:
				label_count += 1
				_labels.add_child(_make_label(rec, rtype, color))
	if segments.is_empty():
		return
	MissionOverlayUtil.emit_line_segments(_mesh, segments)
	_has_surface = true


func _make_label(rec: Dictionary, rtype: int, color: Color) -> Label3D:
	var label := Label3D.new()
	label.text = "%s %s->%s" % [
		type_name(rtype),
		section_name(int(rec.get("section_a", 0))),
		section_name(int(rec.get("section_b", 0))),
	]
	label.position = rec.get("pos", Vector3.ZERO)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.fixed_size = false
	label.pixel_size = 0.005
	label.no_depth_test = true
	label.font_size = 36
	label.outline_size = 10
	label.modulate = color
	label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
	return label
