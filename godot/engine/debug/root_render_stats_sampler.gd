class_name RootRenderStatsSampler
extends RefCounted

# Root-viewport render-time sampling for the F3 Stats tab, split out of the
# game shell (the W4-6 oversize ratchet): measurement flips on only while the
# tab captures (it is not free), then the previous frame's CPU/GPU times land
# on the board each frame, together with the TRUE wall time between
# consecutive host frames (matches fps exactly; Godot's TIME_PROCESS monitor
# does not) — the number that exposes work outside the host's measured legs.

var _board: FrameStatsBoard
var _measured := false
var _viewport_ref: WeakRef = null
# Previous host-frame timestamp for the wall frame row (0 = no prior frame in
# this capture window).
var _last_frame_usec := 0


## Render-time measurement is RenderingServer state, not Node-owned state.
## Observing the board's capture close edge directly means it cannot survive
## until some later process frame (or outlive the host scene).
func setup(board: FrameStatsBoard) -> void:
	_board = board
	board.capture_changed.connect(_on_capture_changed)


## Whether measurement is live on a still-valid viewport (the host's public
## observation seam delegates here, ADR 0018).
func is_measured() -> bool:
	return _measured and _viewport_ref != null \
			and is_instance_valid(_viewport_ref.get_ref())


## Per-frame entry: `viewport` is the host's current root viewport (a changed
## viewport re-arms measurement on the new one).
func sample(viewport: Viewport, stats_on: bool) -> void:
	if not stats_on and not _measured:
		return
	if not stats_on or viewport == null:
		stop()
		return
	var previous: Object = (
			_viewport_ref.get_ref() if _viewport_ref != null else null)
	if not _measured or previous != viewport:
		stop()
		_measured = true
		_viewport_ref = weakref(viewport)
		RenderingServer.viewport_set_measure_render_time(
				viewport.get_viewport_rid(), true)
	var now_usec := Time.get_ticks_usec()
	if _last_frame_usec > 0:
		_board.add(FrameStatsBoard.FRAME_WALL, now_usec - _last_frame_usec)
	_last_frame_usec = now_usec
	var rid := viewport.get_viewport_rid()
	_board.add(FrameStatsBoard.RENDER_ROOT_CPU,
			int(RenderingServer.viewport_get_measured_render_time_cpu(rid) * 1000.0))
	_board.add(FrameStatsBoard.RENDER_ROOT_GPU,
			int(RenderingServer.viewport_get_measured_render_time_gpu(rid) * 1000.0))


func stop() -> void:
	var previous: Object = (
			_viewport_ref.get_ref() if _viewport_ref != null else null)
	if previous is Viewport:
		RenderingServer.viewport_set_measure_render_time(
				(previous as Viewport).get_viewport_rid(), false)
	_viewport_ref = null
	_measured = false
	_last_frame_usec = 0


func _on_capture_changed(active: bool) -> void:
	if not active:
		stop()
