extends GutTest

# ProbeContext.frame_stats (godot/game/probe): a probe that reads the FrameStats board holds it
# for its run, and the shell's --verbose slow-frame readout, which drains the board every frame
# after the other idle callbacks, leaves a held board to the probe (under --verbose,
# perf_mission_rows read empty windows: frames=0, no PRESENT_MISSION sample); finish() lets it go.

const MainGameScript := preload("res://game/main_game.gd")


func test_a_probe_holds_the_shells_board_once_and_finish_lets_it_go() -> void:
	# Not added to the tree: the board and its holds are the shell's own state.
	var game: MainGame = MainGameScript.new()
	autofree(game)
	var ctx := ProbeContext.new()
	ctx.shell = game
	assert_false(game.is_frame_stats_held())
	assert_eq(ctx.frame_stats(), game.get_frame_stats())
	assert_eq(ctx.frame_stats(), game.get_frame_stats())
	assert_true(game.is_frame_stats_held(), "the probe holds the board for its run")
	ctx.finish()
	assert_false(game.is_frame_stats_held(),
			"one hold however often the probe read the board, and finish lets it go")


func test_the_null_shell_takes_no_hold() -> void:
	var shell: GameShell = autofree(GameShell.new())
	var ctx := ProbeContext.new()
	ctx.shell = shell
	assert_null(ctx.frame_stats(), "the null shell has no board")
	assert_false(shell.is_frame_stats_held())
	ctx.finish()


func test_the_slow_frame_readout_leaves_a_held_board_undrained() -> void:
	var board := FrameStats.new()
	var report := SlowFrameReport.new()
	var held := [true]
	report.setup(board, func() -> bool: return bool(held[0]))
	board.set_capture_active(true)
	add_child_autofree(report)
	board.add(FrameStats.FRAME_WALL, 5_000)
	await wait_frames(2)
	assert_eq(int(board.drain().sums[FrameStats.FRAME_WALL]), 5_000,
			"a held board keeps its sums for the probe")
	held[0] = false
	board.add(FrameStats.FRAME_WALL, 7_000)
	await wait_frames(2)
	assert_eq(int(board.drain().sums[FrameStats.FRAME_WALL]), 0,
			"an unheld board is the readout's to drain")
