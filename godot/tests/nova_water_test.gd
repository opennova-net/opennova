extends GutTest

# =============================================================================
# NovaWater surface pins (env #29, docs/env/env-tod-re.md "Water surface"):
# the witnessed per-side material swap (camera-above -> the BLEND material,
# underwater -> the OPAQUE one [orig: selection in render_water_surface
# @ 0x5c33e6..0x5c34ea; Water_ShaderOpaque @ 0x28ee8c8]) and the retirement of
# the invented terrain-fog-curve uniforms (the water fog is the per-row
# spec-alpha factor). ADR 0018 discipline: everything pins through PUBLIC
# seams only — exported/public properties and shader parameters.
#
# Fixture per env_parity_vectors_test.gd _collect_water_mesh: the strip march
# is a function of VIEWPORT PIXELS, so it lives in a code-fixed SubViewport
# (1024x600). NovaWater resolves its camera through its OWN viewport —
# Camera3D.current applies per-viewport — so the camera is created inside the
# SubViewport and made current there.
# =============================================================================

const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const WATER_SHADER := "res://shaders/water.gdshader"

# The engine tick [docs/engine-primer.md: 62 Hz].
const TICK := 1.0 / 62.0


func _make_water_fixture(height: float = 7.0) -> Dictionary:
	var strip_vp := SubViewport.new()
	strip_vp.size = Vector2i(1024, 600)
	add_child_autofree(strip_vp)
	var water: Node = NovaWaterScript.new()
	strip_vp.add_child(water)
	water.water_height = height
	var cam := Camera3D.new()
	strip_vp.add_child(cam)
	cam.global_position = Vector3(100.3, 27.0, -33.7)
	cam.make_current()
	return {"water": water, "camera": cam, "viewport": strip_vp}


func test_zero_height_disables_surface_mirror_and_world_split() -> void:
	var cache := NovaObjectShaderCache.get_singleton()
	cache.clear_water_split_height()
	var fixture := _make_water_fixture(0.0)
	var water: Node = fixture["water"]
	simulate(water, 1, TICK)

	assert_false(water.is_water_active(), "zero is the retail no-water sentinel")
	assert_false(water.mesh_instance.visible)
	assert_eq(water.reflection_viewport.render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(bool(water.water_material.get_shader_parameter("u_has_reflection")))
	assert_false(cache.has_water_split_height())

	water.water_height = -2.0
	simulate(water, 1, TICK)
	assert_true(water.is_water_active(), "signed nonzero water heights are valid")
	assert_true(water.is_water_render_active())
	assert_true(water.mesh_instance.visible)
	assert_eq(water.reflection_viewport.render_target_update_mode,
			SubViewport.UPDATE_ALWAYS)
	assert_true(cache.has_water_split_height())

	water.set_world_rendering_enabled(false)
	simulate(water, 1, TICK)
	assert_true(water.is_water_active(),
			"retained authored height survives an unloaded owner")
	assert_false(water.is_water_render_active(),
			"owner lifecycle gates render consumers independently of height")
	assert_false(water.mesh_instance.visible)
	assert_eq(water.reflection_viewport.render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(cache.has_water_split_height())

	water.set_world_rendering_enabled(true)
	water.water_height = 0.0
	simulate(water, 1, TICK)
	assert_false(water.is_water_active())
	assert_eq(water.reflection_viewport.render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_false(cache.has_water_split_height())


func test_height_precedence_is_direct_then_bms_then_signed_trn_then_env() -> void:
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/minimal/resources")), OK)
	var terrain := NovaTerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, "mnml.trn"), OK)
	terrain.set_water_height(-20) # engine half-units -> -10 world units

	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_water_height(12.0) # engine half-units -> 6 world units
	var env := NovaEnvironment.new()
	env.name = "WaterPrecedenceEnv"
	env.environment_data = env_data
	add_child_autofree(env)

	var water: Node = NovaWaterScript.new()
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
	water.set_height_override(30.0)
	assert_eq(water.water_height, 30.0)
	water.set_height_override(NAN)
	assert_eq(water.water_height, 15.0)
	water.set_mission_water_height_override(NAN)
	assert_eq(water.water_height, -10.0)

	terrain.set_water_height(0)
	water.terrain_data = terrain
	assert_eq(water.water_height, 6.0, "ENV is the final fallback")

func test_reflection_rtt_is_retail_square_and_preserves_horizontal_fov() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]
	var cam: Camera3D = fixture["camera"]

	cam.fov = NovaSimulation.fov_vertical_from_horizontal(
			72.0, float(strip_vp.size.x) / float(strip_vp.size.y))
	simulate(water, 1, TICK)
	assert_eq(water.reflection_viewport.size, Vector2i(256, 256),
			"retail detail 2 allocates a fixed square RTT [orig: sub_5C08B0 @ 0x5c08d1]")
	assert_almost_eq(water.reflection_camera.fov, 72.0, 0.001,
			"square projection preserves the source horizontal FOV")
	var uv_scale: Vector2 = water.water_material.get_shader_parameter(
			"u_reflection_uv_scale")
	assert_almost_eq(uv_scale.x, 1.0, 0.000001,
			"preserved horizontal FOV needs no reflection-U correction")
	assert_almost_eq(uv_scale.y, 600.0 / 1024.0, 0.000001,
			"reflection V converts from the source projection to the square RTT")

	strip_vp.size = Vector2i(1600, 900)
	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.fov = 72.0
	simulate(water, 1, TICK)
	assert_eq(water.reflection_viewport.size, Vector2i(256, 256),
			"display resizing must not resize Water_ReflectionTexture")
	assert_almost_eq(water.reflection_camera.fov, 72.0, 0.001,
			"the new source aspect still projects the same horizontal field")
	uv_scale = water.water_material.get_shader_parameter("u_reflection_uv_scale")
	assert_almost_eq(uv_scale.y, 900.0 / 1600.0, 0.000001,
			"display resizing refreshes the source-to-square V correction")


