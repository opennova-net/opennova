class_name RootFramePhaseSampler
extends Node

## Brackets Godot's idle- and physics-process callback windows while the F3
## Stats page captures. The early probe runs before ordinary nodes and the late
## probe after them, which separates explicitly timed game callbacks, other
## node callbacks, and the engine time outside every callback. That outside
## time is split further at RenderingServer's draw signals: the deferred flush
## (late callback -> frame_pre_draw), the draw itself (frame_pre_draw ->
## frame_post_draw), and servers/input/pacing (frame_post_draw -> the next
## early callback).


class LateBoundary:
	extends Node

	var sampler: RootFramePhaseSampler

	func _process(_delta: float) -> void:
		sampler.finish_process_window()

	func _physics_process(_delta: float) -> void:
		sampler.finish_physics_window()


const _EARLY_PRIORITY := -2_147_483_647
const _LATE_PRIORITY := 2_147_483_647

var _board: FrameStatsBoard
var _late := LateBoundary.new()
var _last_frame_usec := 0
var _process_start_usec := 0
var _process_end_usec := 0
var _pre_draw_usec := 0
var _post_draw_usec := 0
var _physics_start_usec := 0
var _pending_physics_usec := 0
var _last_physics_frames := 0
var _shell_control_start_usec := 0
var _stats_sample_start_usec := 0
var _stats_sample_end_usec := 0
var _draw_signals_connected := false


func _init() -> void:
	name = "RootFramePhaseSampler"
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = _EARLY_PRIORITY
	process_physics_priority = _EARLY_PRIORITY
	_late.name = "LateBoundary"
	_late.process_mode = Node.PROCESS_MODE_ALWAYS
	_late.process_priority = _LATE_PRIORITY
	_late.process_physics_priority = _LATE_PRIORITY
	_late.sampler = self
	add_child(_late)
	_set_capture_active(false)


func setup(board: FrameStatsBoard) -> void:
	_board = board
	board.capture_changed.connect(_set_capture_active)
	_set_capture_active(board.is_capture_active())


## Mark the game shell's explicitly timed process window. This stays separate
## from the root callback boundary so F3 can account for shell control work.
func begin_shell_control() -> bool:
	var active := _board != null and _board.is_capture_active()
	_shell_control_start_usec = Time.get_ticks_usec() if active else 0
	return active


## Sample the previous render frame and the menu-video callback as one F3
## bookkeeping phase. Render measurement still receives the inactive edge.
func sample_render(render_stats: RootRenderStatsSampler, viewport: Viewport,
		menu_shell: MenuShell) -> void:
	var active := _board != null and _board.is_capture_active()
	_stats_sample_start_usec = Time.get_ticks_usec() if active else 0
	render_stats.sample(viewport, active)
	if not active:
		return
	if menu_shell != null:
		var menu_video_us: int = int(menu_shell.consume_video_process_us())
		if menu_video_us > 0:
			_board.add(FrameStatsBoard.FRAME_MENU_VIDEO, menu_video_us)
	_stats_sample_end_usec = Time.get_ticks_usec()
	_board.add(FrameStatsBoard.FRAME_STATS_SAMPLE,
			_stats_sample_end_usec - _stats_sample_start_usec)


func finish_shell_control(timed_work_start_usec: int) -> void:
	if _shell_control_start_usec <= 0:
		return
	_board.add(FrameStatsBoard.FRAME_SHELL_CONTROL,
			(_stats_sample_start_usec - _shell_control_start_usec) +
			(timed_work_start_usec - _stats_sample_end_usec))
	_shell_control_start_usec = 0


func record_shell_spans(probe_t0: int, probe_t1: int, probe_t2: int,
		probe_t3: int, probe_t4: int, probe_t5: int,
		probe_spans: Dictionary, probe_enabled: bool) -> void:
	if probe_enabled:
		probe_spans["before"] = probe_t1 - probe_t0
		probe_spans["world"] = probe_t2 - probe_t1
		probe_spans["after"] = probe_t3 - probe_t2
		probe_spans["hud"] = probe_t4 - probe_t3
		probe_spans["round_flow"] = probe_t5 - probe_t4
	if _board == null or not _board.is_capture_active():
		return
	_board.add(FrameStatsBoard.FRAME_PLAYER_BEFORE, probe_t1 - probe_t0)
	_board.add(FrameStatsBoard.FRAME_WORLD, probe_t2 - probe_t1)
	_board.add(FrameStatsBoard.FRAME_PLAYER_AFTER, probe_t3 - probe_t2)
	_board.add(FrameStatsBoard.FRAME_HUD, probe_t4 - probe_t3)
	_board.add(FrameStatsBoard.FRAME_ROUND_FLOW, probe_t5 - probe_t4)


