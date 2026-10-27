extends GutTest

# The methods of tests/particle_renderer_backend_test.gd that need a RenderingDevice,
# split out of it so the windowed run selects them (G-100).


func _brightest_pixel(image: Image) -> Dictionary:
	var best := Color(0.0, 0.0, 0.0, 0.0)
	var position := Vector2i.ZERO
	var score := -1.0
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			var pixel_score := maxf(pixel.r, maxf(pixel.g, pixel.b))
			if pixel_score > score:
				score = pixel_score
				best = pixel
				position = Vector2i(x, y)
	return {"position": position, "color": best}


func _live_fog_source() -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.fog_type = 1
	data.fog_level = 1.0
	for keyframe in data.get_tod_keyframes():
		keyframe.set_fog_color(Color(0.2, 0.3, 0.4))
	var environment := MissionEnvironment.new()
	environment.environment_data = data
	return environment


func _live_world_scene_at_heights(heights: PackedFloat32Array) -> EffectScene:
	var file := ParticleFixture.catalog(self, "GPU particle",
			"emit_dur = 0.1;\nemit_rate = 20;\nemit_burst = 1;\nage = 2;\nalpha = 1;\nscale = 1;\ngraphic1 = gpu_contract_fallback.tga, blend;\ng1_alpha = 1;\ng1_scale = 1;",
			["GPU effect"])

	var scene := EffectScene.new()
	scene.open([file])
	for height in heights:
		var transform := Transform3D.IDENTITY
		transform.origin.y = height
		var receipt := scene.spawn(EffectSpawnRequest.make(scene.intern("GPU effect"), transform))
		assert_eq(receipt.status, EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)
	return scene


func _live_world_scene() -> EffectScene:
	return _live_world_scene_at_heights(PackedFloat32Array([0.0]))


func test_world_backend_executes_a_live_gpu_submission() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_world_scene()
	renderer.procedural_fallback_enabled = true
	var fog_source := _live_fog_source()
	viewport.add_child(fog_source)
	renderer.environment_source = fog_source
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	# Do not await frame_post_draw: headless compatibility renderers may never
	# emit it. Ordinary process frames still exercise the callback whenever the
	# RenderingDevice compositor is available.
	for _frame in 5:
		await get_tree().process_frame
	# D3D12 readback is asynchronous relative to process_frame. Pin the same
	# force-draw/sync barrier used by the warm test before inspecting pixels;
	# callback diagnostics alone are not proof that the resolved image is ready.
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var world: Dictionary = report.get("world_camera_side", {})
	var backend: Dictionary = report.get("world_camera_backend", {})
	assert_gt(int(world.get("rendered_quad_count", 0)), 0,
			"the fixture must compile visible world geometry")
	assert_gt(int(world.get("draw_command_count", 0)), 0,
			"the fixture must publish at least one material run")
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the post-transparent compositor callback must execute")
	assert_eq(String(backend.get("status", "")), "drawn",
			String(backend.get("failure", "live submission failed")))
	assert_gt(int(backend.get("submitted_commands", 0)), 0)
	assert_eq(int(backend.get("drawn_commands", -1)),
			int(backend.get("submitted_commands", 0)))
	assert_gt(int(backend.get("gpu_draw_calls", 0)), 0)
	assert_eq(int(backend.get("drawn_frame_id", -1)),
			int(backend.get("submitted_frame_id", 0)))
	var image := viewport.get_texture().get_image()
	var center := image.get_pixel(32, 32)
	var peak := _brightest_pixel(image)
	assert_gt(center.r, 0.05,
			"the live particle must reach the framebuffer; brightest pixel=%s at %s" % [
				peak.color, peak.position])
	assert_gt(center.g, center.r + 0.02,
			"full linear fog must tint the white fallback toward the supplied green")
	assert_gt(center.b, center.g + 0.02,
			"full linear fog must tint the white fallback toward the supplied blue")