func test_reflection_camera_matches_orthogonal_and_frustum_sources() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]

	cam.rotation_degrees = Vector3(-20.0, 0.0, 0.0)
	cam.keep_aspect = Camera3D.KEEP_HEIGHT
	cam.set_orthogonal(90.0, 0.25, 500.0)
	cam.h_offset = 2.5
	cam.v_offset = -1.25
	simulate(water, 1, TICK)
	assert_eq(water.reflection_camera.projection, Camera3D.PROJECTION_ORTHOGONAL)
	assert_eq(water.reflection_camera.keep_aspect, Camera3D.KEEP_WIDTH)
	assert_almost_eq(water.reflection_camera.size, 90.0 * 1024.0 / 600.0,
			0.0001, "square mirror preserves the orthographic horizontal span")
	assert_almost_eq(water.reflection_camera.near, 0.25, 0.000001)
	assert_almost_eq(water.reflection_camera.far, 500.0, 0.000001)
	assert_almost_eq(water.reflection_camera.h_offset, 2.5, 0.000001)
	assert_almost_eq(water.reflection_camera.v_offset, 1.25, 0.000001,
			"the mirror-up basis requires the vertical camera offset to flip")
	_assert_reflection_projection_registered(
			water, cam, fixture["viewport"], Vector3(100.3, 12.0, -133.7))

	cam.set_frustum(45.0, Vector2(3.0, -4.0), 0.5, 800.0)
	simulate(water, 1, TICK)
	assert_eq(water.reflection_camera.projection, Camera3D.PROJECTION_FRUSTUM)
	assert_eq(water.reflection_camera.keep_aspect, Camera3D.KEEP_WIDTH)
	assert_almost_eq(water.reflection_camera.size, 45.0 * 1024.0 / 600.0,
			0.0001, "square mirror preserves the frustum horizontal span")
	assert_eq(water.reflection_camera.frustum_offset, Vector2(3.0, 4.0),
			"the off-axis frustum follows the mirror's vertical flip")
	assert_almost_eq(water.reflection_camera.near, 0.5, 0.000001)
	assert_almost_eq(water.reflection_camera.far, 800.0, 0.000001)
	assert_almost_eq(water.reflection_camera.h_offset, 2.5, 0.000001)
	assert_almost_eq(water.reflection_camera.v_offset, 1.25, 0.000001)
	_assert_reflection_projection_registered(
			water, cam, fixture["viewport"], Vector3(100.3, 12.0, -133.7))

	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.set_frustum(30.0, Vector2(-2.0, 1.5), 0.75, 900.0)
	simulate(water, 1, TICK)
	assert_almost_eq(water.reflection_camera.size, 30.0 * 1024.0 / 600.0,
			0.0001, "frustum size remains vertical even with KEEP_WIDTH")
	assert_eq(water.reflection_camera.frustum_offset, Vector2(-2.0, -1.5))
	_assert_reflection_projection_registered(
			water, cam, fixture["viewport"], Vector3(100.3, 12.0, -133.7))
	var uv_scale: Vector2 = water.water_material.get_shader_parameter(
			"u_reflection_uv_scale")
	assert_almost_eq(uv_scale.y, 600.0 / 1024.0, 0.000001,
			"all projection modes share the source-to-square V correction")


