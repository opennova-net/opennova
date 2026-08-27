extends GutTest

# The DevTools Stats-row seam by row id (what the frame_stats probe reads):
# a fed reading formats into the rows the ids name. No ImGui context is
# needed for the feed path, so this pins the binding headless. The release
# flavour (OPENNOVA_DEVTOOLS off) lists no rows and is skipped.


func test_fed_reading_is_readable_by_row_id() -> void:
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	var ids := dev_tools.stats_row_ids()
	if ids.is_empty():
		pending("release flavour: the dev tools are compiled out")
		return
	assert_true(ids.has("frame"), "the frame row is listed")
	assert_true(ids.has("world"), "the world row is listed")
	assert_eq(dev_tools.stats_row_average("frame"), "", "no reading yet")
	assert_eq(dev_tools.stats_row_average("no_such_row"), "", "unknown ids read as empty")

	var sums := PackedInt64Array()
	var peaks := PackedInt64Array()
	var samples := PackedInt32Array()
	sums.resize(FrameStats.SLOT_COUNT)
	peaks.resize(FrameStats.SLOT_COUNT)
	samples.resize(FrameStats.SLOT_COUNT)
	# 10 frames: 100 ms of wall time (10.00 ms mean, 20 ms peak), 12 ms of world.
	sums[FrameStats.FRAME_WALL] = 100_000
	peaks[FrameStats.FRAME_WALL] = 20_000
	samples[FrameStats.FRAME_WALL] = 10
	sums[FrameStats.FRAME_WORLD] = 12_000
	peaks[FrameStats.FRAME_WORLD] = 3_000
	samples[FrameStats.FRAME_WORLD] = 10
	dev_tools.feed_stats_window(10, sums, peaks, samples)

	assert_eq(dev_tools.stats_reading_frames(), 10)
	assert_eq(dev_tools.stats_row_average("frame"), "10.00")
	assert_eq(dev_tools.stats_row_peak("frame"), "20.00")
	assert_eq(dev_tools.stats_row_average("world"), "1.20")
	assert_eq(dev_tools.stats_row_average("sim"), "", "a slot without samples stays empty")
