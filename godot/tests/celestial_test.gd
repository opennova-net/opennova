extends GutTest

# Celestial pins. The committed synthetic 3DIs stand in for the retail body
# models: crate.3di as a plain body and crate_mtrl0_ad_lum_upl113.3di as a
# stock-style sky body (an FF_ST_AD_LUM surface whose RGB generator style 113
# reads CTRL UPL_INTENSITY black to white, the msun/fmoon4/mglare authoring;
# crate_mtrl0_ab_lum_upl113.3di is its FF_ST_AB_LUM twin),
# letting the test exercise the public environment/resource-root lifecycle
# without requiring an external JO install.

const MODEL_FIXTURE_ROOT := "res://../fixtures/threedi/synth"
const MODEL_NAME := "crate.3di"
const SKY_BODY_NAME := "crate_mtrl0_ad_lum_upl113.3di"
const AB_SKY_BODY_NAME := "crate_mtrl0_ab_lum_upl113.3di"
const TICK := 1.0 / 62.0
const FAR_CAMERA_POSITION := Vector3(50000.0, 64.0, -40000.0)


func _make_fixture(moon_name: String = "", body_name: String = MODEL_NAME,
		fog_level: float = -1.0) -> Dictionary:
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(
			ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT)), OK)

	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_sun_3di(body_name)
	env_data.set_moon_3di(moon_name)
	env_data.set_glare_3di(body_name)
	env_data.set_star_3di(MODEL_NAME)
	if fog_level > 0.0:
		env_data.set_fog_level(fog_level)

	var env := MissionEnvironment.new()
	env.name = "CelestialTestEnv"
	env.environment_data = env_data
	add_child_autofree(env)

	var camera := Camera3D.new()
	camera.name = "CelestialTestCamera"
	camera.position = FAR_CAMERA_POSITION
	add_child_autofree(camera)
	camera.make_current()

	var celestial := Celestial.new()
	celestial.name = "CelestialUnderTest"
	celestial.environment_path = NodePath("../CelestialTestEnv")
	add_child_autofree(celestial)
	celestial.set_resource_root(resource_root)
	celestial.advance_frame(TICK)
	return {
		"celestial": celestial,
		"camera": camera,
		"environment": env,
	}


func _body_materials(body: Node) -> Array[ShaderMaterial]:
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(body, meshes)
	var out: Array[ShaderMaterial] = []
	for mesh in meshes:
		var material := mesh.material_override as ShaderMaterial
		if mesh.visible and material != null:
			out.append(material)
	return out


func test_bodies_draw_their_authored_material_through_the_sky_hook() -> void:
	# Retail renders every body through its own material (render_celestial_bodies
	# @ 0x5acaa0 submits the model; celestial_frame.h): the ObjectModel's
	# surface materials stay, taking only the sky placement hook.
	var fixture := _make_fixture("", SKY_BODY_NAME)
	var camera: Camera3D = fixture.camera
	var sun := fixture.celestial.get_node_or_null("Celestial_sun") as ObjectModel
	assert_not_null(sun, "the committed 3DI loads through the public resource-root seam")
	if sun == null:
		return
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(sun, meshes)
	assert_gt(meshes.size(), 0)
	for mesh in meshes:
		assert_true(mesh.ignore_occlusion_culling,
				"reflection relocation must survive main-view occlusion culling")
		assert_true(mesh.extra_cull_margin >= 1.0e5,
				"reflection relocation must survive source-transform frustum culling")
		assert_eq(mesh.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
	var materials := _body_materials(sun)
	assert_gt(materials.size(), 0, "the sun keeps its authored surface materials")
	for material in materials:
		assert_true(material.shader.resource_path.begins_with("res://shaders/object/self_lit/"),
				"FF_ST_AD_LUM renders through the SELFLUM wrapper: %s" % material.shader.resource_path)
		assert_eq(material.get_shader_parameter("u_sky_body"), true)
		assert_eq(material.get_shader_parameter("u_sky_far_pin"), true,
				"the discs ride the sky bracket before the world")
		assert_eq(material.get_shader_parameter("u_sky_anchor_camera"),
				camera.global_position, "the per-pass anchor is the main camera")
		assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_SKY_BODY)
	var glare := fixture.celestial.get_node_or_null("Celestial_glare") as ObjectModel
	assert_not_null(glare)
	if glare == null:
		return
	for material in _body_materials(glare):
		assert_eq(material.get_shader_parameter("u_sky_far_pin"), false,
				"the glow draws at its 64 u anchor after the world")
		assert_eq(material.get_shader_parameter("u_sky_mirror_drawn"), false,
				"the mirror's base pass never submits the glow")
		assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_ALPHA_CAMERA_SIDE,
				"the overlay stage draws the glow; its mesh keeps the default rung")


