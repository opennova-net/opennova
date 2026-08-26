class_name RootFramePhaseSampler
extends Node

## Brackets Godot's idle- and physics-process callback windows while the F3
## Stats page captures. The early probe runs before ordinary nodes and the late
## probe after them, which separates explicitly timed game callbacks, other
## node callbacks, and time in physics/engine/render/frame pacing.


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
var _physics_start_usec := 0
var _pending_physics_usec := 0
var _shell_control_start_usec := 0
var _stats_sample_start_usec := 0
var _stats_sample_end_usec := 0


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
	# Physics callbacks precede this idle frame. Publish their accumulated
	# window here so FrameStatsBoard assigns them to the same render frame.
	_board.add(FrameStatsBoard.FRAME_PHYSICS_CALLBACKS, _pending_physics_usec)
	_pending_physics_usec = 0
	_process_start_usec = now


func _physics_process(_delta: float) -> void:
	if _board != null and _board.is_capture_active():
		_physics_start_usec = Time.get_ticks_usec()


func finish_process_window() -> void:
	if _process_start_usec <= 0 or _board == null or not _board.is_capture_active():
		return
	_board.add(FrameStatsBoard.FRAME_PROCESS_CALLBACKS,
			Time.get_ticks_usec() - _process_start_usec)
	_process_start_usec = 0


func finish_physics_window() -> void:
	if _physics_start_usec <= 0 or _board == null or not _board.is_capture_active():
		return
	_pending_physics_usec += Time.get_ticks_usec() - _physics_start_usec
	_physics_start_usec = 0


func _set_capture_active(active: bool) -> void:
	set_process(active)
	set_physics_process(active)
	_late.set_process(active)
	_late.set_physics_process(active)
	if active:
		return
	_last_frame_usec = 0
	_process_start_usec = 0
	_physics_start_usec = 0
	_pending_physics_usec = 0
	_shell_control_start_usec = 0
	_stats_sample_start_usec = 0
	_stats_sample_end_usec = 0
