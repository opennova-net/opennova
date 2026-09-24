extends GutTest

# Reflection-pass fidelity pins for Celestial. The committed crate 3DI
# stands in for the retail body models, letting the test exercise the public
# environment/resource-root lifecycle without requiring an external JO install.

const MODEL_FIXTURE_ROOT := "res://../fixtures/threedi/synth"
const MODEL_NAME := "crate.3di"
const TICK := 1.0 / 62.0
const FAR_CAMERA_POSITION := Vector3(50000.0, 64.0, -40000.0)


func _make_fixture(moon_name: String = "") -> Dictionary:
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(
			ProjectSettings.globalize_path(MODEL_FIXTURE_ROOT)), OK)

	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_sun_3di(MODEL_NAME)
	env_data.set_moon_3di(moon_name)
	env_data.set_glare_3di(MODEL_NAME)
	env_data.set_star_3di(MODEL_NAME)

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
				"the ObjectModel whole-mesh material cannot mask celestial surfaces")
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
	# image [orig: render_star_field @ 0x5ad9c0]: a named star_3di draws nothing.
	var fixture := _make_fixture()
	for child in fixture.celestial.get_children():
		assert_false(child is MultiMeshInstance3D, "no star instances: %s" % child.name)
	assert_null(fixture.celestial.get_node_or_null("StarField"))


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
						fixture.environment.get_sun_direction(),
						"the glare direction and the getter both serve the "
						+ "Godot-world vector (util/axes.h swap applied once)")


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
	assert_gt(float(glare_body.get("opacity", 0.0)), 0.0,
			"the settled brightness publishes a visible glare opacity")
	assert_true(bool(glare_body.get("visible", false)),
			"the glare model is shown once its opacity is non-zero")


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
			"a settled glint with a facing view shows the mirrored body")
	var expected := camera.global_position + mirrored * 128.0
	assert_true(glint.global_position.is_equal_approx(expected),
			"the glint places at camera + sun * 128 with the height negated")
	var bodies: Dictionary = diag.get("bodies", {})
	assert_gt(float((bodies.get("glint", {}) as Dictionary).get("opacity", 0.0)), 0.0)


func test_discs_publish_the_bloom_pass_opacity_beside_the_beauty_one() -> void:
	# The bloom pass redraws the discs through render_celestial_bodies(1), the
	# fog-shader path: the moon takes fogDistInt x 0.0002 x (1 - overcast)
	# there instead of the (fogDistInt - 400)/600 ramp of the direct draw,
	# the sun has no fog-shader variant (engine celestial_frame.h). Both
	# publish that Q3 opacity as u_q3_opacity for the typed producer read.
	var fixture := _make_fixture(MODEL_NAME)
	var env: MissionEnvironment = fixture.environment
	var celestial: Celestial = fixture.celestial
	var moon := celestial.get_node_or_null("Celestial_moon") as Node3D
	assert_not_null(moon, "the moon loads through the same resource-root seam")
	if moon == null:
		return
	var moon_meshes: Array[MeshInstance3D] = []
	_collect_meshes(moon, moon_meshes)
	assert_gt(moon_meshes.size(), 0)
	var fog_int := floorf(env.get_fog_level())
	var expected_moon_q3 := clampf(fog_int * 0.0002, 0.0, 1.0) 			* (1.0 - env.get_overcast_blend())
	for mesh in moon_meshes:
		for surface in mesh.mesh.get_surface_count():
			var material := mesh.get_surface_override_material(surface) as ShaderMaterial
			assert_not_null(material)
			if material == null:
				continue
			assert_almost_eq(float(material.get_shader_parameter("u_q3_opacity")),
					expected_moon_q3, 0.001,
					"the moon's Q3 opacity is the fog-shader leg")
			assert_ne(float(material.get_shader_parameter("u_q3_opacity")),
					float(material.get_shader_parameter("u_opacity")),
					"the fog-shader leg differs from the direct-draw ramp")
	var sun := celestial.get_node_or_null("Celestial_sun") as Node3D
	assert_not_null(sun)
	if sun == null:
		return
	var sun_meshes: Array[MeshInstance3D] = []
	_collect_meshes(sun, sun_meshes)
	for mesh in sun_meshes:
		for surface in mesh.mesh.get_surface_count():
			var material := mesh.get_surface_override_material(surface) as ShaderMaterial
			if material == null:
				continue
			assert_eq(float(material.get_shader_parameter("u_q3_opacity")),
					float(material.get_shader_parameter("u_opacity")),
					"the sun has no fog-shader alpha variant")


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)
