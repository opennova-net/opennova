extends GutTest

# DebugStatsPane: the F3 Stats tab. Fabricated windows pin the row layout and
# formatting; the capture tests pin the edge-gating contract (the board only
# accumulates while the tab is actually the visible overlay tab).

const PaneScript := preload("res://engine/debug/debug_stats_pane.gd")
const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")


func _make_pane() -> DebugStatsPane:
	var pane: DebugStatsPane = PaneScript.new()
	add_child_autofree(pane)
	return pane


func _blank_window() -> Array:
	var sums := PackedInt64Array()
	sums.resize(FrameStatsBoard.SLOT_COUNT)
	var maxes := PackedInt64Array()
	maxes.resize(FrameStatsBoard.SLOT_COUNT)
	var counts := PackedInt32Array()
	counts.resize(FrameStatsBoard.SLOT_COUNT)
	return [sums, maxes, counts]


func _row(pane: DebugStatsPane, id: String) -> TreeItem:
	return pane._items[id] as TreeItem


func test_rows_cover_every_major_system() -> void:
	var pane := _make_pane()
	for id in ["frame", "world", "foliage", "sim", "net", "effects", "present",
			"snapshot", "mission_rows", "wire_rows", "fire", "destruction",
			"throwable", "occl", "occl_restore", "occl_build", "occl_probe",
			"occl_apply", "env", "audio", "hud", "render"]:
		assert_true(pane._items.has(id), "the Stats tab carries a '%s' row" % id)


func test_render_window_formats_avg_and_max() -> void:
	var pane := _make_pane()
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var maxes: PackedInt64Array = window[1]
	var counts: PackedInt32Array = window[2]
	# 20 ms across 10 frames = 2.00 ms/frame avg; worst frame 5 ms.
	sums[FrameStatsBoard.SIM_STEP] = 20_000
	maxes[FrameStatsBoard.SIM_STEP] = 5_000
	counts[FrameStatsBoard.SIM_STEP] = 10
	pane.render_window(10, sums, maxes, counts, null, null)
	assert_eq(_row(pane, "sim").get_text(1), "2.00", "avg = window mean per frame")
	assert_eq(_row(pane, "sim").get_text(2), "5.00", "max = worst single frame")
	assert_eq(_row(pane, "hud").get_text(1), "-", "an unfed system reads as absent")


func test_group_rows_sum_their_children() -> void:
	var pane := _make_pane()
	var window := _blank_window()
	var sums: PackedInt64Array = window[0]
	var counts: PackedInt32Array = window[2]
	sums[FrameStatsBoard.OCCL_RESTORE] = 1_000
	counts[FrameStatsBoard.OCCL_RESTORE] = 10
	sums[FrameStatsBoard.OCCL_BUILD] = 2_000
	counts[FrameStatsBoard.OCCL_BUILD] = 10
	sums[FrameStatsBoard.OCCL_PROBE] = 3_000
	counts[FrameStatsBoard.OCCL_PROBE] = 10
	sums[FrameStatsBoard.OCCL_APPLY] = 4_000
	counts[FrameStatsBoard.OCCL_APPLY] = 10
	pane.render_window(10, sums, window[1], counts, null, null)
	assert_eq(_row(pane, "occl").get_text(1), "1.00",
			"the Occlusion group sums restore+build+probe+apply(+glue)")
	assert_eq(_row(pane, "occl_apply").get_text(1), "0.40")


func test_capture_follows_host_visibility_and_own_visibility() -> void:
	var pane := _make_pane()
	var board := FrameStatsBoard.new()
	pane.set_frame_stats_board(board)
	assert_false(board.enabled, "capture starts off")
	pane.set_capture_active(true)
	assert_true(board.enabled, "visible tab + visible overlay -> capturing")
	pane.visible = false
	assert_false(board.enabled, "switching away from the tab stops capture")
	pane.visible = true
	assert_true(board.enabled, "switching back resumes")
	pane.set_capture_active(false)
	assert_false(board.enabled, "closing the overlay stops capture")


func test_overlay_grows_a_stats_tab_and_gates_capture_by_tab() -> void:
	var overlay = add_child_autofree(OverlayScript.new())
	var stats = overlay._tabs.get_node_or_null("Stats")
	assert_not_null(stats, "the overlay carries the Stats tab")
	assert_true(stats is DebugStatsPane)

	var board := FrameStatsBoard.new()
	overlay.set_frame_stats_board(board)
	overlay.toggle()
	await get_tree().process_frame
	assert_false(board.enabled,
			"opening the overlay on another tab leaves capture off")
	overlay._tabs.current_tab = (stats as Control).get_index()
	await get_tree().process_frame
	assert_true(board.enabled, "picking the Stats tab starts capture")
	overlay.toggle()
	assert_false(board.enabled, "closing the overlay stops capture")


func test_refresh_drains_the_board_window() -> void:
	var pane := _make_pane()
	var board := FrameStatsBoard.new()
	pane.set_frame_stats_board(board)
	pane.set_capture_active(true)
	assert_true(board.enabled)
	# Two frames of a 4 ms sim step land in the window...
	board.add(FrameStatsBoard.SIM_STEP, 4_000)
	board.add(FrameStatsBoard.SIM_STEP, 4_000)
	await get_tree().process_frame
	await get_tree().process_frame
	# ...the divided refresh reads it on its second call.
	pane.refresh(null, null)
	pane.refresh(null, null)
	assert_false(_row(pane, "sim").get_text(1) == "-",
			"a captured window renders onto the sim row")
	assert_eq(board.window_sums()[FrameStatsBoard.SIM_STEP], 0,
			"the drained window resets for the next reading")
