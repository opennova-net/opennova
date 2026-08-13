extends GutTest

# D-TERRAIN-8: the camera-below-water terrain modulation plumbing. The engine
# compiler stamps TerrainDrawList.below_water from the RENDER eye vs the live
# water height (strict <, 0 = the retail no-water sentinel), and the Terrain
# node pushes the flag plus the water module's live noise texture onto the
# shared surface material [orig: cameraY < Env_WaterHeightFixed @ 0x60FEE0 ->
# dword_319FB3C @ 0x60915F; live t3 slot swap @ 0x6043f2]. The shader-side
# math contract lives in terrain_shader_contract_test.gd.

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"
const TICK := 1.0 / 62.0


func _make_fixture(water_height: float) -> Dictionary:
	var vp := SubViewport.new()
	vp.size = Vector2i(320, 180)
	add_child_autofree(vp)

	var data := TerrainData.new()
	data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(data.load(), OK, "the Dvxi5 fixture terrain must load")

	var terrain: Terrain = Terrain.new()
	vp.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()

	var water: Node = Water.new()
	vp.add_child(water)
	water.water_height = water_height
	water.advance_frame(TICK)
	terrain.water_path = terrain.get_path_to(water)

	var cam := Camera3D.new()
	vp.add_child(cam)
	cam.make_current()
	return {"terrain": terrain, "water": water, "camera": cam}


func test_render_eye_height_flips_the_below_water_uniform() -> void:
	var fixture := _make_fixture(7.0)
	var terrain: Terrain = fixture["terrain"]
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	assert_true(terrain.get_terrain_material() != null,
			"the Dvxi5 build must produce the shared surface material")

	cam.global_position = Vector3(64.0, 27.0, 64.0)
	terrain.render_frame()
	var material: ShaderMaterial = terrain.get_terrain_material()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"a camera above the surface reads dry")

	cam.global_position = Vector3(64.0, 3.0, 64.0)
	terrain.render_frame()
	assert_true(bool(material.get_shader_parameter("u_below_water")),
			"a camera below the surface sets the flag")
	assert_eq(material.get_shader_parameter("u_water_noise"),
			water.get_noise_color_texture(),
			"the terrain material binds the water module's LIVE noise texture")

	# The classification samples the RENDER eye (get_camera_transform), so a
	# v_offset alone must flip it — the same contract the frame clear pins
	# (game_world_test.gd waterline cases).
	cam.global_position = Vector3(64.0, 8.0, 64.0)
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"one unit above the surface reads dry before the offset")
	cam.v_offset = -2.0
	terrain.render_frame()
	assert_true(bool(material.get_shader_parameter("u_below_water")),
			"a v_offset dropping only the render eye below must flip the flag")
	cam.v_offset = 0.0


func test_zero_height_sentinel_and_missing_water_node_read_dry() -> void:
	var fixture := _make_fixture(0.0)
	var terrain: Terrain = fixture["terrain"]
	var cam: Camera3D = fixture["camera"]
	var material: ShaderMaterial = terrain.get_terrain_material()

	cam.global_position = Vector3(64.0, -5.0, 64.0)
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")),
			"height 0 is the no-water sentinel even with a sunken camera")

	# Unwire the water node entirely: the feed reads dry and the noise unbinds.
	terrain.water_path = NodePath()
	terrain.render_frame()
	assert_false(bool(material.get_shader_parameter("u_below_water")))
	var unbound = material.get_shader_parameter("u_water_noise")
	assert_true(unbound == null or not (unbound is Texture2D),
			"no water node leaves no live noise texture bound")
