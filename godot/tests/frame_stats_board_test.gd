extends GutTest

# FrameStatsBoard: the fixed-slot accumulator behind the F3 Stats tab. Hosts
# add microsecond spans per frame while enabled; the pane drains windows at
# refresh cadence. These tests pin the window math and the feed contracts the
# hosts rely on (MissionRuntime._present_frame per-pass split included).

const MissionRuntimeScript := preload("res://engine/world/mission_runtime.gd")


func test_window_accumulates_sums_maxes_and_counts() -> void:
	var board := FrameStatsBoard.new()
	board.add(FrameStatsBoard.SIM_STEP, 1000)
	board.add(FrameStatsBoard.SIM_STEP, 3000)
	board.add(FrameStatsBoard.OCCL_APPLY, 250)
	assert_eq(board.window_sums()[FrameStatsBoard.SIM_STEP], 4000,
			"sums accumulate per slot")
	assert_eq(board.window_maxes()[FrameStatsBoard.SIM_STEP], 3000,
			"the worst single add is retained")
	assert_eq(board.window_counts()[FrameStatsBoard.SIM_STEP], 2,
			"counts distinguish fed slots from untouched ones")
	assert_eq(board.window_sums()[FrameStatsBoard.OCCL_APPLY], 250)
	assert_eq(board.window_counts()[FrameStatsBoard.PRESENT_FIRE], 0,
			"untouched slots stay at zero count")


func test_reset_window_clears_everything() -> void:
	var board := FrameStatsBoard.new()
	board.add(FrameStatsBoard.FRAME_HUD, 777)
	board.reset_window()
	assert_eq(board.window_sums()[FrameStatsBoard.FRAME_HUD], 0)
	assert_eq(board.window_maxes()[FrameStatsBoard.FRAME_HUD], 0)
	assert_eq(board.window_counts()[FrameStatsBoard.FRAME_HUD], 0)


func test_window_frames_rides_the_process_frame_counter() -> void:
	var board := FrameStatsBoard.new()
	board.reset_window()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_true(board.window_frames() >= 2,
			"window length comes from real render frames, host-independent")


class PresentPassStub:
	extends RefCounted
	var presented := 0
	var last_ticks := -1
	func present(ticks := -1) -> void:
		presented += 1
		last_ticks = ticks


class WireStatsStub:
	extends RefCounted
	func get_stats() -> Dictionary:
		return {"live": 3, "spawned": 5, "unresolved": 1}


func test_mission_runtime_present_frame_times_each_pass() -> void:
	# The per-pass split behind the Stats tab's Present rows: with the board
	# capturing, _present_frame lands one span per existing pass and still
	# drives every pass exactly once.
	var runtime: Node = MissionRuntimeScript.new()
	add_child_autofree(runtime)
	var board := FrameStatsBoard.new()
	board.enabled = true
	runtime.set_frame_stats_board(board)
	var fire := PresentPassStub.new()
	var destruction := PresentPassStub.new()
	var throwable := PresentPassStub.new()
	runtime._fire_present = fire
	runtime._destruction_present = destruction
	runtime._throwable_present = throwable

	runtime._present_frame(3, true)
	assert_eq(fire.presented, 1, "the fire pass ran once")
	assert_eq(fire.last_ticks, 3, "...with the frame's tick count")
	assert_eq(destruction.presented, 1)
	assert_eq(throwable.presented, 1)
	var counts := board.window_counts()
	assert_eq(counts[FrameStatsBoard.PRESENT_FIRE], 1, "fire span landed")
	assert_eq(counts[FrameStatsBoard.PRESENT_DESTRUCTION], 1, "destruction span landed")
	assert_eq(counts[FrameStatsBoard.PRESENT_THROWABLE], 1, "throwable span landed")
	assert_eq(counts[FrameStatsBoard.PRESENT_MISSION], 0,
			"no mission-rows pass, no mission-rows span")
	assert_true(int(runtime._perf_present_us) >= 0,
			"the bundled probe counter still updates")


func test_mission_runtime_present_frame_skips_measurement_when_off() -> void:
	var runtime: Node = MissionRuntimeScript.new()
	add_child_autofree(runtime)
	var board := FrameStatsBoard.new()
	runtime.set_frame_stats_board(board)
	runtime._fire_present = PresentPassStub.new()
	runtime._present_frame(1, false)
	assert_eq(board.window_counts()[FrameStatsBoard.PRESENT_FIRE], 0,
			"a closed Stats tab leaves the board untouched")


func test_mission_runtime_wire_stats_accessor() -> void:
	var runtime: Node = MissionRuntimeScript.new()
	add_child_autofree(runtime)
	assert_eq(runtime.get_wire_present_stats(), {},
			"no wire pass reads as empty stats")
	runtime._wire_present = WireStatsStub.new()
	assert_eq(int(runtime.get_wire_present_stats().get("live", 0)), 3,
			"the wire pass stats surface through the runtime accessor")
