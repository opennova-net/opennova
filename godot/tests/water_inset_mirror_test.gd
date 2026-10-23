extends GutTest

# The weapon Inset pass's own water (engine environment/water_mirror.h
# inset_mirror_source and bloom_water_mirror_image, environment/water_frame.h
# water_pass_latch carry the witnesses): its g_WaterActive over its own view's
# visible terrain and the latch the main collect left, its own mirror of the
# view block (the Inset's compose without its slot offsets) at its fov and
# aspect 1.0, its own strip on its bit sampling that mirror, and the bloom's
# water redraw sampling the last mirror the frame rendered. Node state only:
# headless.

const INSET_VIEW := 1 << 21
const MAIN_VIEW_WATER := 1 << 23
const TICK := 1.0 / 62.0


func _fixture() -> Dictionary:
	var view := SubViewport.new()
	view.size = Vector2i(256, 192)
	add_child_autofree(view)
	var water := Water.new()
	view.add_child(water)
	water.water_height = 7.0
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.make_current()
	camera.global_transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-20.0), 0.0, 0.0)),
			Vector3(100.3, 27.0, -33.7))
	var inset_view := SubViewport.new()
	inset_view.size = Vector2i(96, 96)
	view.add_child(inset_view)
	var inset := Camera3D.new()
	inset.keep_aspect = Camera3D.KEEP_WIDTH
	inset.fov = 9.5
	inset.near = 0.2
	inset_view.add_child(inset)
	inset.make_current()
	# The view block is the Inset's compose; the Inset camera adds its slot
	# offsets (a pitch and a yaw) to it.
	var block := Transform3D(Basis.from_euler(Vector3(deg_to_rad(-20.0), 0.0, 0.0)),
			Vector3(100.3, 27.0, -33.7))
	inset.global_transform = Transform3D(
			Basis.from_euler(Vector3(deg_to_rad(-22.0), deg_to_rad(1.5), 0.0)), block.origin)
	water.advance_frame(TICK)
	return {"water": water, "camera": camera, "inset": inset, "block": block}


func _vertices(instance: MeshInstance3D) -> PackedVector3Array:
	var mesh := instance.mesh as ArrayMesh
	if mesh == null or mesh.get_surface_count() == 0:
		return PackedVector3Array()
	return PackedVector3Array(mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX])


func test_the_inset_mirrors_its_view_block_square_at_its_fov() -> void:
	var fixture := _fixture()
	var water: Water = fixture["water"]
	var block: Transform3D = fixture["block"]
	water.set_inset_visible_terrain_bounds(true, 0.0, 30.0)
	water.advance_inset_frame(fixture["inset"], block)
	assert_true(water.is_inset_water_pass_active())
	assert_true(water.is_inset_mirror_rendered())
	var target: SubViewport = water.get_inset_reflection_viewport()
	assert_ne(target, water.get_reflection_viewport(), "the Inset's own target")
	assert_eq(target.render_target_update_mode, SubViewport.UPDATE_ALWAYS)
	var mirror: Camera3D = water.get_inset_reflection_camera()
	# z' = 2 wh - z, yaw kept, pitch negated: the view block's mirror, so the
	# Inset camera's slot offsets stay out of it.
	var eye := mirror.global_transform.origin
	assert_almost_eq(eye.y, 2.0 * 7.0 - 27.0, 0.001)
	assert_almost_eq(eye.x, 100.3, 0.001)
	var forward := -mirror.global_transform.basis.z
	var block_forward := -block.basis.z
	assert_almost_eq(forward.y, -block_forward.y, 0.001)
	assert_almost_eq(forward.x, block_forward.x, 0.001, "no yaw offset in the mirror")
	assert_almost_eq(forward.z, block_forward.z, 0.001)
	# The Inset's fov across the width, over the square target.
	assert_almost_eq(mirror.fov, 9.5, 0.001)
	assert_eq(mirror.keep_aspect, Camera3D.KEEP_WIDTH)
	assert_eq(target.size_2d_override, Vector2i(512, 512))
	# Its population is the main mirror's (the main view's verdicts).
	assert_eq(mirror.cull_mask, Water.REFLECTION_CULL_MASK)
	water.release_runtime_renderer_resources()


