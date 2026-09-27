extends GutTest

# =============================================================================
# Water surface pins (env #29, docs/env/env-tod-re.md "Water surface"):
# the witnessed per-side material swap (camera-above -> the BLEND material,
# underwater -> the OPAQUE one [orig: selection in render_water_surface
# @ 0x5c33e6..0x5c34ea; Water_ShaderOpaque @ 0x28ee8c8]) and the retirement of
# the invented terrain-fog-curve uniforms (the water fog is the per-row
# spec-alpha factor). ADR 0018 discipline: everything pins through PUBLIC
# seams only — exported/public properties and shader parameters.
#
# Fixture per env_parity_vectors_test.gd _collect_water_mesh: the strip march
# is a function of VIEWPORT PIXELS, so it lives in a code-fixed SubViewport
# (1024x600). Water resolves its camera through its OWN viewport —
# Camera3D.current applies per-viewport — so the camera is created inside the
# SubViewport and made current there.
# =============================================================================

const WATER_SHADER := "res://shaders/water.gdshader"
const FULL_00_ENV_FIXTURE := "res://../fixtures/env/synth_full.env"

# The engine tick [docs/engine-primer.md: 62 Hz].
const TICK := 1.0 / 62.0


func _make_water_fixture(height: float = 7.0) -> Dictionary:
	var strip_vp := SubViewport.new()
	strip_vp.size = Vector2i(1024, 600)
	add_child_autofree(strip_vp)
	var water: Node = Water.new()
	strip_vp.add_child(water)
	water.water_height = height
	var cam := Camera3D.new()
	strip_vp.add_child(cam)
	cam.global_position = Vector3(100.3, 27.0, -33.7)
	cam.make_current()
	return {"water": water, "camera": cam, "viewport": strip_vp}


func test_zero_height_disables_surface_mirror_and_world_split() -> void:
	var cache := ObjectShaderCache.get_singleton()
	cache.clear_water_plane()
	var fixture := _make_water_fixture(0.0)
	var water: Node = fixture["water"]
	water.advance_frame(TICK)

	assert_false(water.is_water_active(), "zero is the retail no-water sentinel")
	assert_false(water.get_mesh_instance().visible)
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(bool(water.get_water_material().get_shader_parameter("u_has_reflection")))
	assert_false(cache.has_water_plane())

	water.water_height = -2.0
	water.advance_frame(TICK)
	assert_true(water.is_water_active(), "signed nonzero water heights are valid")
	assert_true(water.is_water_render_active())
	assert_true(water.get_mesh_instance().visible)
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_ALWAYS)
	assert_true(cache.has_water_plane())

	water.set_world_rendering_enabled(false)
	water.advance_frame(TICK)
	assert_true(water.is_water_active(),
			"retained authored height survives an unloaded owner")
	assert_false(water.is_water_render_active(),
			"owner lifecycle gates render consumers independently of height")
	assert_false(water.get_mesh_instance().visible)
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(cache.has_water_plane())

	water.set_world_rendering_enabled(true)
	water.water_height = 0.0
	water.advance_frame(TICK)
	assert_false(water.is_water_active())
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(cache.has_water_plane())


func test_water_pass_follows_the_visible_terrain_height_range() -> void:
	# Retail runs the reflection prerender, the noise pair and the strip only
	# while the lowest visible terrain sits at or below the water height, or
	# the Blink walk saw the water last frame [orig:
	# terrain_setup_view_and_lighting @ 0x60fe40 @ 0x60ff12..0x60ff33].
	var fixture := _make_water_fixture(4.0)
	var water: Node = fixture["water"]
	assert_true(water.is_water_pass_active(),
			"no tracked terrain bounds keep the pass live")
	water.set_visible_terrain_bounds(true, 6.0, 40.0)
	assert_false(water.is_water_pass_active(),
			"terrain wholly above the water switches the pass off")
	water.advance_frame(TICK)
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED,
			"an inactive pass renders no mirror")
	water.set_blink_water_visible(true)
	assert_true(water.is_water_pass_active(),
			"last frame's Blink water-visible forces the pass")
	water.set_blink_water_visible(false)
	water.set_visible_terrain_bounds(true, 2.0, 40.0)
	assert_true(water.is_water_pass_active(),
			"terrain reaching below the water keeps the pass live")
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	assert_true(water.is_water_pass_active(),
			"losing the bounds (no terrain in view) falls back to live")


func test_height_precedence_is_bms_then_signed_trn_then_env() -> void:
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(ProjectSettings.globalize_path(
			RuntimeFixture.directory())), OK)
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, "mnml.trn"), OK)
	terrain.set_water_height(-20) # engine half-units -> -10 world units

	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_water_height(12.0) # engine half-units -> 6 world units
	var env := MissionEnvironment.new()
	env.name = "WaterPrecedenceEnv"
	env.environment_data = env_data
	add_child_autofree(env)

	var water: Node = Water.new()
	water.environment_path = NodePath("../WaterPrecedenceEnv")
	water.terrain_data = terrain
	add_child_autofree(water)
	assert_almost_eq(water.water_height, -10.0, 0.001,
			"signed TRN water beats ENV")

	water.set_mission_water_height_override(0.0)
	assert_eq(water.water_height, 0.0,
			"an explicit BMS zero disables water and still beats TRN")
	water.set_mission_water_height_override(15.0)
	assert_eq(water.water_height, 15.0)
	water.set_mission_water_height_override(NAN)
	assert_eq(water.water_height, -10.0)

	terrain.set_water_height(0)
	water.terrain_data = terrain
	assert_eq(water.water_height, 6.0, "ENV is the final fallback")