func test_a_rebuilt_body_scene_rebinds_the_sky_hook() -> void:
	# A model scene rebuild mints fresh surface materials; the next frame must
	# bind them to the sky hook again, or the disc would draw as a plain
	# object 64 u ahead of the eye, over the world.
	var fixture := _make_fixture("", SKY_BODY_NAME)
	var celestial: Celestial = fixture.celestial
	var sun := celestial.get_node_or_null("Celestial_sun") as ObjectModel
	assert_not_null(sun)
	if sun == null:
		return
	var serial_before := sun.get_scene_build_serial()
	sun.rebuild()
	assert_ne(sun.get_scene_build_serial(), serial_before, "the rebuild minted a new scene")
	celestial.advance_frame(TICK)
	var materials := _body_materials(sun)
	assert_gt(materials.size(), 0)
	for material in materials:
		assert_eq(material.get_shader_parameter("u_sky_body"), true)
		assert_eq(material.get_shader_parameter("u_sky_far_pin"), true)
		assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_SKY_BODY)
	var body: Dictionary = (celestial.get_diagnostics().get("bodies", {})
			as Dictionary).get("sun", {})
	assert_eq(int(body.get("sky_hooked_surfaces", -1)), int(body.get("surfaces", -2)),
			"every live sun surface carries the sky hook")


func test_upl_intensity_drives_the_authored_self_lum() -> void:
	# The submit alpha lands in CTRL register 32 (UPL_INTENSITY), which the
	# material's RgbGen style 113 reads into SelfLumColor. Without a moon model
	# the sun keeps its own alpha (1.0 at no overcast / SunDim).
	var fixture := _make_fixture("", SKY_BODY_NAME)
	var sun := fixture.celestial.get_node_or_null("Celestial_sun") as ObjectModel
	assert_not_null(sun)
	if sun == null:
		return
	assert_eq(int(sun.get_ctrl_values().get("UPL_INTENSITY", -1)), 0x10000)
	for material in _body_materials(sun):
		assert_almost_eq(Vector3(material.get_shader_parameter("u_rgb_mod")),
				Vector3.ONE, Vector3(0.01, 0.01, 0.01), "full sun alpha = white SelfLumColor")


