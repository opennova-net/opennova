extends RefCounted

# Stateless helpers shared by the in-world debug overlays: the connector / wire-box line
# material + emit. Referenced via preload (no class_name), the same convention as the overlays.


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