func test_reflection_rtt_is_the_512_square_at_the_main_field() -> void:
	# Retail's 512 x 512 RTT renders with the main view's projection (the
	# target's h/w as the vertical scale over the square viewport, retail
	# render_main_scene @ 0x5c1255..0x5c163e): the main field over the square,
	# non-square texels. The raster is served that frustum
	# (TargetProjectionXrInterface); the mirror camera node keeps the source's
	# frame, so its own projection is the source camera's.
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]
	var cam: Camera3D = fixture["camera"]

	cam.fov = 60.0
	water.advance_frame(TICK)
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	assert_eq(mirror_vp.size, Vector2i(512, 512), "retail's 512 square")
	assert_true(mirror_vp.use_xr, "the square is served the main view's frustum")
	assert_eq(mirror_vp.size_2d_override, Vector2i(1024, 600),
			"the mirror camera's frame is the source's")
	var served := TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_almost_eq(served.y.y / served.x.x, 1024.0 / 600.0, 0.0001,
			"the source's aspect over the square")
	assert_almost_eq(water.get_reflection_camera().fov, 60.0, 0.001,
			"the mirror takes the source's own field")
	assert_eq(water.get_reflection_camera().keep_aspect, cam.keep_aspect)
	_assert_reflection_projection_registered(water, cam, Vector3(100.3, 12.0, -133.7))

	strip_vp.size = Vector2i(1600, 900)
	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.fov = 72.0
	water.advance_frame(TICK)
	assert_eq(mirror_vp.size, Vector2i(512, 512), "a display resize keeps the square")
	assert_eq(mirror_vp.size_2d_override, Vector2i(1600, 900))
	served = TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_almost_eq(served.y.y / served.x.x, 1600.0 / 900.0, 0.0001,
			"at the new aspect")
	assert_almost_eq(water.get_reflection_camera().fov, 72.0, 0.001)
	assert_eq(water.get_reflection_camera().keep_aspect, Camera3D.KEEP_WIDTH)
	_assert_reflection_projection_registered(water, cam, Vector3(100.3, 12.0, -133.7))


func test_releasing_the_renderer_takes_its_water_split_with_it() -> void:
	# The split height this water pushed is the session's (the render order's
	# transparent bracket and the mirror's per-draw CLIP arming read it): a
	# released water must not leave it behind for the next world, since the
	# later tree exit no longer clears it.
	var fixture := _make_water_fixture()
	var water: Water = fixture["water"]
	water.advance_frame(TICK)
	var cache := ObjectShaderCache.get_singleton()
	assert_true(cache.has_water_plane(), "a live water publishes its split")
	water.release_runtime_renderer_resources()
	assert_false(cache.has_water_plane(), "the released water takes its split with it")


func test_process_exit_releases_the_reflection_decode_before_its_viewport() -> void:
	var fixture := _make_water_fixture()
	var water := fixture["water"] as Water
	water.advance_frame(TICK)

	var mirror_camera := water.get_reflection_camera()
	assert_not_null(mirror_camera)
	var compositor := mirror_camera.compositor
	assert_not_null(compositor)
	var effects := compositor.get_compositor_effects()
	assert_eq(effects.size(), 1)
	var decode := effects[0] as FrameFxCompositorEffect
	assert_not_null(decode)
	if decode == null:
		return
	assert_false(bool(decode.get_backend_report().get("shutdown", false)))

	water.release_runtime_renderer_resources()
	assert_true(bool(decode.get_backend_report().get("shutdown", false)),
			"Water drains the reflection decode while RenderingDevice is live")
	assert_eq(compositor.get_compositor_effects().size(), 0,
			"the retained mirror compositor no longer owns the decode effect")
	assert_null(water.get_reflection_viewport())
	assert_null(water.get_reflection_camera())

	# MainGame's explicit release and SceneTree fallback may converge here.
	water.release_runtime_renderer_resources()
	assert_true(bool(decode.get_backend_report().get("shutdown", false)))


func test_leaving_the_tree_releases_the_reflection_decode_and_reentry_rearms_it() -> void:
	# A Water freed or detached outside release_runtime_renderer_resources()
	# (GUT fixtures, embedder previews) must not leak its RenderingDevice
	# chain: EXIT_TREE runs the same idempotent release leg FrameFx has, and
	# ENTER_TREE re-arms a fresh decode on the retained mirror camera.
	var fixture := _make_water_fixture()
	var water := fixture["water"] as Water
	var strip_vp: SubViewport = fixture["viewport"]
	water.advance_frame(TICK)
	var mirror_camera := water.get_reflection_camera()
	assert_not_null(mirror_camera)
	var first_compositor := mirror_camera.compositor
	assert_not_null(first_compositor)
	var first_decode := first_compositor.get_compositor_effects()[0] 			as FrameFxCompositorEffect
	assert_not_null(first_decode)
	if first_decode == null:
		return

	strip_vp.remove_child(water)
	assert_true(bool(first_decode.get_backend_report().get("shutdown", false)),
			"leaving the tree drains and releases the mirror decode")
	assert_false(first_decode.enabled)
	assert_null(mirror_camera.compositor,
			"the mirror camera no longer carries the released compositor")
	assert_eq(first_compositor.get_compositor_effects().size(), 0)

	strip_vp.add_child(water)
	var second_compositor := mirror_camera.compositor
	assert_not_null(second_compositor, "re-entry re-arms the mirror decode")
	if second_compositor == null:
		return
	var second_decode := second_compositor.get_compositor_effects()[0] 			as FrameFxCompositorEffect
	assert_not_null(second_decode)
	if second_decode == null:
		return
	assert_ne(second_decode.get_instance_id(), first_decode.get_instance_id(),
			"the released effect stays shut down; re-entry uses a fresh one")
	assert_false(bool(second_decode.get_backend_report().get("shutdown", true)))
	water.advance_frame(TICK)
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_ALWAYS, "the strip and mirror resume after re-entry")

	# The explicit process-exit release converges with the EXIT_TREE leg.
	water.release_runtime_renderer_resources()
	assert_true(bool(second_decode.get_backend_report().get("shutdown", false)))
	assert_null(water.get_reflection_camera())
	water.release_runtime_renderer_resources()


