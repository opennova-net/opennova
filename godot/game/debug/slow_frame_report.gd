class_name SlowFrameReport
extends Node

## The slow-frame readout under --verbose (an instrument with no behaviour). It arms the
## shell's FrameStats board, drains it once a frame after the other idle callbacks, and
## prints one line for each drained window whose wall time reached SLOW_FRAME_US, naming
## the time slots that took the most of it. A joiner's verbose log then carries, beside its
## LoadTimeline and JoinWindow lines, where every heavy frame of the join went (the frames
## after the S2C 0x0F included); unix_ms lines it up with the capture. The open dev tools
## own the board: no drain while they are up. Beside it, a PipelineCompiles line names each
## frame whose read found pipelines compiled since the previous read (Godot's, by source, our
## RenderingDevice GLSL compiles and our RenderingDevice pipeline creates): the frames that pay a
## first draw's compiles. While the world times its first frames past a load
## (GameWorld.is_post_load_frames_open: a joiner's run on to its first C2S 0x0C), every window
## under SLOW_FRAME_US prints too, as a PostLoadFrame line: the S2C 0x0F's first-send window,
## frame by frame.

const SLOW_FRAME_US := 100_000
const TOP_SLOTS := 10
const MIN_SLOT_US := 1_000
## Godot's pipeline compilation sources (RenderingServer.get_rendering_info), in compile_totals'
## order; our RenderingDevice GLSL compiles (count, microseconds), then our RenderingDevice
## pipeline creates (count, microseconds), follow them.
const PIPELINE_SOURCES: Array[String] = ["canvas", "mesh", "surface", "draw", "specialization"]
## The label of a post-load frame's line under SLOW_FRAME_US.
const POST_LOAD_LABEL := "PostLoadFrame"

var _board: FrameStats = null
var _tools_open := Callable()
var _post_load_open := Callable()
var _time_slots := PackedInt32Array()
var _compile_totals := PackedInt64Array()


func _init() -> void:
	name = "SlowFrameReport"
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 2_147_483_647


## `tools_open` answers whether the dev tools are up (they drain the board then);
## `post_load_open` whether the world still times its first frames past a load.
func setup(board: FrameStats, tools_open: Callable, post_load_open := Callable()) -> void:
	_board = board
	_tools_open = tools_open
	_post_load_open = post_load_open
	_time_slots = time_slots()


func _process(_delta: float) -> void:
	if _board == null or (_tools_open.is_valid() and bool(_tools_open.call())):
		return
	if not _board.is_capture_active():
		_board.set_capture_active(true)
		return
	var frame := Engine.get_process_frames()
	var unix_ms := int(Time.get_unix_time_from_system() * 1000.0)
	var window := _board.drain()
	var line := report_line(window, _time_slots, frame, unix_ms)
	if line.is_empty() and _post_load_open.is_valid() and bool(_post_load_open.call()):
		line = report_line(window, _time_slots, frame, unix_ms, 0, POST_LOAD_LABEL)
	if not line.is_empty():
		print_verbose(line)
	# The pipelines compiled since the last frame's read (the previous frame's draw, this
	# frame's process): which first draw pays which compiles.
	var totals := compile_totals()
	var compiles := compile_line(_compile_totals, totals, frame, unix_ms)
	_compile_totals = totals
	if not compiles.is_empty():
		print_verbose(compiles)


## The board's time slots (every slot whose description does not mark it a VALUE).
static func time_slots() -> PackedInt32Array:
	var slots := PackedInt32Array()
	for slot in range(FrameStats.SLOT_COUNT):
		if not FrameStats.slot_description(slot).begins_with("VALUE"):
			slots.append(slot)
	return slots


## The process's compile totals so far: Godot's pipeline compilations by source
## (PIPELINE_SOURCES' order), then our RenderingDevice GLSL compiles' count and microseconds,
## then our RenderingDevice pipeline creates' count and microseconds.
static func compile_totals() -> PackedInt64Array:
	var totals := PackedInt64Array()
	for info in [RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_MESH,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW,
			RenderingServer.RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION]:
		totals.append(RenderingServer.get_rendering_info(info))
	totals.append_array(FrameStats.rd_glsl_compile_totals())
	totals.append_array(FrameStats.rd_pipeline_create_totals())
	return totals


## One frame's compile line, or "" when nothing compiled since `previous` (a first read, whose
## `previous` is empty, prints nothing): each Godot source that compiled, with its count, then
## our GLSL compiles and our pipeline creates, each with its count and milliseconds.
static func compile_line(previous: PackedInt64Array, current: PackedInt64Array, frame: int,
		unix_ms: int) -> String:
	var sources := PIPELINE_SOURCES.size()
	if previous.size() != sources + 4 or current.size() != sources + 4:
		return ""
	var parts := PackedStringArray()
	for i in sources:
		var compiled := current[i] - previous[i]
		if compiled > 0:
			parts.append("%s %d" % [PIPELINE_SOURCES[i], compiled])
	var glsl := current[sources] - previous[sources]
	if glsl > 0:
		parts.append("rd_glsl %d (%d ms)" % [glsl,
				(current[sources + 1] - previous[sources + 1]) / 1000])
	var pipelines := current[sources + 2] - previous[sources + 2]
	if pipelines > 0:
		parts.append("rd_pipelines %d (%d ms)" % [pipelines,
				(current[sources + 3] - previous[sources + 3]) / 1000])
	if parts.is_empty():
		return ""
	return "PipelineCompiles: frame=%d unix_ms=%d: %s" % [frame, unix_ms, ", ".join(parts)]


## One window's line, or "" when its FRAME_WALL sum stays under `min_wall_us`: the label, the
## wall, the render frames the window spans, then up to TOP_SLOTS of `slots` at MIN_SLOT_US or
## more, largest first, in milliseconds. Slots nest (a GameWorld leg sits inside
## frame_world), so the parts do not sum to the wall.
static func report_line(window: FrameStatsWindow, slots: PackedInt32Array, frame: int,
		unix_ms: int, min_wall_us: int = SLOW_FRAME_US, label: String = "SlowFrame") -> String:
	if window == null:
		return ""
	var sums := window.sums
	if sums.size() <= FrameStats.FRAME_WALL:
		return ""
	var wall := sums[FrameStats.FRAME_WALL]
	if wall < min_wall_us:
		return ""
	var ranked: Array[Vector2i] = []
	for slot in slots:
		if slot == FrameStats.FRAME_WALL or slot >= sums.size() or sums[slot] < MIN_SLOT_US:
			continue
		ranked.append(Vector2i(int(sums[slot]), slot))
	ranked.sort_custom(func(a: Vector2i, b: Vector2i) -> bool: return a.x > b.x)
	var parts := PackedStringArray()
	for i in mini(TOP_SLOTS, ranked.size()):
		parts.append("%s %d" % [FrameStats.slot_name(ranked[i].y).to_lower(), ranked[i].x / 1000])
	return "%s: frame=%d unix_ms=%d wall %d ms over %d frame(s): %s" % [
		label, frame, unix_ms, wall / 1000, window.frames, ", ".join(parts)]
