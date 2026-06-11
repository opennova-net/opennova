extends GutTest

# C7 driver-contract pins for the recovered two-pass dome spec
# [orig: render_skybox @ 0x579080]. The per-fragment combine itself is shader
# code (verified by visual A/B); these pin what NovaSky pushes into it.

const FULL_00_ENV_FIXTURE := "res://../fixtures/env/full_00.env"
const SKY_SHADER := "res://shaders/sky.gdshader"


func _make() -> Dictionary:
	var env_node: Node = NovaEnvironment.new()
	add_child_autofree(env_node)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	env.load()
	env_node.environment_data = env
	var sky: Node3D = NovaSky.new()
	add_child_autofree(sky)
	sky._cached_env = env_node
	return {"sky": sky, "env_node": env_node, "env": env}


func test_keyframed_path_pushes_spec_uniforms() -> void:
	var ctx := _make()
	ctx.sky._process(0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	assert_eq(mat.get_shader_parameter("u_flat_pass"), false, "advanced clouds take the keyframed path")
	assert_eq(mat.get_shader_parameter("u_sky_base"), ctx.env_node.get_sky_base(), "c11 skybase")
	assert_eq(mat.get_shader_parameter("u_cloud_base"), ctx.env_node.get_cloud_base(), "c24 cloudbase")
	assert_eq(mat.get_shader_parameter("u_cloud_highlight"), ctx.env_node.get_cloud_highlight(), "c27 cloudhighlight")
	assert_eq(mat.get_shader_parameter("u_cloud_edge"), ctx.env_node.get_cloud_edge(), "c26 cloudedge")
	assert_eq(mat.get_shader_parameter("u_sun_dir"), ctx.env_node.get_sun_direction(),
		"pass 1 is always sun-driven [orig: render_skybox @ 0x579287]")
	assert_eq(mat.get_shader_parameter("u_light_dir"), ctx.env_node.get_light_direction(),
		"pass 2 follows the active light [orig: render_skybox @ 0x579291]")
	assert_eq(mat.get_shader_parameter("u_fog_color"), ctx.env_node.get_fog_color(),
		"the dome fogs with the shared scene fog color [orig: CD3DDevice_SetActiveFogColor @ 0x677040]")
	assert_eq(float(mat.get_shader_parameter("u_fog_end")), ctx.env_node.get_fog_level(), "dome fog end distance")


func test_flat_pass_does_not_stuff_keyframed_uniforms() -> void:
	var ctx := _make()
	ctx.sky._process(0.016)
	var keyframed_base: Vector3 = ctx.sky.sky_material.get_shader_parameter("u_sky_base")
	ctx.env.set_advanced_clouds(0)
	ctx.sky._process(0.016)
	var mat: ShaderMaterial = ctx.sky.sky_material
	assert_eq(mat.get_shader_parameter("u_flat_pass"), true, "advanced_clouds 0 takes the flat pass")
	assert_eq(mat.get_shader_parameter("u_flat_color"), ctx.env_node.get_cloud_tint(),
		"the flat dome color is cloud_rgb [orig: render_skybox @ 0x579b42]")
	assert_eq(mat.get_shader_parameter("u_sky_base"), keyframed_base,
		"the flat pass no longer overwrites the keyframed uniforms")


func test_cloud_tint_uniform_is_gone_from_the_shader() -> void:
	var shader := load(SKY_SHADER) as Shader
	assert_false(shader.code.contains("u_cloud_tint"),
		"the fabricated keyframed-path cloud tint is deleted (divergence #20 fix)")
	assert_false(shader.code.contains("u_moon_dir"),
		"sun/moon glow terms are deleted - celestial bodies are NovaCelestial's job")


func test_dome_rides_at_half_camera_height() -> void:
	var ctx := _make()
	var cam := Camera3D.new()
	add_child_autofree(cam)
	cam.global_position = Vector3(10.0, 8.0, 6.0)
	ctx.sky._cached_cam = cam
	ctx.sky._process(0.016)
	assert_eq(ctx.sky.mesh_instance.global_position, Vector3(10.0, 4.0, 6.0),
		"dome anchor = camera xz at HALF the camera height [orig: render_skybox @ 0x5790d0]")
