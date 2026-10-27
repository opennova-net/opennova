class_name LoadingPresentInstrument
extends RefCounted

## The loading screen's slow-present readout under --verbose (an instrument with no behaviour; the
## LF program's, kept while D-NET-1377 is open). LoadingScreen makes one only under --verbose and
## calls it around each present's forced draw. A present whose forced draw ran 50 ms or more names
## its split (the forced draw's frame setup, the scene and canvas updates ahead of the draws), the
## measured draws of the frame drawn before it, the offscreen targets it held, the texture and
## buffer memory the load made on the device since the previous present, and what the forced draw
## drew on the screen viewport: its canvas draw calls and items, each canvas layer with visible
## canvas items there, and the pipelines the forced draw compiled. The device keeps a ring of
## frames: a forced draw ends by waiting on the fence of the frame its next slot last held and
## reading that frame's GPU and CPU timestamps, so the measured draws (and the wall past the draw's
## own recording) are the frame drawn before this present (in a load, the load's one forced world
## frame when it ran since the previous present), never the present's own. When the screen leaves
## the tree, one line tallies its presents: their count, summed and longest wall, so a load's wall
## can be told from its presents' share of it.

const SLOW_PRESENT_USEC := 50000

# The device memory at the last present: the slow-present line names what the load made on the
# device between two presents.
var _present_texture_bytes := -1
var _present_buffer_bytes := -1
# This present's reads ahead of its forced draw.
var _texture_bytes := -1
var _buffer_bytes := -1
var _compiles_before: Array = []
var _root: Viewport = null
# The presents this screen made.
var _presents := 0
var _presents_usec := 0
var _present_max_usec := 0


## Ahead of the forced draw: the root's and the screen viewport's draws are timed from the load's
## first present, and the device memory and pipeline compiles are read.
func before_draw(root: Viewport, screen_viewport: Viewport) -> void:
	_root = root
	RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
	RenderingServer.viewport_set_measure_render_time(screen_viewport.get_viewport_rid(), true)
	_texture_bytes = RenderingServer.get_rendering_info(
			RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED)
	_buffer_bytes = RenderingServer.get_rendering_info(
			RenderingServer.RENDERING_INFO_BUFFER_MEM_USED)
	_compiles_before = _pipeline_compiles()


## Past the forced draw: the slow-present lines when it ran SLOW_PRESENT_USEC or more, then the mark
## the next frame's recording opens with, so the next slow-present line's GPU marks of that frame
## start at its first recorded command.
func after_draw(draw_usec: int, present_start_ms: int, progress: int, screen_viewport: Viewport,
		held: Array) -> void:
	if draw_usec >= SLOW_PRESENT_USEC:
		_note_slow_present(draw_usec, present_start_ms, progress, screen_viewport, held)
	var device := RenderingServer.get_rendering_device()
	if device != null:
		device.capture_timestamp("LoadingScreen past a present")
	_present_texture_bytes = _texture_bytes
	_present_buffer_bytes = _buffer_bytes


## One present's wall, for the tally.
func tally(present_usec: int) -> void:
	_presents += 1
	_presents_usec += present_usec
	_present_max_usec = maxi(_present_max_usec, present_usec)


## The tally line, when the screen made a present.
func report_presents() -> void:
	if _presents > 0:
		print_verbose("LoadingScreen: %d present(s), %d ms (the longest %d ms)" % [
				_presents, _presents_usec / 1000, _present_max_usec / 1000])


