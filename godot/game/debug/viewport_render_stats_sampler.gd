class_name ViewportRenderStatsSampler
extends RefCounted

## Measured render time + visible-pass submission counts for one auxiliary
## viewport set on the F3 Stats board (the Q3 view, the viewmodel pass, the
## slot-shadow capture chain). Measurement is RenderingServer state that costs
## while armed, so it flips on only while the board captures and off on the
## capture close edge; the previous frame's numbers land on the board each
## frame. Latched per slot index by weakref so a freed viewport never sees a
## stale-RID RenderingServer call. No per-frame allocations while capturing.

var _board: FrameStats = null
var _cpu_slot: int
var _gpu_slot: int
var _objects_slot: int
var _draws_slot: int
var _latched: Array[WeakRef] = []
var _frame_on := false


func _init(board: FrameStats, cpu_slot: int, gpu_slot: int,
		objects_slot: int, draws_slot: int) -> void:
	_board = board
	_cpu_slot = cpu_slot
	_gpu_slot = gpu_slot
	_objects_slot = objects_slot
	_draws_slot = draws_slot
	if board != null:
		board.capture_changed.connect(_on_capture_changed)


## Whether any latched viewport is still measured (test/observation seam).
func is_measured() -> bool:
	for ref in _latched:
		if ref != null and is_instance_valid(ref.get_ref()):
			return true
	return false


## Per-frame entry. Returns whether sample_viewport() should follow this frame;
## a closed capture disarms every latched viewport.
func begin_frame(stats_on: bool) -> bool:
	_frame_on = stats_on and _board != null
	if not _frame_on:
		stop()
	return _frame_on


## One viewport of the set (index = its stable position in the set). A changed
## viewport at an index re-arms measurement on the new one. `counted` lands the
## previous frame's numbers on the board; pass false for a viewport that did
## not render (its counters would be stale).
func sample_viewport(index: int, viewport: Viewport, counted: bool) -> void:
	if not _frame_on or index < 0:
		return
	while _latched.size() <= index:
		_latched.append(null)
	var previous_ref := _latched[index]
	var previous: Object = previous_ref.get_ref() if previous_ref != null else null
	if previous != viewport:
		if previous is Viewport:
			RenderingServer.viewport_set_measure_render_time(
					(previous as Viewport).get_viewport_rid(), false)
		_latched[index] = weakref(viewport) if viewport != null else null
		if viewport != null:
			RenderingServer.viewport_set_measure_render_time(
					viewport.get_viewport_rid(), true)
	if viewport == null or not counted:
		return
	var rid := viewport.get_viewport_rid()
	_board.add(_cpu_slot,
			int(RenderingServer.viewport_get_measured_render_time_cpu(rid) * 1000.0))
	_board.add(_gpu_slot,
			int(RenderingServer.viewport_get_measured_render_time_gpu(rid) * 1000.0))
	_board.add(_objects_slot,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME))
	_board.add(_draws_slot,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME))


func stop() -> void:
	for ref in _latched:
		var previous: Object = ref.get_ref() if ref != null else null
		if previous is Viewport:
			RenderingServer.viewport_set_measure_render_time(
					(previous as Viewport).get_viewport_rid(), false)
	_latched.clear()
	_frame_on = false


func _on_capture_changed(active: bool) -> void:
	if not active:
		stop()
