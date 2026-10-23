extends GutTest

# Each view's water letter label: the main collect's and the Inset scene
# core's own collect's g_BlinkWaterVisible (engine world/occlusion.h
# OcclusionView carries the witness). A label gates its view's strip alone
# (engine renderer/scene_pass_gates.h scene_water_visible carries the reach):
# the mirror prerenders gate on their scene's g_WaterActive alone and the
# bloom's water redraw on neither, so the water node, both mirrors and the
# redraw keep drawing under skipping labels. While the Inset pass draws its own
# water each view has its own strip: the main view's on MAIN_VIEW_WATER and the
# Inset's on its own bit, marched at its own screen (engine
# environment/water_mirror.h inset_mirror_source); with the Inset down the one
# strip rides the water layer under the main label. Node state only: headless.

const WATER_LAYER := 1 << 10
const INSET_VIEW := 1 << 21
const MAIN_VIEW_NO_MIRROR := 1 << 22
const MAIN_VIEW_WATER := 1 << 23
const TICK := 1.0 / 62.0


func _water() -> Dictionary:
	var view := SubViewport.new()
	view.size = Vector2i(64, 64)
	add_child_autofree(view)
	var water := Water.new()
	view.add_child(water)
	water.water_height = 5.0
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.make_current()
	water.advance_frame(1.0 / 62.5)
	var inset_view := SubViewport.new()
	inset_view.size = Vector2i(32, 32)
	view.add_child(inset_view)
	var inset := Camera3D.new()
	inset_view.add_child(inset)
	inset.make_current()
	return {"water": water, "inset": inset}


# An eye above the water looking down at it, with the weapon Inset's view.
func _scene() -> Dictionary:
	var view := SubViewport.new()
	view.size = Vector2i(256, 192)
	add_child_autofree(view)
	var water := Water.new()
	view.add_child(water)
	water.water_height = 7.0
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.make_current()
	var block := Transform3D(Basis.from_euler(Vector3(deg_to_rad(-20.0), 0.0, 0.0)),
			Vector3(100.3, 27.0, -33.7))
	camera.global_transform = block
	var inset_view := SubViewport.new()
	inset_view.size = Vector2i(96, 96)
	view.add_child(inset_view)
	var inset := Camera3D.new()
	inset.keep_aspect = Camera3D.KEEP_WIDTH
	inset.fov = 9.5
	inset.near = 0.2
	inset_view.add_child(inset)
	inset.make_current()
	inset.global_transform = Transform3D(
			Basis.from_euler(Vector3(deg_to_rad(-22.0), deg_to_rad(1.5), 0.0)), block.origin)
	return {"water": water, "inset": inset, "block": block}


func _surfaces(instance: MeshInstance3D) -> int:
	var mesh := instance.mesh as ArrayMesh
	return 0 if mesh == null else mesh.get_surface_count()


func test_each_view_combination_lands_on_its_own_layer() -> void:
	var fixture := _water()
	var water: Water = fixture["water"]
	var inset: Camera3D = fixture["inset"]
	var strip: MeshInstance3D = water.get_mesh_instance()
	var inset_strip: MeshInstance3D = water.get_inset_mesh_instance()
	assert_not_null(strip)
	assert_not_null(inset_strip)
	if strip == null or inset_strip == null:
		return
	assert_eq(strip.layers, WATER_LAYER, "the ordinary strip rides the water layer")
	assert_eq(inset_strip.layers, 0, "no Inset strip while the Inset is down")
	water.set_blink_water_views(false, false)
	assert_eq(strip.layers, 0, "the Inset down: the main label skips the one strip")
	assert_true(water.visible, "a label never hides the water node")
	assert_false(water.is_main_view_water())
	water.set_blink_water_views(true, true)
	assert_eq(strip.layers, WATER_LAYER)
	water.advance_inset_frame(inset, inset.global_transform)
	assert_eq(strip.layers, MAIN_VIEW_WATER, "the main view's strip is the main view's alone")
	assert_eq(inset_strip.layers, INSET_VIEW, "the Inset draws its own strip")
	water.set_blink_water_views(true, false)
	assert_eq(strip.layers, MAIN_VIEW_WATER, "the main view alone")
	assert_eq(inset_strip.layers, 0, "the Inset's label skips its strip")
	water.set_blink_water_views(false, true)
	assert_eq(strip.layers, 0, "the main view's label skips its strip")
	assert_eq(inset_strip.layers, INSET_VIEW, "the Inset alone")
	water.set_blink_water_views(false, false)
	assert_true(water.visible, "neither strip draws; the node stays")
	assert_eq(strip.layers, 0)
	assert_eq(inset_strip.layers, 0)
	water.set_blink_water_views(true, true)
	water.advance_inset_frame(null, Transform3D())
	assert_eq(strip.layers, WATER_LAYER, "the Inset down: the ordinary strip again")
	assert_eq(inset_strip.layers, 0)