func test_the_inset_strip_rides_its_own_bit_over_its_own_mirror() -> void:
	var fixture := _fixture()
	var water: Water = fixture["water"]
	water.set_inset_visible_terrain_bounds(true, 0.0, 30.0)
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	var strip: MeshInstance3D = water.get_inset_mesh_instance()
	assert_eq(strip.layers, INSET_VIEW)
	var rows := _vertices(strip)
	assert_gt(rows.size(), 0, "the Inset marches its own rows")
	assert_eq(water.get_mesh_instance().layers, MAIN_VIEW_WATER,
			"the main view's strip is its own")
	assert_ne(rows, _vertices(water.get_mesh_instance()),
			"the Inset's rows are marched at its own screen")
	var material := water.get_inset_water_material()
	assert_eq((strip.mesh as ArrayMesh).surface_get_material(0), material)
	assert_eq(material.get_shader_parameter("u_reflection"),
			water.get_inset_reflection_viewport().get_texture())
	assert_true(bool(material.get_shader_parameter("u_has_reflection")))
	water.release_runtime_renderer_resources()


func test_no_inset_mirror_or_strip_without_its_water_pass() -> void:
	var fixture := _fixture()
	var water: Water = fixture["water"]
	# Every visible sector above the water and the latch clear: the Inset's
	# g_WaterActive is down, so no prerender and no strip.
	water.set_blink_water_visible(false)
	water.open_inset_water_latch()
	water.latch_blink_water(true, true)
	water.set_inset_visible_terrain_bounds(true, 20.0, 40.0)
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	assert_false(water.is_inset_water_pass_active())
	assert_false(water.is_inset_mirror_rendered())
	assert_eq(water.get_inset_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_eq(_vertices(water.get_inset_mesh_instance()).size(), 0)
	# The latch as the main collect left it forces it (the Inset collect's own
	# store came after the Inset's test read it).
	water.set_blink_water_visible(true)
	water.open_inset_water_latch()
	water.latch_blink_water(true, false)
	assert_false(water.get_blink_water_latch(), "the Inset collect's store")
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	assert_true(water.is_inset_water_pass_active())
	assert_true(water.is_inset_mirror_rendered())
	# The Inset down retires it all.
	water.advance_inset_frame(null, Transform3D())
	assert_false(water.is_inset_mirror_rendered())
	assert_eq(water.get_inset_reflection_viewport().render_target_update_mode,
			SubViewport.UPDATE_DISABLED)
	assert_eq(_vertices(water.get_inset_mesh_instance()).size(), 0)
	assert_eq(water.get_inset_mesh_instance().layers, 0)
	water.release_runtime_renderer_resources()


func test_the_bloom_water_samples_the_last_mirror_rendered() -> void:
	var fixture := _fixture()
	var water: Water = fixture["water"]
	var night_vision := water.get_night_vision_material()
	assert_ne(night_vision, water.get_water_material(), "the redraw's own material")
	assert_eq((water.get_night_vision_mesh_instance().mesh as ArrayMesh).surface_get_material(0),
			night_vision)
	assert_eq(night_vision.get_shader_parameter("u_reflection"),
			water.get_reflection_viewport().get_texture(), "the main frame's image")
	water.set_inset_visible_terrain_bounds(true, 0.0, 30.0)
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	assert_eq(night_vision.get_shader_parameter("u_reflection"),
			water.get_inset_reflection_viewport().get_texture(),
			"the Inset rendered its mirror after the main scene's water: the bloom samples it")
	assert_true(bool(night_vision.get_shader_parameter("u_has_reflection")))
	# The main material keeps the main frame's image.
	assert_eq(water.get_water_material().get_shader_parameter("u_reflection"),
			water.get_reflection_viewport().get_texture())
	water.advance_inset_frame(null, Transform3D())
	assert_eq(night_vision.get_shader_parameter("u_reflection"),
			water.get_reflection_viewport().get_texture())
	water.release_runtime_renderer_resources()


func test_the_latch_is_one_word_for_both_scenes() -> void:
	var fixture := _fixture()
	var water: Water = fixture["water"]
	water.set_visible_terrain_bounds(true, 20.0, 40.0)
	water.set_blink_water_visible(false)
	water.latch_blink_water(true, true)
	assert_true(water.get_blink_water_latch(), "the main collect's water pass stores its verdict")
	assert_true(water.is_water_pass_active())
	water.latch_blink_water(false, false)
	assert_true(water.get_blink_water_latch(), "a letter-skipped pass leaves the latch")
	water.open_inset_water_latch()
	water.latch_blink_water(true, false)
	assert_false(water.get_blink_water_latch(), "the Inset collect's store")
	assert_false(water.is_water_pass_active(),
			"the next main scene reads the Inset collect's verdict")
	water.set_inset_visible_terrain_bounds(true, 20.0, 40.0)
	assert_true(water.is_inset_water_pass_active(),
			"the Inset's own test read the main collect's verdict before its store")
	water.release_runtime_renderer_resources()