func test_collapsed_source_viewport_never_builds_an_invalid_mirror_projection() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]

	# Workspace layout can briefly expose a very thin but nonzero viewport.
	# Its horizontal FOV approaches 180 degrees, beyond Camera3D's legal
	# range, even though the source camera itself remains ordinary.
	strip_vp.size = Vector2i(1024, 2)
	simulate(water, 1, TICK)
	assert_almost_eq(water.reflection_camera.fov, 179.0, 0.001,
			"a drawable extreme aspect is bounded to Camera3D's legal FOV")

	strip_vp.size = Vector2i(1024, 1)
	simulate(water, 1, TICK)
	assert_eq((water.mesh_instance.mesh as ArrayMesh).get_surface_count(), 0,
			"the strip and mirror share the same collapsed-viewport gate")
	assert_eq(water.reflection_viewport.render_target_update_mode,
			SubViewport.UPDATE_DISABLED,
			"a collapsed editor view must not spend a stale reflection pass")
	assert_false(bool(water.water_material.get_shader_parameter("u_has_reflection")))


func test_surface_and_mirror_follow_a_new_current_camera() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var viewport: SubViewport = fixture["viewport"]
	var old_camera: Camera3D = fixture["camera"]
	simulate(water, 1, TICK)

	var new_camera := Camera3D.new()
	viewport.add_child(new_camera)
	new_camera.global_position = Vector3(-240.0, 42.0, 315.0)
	new_camera.make_current()
	simulate(water, 1, TICK)

	assert_false(old_camera.current)
	assert_true(new_camera.current)
	assert_eq(water.reflection_camera.global_position,
			Vector3(new_camera.global_position.x,
					2.0 * water.water_height - new_camera.global_position.y,
					new_camera.global_position.z),
			"strip tessellation and the mirror RTT reacquire the viewport's current camera")


func test_reflection_lookup_stays_registered_while_view_rotates() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var strip_vp: SubViewport = fixture["viewport"]
	var cam: Camera3D = fixture["camera"]
	cam.fov = NovaSimulation.fov_vertical_from_horizontal(
			72.0, float(strip_vp.size.x) / float(strip_vp.size.y))
	var real_world_point := Vector3(100.3, 12.0, -133.7)

	simulate(water, 1, TICK)
	_assert_reflection_projection_registered(
			water, cam, strip_vp, real_world_point)

	cam.rotation_degrees = Vector3(-12.0, 20.0, 0.0)
	simulate(water, 1, TICK)
	_assert_reflection_projection_registered(
			water, cam, strip_vp, real_world_point)


func test_reflection_shader_applies_square_projection_scale() -> void:
	var shader := load(WATER_SHADER) as Shader
	assert_true(shader.code.contains(
			"refl_uv = vec2(0.5) + (refl_uv - vec2(0.5)) * u_reflection_uv_scale;"),
			"the witnessed row result must enter the square mirror's projection before sampling")