func test_pipeline_warm_is_serviced_by_the_real_rd_compositor() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	viewport.add_child(renderer)
	await get_tree().process_frame

	# No scene/particles are needed: delayed emitters are exactly why the World
	# compositor must manufacture its real framebuffer-specific pipeline set.
	renderer.warm_pipelines(Vector3.ZERO)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var backend: Dictionary = report.get("world_camera_backend", {})
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the forced draw/sync must service the callback before reset can cancel it")
	assert_eq(int(backend.get("pipeline_warm_requests", 0)), 1)
	assert_eq(int(backend.get("pipeline_warm_requests_serviced", 0)), 1,
			String(backend.get("failure", "RD pipeline warm was not serviced")))
	assert_eq(int(backend.get("warmed_pipeline_modes", 0)), 8,
			"all retail blend modes must exist on the actual World framebuffer")
	assert_gt(int(backend.get("warmed_framebuffer_formats", 0)), 0)
	assert_true(bool(backend.get("scene_snapshot_pipeline_warmed", false)),
			"Distort's resolved-scene copy pipeline must be warmed too")
	renderer.shutdown()
	assert_engine_error_count(0,
			"explicit shutdown frees warmed RenderingDevice resources exactly once")


const OVERLAY_BACKENDS := ["world_overlay_backend", "reflection_overlay_backend",
	"second_scene_overlay_backend", "inset_mirror_overlay_backend"]


# The overlay passes' warm (D-NET-1375): every view's overlay pass takes the
# request with the particle passes', and the frame's callback makes every
# overlay combine's pipeline for its view's framebuffer format into the one
# device every view's overlay pass shares, drawing nothing, so no view's first
# draw of a combine (the first tracer seen in a reflection) makes one. Retail's
# tail draws through render states made with the device and the mission
# textures (engine renderer/scene_overlay.h kSceneOverlayPipelineStates carries
# the witness).
func test_the_overlay_warm_makes_every_combine_pipeline_in_the_shared_device() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 5)
	camera.current = true
	viewport.add_child(camera)
	var renderer := ParticleRenderer.new()
	viewport.add_child(renderer)
	await get_tree().process_frame

	renderer.warm_pipelines(Vector3.ZERO)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	var report := renderer.get_debug_draw_list_report()
	var device_id := int((report.get("world_overlay_backend", {}) as Dictionary).get("device_id", 0))
	assert_ne(device_id, 0)
	for key in OVERLAY_BACKENDS:
		var overlay: Dictionary = report.get(key, {})
		assert_eq(int(overlay.get("pipeline_warm_requests", 0)), 1, "%s takes the warm request" % key)
		assert_eq(int(overlay.get("device_id", -1)), device_id,
				"%s draws through the one shared device" % key)
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	report = renderer.get_debug_draw_list_report()
	var world: Dictionary = report.get("world_overlay_backend", {})
	if not bool(world.get("callback_seen", false)):
		renderer.shutdown()
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_eq(int(world.get("pipeline_warm_requests_serviced", 0)), 1,
			String(world.get("failure", "the overlay warm was not serviced")))
	var formats := int(world.get("warmed_framebuffer_formats", 0))
	assert_gt(formats, 0)
	assert_eq(int(world.get("device_pipelines", 0)), int(world.get("pipeline_states", -1)) * formats,
			"every combine's pipeline, once per framebuffer format, and no draw made one")
	assert_eq(int((report.get("second_scene_overlay_backend", {}) as Dictionary).get(
			"device_pipelines", -1)), int(world.get("device_pipelines", 0)),
			"the Inset's pass finds them in the shared device")
	renderer.shutdown()
	assert_engine_error_count(0,
			"explicit shutdown frees the shared overlay device exactly once")


# --- The second scene view (the weapon Inset pass) ---
#
# The original renders the Inset aperture through the very scene routine the
# main view runs, particle passes included, so that camera needs the world's
# particles compiled for ITS eye. The device shape is HudInsetScope's: a
# sibling SubViewport sharing the main World3D, one camera, no
# WorldEnvironment of its own.

func _main_view(size := Vector2i(64, 64)) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.own_world_3d = true
	add_child_autofree(viewport)
	return viewport


