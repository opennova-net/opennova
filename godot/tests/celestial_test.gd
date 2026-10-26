extends GutTest

# Celestial pins. The committed synthetic 3DIs stand in for the retail body
# models: crate.3di as a plain body and crate_mtrl0_ad_lum_upl113.3di as a
# stock-style sky body (an FF_ST_AD_LUM surface whose RGB generator style 113
# reads CTRL UPL_INTENSITY black to white, the msun/fmoon4/mglare authoring),
# letting the test exercise the public environment/resource-root lifecycle
# without requiring an external JO install.

const MODEL_FIXTURE_ROOT := "res://../fixtures/threedi/synth"
const MODEL_NAME := "crate.3di"
const SKY_BODY_NAME := "crate_mtrl0_ad_lum_upl113.3di"
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
	# Retail renders every body through its own material (Render_CelestialBodies
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


func test_view_aligned_bodies_turn_with_the_camera() -> void:
	# Every stock body's parts pose view-aligned (PANM rotation type 3): retail
	# copies the pass camera's inverse view rotation into each part, so the
	# quads face the camera at any view (celestial_frame.h). The body turns
	# with the camera: render +X/+Y/+Z (imported as -X/+Y/+Z) land on the
	# camera's right/up/forward. A model with a static part keeps the identity
	# render rotation, the +90 degree yaw, whatever the view.
	var identity_render := Basis(Vector3.UP, PI * 0.5)
	var eps := Vector3.ONE * 1.0e-4
	var fixture := _make_fixture("", "panm_live_02_view3.3di")
	var celestial: Celestial = fixture.celestial
	var camera: Camera3D = fixture.camera
	var sun := celestial.get_node_or_null("Celestial_sun") as ObjectModel
	assert_not_null(sun, "the one-part view-aligned shed loads as the sun")
	if sun == null:
		return
	camera.look_at(camera.global_position + Vector3.RIGHT, Vector3.UP)
	celestial.advance_frame(TICK)
	assert_almost_eq(sun.global_basis.x, identity_render.x, eps,
			"a level camera looking east is the build's reference turn")
	assert_almost_eq(sun.global_basis.z, identity_render.z, eps)
	camera.look_at(camera.global_position + Vector3(-0.6, 0.5, -0.62), Vector3.UP)
	celestial.advance_frame(TICK)
	var body := sun.global_basis
	var view := camera.global_basis
	assert_almost_eq(body * Vector3.LEFT, view.x, eps, "render +X lands on the camera's right")
	assert_almost_eq(body * Vector3.UP, view.y, eps, "render +Y lands on the camera's up")
	assert_almost_eq(body * Vector3.BACK, -view.z, eps, "render +Z looks along the view")


func test_a_static_part_body_keeps_the_identity_render_rotation() -> void:
	# The crate's one PANM row is inert (rotation type 0): no camera turn.
	var identity_render := Basis(Vector3.UP, PI * 0.5)
	var eps := Vector3.ONE * 1.0e-4
	var fixture := _make_fixture("", MODEL_NAME)
	var camera: Camera3D = fixture.camera
	var crate := (fixture.celestial as Celestial).get_node_or_null("Celestial_sun") as ObjectModel
	assert_not_null(crate)
	if crate == null:
		return
	camera.look_at(camera.global_position + Vector3(-0.6, 0.5, -0.62), Vector3.UP)
	(fixture.celestial as Celestial).advance_frame(TICK)
	assert_almost_eq(crate.global_basis.x, identity_render.x, eps,
			"a static-part body keeps the identity render rotation")
	assert_almost_eq(crate.global_basis.z, identity_render.z, eps)


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
	# Render_CelestialBodies writes the sun alpha, submits the sun, writes the
	# moon alpha, submits the moon and flushes ONCE [orig: @ 0x5acbfa,
	# @ 0x5acc1c, @ 0x5accc1, @ 0x5accdd, @ 0x5acce9], but each submit
	# snapshots its material's registers and the flush restores them before
	# that batch's RgbGen [orig: Render_CollectRenderObjectsForBatch
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
	# image [orig: Star_RenderField_Unused @ 0x5ad9c0]: a named star_3di draws nothing.
	var fixture := _make_fixture()
	for child in fixture.celestial.get_children():
		assert_false(child is MultiMeshInstance3D, "no star instances: %s" % child.name)
	assert_null(fixture.celestial.get_node_or_null("StarField"))