func _note_slow_present(draw_usec: int, present_start_ms: int, progress: int,
		screen_viewport: Viewport, held: Array) -> void:
	var held_names := PackedStringArray()
	for row in held:
		held_names.append("%s %dx%d mode %d" % [row[2], row[3].x, row[3].y, row[1]])
	var root_rid := _root.get_viewport_rid()
	var screen_rid := screen_viewport.get_viewport_rid()
	var line := ("LoadingScreen: present at %d%%: %d ms, force_draw %d ms (frame setup %.1f ms);"
			+ " the frame drawn before it (the timestamps a forced draw reads): the root gpu"
			+ " %.1f ms cpu %.1f ms, the screen viewport %s gpu %.1f ms cpu %.1f ms; the targets"
			+ " held: [%s]")
	print_verbose(line % [
			progress, GameWorld.tick_count_ms() - present_start_ms, draw_usec / 1000,
			RenderingServer.get_frame_setup_time_cpu(),
			RenderingServer.viewport_get_measured_render_time_gpu(root_rid),
			RenderingServer.viewport_get_measured_render_time_cpu(root_rid),
			screen_viewport.name,
			RenderingServer.viewport_get_measured_render_time_gpu(screen_rid),
			RenderingServer.viewport_get_measured_render_time_cpu(screen_rid),
			", ".join(held_names)])
	if _present_texture_bytes >= 0 and _texture_bytes >= 0:
		print_verbose("LoadingScreen: since the previous present: textures %+d MB, buffers %+d MB"
				% [
				(_texture_bytes - _present_texture_bytes) >> 20,
				(_buffer_bytes - _present_buffer_bytes) >> 20])
	if _texture_bytes >= 0:
		# The device frees a frame's released objects once that frame's fence is waited
		# (rendering_device.cpp _begin_frame -> _free_pending_resources), inside the forced
		# draw: a drop here is the objects the frame drawn before it released.
		print_verbose("LoadingScreen: across the forced draw: textures %+.1f MB, buffers %+.1f MB"
				% [
				(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED)
						- _texture_bytes) / 1048576.0,
				(RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED)
						- _buffer_bytes) / 1048576.0])
	var layers := {}
	for node in screen_viewport.find_children("*", "CanvasItem", true, false):
		var item := node as CanvasItem
		if item.get_viewport() != screen_viewport or not item.is_visible_in_tree():
			continue
		var layer := item.get_canvas_layer_node()
		if layer != null and not layer.visible:
			continue
		var key := "(canvas)" if layer == null else "%s %d" % [layer.name, layer.layer]
		layers[key] = int(layers.get(key, 0)) + 1
	var layer_rows := PackedStringArray()
	for key in layers:
		layer_rows.append("%s items %d" % [key, layers[key]])
	var compiles := _pipeline_compiles()
	var compiled := PackedStringArray()
	for i in range(mini(compiles.size(), _compiles_before.size())):
		compiled.append(str(compiles[i] - _compiles_before[i]))
	var drawn := ("LoadingScreen: the forced draw on %s: canvas %d draws over %d items; the"
			+ " visible layers [%s]; pipelines compiled (canvas, mesh, surface, draw,"
			+ " specialization) [%s]")
	print_verbose(drawn % [
			screen_viewport.name,
			RenderingServer.viewport_get_render_info(screen_rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_CANVAS,
					RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME),
			RenderingServer.viewport_get_render_info(screen_rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_CANVAS,
					RenderingServer.VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME),
			", ".join(layer_rows), ", ".join(compiled)])
	_note_frame_marks()


## The GPU split of the frame drawn before the present: every RenderingDevice timestamp that frame
## recorded (the previous present's closing mark, the mission load's "load <span>" marks,
## LoadTimeline.set_gpu_marks, and the measured viewports' vp_ pairs), each as its GPU and CPU
## offset from the first, then the GPU span: a span near the force_draw's wall is that frame's GPU
## work, and the gap ahead of a mark is the work recorded since the mark before it.
func _note_frame_marks() -> void:
	var device := RenderingServer.get_rendering_device()
	if device == null:
		return
	var count := device.get_captured_timestamps_count()
	if count == 0:
		return
	var gpu_base := device.get_captured_timestamp_gpu_time(0)
	var cpu_base := device.get_captured_timestamp_cpu_time(0)
	var marks := PackedStringArray()
	for i in count:
		# GPU times are nanoseconds, CPU times microseconds.
		marks.append("%s +%.1f (cpu +%.1f)" % [device.get_captured_timestamp_name(i),
				(device.get_captured_timestamp_gpu_time(i) - gpu_base) / 1000000.0,
				(device.get_captured_timestamp_cpu_time(i) - cpu_base) / 1000.0])
	print_verbose(("LoadingScreen: the frame drawn before it (frame %d), its GPU marks (ms from the"
			+ " first): GPU span %.1f ms over %d mark(s): [%s]") % [
			device.get_captured_timestamps_frame(),
			(device.get_captured_timestamp_gpu_time(count - 1) - gpu_base) / 1000000.0, count,
			", ".join(marks)])


## The process's pipeline compilations so far, by kind (canvas, mesh, surface, draw,
## specialization).
static func _pipeline_compiles() -> Array:
	var counts := []
	for info in [RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_MESH,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION]:
		counts.append(RenderingServer.get_rendering_info(info))
	return counts