func test_reflection_camera_matches_orthogonal_and_frustum_sources() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]

	cam.rotation_degrees = Vector3(-20.0, 0.0, 0.0)
	cam.keep_aspect = Camera3D.KEEP_HEIGHT
	cam.set_orthogonal(90.0, 0.25, 500.0)
	cam.h_offset = 2.5
	cam.v_offset = -1.25
	water.advance_frame(TICK)
	assert_eq(water.get_reflection_camera().projection, Camera3D.PROJECTION_ORTHOGONAL)
	assert_eq(water.get_reflection_camera().keep_aspect, Camera3D.KEEP_HEIGHT)
	assert_almost_eq(water.get_reflection_camera().size, 90.0, 0.0001,
			"the mirror takes the source's orthographic span")
	assert_almost_eq(water.get_reflection_camera().near, 0.25, 0.000001)
	assert_almost_eq(water.get_reflection_camera().far, 500.0, 0.000001)
	assert_almost_eq(water.get_reflection_camera().h_offset, 2.5, 0.000001)
	assert_almost_eq(water.get_reflection_camera().v_offset, 1.25, 0.000001,
			"the mirror-up basis requires the vertical camera offset to flip")
	_assert_reflection_projection_registered(water, cam, Vector3(100.3, 12.0, -133.7))

	cam.set_frustum(45.0, Vector2(3.0, -4.0), 0.5, 800.0)
	water.advance_frame(TICK)
	assert_eq(water.get_reflection_camera().projection, Camera3D.PROJECTION_FRUSTUM)
	assert_eq(water.get_reflection_camera().keep_aspect, Camera3D.KEEP_HEIGHT)
	assert_almost_eq(water.get_reflection_camera().size, 45.0, 0.0001,
			"the mirror takes the source's frustum span")
	assert_eq(water.get_reflection_camera().frustum_offset, Vector2(3.0, 4.0),
			"the off-axis frustum follows the mirror's vertical flip")
	assert_almost_eq(water.get_reflection_camera().near, 0.5, 0.000001)
	assert_almost_eq(water.get_reflection_camera().far, 800.0, 0.000001)
	assert_almost_eq(water.get_reflection_camera().h_offset, 2.5, 0.000001)
	assert_almost_eq(water.get_reflection_camera().v_offset, 1.25, 0.000001)
	_assert_reflection_projection_registered(water, cam, Vector3(100.3, 12.0, -133.7))

	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.set_frustum(30.0, Vector2(-2.0, 1.5), 0.75, 900.0)
	water.advance_frame(TICK)
	assert_almost_eq(water.get_reflection_camera().size, 30.0, 0.0001)
	assert_eq(water.get_reflection_camera().keep_aspect, Camera3D.KEEP_WIDTH)
	assert_eq(water.get_reflection_camera().frustum_offset, Vector2(-2.0, -1.5))
	_assert_reflection_projection_registered(water, cam, Vector3(100.3, 12.0, -133.7))


func test_collapsed_source_viewport_never_builds_an_invalid_mirror_projection() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]

	# Workspace layout can briefly expose a very thin but nonzero viewport.
	# Its horizontal FOV approaches 180 degrees, beyond Camera3D's legal
	# range, even though the source camera itself remains ordinary.
	strip_vp.size = Vector2i(1024, 2)
	water.advance_frame(TICK)
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	assert_eq(mirror_vp.size, Vector2i(512, 512), "the raster stays retail's square")
	var served := TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_true(is_finite(served.x.x) and served.x.x > 0.0 and is_finite(served.y.y),
			"a drawable extreme aspect still serves a finite projection")
	assert_almost_eq(water.get_reflection_camera().fov,
			(fixture["camera"] as Camera3D).fov, 0.001)

	strip_vp.size = Vector2i(1024, 1)
	water.advance_frame(TICK)
	assert_eq((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0,
			"the strip and mirror share the same collapsed-viewport gate")
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED,
			"a collapsed editor view must not spend a stale reflection pass")
	assert_false(bool(water.get_water_material().get_shader_parameter("u_has_reflection")))


func test_surface_and_mirror_follow_a_new_current_camera() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var viewport: SubViewport = fixture["viewport"]
	var old_camera: Camera3D = fixture["camera"]
	water.advance_frame(TICK)

	var new_camera := Camera3D.new()
	viewport.add_child(new_camera)
	new_camera.global_position = Vector3(-240.0, 42.0, 315.0)
	new_camera.make_current()
	water.advance_frame(TICK)

	assert_false(old_camera.current)
	assert_true(new_camera.current)
	assert_eq(water.get_reflection_camera().global_position,
			Vector3(new_camera.global_position.x,
					2.0 * water.water_height - new_camera.global_position.y,
					new_camera.global_position.z),
			"strip tessellation and the mirror RTT reacquire the viewport's current camera")


func test_reflection_lookup_stays_registered_while_view_rotates() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]
	var cam: Camera3D = fixture["camera"]
	cam.fov = Simulation.fov_vertical_from_horizontal(
			72.0, float(strip_vp.size.x) / float(strip_vp.size.y))
	var real_world_point := Vector3(100.3, 12.0, -133.7)

	water.advance_frame(TICK)
	_assert_reflection_projection_registered(water, cam, real_world_point)

	cam.rotation_degrees = Vector3(-12.0, 20.0, 0.0)
	water.advance_frame(TICK)
	_assert_reflection_projection_registered(water, cam, real_world_point)


# A world point through a view transform and projection, as a (0..1) texture
# coordinate of the raster that projection draws.
func _project_uv(eye: Transform3D, projection: Projection, point: Vector3) -> Vector2:
	var view := eye.affine_inverse() * point
	var clip := projection * Vector4(view.x, view.y, view.z, 1.0)
	return Vector2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5)


# The mirror RTT's raster as drawn: the transform and projection served to
# its 512 square.
func _mirror_uv(water: Node, point: Vector3) -> Vector2:
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	return _project_uv(TargetProjectionXrInterface.served_transform(mirror_vp),
			TargetProjectionXrInterface.served_projection(mirror_vp), point)