func _shared_world_camera(main_viewport: SubViewport) -> Camera3D:
	var viewport := SubViewport.new()
	viewport.size = main_viewport.size
	viewport.world_3d = main_viewport.find_world_3d()
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)
	return camera


# One Bump quad, world-oriented. A Bump graphic takes the vertex-lit colour
# path: its DIFFUSE word is the light vector seen through the compiling view's
# basis. YAWANDPITCH (0x100) orients the quad in the world instead of at the
# camera, so its corners are the same for every view and the ONLY bytes that
# differ between two views' compiles are that lit colour.
func _live_lit_scene() -> EffectScene:
	return _single_quad_scene("lit", 3, "bump_scale = 1;\nflags = YAWANDPITCH;")


# One live quad at (0, 1, 0), through the same PTL loader as mounted effects.
func _single_quad_scene(name: String, blend: int, properties: String = "") -> EffectScene:
	var file := ParticleFixture.parse(self, _overlap_document(name, blend, properties))
	var scene := EffectScene.new()
	scene.open([file])
	_overlap_spawn(scene, name, 0.0)
	return scene



func _slot(report: Dictionary, key: String) -> Dictionary:
	return report.get(key, {}) as Dictionary


func test_second_scene_backend_draws_the_particle_into_its_own_target() -> void:
	var viewport := _main_view()
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	# The main eye looks AWAY from the quad at (0, 1, 0); only the second eye
	# faces it, so a lit centre pixel can only be the second view's draw.
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.rotation_degrees = Vector3(0.0, 180.0, 0.0)
	camera.current = true
	viewport.add_child(camera)
	var second_camera := _shared_world_camera(viewport)
	var second_viewport := second_camera.get_viewport() as SubViewport
	second_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	second_camera.position = Vector3(0.0, 1.0, -5.0)
	second_camera.rotation_degrees = Vector3(0.0, 180.0, 0.0)

	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("impact", 0)
	renderer.image_provider = _overlap_source_image  # opaque red
	renderer.set_water_plane(-100.0, null)
	renderer.set_second_scene_camera(second_camera)
	viewport.add_child(renderer)
	renderer.render_now(GameWorld.current_frame_clock_ms())
	for _frame in 5:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var report := renderer.get_debug_draw_list_report()
	var backend := _slot(report, "second_scene_camera_backend")
	assert_gt(int(_slot(report, "second_scene_camera_side").get("rendered_quad_count", 0)), 0)
	if not bool(backend.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_true(bool(backend.get("callback_seen", false)),
			"the second view's post-transparent callback must execute")
	assert_eq(String(backend.get("status", "")), "drawn",
			String(backend.get("failure", "second scene submission failed")))
	assert_eq(int(backend.get("drawn_commands", -1)), int(backend.get("submitted_commands", 0)))
	var second_pixel := second_viewport.get_texture().get_image().get_pixel(32, 32)
	var main_pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	assert_gt(second_pixel.r, 0.95,
			"the world's particle reaches the second view's framebuffer; got %s" % second_pixel)
	assert_lt(main_pixel.r, 0.05,
			"the main eye faces away, so its own target stays background; got %s" % main_pixel)
	assert_engine_error_count(0)


func _overlap_source_image(name: String) -> Image:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	var color := Color(0.5, 0.5, 1.0, 1.0)
	if name == "impact.tga":
		color = Color.RED
	image.fill(color)
	return image


func _overlap_document(name: String, blend: int, properties: String = "") -> String:
	var modes := ["blend", "additive", "premult", "bump", "mod", "mod2x", "bumpadd", "distort"]
	var authored := "emit_dur = 0.1;\nemit_rate = 10;\nemit_burst = 1;\nage = 100;\nalpha = 1;\n"
	for index in range(1, 5):
		authored += "color%d = 255, 255, 255;\n" % index
	authored += "graphic1 = %s.tga, %s;\ng1_alpha = 1;\ng1_scale = 1;\n" % [name, modes[blend]]
	return ParticleFixture.definition(name, authored + properties) + ParticleFixture.effect(name, [name])



func _overlap_spawn(scene: EffectScene, name: String, depth: float) -> void:
	var pose := Transform3D.IDENTITY
	pose.origin = Vector3(0.0, 1.0, depth)
	assert_eq(scene.spawn(EffectSpawnRequest.make(scene.intern(name), pose)).status,
			EffectScene.SPAWN_STATUS_SPAWNED)
	scene.advance_in_place(0.1)


func _overlap_image(viewport: SubViewport, renderer: ParticleRenderer) -> Image:
	renderer.render_now(GameWorld.current_frame_clock_ms())
	for frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


# A viewport whose ParticleRenderer draws its distortion subset through a real
# FrameFx node's type-0 row (the world wiring registers the same drawer every
# frame through EffectWorld), over a wall of one-pixel red/black columns.
func _framefx_distortion_view() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var stripes := Shader.new()
	stripes.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;

void fragment() {
	ALBEDO = vec3(mod(floor(FRAGCOORD.x), 2.0), 0.0, 0.0);
}
"""
	var wall_material := ShaderMaterial.new()
	wall_material.shader = stripes
	var wall_mesh := QuadMesh.new()
	wall_mesh.size = Vector2(40.0, 40.0)
	var wall := MeshInstance3D.new()
	wall.mesh = wall_mesh
	wall.material_override = wall_material
	wall.position = Vector3(0.0, 1.0, -2.0)
	viewport.add_child(wall)
	var fx := FrameFx.new()
	viewport.add_child(fx)
	var file := ParticleFixture.parse(self, _overlap_document("impact", 0)
			+ _overlap_document("haze", 7))
	var scene := EffectScene.new()
	scene.open([file])
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.image_provider = _overlap_source_image
	renderer.set_water_plane(-100.0, null)
	viewport.add_child(renderer)
	renderer.attach_distortion_row(fx)
	return {"viewport": viewport, "fx": fx, "scene": scene, "renderer": renderer}


func _framefx_image(view: Dictionary) -> Image:
	var renderer := view["renderer"] as ParticleRenderer
	var fx := view["fx"] as FrameFx
	renderer.render_now(GameWorld.current_frame_clock_ms())
	fx.set_view_effects(0, 0, false, false, 0, false, false, false, false, false, false, false)
	fx.advance_screen_effects()
	for frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return (view["viewport"] as SubViewport).get_texture().get_image()


# The largest red step between neighbouring pixels of row 64 in [from, to).
func _column_contrast(image: Image, from: int, to: int) -> float:
	var contrast := 0.0
	for x in range(from, to):
		contrast = maxf(contrast, absf(image.get_pixel(x + 1, 64).r - image.get_pixel(x, 64).r))
	return contrast


# Class-7 emitters (a distort first graphic) leave both water-split scene
# passes and draw in FrameFX's type-0 row over the finished frame, with texture
# slot 2 = the row's 256A work target sampled at the particle's own screen
# position (retail FrameFX_DistortionPass @ 0x5838F8 -> the flag-4 pass of
# EffectWorld_RenderDistortionPass @ 0x5F72F7; the class test in
# CParticleGroup_RenderChildren @ 0x5E58D2). The row runs only while such an
# emitter lives (EffectWorld_HasDistortionParticles @ 0x5F6640).
func test_distortion_particles_draw_in_the_framefx_row() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var view := _framefx_distortion_view()
	var scene := view["scene"] as EffectScene
	var renderer := view["renderer"] as ParticleRenderer
	var fx := view["fx"] as FrameFx

	# The impact hides behind the wall; it only shows the scene pass still runs.
	_overlap_spawn(scene, "impact", -3.0)
	var before := await _framefx_image(view)
	assert_gt(_column_contrast(before, 58, 70), 0.8, "the wall columns are crisp")
	var report := renderer.get_debug_draw_list_report()
	assert_false(bool(report.get("distortion_present", true)),
			"no class-7 emitter lives, so the row does not run")
	assert_eq(int(fx.get_backend_report().get("distortion_sets", -1)), 0)

	_overlap_spawn(scene, "haze", 1.0)
	var after := await _framefx_image(view)
	report = renderer.get_debug_draw_list_report()
	assert_eq(int(_slot(report, "world_camera_side").get("selected_emitters", -1)), 1,
			"the haze leaves the camera-side scene pass; the impact stays")
	assert_eq(int(_slot(report, "world_far_side").get("selected_emitters", -1)), 0,
			"and the far-side scene pass")
	assert_eq(int(_slot(report, "distortion").get("selected_emitters", -1)), 1,
			"the distortion subset carries the haze")
	assert_true(bool(report.get("distortion_present", false)))
	var backend: Dictionary = report.get("distortion_backend", {})
	assert_eq(String(backend.get("status", "")), "drawn", String(backend.get("failure", "")))
	assert_eq(int(backend.get("drawn_commands", -1)), 1)
	var world_backend: Dictionary = report.get("world_camera_backend", {})
	assert_eq(int(world_backend.get("scene_color_copies", -1)), 0,
			"no scene pass snapshots the frame for the haze")
	assert_eq(int(fx.get_backend_report().get("distortion_sets", -1)), 2,
			"the particle set on 256A and the ribbon set on 256B")
	# The haze paints the half-resolution capture back at its own position: the
	# one-pixel columns under it average out, the rest of the frame stays crisp.
	assert_lt(_column_contrast(after, 58, 70), 0.35,
			"the haze shows the 256A work target, not the frame")
	assert_almost_eq(after.get_pixel(64, 64).r, 0.5, 0.25)
	assert_gt(_column_contrast(after, 4, 16), 0.8, "outside the haze the frame is untouched")


func _asymmetric_dirt_image(_name: String) -> Image:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in image.get_height():
		for x in image.get_width():
			image.set_pixel(x, y, Color(1.0, 0.0, 0.0, 1.0 - float(y) / 31.0))
	return image


func test_dirt_splash_keeps_its_dense_base_below_its_fading_top() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var file := ParticleFixture.parse(self, _overlap_document("dirt", 0, "g1_scale = 2;"))
	var scene := EffectScene.new()
	scene.open([file])
	var renderer := ParticleRenderer.new()
	renderer.scene = scene
	renderer.image_provider = _asymmetric_dirt_image
	renderer.set_water_plane(-100.0, null)
	viewport.add_child(renderer)
	_overlap_spawn(scene, "dirt", 0.0)
	var image := await _overlap_image(viewport, renderer)
	var top := Vector2i(camera.unproject_position(Vector3(0.0, 1.7, 0.0)))
	var base := Vector2i(camera.unproject_position(Vector3(0.0, 0.3, 0.0)))
	# The expansion's drtspl textures put the dense base at source V=0.
	# Retail maps that edge to negative local Y, below the fading V=1 tail.
	assert_gt(image.get_pixelv(base).r, 0.8, "the dense source edge belongs at the base of the splash")
	assert_lt(image.get_pixelv(top).r, image.get_pixelv(base).r - 0.2,
			"the upper plume must fade instead of showing the texture's dense cut edge")
	assert_engine_error_count(0)


# A Bump graphic's material carries SPECULARENABLE instead of FOGENABLE
# (retail CParticleTexture_InitTextureAndChannels @ 0x5E8424 stores 0x10000 as
# its intrinsic pass word): the SPECULAR vertex colour (the modulated particle
# RGB) adds after the DOT3 stages and full scene fog leaves it untinted.
func test_bump_adds_its_specular_colour_and_does_not_fog() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var renderer := ParticleRenderer.new()
	renderer.scene = _live_lit_scene()
	renderer.image_provider = _overlap_source_image
	renderer.set_water_plane(-100.0, null)
	var fog_source := _live_fog_source()
	viewport.add_child(fog_source)
	renderer.environment_source = fog_source
	viewport.add_child(renderer)
	var image := await _overlap_image(viewport, renderer)
	var center := image.get_pixel(64, 64)
	var backend: Dictionary = renderer.get_debug_draw_list_report().get("world_camera_backend", {})
	assert_eq(String(backend.get("status", "")), "drawn", String(backend.get("failure", "")))
	assert_eq(String(backend.get("fog_material_targets", "")),
			"scene,black,black,none,white,gray127,none,none")
	assert_gt(center.r, 0.9, "the specular add lifts the lit bump to the white particle colour: %s" % center)
	assert_almost_eq(center.b, center.r, 0.03,
			"the fog colour (0.2, 0.3, 0.4) must not tint an unfogged bump: %s" % center)
	assert_engine_error_count(0)


func _thermal_view_source(thermal: bool) -> MissionEnvironment:
	var data := EnvFile.new()
	data.reset_to_default()
	data.fog_level = 0.0
	var environment := MissionEnvironment.new()
	environment.environment_data = data
	environment.set_thermal_view(thermal, false)
	return environment


# `far_side`: the eye above a water plane the particle sits below, so the
# particle draws in pass A (the far-side transparent list) instead of the
# camera-side compositor pass.
func _thermal_pixel(blend: int, thermal: bool, background: Color,
		far_side: bool = false) -> Color:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)
	if far_side:
		camera.look_at_from_position(Vector3(0.0, 2.0, 5.0), Vector3(0.0, 1.0, 0.0))
	else:
		camera.position = Vector3(0.0, 1.0, 5.0)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	clear.environment.background_color = background
	viewport.add_child(clear)
	var renderer := ParticleRenderer.new()
	renderer.scene = _single_quad_scene("thermal", blend)
	renderer.image_provider = _overlap_source_image
	renderer.set_water_plane(1.5 if far_side else -100.0, null)
	var source := _thermal_view_source(thermal)
	viewport.add_child(source)
	renderer.environment_source = source
	viewport.add_child(renderer)
	var image := await _overlap_image(viewport, renderer)
	return image.get_pixel(64, 64)


# A thermal-view frame binds each texture's secondary material
# (retail CParticleBatch_FlushAndBindMaterial @ 0x5E42BF): Blend inverts its
# colour, MODULATE(1 - TEXTURE, 1 - DIFFUSE), and Additive darkens under
# ZERO/INVSRCCOLOR (retail CParticleTexture_InitTextureAndChannels
# @ 0x5E8584 / @ 0x5E8390).
func test_thermal_frames_bind_the_secondary_particle_materials() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# The texel is (0.5, 0.5, 1.0): its blue complement is zero.
	var blend := await _thermal_pixel(0, false, Color.BLACK)
	var blend_thermal := await _thermal_pixel(0, true, Color.BLACK)
	assert_gt(blend.b, 0.3, "the primary Blend material shows the texel: %s" % blend)
	assert_lt(blend_thermal.b, 0.05, "the thermal Blend inverts: 1 - T.b = 0: %s" % blend_thermal)
	var grey := Color(0.5, 0.5, 0.5)
	var additive := await _thermal_pixel(1, false, grey)
	var additive_thermal := await _thermal_pixel(1, true, grey)
	assert_gt(additive.b, 0.55, "the primary Additive material brightens: %s" % additive)
	assert_lt(additive_thermal.b, 0.45,
			"the thermal Additive material darkens the grey behind it: %s" % additive_thermal)
	# Pass A (the water's far side) binds the same secondary materials.
	var far_blend := await _thermal_pixel(0, false, Color.BLACK, true)
	var far_blend_thermal := await _thermal_pixel(0, true, Color.BLACK, true)
	assert_gt(far_blend.b, 0.3, "pass A's primary Blend shows the texel: %s" % far_blend)
	assert_lt(far_blend_thermal.b, 0.05,
			"pass A's thermal Blend inverts: %s" % far_blend_thermal)
	var far_additive := await _thermal_pixel(1, false, grey, true)
	var far_additive_thermal := await _thermal_pixel(1, true, grey, true)
	assert_gt(far_additive.b, 0.55, "pass A's primary Additive brightens: %s" % far_additive)
	assert_lt(far_additive_thermal.b, 0.45,
			"pass A's thermal Additive darkens the grey behind it: %s" % far_additive_thermal)
	assert_engine_error_count(0)
