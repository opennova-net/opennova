extends GutTest

# The water strip's per-view blink-water gate while the weapon Inset renders:
# the Inset scene core draws its water passes on its own collect's
# g_BlinkWaterVisible (engine world/occlusion.h OcclusionView carries the
# witness), so the one strip carries both views' verdicts as its layer.

const WATER_LAYER := 1 << 10
const INSET_VIEW := 1 << 21
const MAIN_VIEW_NO_MIRROR := 1 << 22
const MAIN_VIEW_WATER := 1 << 23


func _water() -> Water:
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
	return water


func test_each_view_combination_lands_on_its_own_layer() -> void:
	var water := _water()
	var strip: MeshInstance3D = water.get_mesh_instance()
	assert_not_null(strip)
	if strip == null:
		return
	assert_eq(strip.layers, WATER_LAYER, "the ordinary strip rides the water layer")
	water.set_blink_water_views(true, false)
	assert_true(water.visible)
	assert_eq(strip.layers, MAIN_VIEW_WATER, "the main view alone")
	water.set_blink_water_views(false, true)
	assert_true(water.visible)
	assert_eq(strip.layers, INSET_VIEW, "the Inset alone")
	water.set_blink_water_views(false, false)
	assert_false(water.visible, "neither view draws the water")
	water.set_blink_water_views(true, true)
	assert_true(water.visible)
	assert_eq(strip.layers, WATER_LAYER, "both views: the ordinary strip again")


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
