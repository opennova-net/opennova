extends GutTest

# The first-person viewmodel's blended strips against the world. Retail draws
# the viewmodel after the sky pass and before every world draw (the dome
# @0x5ca81a, the gun @0x5ca829 in Render_ProcessMainSceneFrame), and a blended
# fixed-function strip writes no depth (NOWRITE forced @0x5afc8d..0x5afcaa), so
# every world draw passing LESSEQUAL afterwards paints over it: the gun's glow
# survives only over the gun's own depth band and over the empty sky. The
# check renders through the real device and is pending under the headless
# dummy renderer.

const GLOW_SHADER := "res://shaders/object/self_lit/additive_double_sided.gdshader"
const GUN_SHADER := "res://shaders/object/fixed/opaque_double_sided.gdshader"


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


func _white_texture() -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, Color(1, 1, 1, 1))
	return ImageTexture.create_from_image(image)


func _flat_world_material(color: Color) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform vec3 u_color;
void fragment() {
	ALBEDO = u_color;
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_color", Vector3(color.r, color.g, color.b))
	return material


func _quad(parent: Node, size: Vector2, position: Vector3, material: Material) -> MeshInstance3D:
	var mesh := QuadMesh.new()
	mesh.size = size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	instance.position = position
	parent.add_child(instance)
	return instance


func _viewport() -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 32)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	environment.environment.background_color = Color(0, 0, 0, 1)
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.near = 0.2
	camera.far = 100.0
	camera.fov = 60.0
	viewport.add_child(camera)
	camera.make_current()
	return viewport


func _object_material(path: String) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = load(path) as Shader
	material.set_shader_parameter("u_diffuse", _white_texture())
	return material


func test_a_blended_viewmodel_strip_leaves_the_world_to_the_world() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := _viewport()
	# The world: an opaque red wall behind the LEFT half of the view.
	_quad(viewport, Vector2(6, 12), Vector3(-3.0, 0.0, -5.0),
			_flat_world_material(Color(1, 0, 0)))
	# The gun: an opaque strip in its depth band over the bottom rows, and the
	# additive self-lit glow in front of everything.
	var gun := _quad(viewport, Vector2(4.0, 0.2), Vector3(0.0, -0.18, -1.0),
			_object_material(GUN_SHADER))
	gun.set_instance_shader_parameter("u_viewmodel_pass", true)
	var glow := _quad(viewport, Vector2(4.0, 4.0), Vector3(0.0, 0.0, -0.9),
			_object_material(GLOW_SHADER))
	glow.set_instance_shader_parameter("u_viewmodel_pass", true)
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	var image := viewport.get_texture().get_image()
	var over_world := image.get_pixel(12, 8)
	var over_sky := image.get_pixel(52, 8)
	assert_gt(over_world.r, 0.9, "the wall is drawn")
	assert_lt(over_world.g + over_world.b, 0.1,
			"the world paints over the gun's blended glow")
	assert_gt(over_sky.g, 0.5, "the glow survives where no world lies behind it")
