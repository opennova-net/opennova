class_name RootRenderStatsSampler
extends RefCounted

# Root-viewport render-time sampling for the F3 Stats tab, split out of the
# game shell (the W4-6 oversize ratchet): measurement flips on only while the
# tab captures (it is not free), then the previous frame's CPU/GPU times land
# on the board each frame. RootFramePhaseSampler owns wall/process timing.

var _board: FrameStatsBoard
var _measured := false
var _viewport_ref: WeakRef = null


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
	var rid := viewport.get_viewport_rid()
	_board.add(FrameStatsBoard.RENDER_ROOT_CPU,
			int(RenderingServer.viewport_get_measured_render_time_cpu(rid) * 1000.0))
	_board.add(FrameStatsBoard.RENDER_ROOT_GPU,
			int(RenderingServer.viewport_get_measured_render_time_gpu(rid) * 1000.0))
	# Per-pass submission counts (previous frame): what the main view and the
	# shadow maps each rendered. Free counters — always tracked by the server.
	_board.add(FrameStatsBoard.RENDER_MAIN_OBJECTS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME))
	_board.add(FrameStatsBoard.RENDER_MAIN_DRAWS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME))
	_board.add(FrameStatsBoard.RENDER_SHADOW_OBJECTS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_SHADOW,
					RenderingServer.VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME))
	_board.add(FrameStatsBoard.RENDER_SHADOW_DRAWS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_SHADOW,
					RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME))


func stop() -> void:
	var previous: Object = (
			_viewport_ref.get_ref() if _viewport_ref != null else null)
	if previous is Viewport:
		RenderingServer.viewport_set_measure_render_time(
				(previous as Viewport).get_viewport_rid(), false)
	_viewport_ref = null
	_measured = false


func _on_capture_changed(active: bool) -> void:
	if not active:
		stop()