func test_the_star_model_and_upl_load_as_data_and_never_build() -> void:
	# The leg loads the star model it names and upl.3di past the glare, and no
	# frame draws either: their only reader has no caller
	# [orig: EffectWorld_LoadCelestialModels @ 0x5adc50, the calls @ 0x5adcfe /
	# @ 0x5add2a; Star_RenderField_Unused @ 0x5ad9c0]. No sun, moon or glare here,
	# so nothing builds at all.
	var fixture := _make_fixture("", "")
	var celestial: Celestial = fixture.celestial
	var diag := celestial.get_diagnostics()
	assert_true(bool(diag.get("star_loaded", false)), "the named star model loads")
	assert_false(bool(diag.get("upl_loaded", true)), "the mount holds no upl.3di")
	assert_false(bool(diag.get("built", true)))
	assert_eq(celestial.get_child_count(), 0, "neither model builds a node")

	# A mount with upl.3di: the next leg (a resource root set) loads it too.
	var dir := OS.get_cache_dir().path_join("opennova_celestial_upl_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var model := ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT).path_join(MODEL_NAME)
	for file_name in [MODEL_NAME, "upl.3di"]:
		assert_eq(DirAccess.copy_absolute(model, dir.path_join(file_name)), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	celestial.set_resource_root(root)
	diag = celestial.get_diagnostics()
	assert_true(bool(diag.get("star_loaded", false)))
	assert_true(bool(diag.get("upl_loaded", false)), "upl.3di loads whatever the .env names")
	assert_eq(celestial.get_child_count(), 0, "neither model builds a node")

	# An environment that names no star model loads none; upl.3di still loads.
	var env: MissionEnvironment = fixture.environment
	env.environment_data.set_star_3di("")
	celestial.set_resource_root(root)
	diag = celestial.get_diagnostics()
	assert_false(bool(diag.get("star_loaded", true)), "no star name, no star load")
	assert_true(bool(diag.get("upl_loaded", false)))
	TestFs.remove_dir_recursive(dir)


# Retail destroys the departing mission's model defs at the next start, ahead of
# loading them again (engine environment/celestial_frame.h kCelestialUplModel carries
# the witness): the world's unload releases the bodies and the defs, so none waits
# through the next load to be freed in its first frames.
func test_release_bodies_frees_the_bodies_and_the_model_defs() -> void:
	var fixture := _make_fixture()
	var celestial: Celestial = fixture.celestial
	var sun := celestial.get_node_or_null("Celestial_sun")
	assert_not_null(sun, "the fixture builds a sun")
	assert_true(bool(celestial.get_diagnostics().get("star_loaded", false)))
	celestial.release_bodies()
	assert_eq(celestial.get_child_count(), 0, "no body is left under the node")
	if sun != null:
		assert_true(sun.is_queued_for_deletion(), "the departing sun is freed")
	var diag := celestial.get_diagnostics()
	assert_false(bool(diag.get("built", true)))
	assert_false(bool(diag.get("star_loaded", true)), "the model defs go too")
	# The next start's leg loads and builds them again.
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT)), OK)
	celestial.set_resource_root(root)
	assert_not_null(celestial.get_node_or_null("Celestial_sun"), "the next leg builds the sun")


func test_glare_submits_only_a_positive_alpha() -> void:
	# The glow's beauty submit is skipped at a non-positive alpha
	# [orig: Render_SkyboxSunGlow @ 0x5ad0ae]; the view dot is the MAIN
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
	# +-16 dead-band hold [orig: Render_SkyboxSunGlow @ 0x5acdfb..0x5acf7f],
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


