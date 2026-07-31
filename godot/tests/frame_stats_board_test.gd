extends GutTest

# FrameStatsBoard: the fixed-slot accumulator behind F3 Stats. These tests pin
# its semantic interface: explicit capture edges, atomic drains, and true
# per-render-frame peaks when a fixed-tick producer writes several times.


func test_capture_window_accumulates_sums_peaks_and_sample_frames() -> void:
	var board := FrameStatsBoard.new()
	assert_false(board.is_capture_active())
	board.set_capture_active(true)
	board.add(FrameStatsBoard.SIM_STEP, 1000)
	board.add(FrameStatsBoard.SIM_STEP, 3000)
	board.add(FrameStatsBoard.OCCL_APPLY, 250)

	var window := board.drain()
	assert_eq(window.sums[FrameStatsBoard.SIM_STEP], 4000,
			"sums accumulate per slot")
	assert_eq(window.peaks[FrameStatsBoard.SIM_STEP], 4000,
			"writes in one render frame form one peak")
	assert_eq(window.sample_frames[FrameStatsBoard.SIM_STEP], 1,
			"sample count records distinct render frames, not add calls")
	assert_eq(window.sums[FrameStatsBoard.OCCL_APPLY], 250)
	assert_eq(window.sample_frames[FrameStatsBoard.PRESENT_FIRE], 0,
			"untouched slots stay absent")


func test_peak_compares_complete_render_frames() -> void:
	var board := FrameStatsBoard.new()
	board.set_capture_active(true)
	board.add(FrameStatsBoard.EFFECTS_TICK, 100)
	board.add(FrameStatsBoard.EFFECTS_TICK, 200)
	await get_tree().process_frame
	board.add(FrameStatsBoard.EFFECTS_TICK, 250)
	var window := board.drain()
	assert_eq(window.sums[FrameStatsBoard.EFFECTS_TICK], 550)
	assert_eq(window.peaks[FrameStatsBoard.EFFECTS_TICK], 300,
			"the catch-up frame's two fixed ticks beat the later single tick")
	assert_eq(window.sample_frames[FrameStatsBoard.EFFECTS_TICK], 2)


func test_closed_capture_rejects_feeds_and_edges_reset_the_window() -> void:
	var board := FrameStatsBoard.new()
	board.add(FrameStatsBoard.FRAME_HUD, 777)
	board.set_capture_active(true)
	board.add(FrameStatsBoard.FRAME_HUD, 111)
	board.set_capture_active(false)
	board.add(FrameStatsBoard.FRAME_HUD, 999)
	board.set_capture_active(true)
	var window := board.drain()
	assert_eq(window.sums[FrameStatsBoard.FRAME_HUD], 0,
			"capture edges start a clean window and closed feeds are ignored")
	assert_eq(window.peaks[FrameStatsBoard.FRAME_HUD], 0)
	assert_eq(window.sample_frames[FrameStatsBoard.FRAME_HUD], 0)


func test_drain_reports_render_frames_and_resets_atomically() -> void:
	var board := FrameStatsBoard.new()
	board.set_capture_active(true)
	board.add(FrameStatsBoard.FRAME_WALL, 1000)
	await get_tree().process_frame
	await get_tree().process_frame
	var first := board.drain()
	assert_true(first.frames >= 2,
			"window length comes from real render frames, owner-independent")
	assert_eq(first.sums[FrameStatsBoard.FRAME_WALL], 1000)
	var second := board.drain()
	assert_eq(second.sums[FrameStatsBoard.FRAME_WALL], 0,
			"drain copies and resets as one operation")
