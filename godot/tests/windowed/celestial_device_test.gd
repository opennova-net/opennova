extends GutTest

# The methods of tests/celestial_test.gd that need a RenderingDevice, split out of it so
# the windowed run selects them (G-100).

# Celestial pins. The committed synthetic 3DIs stand in for the retail body
# models: crate.3di as a plain body and crate_mtrl0_ad_lum_upl113.3di as a
# stock-style sky body (an FF_ST_AD_LUM surface whose RGB generator style 113
# reads CTRL UPL_INTENSITY black to white, the msun/fmoon4/mglare authoring;
# crate_mtrl0_ab_lum_upl113.3di is its FF_ST_AB_LUM twin),
# letting the test exercise the public environment/resource-root lifecycle
# without requiring an external JO install.

const MODEL_FIXTURE_ROOT := "res://../fixtures/threedi/synth"
const SKY_BODY_NAME := "crate_mtrl0_ad_lum_upl113.3di"
const AB_SKY_BODY_NAME := "crate_mtrl0_ab_lum_upl113.3di"
const TICK := 1.0 / 62.0
const FAR_CAMERA_POSITION := Vector3(50000.0, 64.0, -40000.0)


func _body_materials(body: Node) -> Array[ShaderMaterial]:
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(body, meshes)
	var out: Array[ShaderMaterial] = []
	for mesh in meshes:
		var material := mesh.material_override as ShaderMaterial
		if mesh.visible and material != null:
			out.append(material)
	return out


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
		FrameFx.set_q3_object_diffuse(material, white_texture, 1000.0)
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
	# pass) never override the material blend [orig: Render_SkyboxSunGlow
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
