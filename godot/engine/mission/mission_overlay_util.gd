extends RefCounted

# Stateless helpers shared by the three in-world mission overlays (marker / waypoint / area-trigger).
# Each overlay keeps its own state + rebuild logic and calls in here for the pieces that were
# byte-identical across them: the cube gizmo build, the selection re-tint, and the connector-line
# material + emit. Referenced via preload (no class_name), the same convention as the overlays.

# Half-extent (world units) of a marker gizmo cube; also its pick-target half-size. The controller's
# pick AABBs are sized to this, so keep it stable.
const MARKER_HALF := 1.5
const COLOR_NEUTRAL := Color(0.95, 0.85, 0.2)
const COLOR_SELECTED := Color(0.2, 1.0, 0.5)

# Marker / waypoint label sizing. The labels are billboarded but NOT fixed_size: they live in world
# space so they scale with camera distance -- readable when you zoom toward an entity, and shrinking
# away to nothing in a high-altitude overview. (fixed_size kept every label a constant on-screen size
# regardless of zoom, so a map overview of hundreds of sound-emitter / marker names piled into an
# unreadable wall of huge text.) World text height = font_size * LABEL_PIXEL_SIZE; ~0.96 units here,
# a bit under the 1.5-unit gizmo half-extent, so a label sits just above its cube up close.
const LABEL_FONT_SIZE := 32
const LABEL_OUTLINE_SIZE := 8
const LABEL_PIXEL_SIZE := 0.03


# A cube gizmo at `pos` tinted `color`, with `base_color` meta so apply_selection can restore it.
# A non-empty `label_text` adds a constant-size, always-on-top label floating above the cube (its
# own node, so the selection re-tint never disturbs it); an empty string omits the label.
static func make_gizmo(mesh: BoxMesh, pos: Vector3, color: Color, label_text: String) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = pos
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = color
	mi.material_override = mat
	mi.set_meta("base_color", color)
	if not label_text.is_empty():
		var label := Label3D.new()
		label.text = label_text
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.fixed_size = false
		label.pixel_size = LABEL_PIXEL_SIZE
		label.no_depth_test = true
		label.font_size = LABEL_FONT_SIZE
		label.outline_size = LABEL_OUTLINE_SIZE
		label.modulate = Color.WHITE
		label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		label.position = Vector3(0.0, MARKER_HALF + 0.6, 0.0)
		mi.add_child(label)
	return mi


# Reset every gizmo to its base_color meta, then highlight selected_index (or none, with -1).
# gizmo_by_marker maps marker_index -> MeshInstance3D.
static func apply_selection(gizmo_by_marker: Dictionary, selected_index: int) -> void:
	for mi in gizmo_by_marker:
		var giz: MeshInstance3D = gizmo_by_marker[mi]
		var mat := giz.material_override as StandardMaterial3D
		if mat != null:
			mat.albedo_color = giz.get_meta("base_color", COLOR_NEUTRAL)
	if selected_index >= 0 and gizmo_by_marker.has(selected_index):
		var sel: MeshInstance3D = gizmo_by_marker[selected_index]
		var sel_mat := sel.material_override as StandardMaterial3D
		if sel_mat != null:
			sel_mat.albedo_color = COLOR_SELECTED


# Unshaded, vertex-coloured, alpha-blended material for the connector / wire-box lines (dim context
# lines carry alpha < 1, so the material must blend).
static func line_material() -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	return mat


# Emit one PRIMITIVE_LINES surface from `segments` ([{ a, b, color }]) onto an already-cleared
# ImmediateMesh. No-op on an empty list (surface_end on an empty surface is invalid).
static func emit_line_segments(line_mesh: ImmediateMesh, segments: Array) -> void:
	if segments.is_empty():
		return
	line_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for seg in segments:
		line_mesh.surface_set_color(seg["color"])
		line_mesh.surface_add_vertex(seg["a"])
		line_mesh.surface_set_color(seg["color"])
		line_mesh.surface_add_vertex(seg["b"])
	line_mesh.surface_end()