# `source_projection` is the projection the source view draws with (the
# camera's own when null).
func _assert_reflection_projection_registered(water: Node, cam: Camera3D,
		world_point: Vector3, source_projection: Variant = null) -> void:
	# A planar reflection projects a real point through the mirror's raster at
	# the same location where the main view sees its virtual point below the
	# water plane. Compare the source projection with the one the mirror RTT
	# is served; this catches view-dependent swimming without requiring a
	# raster readback.
	var virtual_point := world_point
	virtual_point.y = 2.0 * water.water_height - world_point.y
	assert_false(cam.is_position_behind(virtual_point),
			"the virtual reflection point must be in front of the source camera")
	assert_false(water.get_reflection_camera().is_position_behind(world_point),
			"the real point must be in front of the mirror camera")
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	assert_true(TargetProjectionXrInterface.is_serving(mirror_vp),
			"the mirror RTT is served its frustum")
	var projection: Projection = source_projection if source_projection != null \
			else cam.get_camera_projection()
	var main_uv := _project_uv(cam.get_camera_transform(), projection, virtual_point)
	var sampled_uv := Vector2(main_uv.x, 1.0 - main_uv.y)
	var mirror_uv := _mirror_uv(water, world_point)
	var one_rtt_texel := 1.0 / float(TargetProjectionXrInterface.TARGET_SIDE)
	assert_almost_eq(sampled_uv.x, mirror_uv.x, one_rtt_texel,
			"reflection U stays registered to the mirrored world point")
	assert_almost_eq(sampled_uv.y, mirror_uv.y, one_rtt_texel,
			"reflection V stays registered to the mirrored world point")
	# The mirror camera node projects through the source's frame, so its own
	# projection meets the served raster.
	var node_uv: Vector2 = (water.get_reflection_camera().unproject_position(world_point)
			/ mirror_vp.get_visible_rect().size)
	assert_almost_eq(node_uv.x, mirror_uv.x, one_rtt_texel,
			"the mirror camera node's U meets the served raster")
	assert_almost_eq(node_uv.y, mirror_uv.y, one_rtt_texel,
			"the mirror camera node's V meets the served raster")

	# Exercise the exact strip payload consumed by the shader too. Sample each
	# row's LEFT vertex because vbase is built from that vertex's rhw and then
	# copied across the row. vbase = 1 - min(300 * rhw + 0.15, 2) / 256
	# (flt_7DBF68 = 300.0, retail render_water_strip_detailed @ 0x5c2f04).
	var mesh := water.get_mesh_instance().mesh as ArrayMesh
	assert_gt(mesh.get_surface_count(), 0,
			"the reflected projection fixture must produce water strip rows")
	if mesh.get_surface_count() == 0:
		return
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var custom0: PackedFloat32Array = arrays[Mesh.ARRAY_CUSTOM0]
	var middle_left := int(float(positions.size()) / 6.0) * 3
	var left_vertices := PackedInt32Array([
		0,
		middle_left,
		positions.size() - 3,
	])
	for vertex_index in left_vertices:
		var base := vertex_index * 4
		var q := minf(300.0 * custom0[base + 1] + 0.15, 2.0)
		var vbase_bias := q / 256.0
		var sampled_strip_uv := Vector2(custom0[base + 2], custom0[base + 3])
		var mirror_strip_uv := _mirror_uv(water, positions[vertex_index])
		assert_almost_eq(sampled_strip_uv.x, mirror_strip_uv.x, one_rtt_texel,
				"strip reflection U stays registered to its mirrored water point")
		assert_almost_eq(sampled_strip_uv.y,
				mirror_strip_uv.y - vbase_bias, one_rtt_texel,
				"strip reflection V keeps its witnessed vbase bias")


func test_above_water_renders_blend_side() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	water.advance_frame(TICK)
	var mesh: ArrayMesh = water.get_mesh_instance().mesh
	assert_gt(mesh.get_surface_count(), 0,
			"camera above the plane must march strip rows")
	assert_eq(water.get_water_material().get_shader_parameter("u_underwater_view"), false,
			"camera above -> the BLEND material side [orig: selection @ 0x5c33e6..0x5c34ea]")


func test_underwater_swaps_to_opaque_side() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	var env_data := EnvFile.new()
	env_data.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	assert_eq(env_data.load(), OK)
	var env := MissionEnvironment.new()
	env.name = "UnderwaterWaterEnv"
	env.environment_data = env_data
	fixture["viewport"].add_child(env)
	water.environment_path = NodePath("../UnderwaterWaterEnv")
	water.set_mission_water_height_override(7.0)
	water.advance_frame(TICK)
	# Drop the camera below the 7.0 plane: the pass swaps to the OPAQUE
	# material [orig: Water_ShaderOpaque @ 0x28ee8c8; selection
	# @ 0x5c33e6..0x5c34ea].
	cam.global_position = Vector3(100.3, 1.0, -33.7)
	env.set_underwater_view(true)
	water.advance_frame(TICK)
	assert_eq(water.get_water_material().get_shader_parameter("u_underwater_view"), true,
			"camera below -> the OPAQUE material side")
	assert_true(Vector3(water.get_water_material().get_shader_parameter("u_fog_color"))
			.is_equal_approx(env.get_scene_fog_color()),
			"the underwater surface strip fogs toward the selected lit-water color")


