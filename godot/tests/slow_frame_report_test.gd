extends GutTest

# SlowFrameReport (godot/game/debug): the verbose slow-frame readout's line over a drained
# FrameStats window. A window under SLOW_FRAME_US prints nothing; a slow one names its wall,
# its frames and its time slots, largest first, at MIN_SLOT_US or more; VALUE slots never.


func _drained(amounts: Array) -> FrameStatsWindow:
	var board := FrameStats.new()
	board.set_capture_active(true)
	for pair: Array in amounts:
		board.add(int(pair[0]), int(pair[1]))
	return board.drain()


func test_a_window_under_the_threshold_prints_nothing() -> void:
	var window := _drained([[FrameStats.FRAME_WALL, SlowFrameReport.SLOW_FRAME_US - 1],
			[FrameStats.FRAME_DRAW, 90_000]])
	assert_eq(SlowFrameReport.report_line(window, SlowFrameReport.time_slots(), 7, 1000), "")


func test_a_slow_window_names_its_largest_time_slots() -> void:
	var window := _drained([[FrameStats.FRAME_WALL, 626_000],
			[FrameStats.FRAME_DRAW, 540_000],
			[FrameStats.FRAME_WORLD, 61_000],
			[FrameStats.WORLD_TERRAIN, 999],
			[FrameStats.FRAME_NODES_ADDED, 900_000]])
	var line := SlowFrameReport.report_line(window, SlowFrameReport.time_slots(), 42, 1792725511957)
	assert_true(line.begins_with("SlowFrame: frame=42 unix_ms=1792725511957 wall 626 ms over "), line)
	var draw_at := line.find(FrameStats.slot_name(FrameStats.FRAME_DRAW).to_lower() + " 540")
	var world_at := line.find(FrameStats.slot_name(FrameStats.FRAME_WORLD).to_lower() + " 61")
	assert_gt(draw_at, 0, "the draw slot is named with its milliseconds")
	assert_gt(world_at, draw_at, "largest first")
	assert_eq(line.find(FrameStats.slot_name(FrameStats.WORLD_TERRAIN).to_lower()), -1,
			"a slot under MIN_SLOT_US is left out")
	assert_eq(line.find(FrameStats.slot_name(FrameStats.FRAME_NODES_ADDED).to_lower()), -1,
			"a VALUE slot is never a time")