func _process(_delta: float) -> void:
	if _board == null or not _board.is_capture_active():
		return
	var now := Time.get_ticks_usec()
	if _last_frame_usec > 0:
		_board.add(FrameStatsBoard.FRAME_WALL, now - _last_frame_usec)
	_last_frame_usec = now
	# The tail of the previous iteration: everything after its draw returned
	# (audio/script frame hooks, pacing, input pump, this iteration's physics
	# servers and SceneTree head) up to this earliest idle callback.
	if _post_draw_usec > 0:
		_board.add(FrameStatsBoard.FRAME_PACING_INPUT, now - _post_draw_usec)
	_post_draw_usec = 0
	# Physics callbacks precede this idle frame. Publish their accumulated
	# window here so FrameStatsBoard assigns them to the same render frame.
	_board.add(FrameStatsBoard.FRAME_PHYSICS_CALLBACKS, _pending_physics_usec)
	_pending_physics_usec = 0
	var physics_frames := Engine.get_physics_frames()
	if _last_physics_frames > 0:
		_board.add(FrameStatsBoard.FRAME_PHYSICS_ITERATIONS,
				physics_frames - _last_physics_frames)
	_last_physics_frames = physics_frames
	# Godot's own monitors for the previous iteration: TIME_PROCESS spans
	# MainLoop.process + the deferred flush + RenderingServer sync/draw, so
	# it cross-checks the callback + flush + draw rows; TIME_PHYSICS_PROCESS is
	# the physics servers' window (max over the iterations, not a sum).
	_board.add(FrameStatsBoard.FRAME_TIME_PROCESS,
			int(Performance.get_monitor(Performance.TIME_PROCESS) * 1_000_000.0))
	_board.add(FrameStatsBoard.FRAME_PHYSICS_SERVER,
			int(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)
					* 1_000_000.0))
	_process_start_usec = now


func _physics_process(_delta: float) -> void:
	if _board != null and _board.is_capture_active():
		_physics_start_usec = Time.get_ticks_usec()


func finish_process_window() -> void:
	if _process_start_usec <= 0 or _board == null or not _board.is_capture_active():
		return
	_process_end_usec = Time.get_ticks_usec()
	_board.add(FrameStatsBoard.FRAME_PROCESS_CALLBACKS,
			_process_end_usec - _process_start_usec)
	_process_start_usec = 0


func finish_physics_window() -> void:
	if _physics_start_usec <= 0 or _board == null or not _board.is_capture_active():
		return
	_pending_physics_usec += Time.get_ticks_usec() - _physics_start_usec
	_physics_start_usec = 0


# RenderingServer.frame_pre_draw: the deferred flush (every call_deferred and
# queue_redraw -> _draw), transform-notification flush, SceneTree timers/tweens/
# delete queue, and the RenderingServer sync are behind us.
func _on_frame_pre_draw() -> void:
	if _board == null or not _board.is_capture_active():
		return
	var now := Time.get_ticks_usec()
	if _process_end_usec > 0:
		_board.add(FrameStatsBoard.FRAME_DEFERRED_FLUSH, now - _process_end_usec)
	_process_end_usec = 0
	_pre_draw_usec = now


# RenderingServer.frame_post_draw: RenderingServer.draw returned — every
# viewport culled, draw lists built, commands submitted, frame presented.
func _on_frame_post_draw() -> void:
	if _board == null or not _board.is_capture_active():
		return
	var now := Time.get_ticks_usec()
	if _pre_draw_usec > 0:
		_board.add(FrameStatsBoard.FRAME_DRAW, now - _pre_draw_usec)
	_pre_draw_usec = 0
	_post_draw_usec = now


func _set_capture_active(active: bool) -> void:
	set_process(active)
	set_physics_process(active)
	_late.set_process(active)
	_late.set_physics_process(active)
	if active != _draw_signals_connected:
		if active:
			RenderingServer.frame_pre_draw.connect(_on_frame_pre_draw)
			RenderingServer.frame_post_draw.connect(_on_frame_post_draw)
		else:
			RenderingServer.frame_pre_draw.disconnect(_on_frame_pre_draw)
			RenderingServer.frame_post_draw.disconnect(_on_frame_post_draw)
		_draw_signals_connected = active
	if active:
		return
	_last_frame_usec = 0
	_process_start_usec = 0
	_process_end_usec = 0
	_pre_draw_usec = 0
	_post_draw_usec = 0
	_physics_start_usec = 0
	_pending_physics_usec = 0
	_last_physics_frames = 0
	_shell_control_start_usec = 0
	_stats_sample_start_usec = 0
	_stats_sample_end_usec = 0