func test_mirror_clears_to_the_skyfog_or_black_under_the_indoors_letter() -> void:
	# The mirror RTT has its own clear: the skyfog on the prerender's outdoors
	# path, black under the indoors blink letter, and no thermal, waterline or
	# NVG leg (EnvironmentState::water_mirror_clear_color; retail
	# render_main_scene @ 0x5c1342..0x5c1353, @ 0x5c1474, @ 0x5c1597). The
	# mirror camera carries it in its own BG_COLOR environment, pre-encoded
	# like the beauty clear, whatever the beauty clear shows.
	var fixture := _make_water_fixture()
	var water: Water = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	water.advance_frame(TICK)
	assert_null(water.get_reflection_camera().environment,
			"an envless water keeps the World's clear")
	var env_data := EnvFile.new()
	env_data.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV_FIXTURE))
	assert_eq(env_data.load(), OK)
	var env := MissionEnvironment.new()
	env.name = "MirrorClearEnv"
	env.environment_data = env_data
	fixture["viewport"].add_child(env)
	water.environment_path = NodePath("../MirrorClearEnv")
	water.set_mission_water_height_override(7.0)
	water.advance_frame(TICK)
	var mirror_env: Environment = water.get_reflection_camera().environment
	assert_not_null(mirror_env, "a loaded environment gives the mirror its own clear")
	if mirror_env == null:
		return
	assert_eq(mirror_env.background_mode, Environment.BG_COLOR)
	assert_eq(mirror_env.ambient_light_source, Environment.AMBIENT_SOURCE_DISABLED)
	var skyfog: Vector3 = env.get_frame_clear_color()
	var outdoors := Color(skyfog.x, skyfog.y, skyfog.z).linear_to_srgb()
	assert_true(mirror_env.background_color.is_equal_approx(outdoors),
			"outdoors the mirror clears to the skyfog")

	# The eye below the water (the beauty clear turns to the lit water) and
	# the thermal view (the beauty clear's 0x808080) leave the mirror's alone.
	cam.global_position = Vector3(100.3, 1.0, -33.7)
	env.set_underwater_view(true)
	env.set_thermal_view(true, true)
	water.advance_frame(TICK)
	assert_true(mirror_env.background_color.is_equal_approx(outdoors),
			"no waterline or thermal leg reaches the mirror clear")
	env.set_thermal_view(false, false)

	water.set_mirror_scene_outdoors(false)
	assert_false(water.is_mirror_scene_outdoors())
	assert_true(mirror_env.background_color.is_equal_approx(Color.BLACK),
			"the indoors letter clears the mirror to black")
	water.set_mirror_scene_outdoors(true)
	assert_true(mirror_env.background_color.is_equal_approx(outdoors))


func test_water_material_has_no_far_discard_uniforms() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	water.advance_frame(TICK)
	assert_not_null(water.get_water_material().shader, "the water shader must be loaded")
	# ShaderMaterial.get_shader_parameter returns null for unknown/unset names:
	# the retired terrain-fog-curve uniform must be GONE and the per-side swap
	# uniform PRESENT (fed by the strip rebuild).
	assert_null(water.get_water_material().get_shader_parameter("u_fog_type"),
			"u_fog_type retired: water fog rides the per-row spec-alpha factor")
	assert_not_null(water.get_water_material().get_shader_parameter("u_underwater_view"),
			"u_underwater_view must be fed by the strip rebuild")


func test_strip_and_mirror_register_to_the_live_aspect_mode_target() -> void:
	# While the local player presenter draws an aspect mode through its
	# stretched target (LocalPlayerPresenter.view_projection: the target camera
	# at the horizontal fov over a viewport of the SELECTED ratio, blitted over
	# the whole surface), THAT camera draws the water into THAT viewport: the
	# strip march and the mirror register to the target's projection and
	# pixels. The surface camera then carries only the frustum's culling
	# superset, whose mirror would open the horizontal field past the frame's
	# and register the reflection V to the surface ratio, not the selected one.
	var world := WorldFixture.boot_minimal(self)
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	var size := camera.get_viewport().get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		pending("the headless viewport reports no size, so no projection target can be live")
		return
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, ControlsModel.new())
	# Retail's stock mode 0 (4:3) unless the surface is 4:3 itself, then 16:9.
	var surface_ratio := size.y / size.x
	var mode := 0 if absf(surface_ratio - 0.75) > 0.001 else 2
	var selected := 0.75 if mode == 0 else 0.5625
	var sim := world.get_sim()
	sim.set_local_player_aspect_mode(mode)
	for i in 2:
		var frame_input := presenter.before_world_tick(TICK, false, true)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()
	var through: Camera3D = presenter.projection_camera()
	assert_not_null(through, "a mode off the surface ratio draws through a target")
	if through == null:
		presenter.teardown()
		return
	var water: Water = world.get_water_node()
	water.set_mission_water_height_override(7.0)
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.advance_frame(TICK)
	assert_almost_eq(water.get_reflection_camera().fov, 80.0, 0.001,
			"the mirror takes the TARGET camera's field (the frame's policy 80), "
			+ "never the surface camera's culling superset")
	var target_vp := through.get_viewport()
	var target_size := Vector2(target_vp.get_visible_rect().size)
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	var served := TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_eq(mirror_vp.size, Vector2i(512, 512), "the mirror raster is retail's square")
	assert_almost_eq(served.y.y / served.x.x, target_size.x / target_size.y, 0.0001,
			"the mirror frustum takes the TARGET's ratio (the selected mode)")
	assert_almost_eq(served.y.y / served.x.x, 1.0 / selected,
			maxf(0.01, 2.0 / target_size.y), "whole-pixel targets round the selected ratio")

	# Back to the surface's own ratio: the target goes away and the surface
	# camera draws the water again.
	sim.set_local_player_aspect_mode(-1)
	var native_input := presenter.before_world_tick(TICK, false, true)
	world.tick(camera.global_position, camera.global_transform, TICK, native_input)
	presenter.after_world_tick()
	assert_null(presenter.projection_viewport(), "a native mode releases the target")
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.advance_frame(TICK)
	served = TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_eq(mirror_vp.size, Vector2i(512, 512))
	assert_almost_eq(served.y.y / served.x.x, 1.0 / surface_ratio, 0.0001,
			"a native mode serves the mirror the surface's own ratio again")
	presenter.teardown()