func test_sun_veil_publishes_the_dot16_alpha_global_when_facing_the_sun() -> void:
	# The sun-glare screen veil [orig:
	# Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0]: with the glare
	# occlusion settled and the camera facing the sun, the dot^16 chain must
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
	# The water-reflected sun glint [orig: Environment_UpdateSunGlare @ 0x5ad130]: with
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


func _glint_state(celestial: Celestial) -> Dictionary:
	return celestial.get_diagnostics().get("water_glint", {}) as Dictionary


# The weapon Inset pass calls the glint's leg again at its own camera, after
# the main scene's call, on the one accumulator (engine
# renderer/scene_overlay.h kInsetOverlayOrder carries the witness): the
# counter steps twice a frame while it renders, the Inset's draw sits at its
# own eye with its own view dot, and the body keeps its main placement.
func test_the_inset_pass_calls_the_glint_leg_again_at_its_camera() -> void:
	var fixture := _make_fixture()
	var celestial: Celestial = fixture.celestial
	var camera: Camera3D = fixture.camera
	var env: MissionEnvironment = fixture.environment
	var glint := celestial.get_node_or_null("Celestial_glint") as Node3D
	assert_not_null(glint)
	if glint == null:
		return
	env.environment_data.set_water_height(-200.0)
	var sun_dir: Vector3 = env.get_sun_direction()
	var mirrored := Vector3(sun_dir.x, -sun_dir.y, sun_dir.z)
	var up := Vector3.RIGHT if absf(mirrored.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + mirrored, up)
	celestial.settle_glare_occlusion()
	celestial.advance_frame(TICK)
	var before := int(_glint_state(celestial).get("frame_index", 0))
	celestial.advance_frame(TICK)
	var state := _glint_state(celestial)
	assert_eq(int(state.get("frame_index", 0)) - before, 1, "one scene pass, one call")
	assert_false(bool(state.get("inset_drawn", true)), "no Inset pass, no Inset draw")
	# The Inset pass renders from an eye beside the main one, facing away from
	# the mirrored sun.
	var inset := Camera3D.new()
	add_child_autofree(inset)
	inset.global_position = camera.global_position + Vector3(3.0, 0.0, 0.0)
	inset.look_at(inset.global_position - mirrored, up)
	celestial.set_inset_view(inset)
	before = int(state.get("frame_index", 0))
	celestial.advance_frame(TICK)
	state = _glint_state(celestial)
	assert_eq(int(state.get("frame_index", 0)) - before, 2,
			"both passes call the leg on the one accumulator")
	assert_eq(int(state.get("brightness", 0)), 256, "clear rays hold the settled brightness")
	assert_true(bool(state.get("inset_drawn", false)), "the brightness draws the Inset's glint")
	assert_true((state.get("inset_eye", Vector3.ZERO) as Vector3).is_equal_approx(
			inset.global_position), "from the Inset's own eye")
	assert_eq(int(state.get("inset_upl", -1)), 0,
			"facing away, the Inset's view dot submits nothing")
	var bodies: Dictionary = celestial.get_diagnostics().get("bodies", {})
	assert_gt(int((bodies.get("glint", {}) as Dictionary).get("upl", 0)), 0,
			"while the main view, facing it, still does")
	assert_true(glint.global_position.is_equal_approx(camera.global_position + mirrored * 128.0),
			"the body keeps the main pass's placement")
	# The capture settle steps the calls the live frame makes, per frame.
	before = int(_glint_state(celestial).get("frame_index", 0))
	celestial.settle_glare_occlusion(1)
	assert_eq(int(_glint_state(celestial).get("frame_index", 0)) - before, 2,
			"one settled frame steps both passes' calls")
	celestial.set_inset_view(null)
	celestial.advance_frame(TICK)
	assert_false(bool(_glint_state(celestial).get("inset_drawn", true)),
			"the pass stops: no Inset call")
	before = int(_glint_state(celestial).get("frame_index", 0))
	celestial.settle_glare_occlusion(1)
	assert_eq(int(_glint_state(celestial).get("frame_index", 0)) - before, 1,
			"and the settle steps the main call alone")


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)