func test_each_disc_keeps_its_own_alpha_through_the_shared_flush() -> void:
	# render_celestial_bodies writes the sun alpha, submits the sun, writes the
	# moon alpha, submits the moon and flushes ONCE [orig: @ 0x5acbfa,
	# @ 0x5acc1c, @ 0x5accc1, @ 0x5accdd, @ 0x5acce9], but each submit
	# snapshots its material's registers and the flush restores them before
	# that batch's RgbGen [orig: collect_render_objects_for_batch
	# @ 0x5d91c0..0x5d91de; CRenderBatchQueue_FlushBatches
	# @ 0x5da1d6..0x5da1fd]: the sun stays at its own 1.0 beside a moon at
	# (700 - 400) / 600 = 0.5.
	var fixture := _make_fixture(SKY_BODY_NAME, SKY_BODY_NAME, 700.0)
	var celestial: Celestial = fixture.celestial
	var sun := celestial.get_node_or_null("Celestial_sun") as ObjectModel
	var moon := celestial.get_node_or_null("Celestial_moon") as ObjectModel
	assert_not_null(sun)
	assert_not_null(moon)
	if sun == null or moon == null:
		return
	assert_eq(int(sun.get_ctrl_values().get("UPL_INTENSITY", -1)), 0x10000,
			"the sun keeps its own alpha beside a moon")
	assert_eq(int(moon.get_ctrl_values().get("UPL_INTENSITY", -1)), 0x8000,
			"the moon takes (700 - 400) / 600")
	for material in _body_materials(sun):
		assert_almost_eq(Vector3(material.get_shader_parameter("u_rgb_mod")),
				Vector3.ONE, Vector3(0.01, 0.01, 0.01), "RgbGen 113 at 1.0: white")
	for material in _body_materials(moon):
		assert_almost_eq(Vector3(material.get_shader_parameter("u_rgb_mod")),
				Vector3.ONE * (127.0 / 255.0), Vector3(0.01, 0.01, 0.01),
				"RgbGen 113: 0 + (255 * 0x8000) >> 16 = 127")
	# The bloom pass's redraw: the sun has no fog-shader leg, the moon takes
	# 700 x 0.0002.
	var bodies: Dictionary = celestial.get_diagnostics().get("bodies", {})
	var expected_q3 := int(700.0 * 0.0002 * 65536.0)
	assert_almost_eq(int((bodies.get("moon", {}) as Dictionary).get("q3_upl", -1)),
			expected_q3, 1, "the bloom redraw's moon leg")
	assert_eq(int((bodies.get("sun", {}) as Dictionary).get("q3_upl", -1)), 0x10000,
			"the bloom redraw's sun keeps its own alpha")


func test_sky_pass_gates_reach_the_disc_materials() -> void:
	var fixture := _make_fixture(SKY_BODY_NAME, SKY_BODY_NAME)
	var celestial: Celestial = fixture.celestial
	celestial.set_sky_pass_gates(false, true)
	for key in ["Celestial_sun", "Celestial_moon"]:
		var body := celestial.get_node_or_null(key) as ObjectModel
		assert_not_null(body)
		if body == null:
			continue
		for material in _body_materials(body):
			assert_eq(material.get_shader_parameter("u_sky_beauty_drawn"), false)
			assert_eq(material.get_shader_parameter("u_sky_mirror_drawn"), true)
	var glare := celestial.get_node_or_null("Celestial_glare") as ObjectModel
	for material in _body_materials(glare):
		assert_eq(material.get_shader_parameter("u_sky_mirror_drawn"), false,
				"the sky bracket gates never reach the glow")


func test_additive_source_material_keeps_black_as_transparent_zero() -> void:
	# The blend is the material's typed classification from its ObjectModel
	# creation seam: the mount fixture's FF_ST_AD_LUM heat slab is additive,
	# its FF_ST_OP body is not. Shader text is never sniffed.
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			MODEL_FIXTURE_ROOT + "/mount.3di")), OK)
	model.set_object_data(data)
	var additive_seen := false
	var opaque_seen := false
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material == null or material.shader == null:
			continue
		var path := material.shader.resource_path
		if "/additive" in path:
			additive_seen = true
			assert_true(Celestial.source_material_uses_additive(material),
					"FF_ST_AD_LUM keeps additive blend semantics: %s" % path)
		elif path.ends_with("/opaque.gdshader"):
			opaque_seen = true
			assert_false(Celestial.source_material_uses_additive(material),
					"FF_ST_OP is not additive: %s" % path)
	assert_true(additive_seen, "the mount fixture carries an additive surface")
	assert_true(opaque_seen, "the mount fixture carries an opaque surface")

	var unclassified_shader := Shader.new()
	unclassified_shader.code = "shader_type spatial; render_mode blend_add;"
	var unclassified := ShaderMaterial.new()
	unclassified.shader = unclassified_shader
	assert_false(Celestial.source_material_uses_additive(unclassified),
			"a material outside the classification registry is never additive, "
			+ "whatever its shader text says")