func test_strip_and_mirror_register_to_the_nvg_raster() -> void:
	# While the NVG composite is up the world draws into the NVG raster: the
	# 512 square at the frame's frustum (LocalPlayerPresenter.view_projection,
	# served through TargetProjectionXrInterface). The strip marches over that
	# square under that projection, and the mirror takes the same frustum over
	# its own square (retail NVG_RenderScene @ 0x5d2954..0x5d296d; the mirror,
	# render_main_scene @ 0x5c1619..0x5c163e).
	var world := WorldFixture.boot_minimal(self)
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	var size := camera.get_viewport().get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		pending("the headless viewport reports no size, so no NVG raster can be live")
		return
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, ControlsModel.new())
	var sim := world.get_sim()
	sim.set_local_player_aspect_mode(-1)
	assert_true(sim.request_local_player_nvg_toggle())
	for i in 2:
		var frame_input := presenter.before_world_tick(TICK, false, true)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()
	var target: SubViewport = presenter.projection_viewport()
	assert_true(presenter.is_nvg_raster_active(), "the NVG composite takes the world pass")
	if target == null:
		presenter.teardown()
		return
	var water: Water = world.get_water_node()
	water.set_mission_water_height_override(7.0)
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.advance_frame(TICK)
	var frame_projection: Projection = presenter.view_projection()
	var mirror_vp: SubViewport = water.get_reflection_viewport()
	var served := TargetProjectionXrInterface.served_projection(mirror_vp)
	assert_eq(target.size, Vector2i(512, 512), "the NVG raster is retail's square")
	assert_true(target.use_xr and TargetProjectionXrInterface.is_serving(target),
			"served the frame's frustum")
	assert_eq(frame_projection, TargetProjectionXrInterface.served_projection(target),
			"the presenter's frame projection is the served one")
	var half_h := tan(deg_to_rad(40.0))
	assert_almost_eq(frame_projection.x.x, 1.0 / half_h, 0.0001, "cot(fov_h/2) across the columns")
	assert_almost_eq(frame_projection.y.y, (size.x / size.y) / half_h, 0.0001,
			"the frame's vertical half-extent across the rows")
	assert_eq(mirror_vp.size, Vector2i(512, 512))
	assert_almost_eq(served.y.y / served.x.x, size.x / size.y, 0.0001,
			"the mirror takes the frame frustum's aspect, not the NVG square's")
	assert_almost_eq(served.x.x, frame_projection.x.x, 0.0001,
			"the mirror frustum is the frame's")
	assert_almost_eq(served.y.y, frame_projection.y.y, 0.0001)
	presenter.teardown()


func test_strip_texcoords_are_the_render_basis_world_over_32() -> void:
	# Retail's texcoord 0 (duplicated into texcoord 3) is the unprojected
	# ABSOLUTE render-basis world x/32, z/32 of each strip vertex (retail
	# render_water_strip_detailed @ 0x5c2aec..0x5c2b00, flt_7DBFAC = 1/32). The
	# render basis is the util/axes.h x/z swap of the Godot world, so every
	# vertex carries TEX_UV = (godot z, godot x) / 32, and no scale, bias,
	# offset or cloud scroll reaches the texcoords (the scroll "offsets"
	# render_water_surface stores @ 0x5c33b9 / @ 0x5c33db are never read).
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	water.advance_frame(TICK)
	var mesh := water.get_mesh_instance().mesh as ArrayMesh
	assert_gt(mesh.get_surface_count(), 0, "the fixture view must march strip rows")
	if mesh.get_surface_count() == 0:
		return
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
	for index in [0, positions.size() >> 1, positions.size() - 1]:
		var expected := Vector2(positions[index].z, positions[index].x) / 32.0
		assert_almost_eq(uvs[index].x, expected.x, 0.001,
				"texcoord u is the render-basis world x (Godot z) / 32")
		assert_almost_eq(uvs[index].y, expected.y, 0.001,
				"texcoord v is the render-basis world z (Godot x) / 32")
	assert_null(water.get_water_material().get_shader_parameter("u_water_uv"),
			"no texcoord transform uniform: the shader samples the rows' texcoords")


func test_noise_pattern_is_fixed_in_the_world_at_one_texture_per_32_units() -> void:
	# The strip samples its noise pair at the world-fixed texcoords above: a
	# texture whose u half [0, 0.5) is black and [0.5, 1) white paints bands
	# 16 units wide along Godot z, repeating every 32 units.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var water := Water.new()
	viewport.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.global_position = Vector3(3.0, 27.0, 16.0)
	cam.rotation_degrees = Vector3(-60.0, 0.0, 0.0)
	cam.make_current()
	water.advance_frame(TICK)
	var mesh := water.get_mesh_instance().mesh as ArrayMesh
	assert_gt(mesh.get_surface_count(), 0, "the fixture view must march strip rows")

	var bands := Image.create(64, 4, false, Image.FORMAT_RGBA8)
	for x in 64:
		for y in 4:
			bands.set_pixel(x, y, Color.BLACK if x < 32 else Color.WHITE)
	var material := water.get_water_material()
	material.set_shader_parameter("u_noise_color", ImageTexture.create_from_image(bands))
	var neutral := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	neutral.fill(Color(0.5, 0.5, 1.0, 1.0))
	material.set_shader_parameter("u_noise_normal", ImageTexture.create_from_image(neutral))
	material.set_shader_parameter("u_has_reflection", false)
	material.set_shader_parameter("u_water_color", Vector3.ONE)
	material.set_shader_parameter("u_fog_color", Vector3.ZERO)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var image := viewport.get_texture().get_image()
	# z = 8 -> u = 0.25 (black band); z = -8 -> u = -0.25, wrapping to 0.75
	# (white band).
	var black_px := cam.unproject_position(Vector3(3.0, 7.0, 8.0))
	var white_px := cam.unproject_position(Vector3(3.0, 7.0, -8.0))
	var black := image.get_pixelv(Vector2i(black_px))
	var white := image.get_pixelv(Vector2i(white_px))
	assert_gt(white.r, black.r + 0.2,
			"the band at world z = -8 samples u = 0.75 and the one at z = 8 u = 0.25 "
			+ "(black %s, white %s)" % [black, white])
	water.release_runtime_renderer_resources()


func _strip_arrays(instance: MeshInstance3D) -> Array:
	var mesh := instance.mesh as ArrayMesh
	if mesh == null or mesh.get_surface_count() == 0:
		return []
	return mesh.surface_get_arrays(0)


