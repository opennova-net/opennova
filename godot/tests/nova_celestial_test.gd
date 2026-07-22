extends GutTest

# Reflection-pass fidelity pins for NovaCelestial. The committed CmpFireN 3DI
# stands in for the retail body models, letting the test exercise the public
# environment/resource-root lifecycle without requiring an external JO install.

const MODEL_FIXTURE_ROOT := "res://../fixtures/3dp/CmpFireN"
const MODEL_NAME := "CmpFireN.3di"
const CELESTIAL_SHADER := "res://shaders/celestial.gdshader"
const ADDITIVE_SHADER := "res://shaders/celestial_additive.gdshader"
const TICK := 1.0 / 62.0
const FAR_CAMERA_POSITION := Vector3(50000.0, 64.0, -40000.0)


func _make_fixture() -> Dictionary:
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(
			ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT)), OK)

	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_sun_3di(MODEL_NAME)
	env_data.set_moon_3di("")
	env_data.set_glare_3di(MODEL_NAME)
	env_data.set_star_3di(MODEL_NAME)

	var env := NovaEnvironment.new()
	env.name = "CelestialTestEnv"
	env.environment_data = env_data
	add_child_autofree(env)

	var camera := Camera3D.new()
	camera.name = "CelestialTestCamera"
	camera.position = FAR_CAMERA_POSITION
	add_child_autofree(camera)
	camera.make_current()

	var celestial := NovaCelestial.new()
	celestial.name = "CelestialUnderTest"
	celestial.environment_path = NodePath("../CelestialTestEnv")
	add_child_autofree(celestial)
	celestial.set_resource_root(resource_root)
	simulate(celestial, 1, TICK)
	return {
		"celestial": celestial,
		"camera": camera,
		"environment": env,
	}


func test_body_updates_reach_installed_surface_materials() -> void:
	var fixture := _make_fixture()
	var camera: Camera3D = fixture.camera
	var sun := fixture.celestial.get_node_or_null("Celestial_sun") as Node3D
	assert_not_null(sun, "the committed 3DI loads through the public resource-root seam")
	if sun == null:
		return

	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(sun, meshes)
	assert_gt(meshes.size(), 0)
	for mesh in meshes:
		assert_null(mesh.material_override,
				"the NovaObjectModel whole-mesh material cannot mask celestial surfaces")
		assert_true(mesh.ignore_occlusion_culling,
				"reflection relocation must survive main-view occlusion culling")
		assert_true(mesh.extra_cull_margin >= 1.0e5,
				"reflection relocation must survive source-transform frustum culling")
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
		for surface in mesh.mesh.get_surface_count():
			var material := mesh.get_surface_override_material(surface) as ShaderMaterial
			assert_not_null(material, "each live surface owns a celestial material")
			if material == null:
				continue
			assert_eq(material.get_shader_parameter("u_anchor_camera_world"),
					camera.global_position,
					"TOD/pass-camera state updates the installed material, not a template")


func test_additive_source_material_keeps_black_as_transparent_zero() -> void:
	var source_shader := Shader.new()
	source_shader.code = "shader_type spatial; render_mode blend_add;"
	var source_material := ShaderMaterial.new()
	source_material.shader = source_shader
	assert_true(NovaCelestial.source_material_uses_additive(source_material),
			"FF_ST_AD-style sun/moon textures must keep additive blend semantics")

	source_shader = Shader.new()
	source_shader.code = "shader_type spatial; render_mode blend_mix;"
	source_material.shader = source_shader
	assert_false(NovaCelestial.source_material_uses_additive(source_material))


func test_star_instances_are_local_to_a_camera_anchored_multimesh() -> void:
	var fixture := _make_fixture()
	var camera: Camera3D = fixture.camera
	var field := fixture.celestial.get_node_or_null("StarField") as MultiMeshInstance3D
	assert_not_null(field)
	if field == null:
		return

	assert_eq(field.global_position, camera.global_position,
			"the CPU AABB follows a far-off mission camera")
	assert_true(field.ignore_occlusion_culling)
	assert_true(field.extra_cull_margin >= 1.0e5)
	assert_eq(field.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
	var material := field.material_override as ShaderMaterial
	assert_not_null(material)
	if material != null:
		assert_eq(material.get_shader_parameter("u_anchor_camera_world"),
				camera.global_position)
		assert_eq(material.get_shader_parameter("u_billboard"), true,
				"stars opt into active-pass billboarding")

	for i in field.multimesh.instance_count:
		var local_origin := field.multimesh.get_instance_transform(i).origin
		assert_true(field.custom_aabb.has_point(local_origin),
				"every instance remains inside the camera-local MultiMesh AABB")


func test_glare_keeps_occlusion_brightness_but_fades_from_each_pass_view() -> void:
	var fixture := _make_fixture()
	var glare := fixture.celestial.get_node_or_null("Celestial_glare") as Node3D
	assert_not_null(glare)
	if glare == null:
		return
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(glare, meshes)
	assert_gt(meshes.size(), 0)
	for mesh in meshes:
		for surface in mesh.mesh.get_surface_count():
			var material := mesh.get_surface_override_material(surface) as ShaderMaterial
			assert_not_null(material)
			if material != null:
				assert_eq(material.get_shader_parameter("u_glare_view_fade"), true,
						"the view-dependent dot^4 fold runs in each render pass")
				assert_eq(material.get_shader_parameter("u_glare_direction"),
						fixture.environment.get_sun_direction())


func test_celestial_shaders_anchor_and_billboard_from_the_active_pass() -> void:
	var body_code := (load(CELESTIAL_SHADER) as Shader).code
	assert_true(body_code.contains("pass_eye - u_anchor_camera_world"),
			"sun/moon placement follows the active main or mirror camera")
	assert_true(body_code.contains("POSITION = PROJECTION_MATRIX * VIEW_MATRIX"),
			"pass-relative world placement owns the final clip position")

	var additive_code := (load(ADDITIVE_SHADER) as Shader).code
	assert_true(additive_code.contains(
			"pass_right = normalize(INV_VIEW_MATRIX[0].xyz)"),
			"star right basis comes from the active pass camera")
	assert_true(additive_code.contains(
			"pass_up = normalize(INV_VIEW_MATRIX[1].xyz)"),
			"star up basis comes from the active pass camera")
	assert_true(additive_code.contains("view_dot_sq * view_dot_sq"),
			"the recovered positive dot^4 glare factor runs per pass")


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)