func _assert_reflection_projection_registered(water: Node, cam: Camera3D,
		source_viewport: SubViewport, world_point: Vector3) -> void:
	# A planar reflection projects a real point through the mirror camera at
	# the same location where the main camera sees its virtual point below the
	# water plane. Compare those two PUBLIC camera projections; this catches
	# view-dependent swimming without requiring a raster readback.
	var virtual_point := world_point
	virtual_point.y = 2.0 * water.water_height - world_point.y
	assert_false(cam.is_position_behind(virtual_point),
			"the virtual reflection point must be in front of the source camera")
	assert_false(water.reflection_camera.is_position_behind(world_point),
			"the real point must be in front of the mirror camera")
	var source_size := Vector2(float(source_viewport.size.x),
			float(source_viewport.size.y))
	var mirror_size := Vector2(float(water.reflection_viewport.size.x),
			float(water.reflection_viewport.size.y))
	var main_uv := cam.unproject_position(virtual_point) / source_size
	var raw_row_uv := Vector2(main_uv.x, 1.0 - main_uv.y)
	var uv_scale: Vector2 = water.water_material.get_shader_parameter(
			"u_reflection_uv_scale")
	var sampled_uv := Vector2(0.5, 0.5) + (
			raw_row_uv - Vector2(0.5, 0.5)) * uv_scale
	var mirror_uv: Vector2 = (
			water.reflection_camera.unproject_position(world_point) / mirror_size)
	var one_rtt_texel := 1.0 / float(water.reflection_viewport.size.x)
	assert_almost_eq(sampled_uv.x, mirror_uv.x, one_rtt_texel,
			"reflection U stays registered to the mirrored world point")
	assert_almost_eq(sampled_uv.y, mirror_uv.y, one_rtt_texel,
			"reflection V stays registered to the mirrored world point")

	# Exercise the exact strip payload consumed by the shader too. Sample each
	# row's LEFT vertex because vbase is built from that vertex's rhw and then
	# copied across the row.
	var mesh := water.mesh_instance.mesh as ArrayMesh
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
		var q := minf(297.0 * custom0[base + 1] + 0.15, 2.0)
		var vbase_bias := q / 256.0
		var raw_strip_uv := Vector2(custom0[base + 2], custom0[base + 3])
		var sampled_strip_uv := Vector2(0.5, 0.5) + (
				raw_strip_uv - Vector2(0.5, 0.5)) * uv_scale
		var mirror_strip_uv: Vector2 = (
				water.reflection_camera.unproject_position(positions[vertex_index])
				/ mirror_size)
		assert_almost_eq(sampled_strip_uv.x, mirror_strip_uv.x, one_rtt_texel,
				"strip reflection U stays registered to its mirrored water point")
		assert_almost_eq(sampled_strip_uv.y,
				mirror_strip_uv.y - vbase_bias * uv_scale.y, one_rtt_texel,
				"strip reflection V keeps its witnessed vbase bias after remapping")


func test_above_water_renders_blend_side() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	simulate(water, 1, TICK)
	var mesh: ArrayMesh = water.mesh_instance.mesh
	assert_gt(mesh.get_surface_count(), 0,
			"camera above the plane must march strip rows")
	assert_eq(water.water_material.get_shader_parameter("u_underwater_view"), false,
			"camera above -> the BLEND material side [orig: selection @ 0x5c33e6..0x5c34ea]")


func test_underwater_swaps_to_opaque_side() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	simulate(water, 1, TICK)
	# Drop the camera below the 7.0 plane: the pass swaps to the OPAQUE
	# material [orig: Water_ShaderOpaque @ 0x28ee8c8; selection
	# @ 0x5c33e6..0x5c34ea].
	cam.global_position = Vector3(100.3, 1.0, -33.7)
	simulate(water, 1, TICK)
	assert_eq(water.water_material.get_shader_parameter("u_underwater_view"), true,
			"camera below -> the OPAQUE material side")


func test_water_material_has_no_far_discard_uniforms() -> void:
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	simulate(water, 1, TICK)
	assert_not_null(water.water_material.shader, "the water shader must be loaded")
	# ShaderMaterial.get_shader_parameter returns null for unknown/unset names:
	# the retired terrain-fog-curve uniform must be GONE and the per-side swap
	# uniform PRESENT (fed by the strip rebuild).
	assert_null(water.water_material.get_shader_parameter("u_fog_type"),
			"u_fog_type retired: water fog rides the per-row spec-alpha factor")
	assert_not_null(water.water_material.get_shader_parameter("u_underwater_view"),
			"u_underwater_view must be fed by the strip rebuild")