func test_night_vision_redraw_marches_the_nightvision_rows_above_water_only() -> void:
	# The FrameFX bloom pass redraws the strip as render_water_surface(0, 1)
	# (retail FrameFX_RenderGlowSource @ 0x582a59..0x582a5d): the above-water
	# march with the nightvision row colors — the flat 0.1 base and no
	# specular RGB (retail render_water_strip_detailed @ 0x5c2d5a / @ 0x5c2ef8)
	# — on the same geometry as the beauty strip.
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	water.advance_frame(TICK)
	var night_vision: MeshInstance3D = water.get_night_vision_mesh_instance()
	assert_not_null(night_vision)
	assert_eq(night_vision.layers, 0, "no camera draws the redraw; only the Q3 pass")
	var beauty := _strip_arrays(water.get_mesh_instance())
	var redraw := _strip_arrays(night_vision)
	assert_false(beauty.is_empty(), "the beauty strip marches above water")
	assert_false(redraw.is_empty(), "the nightvision redraw marches above water")
	if beauty.is_empty() or redraw.is_empty():
		return
	assert_eq(PackedVector3Array(redraw[Mesh.ARRAY_VERTEX]),
			PackedVector3Array(beauty[Mesh.ARRAY_VERTEX]),
			"one march: the redraw shares the beauty geometry")
	var beauty_colors: PackedColorArray = beauty[Mesh.ARRAY_COLOR]
	var redraw_colors: PackedColorArray = redraw[Mesh.ARRAY_COLOR]
	var redraw_specular: PackedFloat32Array = redraw[Mesh.ARRAY_CUSTOM1]
	var beauty_specular: PackedFloat32Array = beauty[Mesh.ARRAY_CUSTOM1]
	var any_beauty_specular := false
	for index in redraw_colors.size():
		assert_lt(redraw_colors[index].r, beauty_colors[index].r + 0.0001,
				"the 0.1 base dims the nightvision brightness")
		assert_eq(redraw_specular[index * 4], 0.0, "the redraw drops the specular RGB")
		assert_eq(redraw_specular[index * 4 + 1], 0.0)
		assert_eq(redraw_specular[index * 4 + 2], 0.0)
		assert_eq(redraw_specular[index * 4 + 3], beauty_specular[index * 4 + 3],
				"the distance fog alpha is shared")
		any_beauty_specular = any_beauty_specular or beauty_specular[index * 4] > 0.0
	assert_true(any_beauty_specular, "the beauty rows do carry specular RGB")

	# Underwater the bloom pass's view-0 call has no side (retail
	# render_water_surface @ 0x5c3304): no redraw, while the beauty strip
	# swaps to the underwater side.
	cam.global_position = Vector3(100.3, 1.0, -33.7)
	water.advance_frame(TICK)
	assert_true(_strip_arrays(night_vision).is_empty(), "no nightvision redraw underwater")
	assert_false(_strip_arrays(water.get_mesh_instance()).is_empty(),
			"the underwater beauty side still marches")


func test_eye_exactly_on_the_plane_draws_neither_water_side() -> void:
	# render_water_surface's gates are strict on both sides: the view-0 call
	# skips cam.z <= wh (jle @ 0x5c330a) and the underwater call cam.z >= wh
	# (jge @ 0x5c32fc), so an eye exactly on the plane draws no surface.
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	cam.global_position = Vector3(100.3, 7.0, -33.7)
	water.advance_frame(TICK)
	assert_true(_strip_arrays(water.get_mesh_instance()).is_empty(),
			"no beauty strip at exact equality")
	assert_true(_strip_arrays(water.get_night_vision_mesh_instance()).is_empty(),
			"no nightvision redraw at exact equality")


func test_night_vision_redraw_is_not_gated_on_the_visible_terrain() -> void:
	# The beauty pass follows g_WaterActive, the bloom pass's redraw does not
	# (retail FrameFX_RenderGlowSource @ 0x582a59..0x582a5d).
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	water.set_visible_terrain_bounds(true, 100.0, 200.0)
	water.advance_frame(TICK)
	assert_true(_strip_arrays(water.get_mesh_instance()).is_empty(),
			"the inactive water pass clears the beauty strip")
	assert_false(_strip_arrays(water.get_night_vision_mesh_instance()).is_empty(),
			"the nightvision redraw still marches")


func test_reflected_scene_mirrors_above_the_plane_and_keeps_the_live_eye_below() -> void:
	# render_main_scene mirrors the camera block only while the camera is at or
	# above the water (z' = 2wh - z, pitch/roll negated) and copies it
	# unchanged below (retail render_main_scene @ 0x5c1361..0x5c1370 jl,
	# @ 0x5c13f6..0x5c1414). Below the plane the collectors run unfiltered.
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	cam.rotation_degrees = Vector3(-20.0, 30.0, 0.0)
	water.advance_frame(TICK)
	var mirror: Camera3D = water.get_reflection_camera()
	assert_almost_eq(mirror.global_position.y, 2.0 * 7.0 - 27.0, 0.0001,
			"above the plane the eye reflects about the water")
	assert_almost_eq(mirror.global_basis.z.y, -cam.global_basis.z.y, 0.0001,
			"the mirrored forward flips its vertical component")
	assert_eq(mirror.cull_mask, Water.REFLECTION_CULL_MASK)

	cam.global_position = Vector3(100.3, 1.0, -33.7)
	water.advance_frame(TICK)
	assert_true(mirror.global_position.is_equal_approx(cam.global_position),
			"below the plane the reflected scene renders from the live eye")
	assert_true(mirror.global_basis.is_equal_approx(cam.global_basis),
			"below the plane the reflected scene keeps the live orientation")
	assert_eq(mirror.cull_mask,
			Water.REFLECTION_CULL_MASK | Water.VISUAL_LAYER_WORLD_NO_MIRROR | (1 << 22),
			"below the plane the reflected collectors run unfiltered")
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_WATER, 0,
			"the reflected pass never draws the water layer on either side")


func _depth_fixture(camera_position: Vector3, pitch_deg: float) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var water := Water.new()
	viewport.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.global_position = camera_position
	cam.rotation_degrees = Vector3(pitch_deg, 0.0, 0.0)
	# The retail scene far is the fog distance's integer word + 1 over the
	# envless water's 1000-unit fog (Render_ProcessMainSceneFrame
	# @ 0x5CA4BA..0x5CA4D0), near 0.2.
	cam.near = 0.2
	cam.far = 1001.0
	cam.make_current()
	return {"viewport": viewport, "water": water, "camera": cam}


