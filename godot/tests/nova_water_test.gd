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


func _make_water_fixture() -> Dictionary:
	var strip_vp := SubViewport.new()
	strip_vp.size = Vector2i(1024, 600)
	add_child_autofree(strip_vp)
	var water: Node = NovaWaterScript.new()
	strip_vp.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	strip_vp.add_child(cam)
	cam.global_position = Vector3(100.3, 27.0, -33.7)
	cam.make_current()
	return {"water": water, "camera": cam, "viewport": strip_vp}

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
	cam.fov = NovaSimulation.fov_vertical_from_horizontal(
			72.0, float(strip_vp.size.x) / float(strip_vp.size.y))
	simulate(water, 1, TICK)
	assert_eq(water.reflection_viewport.size, Vector2i(256, 256),
			"display resizing must not resize Water_ReflectionTexture")
	assert_almost_eq(water.reflection_camera.fov, 72.0, 0.001,
			"the new source aspect still projects the same horizontal field")
	uv_scale = water.water_material.get_shader_parameter("u_reflection_uv_scale")
	assert_almost_eq(uv_scale.y, 900.0 / 1600.0, 0.000001,
			"display resizing refreshes the source-to-square V correction")


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
