extends GutTest

# Each view's water letter gate while the weapon Inset renders: the Inset
# scene core draws its water passes on its own collect's g_BlinkWaterVisible
# (engine world/occlusion.h OcclusionView carries the witness). While the
# Inset pass draws its own water each view has its own strip: the main view's
# on MAIN_VIEW_WATER and the Inset's on its own bit, marched at its own screen
# (engine environment/water_mirror.h inset_mirror_source); with the Inset down
# the one strip rides the water layer.

const WATER_LAYER := 1 << 10
const INSET_VIEW := 1 << 21
const MAIN_VIEW_NO_MIRROR := 1 << 22
const MAIN_VIEW_WATER := 1 << 23


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
	water.advance_inset_frame(inset, inset.global_transform)
	assert_eq(strip.layers, MAIN_VIEW_WATER, "the main view's strip is the main view's alone")
	assert_eq(inset_strip.layers, INSET_VIEW, "the Inset draws its own strip")
	water.set_blink_water_views(true, false)
	assert_true(water.visible)
	assert_eq(strip.layers, MAIN_VIEW_WATER, "the main view alone")
	assert_eq(inset_strip.layers, 0, "the Inset's letter skips its strip")
	water.set_blink_water_views(false, true)
	assert_true(water.visible)
	assert_eq(strip.layers, 0, "the main view's letter skips its strip")
	assert_eq(inset_strip.layers, INSET_VIEW, "the Inset alone")
	water.set_blink_water_views(false, false)
	assert_false(water.visible, "neither view draws the water")
	water.set_blink_water_views(true, true)
	assert_true(water.visible)
	water.advance_inset_frame(null, Transform3D())
	assert_eq(strip.layers, WATER_LAYER, "the Inset down: the ordinary strip again")
	assert_eq(inset_strip.layers, 0)


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