func test_no_star_field_is_drawn() -> void:
	# Retail loads the star 3DI but its only renderer has no caller in the
	# image [orig: Star_RenderField_unused @ 0x5ad9c0]: a named star_3di draws nothing.
	var fixture := _make_fixture()
	for child in fixture.celestial.get_children():
		assert_false(child is MultiMeshInstance3D, "no star instances: %s" % child.name)
	assert_null(fixture.celestial.get_node_or_null("StarField"))


func test_glare_submits_only_a_positive_alpha() -> void:
	# The glow's beauty submit is skipped at a non-positive alpha
	# [orig: render_skybox_sun_glow @ 0x5ad0ae]; the view dot is the MAIN
	# camera's, folded on the CPU.
	var fixture := _make_fixture("", SKY_BODY_NAME)
	var celestial: Celestial = fixture.celestial
	var camera: Camera3D = fixture.camera
	var env: MissionEnvironment = fixture.environment
	celestial.settle_glare_occlusion()
	var sun_dir: Vector3 = env.get_sun_direction()
	var up := Vector3.RIGHT if absf(sun_dir.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position - sun_dir, up)
	celestial.advance_frame(TICK)
	var glare := celestial.get_node_or_null("Celestial_glare") as ObjectModel
	assert_not_null(glare)
	if glare == null:
		return
	var away: Dictionary = (celestial.get_diagnostics().get("bodies", {}) as Dictionary).get("glare", {})
	assert_eq(int(away.get("upl", -1)), 0)
	assert_false(bool(away.get("drawn", true)), "looking away submits no glow")
	for material in _body_materials(glare):
		assert_eq(material.get_shader_parameter("u_sky_beauty_drawn"), false)

	camera.look_at(camera.global_position + sun_dir, up)
	celestial.advance_frame(TICK)
	var facing: Dictionary = (celestial.get_diagnostics().get("bodies", {}) as Dictionary).get("glare", {})
	assert_gt(int(facing.get("upl", 0)), 0)
	assert_true(bool(facing.get("drawn", false)))
	assert_true(glare.visible)
	assert_eq(int(glare.get_ctrl_values().get("UPL_INTENSITY", -1)),
			int(facing.get("upl", 0)), "the glow's alpha is its UPL_INTENSITY")
	for material in _body_materials(glare):
		assert_eq(material.get_shader_parameter("u_sky_beauty_drawn"), true)
	assert_eq(celestial.get_overlay_body_node("glare"), glare,
			"the overlay seam hands the glow node to the post-particle stage")


