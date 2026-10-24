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


# The PipelineCompiles line: the compiles since the previous read, Godot's by source and then
# our GLSL compiles and our RenderingDevice pipeline creates with their milliseconds; nothing when
# nothing compiled or on a first read.
func test_the_compile_line_names_what_compiled_since_the_last_read() -> void:
	var before := PackedInt64Array([4, 10, 20, 7, 3, 2, 50_000, 6, 9_000])
	var after := PackedInt64Array([4, 12, 20, 16, 3, 5, 92_000, 10, 17_000])
	assert_eq(SlowFrameReport.compile_line(before, after, 9, 1000),
			"PipelineCompiles: frame=9 unix_ms=1000: mesh 2, draw 9, rd_glsl 3 (42 ms), rd_pipelines 4 (8 ms)")
	var glsl_only := PackedInt64Array([4, 10, 20, 7, 3, 3, 51_000, 6, 9_000])
	assert_eq(SlowFrameReport.compile_line(before, glsl_only, 9, 1000),
			"PipelineCompiles: frame=9 unix_ms=1000: rd_glsl 1 (1 ms)", "no pipeline create, no part")
	assert_eq(SlowFrameReport.compile_line(after, after, 10, 1016), "",
			"a read with nothing compiled prints nothing")
	assert_eq(SlowFrameReport.compile_line(PackedInt64Array(), after, 1, 0), "",
			"a first read prints nothing")


# The post-load frames' line: a window under SLOW_FRAME_US prints with the floor at 0 and the
# PostLoadFrame label, its slots as a slow window's.
func test_a_post_load_window_prints_under_the_threshold_with_its_label() -> void:
	var window := _drained([[FrameStats.FRAME_WALL, 31_000], [FrameStats.FRAME_DRAW, 12_000]])
	var line := SlowFrameReport.report_line(window, SlowFrameReport.time_slots(), 5, 2000, 0,
			SlowFrameReport.POST_LOAD_LABEL)
	assert_true(line.begins_with("PostLoadFrame: frame=5 unix_ms=2000 wall 31 ms over "), line)
	assert_gt(line.find(FrameStats.slot_name(FrameStats.FRAME_DRAW).to_lower() + " 12"), 0, line)


# GameWorld.post_load_frames_continue: the first eight frames always; past them only while a
# joiner awaits its first C2S 0x0C past the final wait, and never past the cap.
func test_the_post_load_frames_run_on_to_a_joiners_first_uplink() -> void:
	assert_true(GameWorld.post_load_frames_continue(7, false), "the first eight frames always")
	assert_false(GameWorld.post_load_frames_continue(8, false),
			"a host's frames, or a joiner's whose first 0x0C left, end at eight")
	assert_true(GameWorld.post_load_frames_continue(8, true), "a joiner awaiting its first 0x0C runs on")
	assert_true(GameWorld.post_load_frames_continue(119, true), "up to the cap")
	assert_false(GameWorld.post_load_frames_continue(120, true), "never past the cap")