func _render_water_frame(fixture: Dictionary, fog_color: Vector3,
		water_color := Vector3(0.408, 0.314, 0.224)) -> Image:
	var water: Water = fixture["water"]
	water.advance_frame(TICK)
	var material := water.get_water_material()
	material.set_shader_parameter("u_has_reflection", false)
	material.set_shader_parameter("u_fog_color", fog_color)
	material.set_shader_parameter("u_water_color", water_color)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		return null
	return (fixture["viewport"] as SubViewport).get_texture().get_image()


func test_water_beyond_the_far_plane_reaches_the_horizon() -> void:
	# The pre-transformed strip is never far-clipped: rows past the scene far
	# plane clamp to the viewport MaxZ (retail render_water_strip_detailed
	# @ 0x5c2c1f..0x5c2c4a) and draw fully fogged water up to the horizon.
	var fixture := _depth_fixture(Vector3(0.0, 107.0, 0.0), -5.0)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var cam: Camera3D = fixture["camera"]
	# The strip's first row is the plane point 2000 units ahead
	# (terrain_project_sector_to_screen @ 0x5c0c7c..0x5c0d25); 1500 units out
	# lies between the 1001 far plane and that row.
	var far_px := cam.unproject_position(Vector3(0.0, 7.0, -1500.0))
	var far_pixel := image.get_pixelv(Vector2i(far_px))
	assert_gt(far_pixel.g, 0.5,
			"water 1500 units out (past the 1001 far plane) draws fogged: %s" % far_pixel)
	water.release_runtime_renderer_resources()


func test_water_loses_a_depth_tie_to_geometry_on_its_plane() -> void:
	# The strip depth replicates the scene curve with far = w while the scene
	# draws at far = w + 1, so the water sits just behind its true depth and a
	# surface exactly on the plane wins the LESSEQUAL test (retail depth chain
	# @ 0x5c2c0c..0x5c2c4a against Render_ProcessMainSceneFrame
	# @ 0x5CA4BA..0x5CA4D0).
	var fixture := _depth_fixture(Vector3(0.0, 27.0, 20.0), -45.0)
	var quad := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40.0, 40.0)
	quad.mesh = plane
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color.RED
	quad.material_override = red
	quad.position = Vector3(0.0, 7.0, 0.0)
	(fixture["viewport"] as SubViewport).add_child(quad)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	var cam: Camera3D = fixture["camera"]
	var pixel := image.get_pixelv(Vector2i(cam.unproject_position(Vector3(0.0, 7.0, 0.0))))
	assert_gt(pixel.r, 0.9, "the coplanar surface keeps its pixel: %s" % pixel)
	assert_lt(pixel.g, 0.1, "the water behind it is depth-rejected: %s" % pixel)
	water.release_runtime_renderer_resources()


func test_water_depth_follows_the_retail_scene_curve_under_any_camera_near() -> void:
	# The replica depth is tested against retail's scene curve (near 0.2,
	# far = w + 1), whatever near plane the Godot camera carries: a wall
	# standing in the water keeps the pixels just above the waterline, where
	# the water behind it lies only a little farther than the wall.
	var fixture := _depth_fixture(Vector3(0.0, 9.0, 0.0), -10.0)
	var cam: Camera3D = fixture["camera"]
	cam.near = 0.05
	var wall := MeshInstance3D.new()
	var wall_mesh := QuadMesh.new()
	wall_mesh.size = Vector2(40.0, 10.0)
	wall.mesh = wall_mesh
	var red := StandardMaterial3D.new()
	red.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	red.albedo_color = Color.RED
	wall.material_override = red
	wall.position = Vector3(0.0, 7.0, -20.0)
	(fixture["viewport"] as SubViewport).add_child(wall)
	var image: Image = await _render_water_frame(fixture, Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0))
	var water: Water = fixture["water"]
	if image == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		water.release_runtime_renderer_resources()
		return
	# 0.5 above the water, 20 out: the ray behind it meets the water ~27 out.
	var pixel := image.get_pixelv(Vector2i(cam.unproject_position(Vector3(0.0, 7.5, -20.0))))
	assert_gt(pixel.r, 0.9, "the wall keeps its pixel above the waterline: %s" % pixel)
	assert_lt(pixel.g, 0.1, "the water behind the wall is depth-rejected: %s" % pixel)
	water.release_runtime_renderer_resources()


func test_wake_rings_face_up_and_hide_from_below() -> void:
	# The wake pass flags (0x100000, retail WaterRing_Draw
	# @ 0x5DE245) carry no cull-none bit, so the rings keep the device's
	# back-face cull. A card wound like the compiled ring (its right-hand
	# normal down, renderer/water_wake_frame.cpp) draws for an eye above it
	# and not for one below.
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/water_wake.gdshader") as Shader
	var white := ImageTexture.create_from_image(
			Image.create_from_data(1, 1, false, Image.FORMAT_RGBA8,
					PackedByteArray([255, 255, 255, 255])))
	material.set_shader_parameter("wake_texture", white)
	material.set_shader_parameter("gradient_texture", white)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-1.0, 0.0, -1.0), Vector3(1.0, 0.0, -1.0),
		Vector3(1.0, 0.0, 1.0), Vector3(-1.0, 0.0, 1.0)])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
	arrays[Mesh.ARRAY_TEX_UV2] = arrays[Mesh.ARRAY_TEX_UV]
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var above: Image = await _render_wake_card(mesh, material, 5.0)
	var below: Image = await _render_wake_card(mesh, material, -5.0)
	if above == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	assert_gt(above.get_pixel(32, 32).r, 0.7, "an eye above sees the ring (0.4 x 2)")
	assert_lt(below.get_pixel(32, 32).r, 0.02, "an eye below the water sees none")


func _render_wake_card(mesh: ArrayMesh, material: ShaderMaterial, eye_y: float) -> Image:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var card := MeshInstance3D.new()
	card.mesh = mesh
	card.material_override = material
	viewport.add_child(card)
	var cam := Camera3D.new()
	viewport.add_child(cam)
	cam.position = Vector3(0.0, eye_y, 0.0)
	cam.rotation_degrees = Vector3(-90.0 if eye_y > 0.0 else 90.0, 0.0, 0.0)
	cam.make_current()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	if RenderingServer.get_rendering_device() == null:
		return null
	return viewport.get_texture().get_image()