func test_settle_glare_occlusion_reaches_the_dead_band_hold() -> void:
	# The capture-refresh seam (the D-RLIT-2 fixture starvation): the glare
	# brightness steps +-16 per frame toward popcount * 32 * fog/1000 with a
	# +-16 dead-band hold [orig: render_skybox_sun_glow @ 0x5acdfb..0x5acf7f],
	# so one zero-delta advance leaves a fresh accumulator dark. With no
	# terrain loaded both jittered rays are clear every frame; the settle must
	# fill the window (0xFF) and hold inside the dead-band around the
	# fog-scaled target.
	var fixture := _make_fixture()
	var celestial: Celestial = fixture.celestial
	var env: MissionEnvironment = fixture.environment
	var camera: Camera3D = fixture.camera
	var before: Dictionary = celestial.get_diagnostics()
	var glare_before: Dictionary = before.get("glare_occlusion", {})
	assert_lte(int(glare_before.get("brightness", 0)), 16,
			"a single advance cannot lift a fresh accumulator past one step")

	var settled: int = celestial.settle_glare_occlusion()
	# target = popcount(0xFF) * 32 * fog * 65536 * (1/65536000), ftol-truncated
	# [orig: @ 0x5acf30..0x5acf58].
	var target := int(256.0 * env.get_fog_level() * 65536.0 * 1.525878978725359e-08)
	assert_gt(settled, target - 17,
			"the settled brightness rises into the dead-band below the target")
	assert_lte(settled, target + 16,
			"the dead-band hold never overshoots past +16")

	# Face the sun so the settled brightness publishes a positive glow alpha.
	var sun_dir: Vector3 = env.get_sun_direction()
	var up := Vector3.RIGHT if absf(sun_dir.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + sun_dir, up)
	celestial.advance_frame(TICK)
	var diag: Dictionary = celestial.get_diagnostics()
	var glare: Dictionary = diag.get("glare_occlusion", {})
	assert_eq(int(glare.get("window", 0)), 0xFF,
			"clear rays fill the whole 8-sample window")
	assert_eq(int(glare.get("brightness", -1)), settled,
			"a settled accumulator HOLDS through the publishing frame")
	var bodies: Dictionary = diag.get("bodies", {})
	assert_true(bodies.has("glare"), "diagnostics list the glare body")
	var glare_body: Dictionary = bodies.get("glare", {})
	assert_gt(int(glare_body.get("upl", 0)), 0,
			"the settled brightness publishes a visible glare alpha")
	assert_true(bool(glare_body.get("visible", false)),
			"the glare model is shown once its alpha is non-zero")


func test_sun_veil_publishes_the_dot32_alpha_global_when_facing_the_sun() -> void:
	# The sun-glare screen veil [orig:
	# Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0]: with the glare
	# occlusion settled and the camera facing the sun, the dot^32 chain must
	# publish a non-zero white-veil alpha through the opennova_sun_veil_alpha
	# shader global and expose the modulator-2 stop-down for the world's veil
	# leg. Facing away serves zero (the dot <= 0 gate).
	var fixture := _make_fixture()
	var celestial: Celestial = fixture.celestial
	var camera: Camera3D = fixture.camera
	var env: MissionEnvironment = fixture.environment
	celestial.settle_glare_occlusion()

	# The getter serves the GODOT-world sun (the util/axes.h swap applies at
	# the MissionEnvironment boundary).
	var sun_dir: Vector3 = env.get_sun_direction()
	# A near-vertical sun is colinear with the default look_at up vector.
	var up := Vector3.RIGHT if absf(sun_dir.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + sun_dir, up)
	celestial.advance_frame(TICK)
	var facing_alpha: float = celestial.get_sun_veil_alpha()
	assert_gt(facing_alpha, 0.0,
			"a settled accumulator and a sun-facing view raise the veil")
	# (The opennova_sun_veil_alpha shader-global push cannot be read back
	# under the headless dummy RenderingServer; the typed getter is the
	# testable seam and the push shares its value.)
	var diag: Dictionary = celestial.get_diagnostics()
	var veil: Dictionary = diag.get("sun_veil", {})
	assert_eq(float(veil.get("alpha", -1.0)), facing_alpha)
	assert_gte(int(veil.get("stopdown", -1)), 0)

	camera.look_at(camera.global_position - sun_dir, up)
	celestial.advance_frame(TICK)
	assert_eq(celestial.get_sun_veil_alpha(), 0.0,
			"looking away from the sun clears the veil")


