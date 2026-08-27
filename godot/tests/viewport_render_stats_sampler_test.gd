extends GutTest

# ViewportRenderStatsSampler: the auxiliary-viewport render sampler behind the
# F3 Q3/viewmodel/slot-capture rows. Measurement is RenderingServer state, so
# the contract under test is the arming edge (only while the board captures,
# released on the close edge and on stop) and the counted-vs-latched split (an
# unarmed slot keeps its measurement but lands nothing on the board).

const SamplerScript := preload("res://game/debug/viewport_render_stats_sampler.gd")

var _board: FrameStats
var _viewports: Array[SubViewport] = []


func before_each() -> void:
	_board = FrameStats.new()
	_viewports.clear()
	for _i in range(2):
		var viewport := SubViewport.new()
		viewport.size = Vector2i(8, 8)
		viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
		add_child_autofree(viewport)
		_viewports.append(viewport)


func _make_sampler() -> ViewportRenderStatsSampler:
	return SamplerScript.new(_board,
			FrameStats.RENDER_SLOT_CPU, FrameStats.RENDER_SLOT_GPU,
			FrameStats.RENDER_SLOT_OBJECTS, FrameStats.RENDER_SLOT_DRAWS)


func test_begin_frame_off_arms_nothing_and_samples_nothing() -> void:
	var sampler := _make_sampler()
	assert_false(sampler.begin_frame(false))
	sampler.sample_viewport(0, _viewports[0], true)
	assert_false(sampler.is_measured())
	assert_eq(_board.drain().sample_frames[FrameStats.RENDER_SLOT_CPU], 0)


func test_capture_arms_counted_viewports_land_and_unarmed_ones_only_latch() -> void:
	var sampler := _make_sampler()
	_board.set_capture_active(true)
	assert_true(sampler.begin_frame(true))
	sampler.sample_viewport(0, _viewports[0], true)
	sampler.sample_viewport(1, _viewports[1], false)
	assert_true(sampler.is_measured())
	var window := _board.drain()
	assert_eq(window.sample_frames[FrameStats.RENDER_SLOT_CPU], 1,
			"one counted viewport lands one sample per slot")
	assert_eq(window.sample_frames[FrameStats.RENDER_SLOT_OBJECTS], 1)
	assert_eq(window.sample_frames[FrameStats.RENDER_SLOT_DRAWS], 1)


func test_capture_close_edge_and_stop_release_measurement() -> void:
	var sampler := _make_sampler()
	_board.set_capture_active(true)
	sampler.begin_frame(true)
	sampler.sample_viewport(0, _viewports[0], true)
	_board.set_capture_active(false)
	assert_false(sampler.is_measured(), "the board's close edge disarms")
	_board.set_capture_active(true)
	sampler.begin_frame(true)
	sampler.sample_viewport(0, _viewports[0], true)
	assert_true(sampler.is_measured())
	sampler.stop()
	assert_false(sampler.is_measured())
	assert_false(sampler.begin_frame(false), "a closed frame reports off")


func test_changed_viewport_at_an_index_rearms_on_the_new_one() -> void:
	var sampler := _make_sampler()
	_board.set_capture_active(true)
	sampler.begin_frame(true)
	sampler.sample_viewport(0, _viewports[0], true)
	sampler.sample_viewport(0, _viewports[1], true)
	assert_true(sampler.is_measured())
	_viewports[1].queue_free()
	await get_tree().process_frame
	assert_false(sampler.is_measured(),
			"a freed latched viewport reads as unmeasured (weakref latch)")
