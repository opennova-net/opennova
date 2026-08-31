extends GutTest

# The DevTools Stats-row seam by row id (what the frame_stats probe reads):
# a fed reading formats into the rows the ids name. No ImGui context is
# needed for the feed path, so this pins the binding headless. The release
# flavour (OPENNOVA_DEVTOOLS off) lists no rows and is skipped.
# The engine-log ring drain is flavour-independent and pinned below.


func test_engine_log_ring_drain_contract() -> void:
	# The io::log ring (base/io/log_ring.h) is installed at extension init in
	# every flavour; the static drain returns one locked snapshot as parallel
	# columns. Ring content here is whatever the process logged (usually
	# nothing headless), so pin the shape and the exclusive-cursor contract;
	# record/wrap/chaining behavior is the io ctest's job.
	var page: Dictionary = DevTools.engine_log_after(0)
	var seqs: PackedInt64Array = page["sequences"]
	var levels: PackedStringArray = page["levels"]
	var texts: PackedStringArray = page["texts"]
	assert_eq(levels.size(), seqs.size(), "one level per sequence")
	assert_eq(texts.size(), seqs.size(), "one text per sequence")
	var last := 0
	for i in range(seqs.size()):
		assert_true(int(seqs[i]) > last, "sequences strictly increase")
		last = int(seqs[i])
	var after: Dictionary = DevTools.engine_log_after(last)
	assert_eq(PackedInt64Array(after["sequences"]).size(), 0,
			"a drain after the newest sequence returns nothing")


func test_select_entity_seam_reads_back_headless() -> void:
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	if dev_tools.stats_row_ids().is_empty():
		pending("release flavour: the dev tools are compiled out")
		return
	assert_eq(dev_tools.selected_entity_handle(), -1, "nothing selected at first")
	dev_tools.select_entity(0x3001)
	assert_eq(dev_tools.selected_entity_handle(), 0x3001,
			"a pick's handle is the selection (pending until a directory push carries it)")
	dev_tools.select_entity(-1)
	assert_eq(dev_tools.selected_entity_handle(), -1, "a negative handle clears")
	dev_tools.select_entity(0x3001)
	dev_tools.select_entity(0xFFFF)
	assert_eq(dev_tools.selected_entity_handle(), -1, "the invalid handle clears too")


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

	# The counter cells: the sim row's ticks/entities/role and the Net row's
	# peers are VALUE slots averaged over the window's frames (10 here).
	sums[FrameStats.SIM_TICKS] = 10
	samples[FrameStats.SIM_TICKS] = 10
	sums[FrameStats.SIM_ENTITY_COUNT] = 120
	samples[FrameStats.SIM_ENTITY_COUNT] = 10
	sums[FrameStats.SIM_ROLE] = 10  # 1 = host, every frame
	samples[FrameStats.SIM_ROLE] = 10
	sums[FrameStats.NET_PEER_COUNT] = 30
	samples[FrameStats.NET_PEER_COUNT] = 10
	dev_tools.feed_stats_window(10, sums, peaks, samples)
	assert_eq(dev_tools.stats_row_info("sim"), "1.0 t/f | 12 entities | host")
	assert_eq(dev_tools.stats_row_info("net"), "3 peers")


func test_ai_view_seam_exists_headless() -> void:
	# The AI window's shell seams: the overlay-state provider setter and the
	# toggle-request signal exist headless (no ImGui context needed); the
	# release flavour keeps the setter as a no-op and never emits.
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	assert_true(dev_tools.has_signal("ai_view_request"),
			"the toggle drain crosses as one bound signal")
	dev_tools.set_ai_view_state_provider(func() -> Dictionary: return {})
	dev_tools.set_ai_view_state_provider(Callable())
	pass_test("the provider setter accepts and clears a Callable")