func test_a_skipping_label_leaves_the_mirror_and_the_bloom_redraw() -> void:
	var fixture := _scene()
	var water: Water = fixture["water"]
	var strip: MeshInstance3D = water.get_mesh_instance()
	var redraw: MeshInstance3D = water.get_night_vision_mesh_instance()
	var mirror: SubViewport = water.get_reflection_viewport()
	# The main label down with g_WaterActive up (no tracked bounds keep it up).
	water.set_blink_water_views(false, false)
	water.advance_frame(TICK)
	assert_true(water.is_water_pass_active())
	assert_true(water.visible)
	assert_eq(strip.layers, 0, "the label skips the strip")
	assert_eq(mirror.render_target_update_mode, SubViewport.UPDATE_ALWAYS,
			"the prerender gates on g_WaterActive alone")
	assert_true(bool(water.get_water_material().get_shader_parameter("u_has_reflection")))
	assert_gt(_surfaces(redraw), 0, "the bloom's redraw draws whatever the label")
	assert_true(redraw.is_visible_in_tree(), "the redraw's Q3 source stays visible")
	assert_eq(water.get_night_vision_material().get_shader_parameter("u_reflection"),
			mirror.get_texture())
	assert_true(bool(water.get_night_vision_material().get_shader_parameter("u_has_reflection")),
			"the redraw samples the mirror the label left rendering")
	# g_WaterActive down (every visible sector above the water, the latch
	# clear): no prerender, and still the redraw.
	water.set_visible_terrain_bounds(true, 20.0, 40.0)
	water.set_blink_water_visible(false)
	water.advance_frame(TICK)
	assert_false(water.is_water_pass_active())
	assert_eq(mirror.render_target_update_mode, SubViewport.UPDATE_DISABLED)
	assert_eq(_surfaces(strip), 0)
	assert_gt(_surfaces(redraw), 0, "the redraw takes neither gate")
	# Both up: the strip draws on the water layer.
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.set_blink_water_views(true, true)
	water.advance_frame(TICK)
	assert_eq(strip.layers, WATER_LAYER)
	assert_gt(_surfaces(strip), 0)
	assert_eq(mirror.render_target_update_mode, SubViewport.UPDATE_ALWAYS)
	water.release_runtime_renderer_resources()


func test_skipping_labels_leave_the_inset_mirror() -> void:
	var fixture := _scene()
	var water: Water = fixture["water"]
	water.advance_frame(TICK)
	water.set_blink_water_views(false, false)
	water.set_inset_visible_terrain_bounds(true, 0.0, 30.0)
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	assert_true(water.is_inset_water_pass_active())
	assert_true(water.is_inset_mirror_rendered(),
			"the Inset's prerender gates on its own g_WaterActive alone")
	var target: SubViewport = water.get_inset_reflection_viewport()
	assert_eq(target.render_target_update_mode, SubViewport.UPDATE_ALWAYS)
	assert_eq(water.get_inset_mesh_instance().layers, 0, "the Inset's label skips its strip")
	assert_eq(_surfaces(water.get_inset_mesh_instance()), 0)
	assert_eq(water.get_mesh_instance().layers, 0, "and the main label the main strip")
	assert_eq(water.get_night_vision_material().get_shader_parameter("u_reflection"),
			target.get_texture(),
			"the bloom's redraw samples the Inset's image under both labels")
	assert_true(bool(water.get_night_vision_material().get_shader_parameter("u_has_reflection")))
	# The Inset's g_WaterActive down: its prerender stops, labels or not.
	water.set_blink_water_visible(false)
	water.open_inset_water_latch()
	water.latch_blink_water(false, false)
	water.set_inset_visible_terrain_bounds(true, 20.0, 40.0)
	water.advance_inset_frame(fixture["inset"], fixture["block"])
	assert_false(water.is_inset_water_pass_active())
	assert_false(water.is_inset_mirror_rendered())
	assert_eq(target.render_target_update_mode, SubViewport.UPDATE_DISABLED)
	water.release_runtime_renderer_resources()


func test_the_main_only_bit_is_the_beauty_cameras_alone() -> void:
	# The beauty camera admits it; neither mirror mask (above or below the
	# plane) nor the Inset camera does, exactly like the water layer itself.
	assert_ne(FrameFx.kBeautyCameraMask & MAIN_VIEW_WATER, 0)
	assert_eq(Water.REFLECTION_CULL_MASK & MAIN_VIEW_WATER, 0)
	var below := Water.REFLECTION_CULL_MASK | Water.VISUAL_LAYER_WORLD_NO_MIRROR \
			| MAIN_VIEW_NO_MIRROR
	assert_eq(below & MAIN_VIEW_WATER, 0)
	assert_eq(Water.REFLECTION_CULL_MASK & WATER_LAYER, 0)
	assert_eq(below & WATER_LAYER, 0)
	# The Inset strip's bit: neither the beauty camera nor either mirror draws it.
	assert_eq(FrameFx.kBeautyCameraMask & INSET_VIEW, 0)
	assert_eq(Water.REFLECTION_CULL_MASK & INSET_VIEW, 0)
	assert_eq(below & INSET_VIEW, 0)