func test_water_glint_settles_and_mirrors_below_the_eye() -> void:
	# The water-reflected sun glint [orig: update_sun_glare @ 0x5ad130]: with
	# a water height authored and no terrain (every visibility ray clear),
	# the settle must chase the glint accumulator to the full 4 * 64 and
	# place the glare 3DI mirrored BELOW the eye (camera + sun * 128 with the
	# height term negated). Without water the glint body stays hidden.
	var fixture := _make_fixture()
	var celestial: Celestial = fixture.celestial
	var camera: Camera3D = fixture.camera
	var env: MissionEnvironment = fixture.environment
	var glint := celestial.get_node_or_null("Celestial_glint") as Node3D
	assert_not_null(glint, "the glint body loads beside the glare body")
	if glint == null:
		return
	celestial.advance_frame(TICK)
	assert_false(glint.visible, "no water height -> no glint")

	# Author a water plane well below the camera and look along the sun so
	# the mirrored view dot exceeds the witnessed dot^4 threshold.
	fixture.environment.environment_data.set_water_height(-200.0)
	var sun_dir: Vector3 = env.get_sun_direction()
	var mirrored := Vector3(sun_dir.x, -sun_dir.y, sun_dir.z)
	var up := Vector3.RIGHT if absf(mirrored.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + mirrored, up)
	celestial.settle_glare_occlusion()
	celestial.advance_frame(TICK)

	var diag: Dictionary = celestial.get_diagnostics()
	var glint_state: Dictionary = diag.get("water_glint", {})
	assert_eq(int(glint_state.get("brightness", 0)), 256,
			"clear rays settle the glint chase onto popcount * 64")
	assert_eq(int(glint_state.get("window", 0)), 0xF)
	assert_true(glint.visible,
			"a settled glint (brightness != 0) submits the mirrored body")
	var expected := camera.global_position + mirrored * 128.0
	assert_true(glint.global_position.is_equal_approx(expected),
			"the glint places at camera + sun * 128 with the height negated")
	var bodies: Dictionary = diag.get("bodies", {})
	assert_gt(int((bodies.get("glint", {}) as Dictionary).get("upl", 0)), 0)


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)


func _q3_glare_peak(glare_name: String, occluder_distance: float = -1.0) -> Dictionary:
	# A sunless sky with only the named glare model, in its own world with a
	# FrameFx: the bloom-pass glow is occlusion-free, so facing the sun its
	# Q3 redraw carries a positive UPL_INTENSITY. Returns the Q3 target's peak
	# around the glow, the glow's Q3 submit value and the backend report.
	# The camera far plane is the 700 u fog's scene far (the game sets it
	# from the same fog word); occluder_distance > 0 puts an opaque wall that
	# far out, square to the view, behind the glow.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(192, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	# FrameFx installs its terminal effect on the world's WorldEnvironment.
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment_resource
	viewport.add_child(world_environment)

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(
			ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT)), OK)
	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_sun_3di("")
	env_data.set_moon_3di("")
	env_data.set_glare_3di(glare_name)
	var env := MissionEnvironment.new()
	env.name = "CelestialQ3Env"
	env.environment_data = env_data
	viewport.add_child(env)

	var camera := Camera3D.new()
	camera.position = FAR_CAMERA_POSITION
	camera.far = 701.0
	camera.current = true
	viewport.add_child(camera)

	var celestial := Celestial.new()
	celestial.environment_path = NodePath("../CelestialQ3Env")
	viewport.add_child(celestial)
	celestial.set_resource_root(resource_root)
	var sun_dir: Vector3 = env.get_sun_direction()
	var up := Vector3.RIGHT if absf(sun_dir.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + sun_dir, up)
	if occluder_distance > 0.0:
		var wall := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(4000.0, 4000.0)
		wall.mesh = quad
		var wall_material := StandardMaterial3D.new()
		wall_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		wall_material.albedo_color = Color.BLACK
		wall_material.cull_mode = BaseMaterial3D.CULL_DISABLED
		wall.material_override = wall_material
		viewport.add_child(wall)
		wall.global_transform = camera.global_transform.translated_local(
				Vector3(0.0, 0.0, -occluder_distance))
	celestial.advance_frame(TICK)
	var glare := celestial.get_node_or_null("Celestial_glare") as ObjectModel
	assert_not_null(glare, "the glare model loads without a sun model")
	if glare == null:
		return {}
	# The synthetic crate names no texture the fixture root carries: give the
	# glow's authored material a white Diffuse1 so its SelfLumColor shows.
	var white := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	white.set_pixel(0, 0, Color.WHITE)
	var white_texture := ImageTexture.create_from_image(white)
	for material in _body_materials(glare):
		material.set_shader_parameter("u_diffuse", white_texture)
		FrameFx.invalidate_q3_object_material(material)
	celestial.advance_frame(TICK)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var image: Image = renderer.get_q3_target_image()
	var glare_state: Dictionary = (celestial.get_diagnostics().get("bodies", {})
			as Dictionary).get("glare", {})
	var center := Vector2i(camera.unproject_position(glare.global_position))
	var peak := 0.0
	if image != null:
		for y in range(center.y - 6, center.y + 7):
			for x in range(center.x - 6, center.x + 7):
				if x < 0 or y < 0 or x >= image.get_width() or y >= image.get_height():
					continue
				var pixel := image.get_pixel(x, y)
				peak = maxf(peak, pixel.r + pixel.g + pixel.b)
	var report := renderer.get_backend_report()
	renderer.shutdown()
	return {"peak": peak, "q3_upl": int(glare_state.get("q3_upl", 0)),
			"report": report, "image": image}


