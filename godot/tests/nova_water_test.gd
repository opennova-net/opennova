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

	strip_vp.size = Vector2i(1600, 900)
	cam.fov = NovaSimulation.fov_vertical_from_horizontal(
			72.0, float(strip_vp.size.x) / float(strip_vp.size.y))
	simulate(water, 1, TICK)
	assert_eq(water.reflection_viewport.size, Vector2i(256, 256),
			"display resizing must not resize Water_ReflectionTexture")
	assert_almost_eq(water.reflection_camera.fov, 72.0, 0.001,
			"the new source aspect still projects the same horizontal field")


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
