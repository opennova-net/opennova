extends GutTest

# The object fog distance classes (shaders/object/shared.gdshaderinc
# obj_scene_fog_visibility), rendered through a harness spatial shader that
# includes the production helper with each wrapper's class define and writes
# the visibility as its colour. A wall perpendicular to the view at a constant
# view depth: the vertex-shader families fog on that planar depth (retail
# oFog = 1 - (z - FogStart) * FogRangeRecip, _BaseInc.fx CalcFogSegmented;
# FogStart/FogRangeRecip [orig: Material_ApplyShaderParameters @ 0x58e20d..0x58e26f]),
# so the view centre and a corner fog alike; a fixed-function pass fogs its
# linear types on the radial eye distance (vertex RANGE fog [orig:
# CD3DDevice_SetFogParameters @ 0x677a50..0x677a69]), so the corner fogs more,
# and its EXP type on view depth (@ 0x6779e4..0x677a48).

const WALL_DEPTH := 100.0
const FOG_START := 0.5
const FOG_END := 120.0
const FOG_GLOBALS := ["opennova_fog_enabled", "opennova_fog_start",
		"opennova_fog_end", "opennova_fog_type"]


func after_each() -> void:
	ShaderGlobals.restore_defaults(FOG_GLOBALS)


func _harness_code(define: String) -> String:
	var code := "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n"
	if not define.is_empty():
		code += "#define %s\n" % define
	code += """#include "res://shaders/object/shared.gdshaderinc"
void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_fog_view_depth = -(VIEW_MATRIX * vec4(v_world_pos, 1.0)).z;
}
void fragment() {
	ALBEDO = vec3(obj_scene_fog_visibility(CAMERA_POSITION_WORLD));
}
"""
	return code


func _render_visibility(define: String, fog_type: int,
		fog_start: float = FOG_START) -> Array[float]:
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", true)
	RenderingServer.global_shader_parameter_set("opennova_fog_start", fog_start)
	RenderingServer.global_shader_parameter_set("opennova_fog_end", FOG_END)
	RenderingServer.global_shader_parameter_set("opennova_fog_type", fog_type)
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.fov = 100.0
	camera.far = 1000.0
	viewport.add_child(camera)
	camera.make_current()
	var wall := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(1000.0, 1000.0)
	wall.mesh = quad
	var shader := Shader.new()
	shader.code = _harness_code(define)
	var material := ShaderMaterial.new()
	material.shader = shader
	wall.material_override = material
	viewport.add_child(wall)
	wall.position = Vector3(0.0, 0.0, -WALL_DEPTH)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	# The viewport texture stores the display encoding of the written value.
	var image := viewport.get_texture().get_image()
	return [image.get_pixel(32, 32).srgb_to_linear().r,
			image.get_pixel(1, 1).srgb_to_linear().r]


func test_vertex_shader_passes_fog_on_planar_view_depth() -> void:
	if DisplayServer.get_name() == "headless":
		pending("fog-distance rasterization needs a windowed renderer")
		return
	var linear: Array[float] = await _render_visibility("OBJ_FOG_VERTEX_SHADER", 1)
	var expected := (FOG_END - WALL_DEPTH) / (FOG_END - FOG_START)
	assert_almost_eq(linear[0], expected, 0.02, "the centre fogs on its view depth")
	assert_almost_eq(linear[1], linear[0], 0.02,
			"a corner at the same view depth fogs exactly like the centre")
	# FOGMODE_SHADER leaves the device EXP off: type 0 still fogs linearly.
	var exp_type: Array[float] = await _render_visibility("OBJ_FOG_VERTEX_SHADER", 0)
	assert_almost_eq(exp_type[0], expected, 0.02,
			"a vertex-shader pass fogs linearly even on an EXP mission")


func test_fixed_function_passes_fog_linear_types_on_the_radial_distance() -> void:
	if DisplayServer.get_name() == "headless":
		pending("fog-distance rasterization needs a windowed renderer")
		return
	var linear: Array[float] = await _render_visibility("", 1)
	var expected := (FOG_END - WALL_DEPTH) / (FOG_END - FOG_START)
	assert_almost_eq(linear[0], expected, 0.02, "the centre's radial distance is its depth")
	assert_lt(linear[1], 0.01, "the corner's radial distance is past the fog end")
	# Type 0 is the device EXP on view depth: centre and corner alike.
	var exp_type: Array[float] = await _render_visibility("", 0)
	assert_almost_eq(exp_type[1], exp_type[0], 0.02,
			"the EXP type fogs on view depth, not the radial distance")
	assert_almost_eq(exp_type[0], exp(-WALL_DEPTH * log(64.0) / FOG_END), 0.02)


func test_skinned_device_pass_keeps_planar_linear_and_depth_exp() -> void:
	if DisplayServer.get_name() == "headless":
		pending("fog-distance rasterization needs a windowed renderer")
		return
	var linear: Array[float] = await _render_visibility("OBJ_FOG_VERTEX_SHADER_DEVICE", 1)
	assert_almost_eq(linear[1], linear[0], 0.02, "SkBasic's VS oFog is planar")
	var exp_type: Array[float] = await _render_visibility("OBJ_FOG_VERTEX_SHADER_DEVICE", 0)
	assert_almost_eq(exp_type[0], exp(-WALL_DEPTH * log(64.0) / FOG_END), 0.02,
			"type 0 stays the device EXP")


func test_linear_start_is_the_pass_start_with_its_overcast_fold() -> void:
	# The pass start already carries Render_SetFogState's (1 - density) fold
	# (type 2 = (1 - density) * end * 0.5 [orig: Render_SetFogState
	# @ 0x58a9c5..0x58a9dc]); the helper must not recompute end * 0.5.
	if DisplayServer.get_name() == "headless":
		pending("fog-distance rasterization needs a windowed renderer")
		return
	var overcast_start := 0.5 * FOG_END * 0.5 # density 0.5
	var result: Array[float] = await _render_visibility("", 2, overcast_start)
	assert_almost_eq(result[0], (FOG_END - WALL_DEPTH) / (FOG_END - overcast_start), 0.02,
			"the overcast-folded start reaches the linear fog")