func test_q3_glow_blends_as_its_material_is_classified() -> void:
	# The glow's submit flags (0x100 in the bloom pass, 0x110 in the beauty
	# pass) never override the material blend [orig: render_skybox_sun_glow
	# @ 0x5ad0f5..0x5ad0fe]: the bloom redraw of an FF_ST_AD_LUM glow adds its
	# SelfLumColor, while an FF_ST_AB_LUM glow's SELFLUM alpha 0 leaves the
	# Q3 target untouched under SRCALPHA / INVSRCALPHA.
	var additive: Dictionary = await _q3_glare_peak(SKY_BODY_NAME)
	if additive.is_empty():
		return
	if not bool((additive.report as Dictionary).get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(additive.q3_upl), 0, "facing the sun the bloom glow submits")
	assert_not_null(additive.image, "the terminal effect exposes its Q3 target")
	assert_gt(float(additive.peak), 0.1,
			"the additive glow adds its SelfLumColor to the Q3 target: %s" % additive.report)
	var alpha_blend: Dictionary = await _q3_glare_peak(AB_SKY_BODY_NAME)
	if alpha_blend.is_empty():
		return
	assert_gt(int(alpha_blend.q3_upl), 0, "the AlphaBlend glow submits the same alpha")
	assert_gt(int((alpha_blend.report as Dictionary).get("q3_drawn_commands", 0)), 0,
			"the AlphaBlend glow reaches the Q3 draw list: %s" % alpha_blend.report)
	assert_lt(float(alpha_blend.peak), 3.0 / 255.0,
			"an AlphaBlend glow paints nothing into the Q3 target")


func test_q3_glow_far_band_hides_behind_far_terrain() -> void:
	# The bloom pass draws the glow through the far band (MinZ 0.98 / MaxZ
	# 0.99996948) against beauty depth written through the scene viewport's
	# MaxZ 0.99996948 [orig: Render_SetViewportFarDepth @ 0x58a840;
	# Render_SetViewport @ 0x58a720]: at a 701 u far plane the 64 u glow
	# survives only over beauty depth past ~585 u. A wall 500 u out hides it;
	# a wall 650 u out does not.
	var near_wall: Dictionary = await _q3_glare_peak(SKY_BODY_NAME, 500.0)
	if near_wall.is_empty():
		return
	if not bool((near_wall.report as Dictionary).get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(int(near_wall.q3_upl), 0, "facing the sun the bloom glow submits")
	assert_lt(float(near_wall.peak), 3.0 / 255.0,
			"terrain 500 u out hides the far-band glow: %s" % near_wall.report)
	var far_wall: Dictionary = await _q3_glare_peak(SKY_BODY_NAME, 650.0)
	if far_wall.is_empty():
		return
	assert_gt(float(far_wall.peak), 0.1,
			"terrain 650 u out lies inside the band, the glow draws over it")
