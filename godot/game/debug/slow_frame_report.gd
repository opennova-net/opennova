class_name SlowFrameReport
extends Node

## The slow-frame readout under --verbose (an instrument with no behaviour). It arms the
## shell's FrameStats board, drains it once a frame after the other idle callbacks, and
## prints one line for each drained window whose wall time reached SLOW_FRAME_US, naming
## the time slots that took the most of it. A joiner's verbose log then carries, beside its
## LoadTimeline and JoinWindow lines, where every heavy frame of the join went (the frames
## after the S2C 0x0F included); unix_ms lines it up with the capture. The open dev tools,
## and a probe holding the board for its run (perf_mission_rows), own the board: no drain
## while they do. Beside it, a PipelineCompiles line names each
## frame whose read found pipelines compiled since the previous read (Godot's, by source, our
## RenderingDevice GLSL compiles and our RenderingDevice pipeline creates): the frames that pay a
## first draw's compiles. While the world times its first frames past a load
## (GameWorld.is_post_load_frames_open: a joiner's run on to its first C2S 0x0C), every window
## under SLOW_FRAME_US prints too, as a PostLoadFrame line: the S2C 0x0F's first-send window,
## frame by frame. And every SESSION_WINDOW_US of wall time a running mission's frames take, one
## SessionLeg line gives the session leg's parts as means per frame (the session frame's ticks,
## sinks and remainder, the present walks, the wire walk's parts, the joiner's tick phases): what
## a steady frame's session leg costs, part by part.

const SLOW_FRAME_US := 100_000
const TOP_SLOTS := 10
const MIN_SLOT_US := 1_000
## Godot's pipeline compilation sources (RenderingServer.get_rendering_info), in compile_totals'
## order; our RenderingDevice GLSL compiles (count, microseconds), then our RenderingDevice
## pipeline creates (count, microseconds), follow them.
const PIPELINE_SOURCES: Array[String] = ["canvas", "mesh", "surface", "draw", "specialization"]
## The label of a post-load frame's line under SLOW_FRAME_US.
const POST_LOAD_LABEL := "PostLoadFrame"
## The label of the session leg's windowed line.
const SESSION_LABEL := "SessionLeg"
## The wall time one SessionLeg line averages over, and the least mean a part prints at (us).
const SESSION_WINDOW_US := 2_000_000
const SESSION_MIN_PART_US := 10
## The post-load frames the canvas-program census runs on.
const CENSUS_FRAMES := 3
const US_PER_MS := 1000.0
## The slot-name prefixes of the session leg's parts (the board's slot order is the tree's).
const SESSION_PREFIXES: Array[String] = ["sim_", "present_", "effects_"]
## The SessionLeg line's remainders: [name, base slot name, the part slot names under it].
const SESSION_REMAINDERS := [
	["session_frame_other", "SIM_ADVANCE", ["SIM_STEP", "SIM_SINK"]],
	["sim_step_other", "SIM_STEP", ["SIM_HOST_PREP", "SIM_HOST_PUMP", "SIM_PLAYER_TAIL",
			"SIM_WEAPON_WALK", "SIM_NET", "SIM_CLIENT_MATERIALIZE", "SIM_CLIENT_MIRROR",
			"SIM_CLIENT_PROXIES", "SIM_CLIENT_WORLD", "SIM_CLIENT_ATTACH", "SIM_CLIENT_PLAYER",
			"SIM_ADM_RESOLVE"]],
	["wire_other", "PRESENT_WIRE", ["PRESENT_WIRE_PLAN", "PRESENT_WIRE_POSE", "PRESENT_WIRE_CTRL",
			"PRESENT_WIRE_BODY", "PRESENT_WIRE_WEAPON", "PRESENT_WIRE_SOUNDS", "PRESENT_WIRE_HELD",
			"PRESENT_WIRE_VIEWS", "PRESENT_WIRE_CULLED"]],
	["runtime_other", "WORLD_RUNTIME", ["SIM_ADVANCE", "PRESENT_SNAPSHOT", "PRESENT_MISSION",
			"PRESENT_WIRE", "PRESENT_FIRE", "PRESENT_DESTRUCTION", "PRESENT_THROWABLE",
			"PRESENT_SCARS"]],
]

var _board: FrameStats = null
var _tools_open := Callable()
var _post_load_open := Callable()
var _time_slots := PackedInt32Array()
var _compile_totals := PackedInt64Array()
var _session_slots := PackedInt32Array()
var _session_sums := PackedInt64Array()
var _session_frames := 0
# The post-load frames the canvas-program census has run on (CanvasProgramCensus, the first few).
var _census_frames := 0


func _init() -> void:
	name = "SlowFrameReport"
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 2_147_483_647


## `tools_open` answers whether the board's reader is elsewhere (the dev tools up, or a
## probe holding it for its run: they drain it then, and this readout leaves it undrained);
## `post_load_open` whether the world still times its first frames past a load.
func setup(board: FrameStats, tools_open: Callable, post_load_open := Callable()) -> void:
	_board = board
	_tools_open = tools_open
	_post_load_open = post_load_open
	_time_slots = time_slots()
	_session_slots = session_slots()


func _process(_delta: float) -> void:
	if _board == null or (_tools_open.is_valid() and bool(_tools_open.call())):
		return
	if not _board.is_capture_active():
		_board.set_capture_active(true)
		return
	var frame := Engine.get_process_frames()
	var unix_ms := int(Time.get_unix_time_from_system() * 1000.0)
	var window := _board.drain()
	var session := _fold_session(window, frame, unix_ms)
	if not session.is_empty():
		print_verbose(session)
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
	# The canvas programs first seen on the first frames the world times past a load: one first
	# seen here is one the load did not make (CanvasProgramCensus; the reveal took the census first).
	if _post_load_open.is_valid() and bool(_post_load_open.call()):
		if _census_frames < CENSUS_FRAMES:
			_census_frames += 1
			CanvasProgramCensus.note(get_tree(), "frame=%d" % frame)
	else:
		_census_frames = 0


## Folds one drained window into the session window (a frame whose session leg ran); past
## SESSION_WINDOW_US of their wall, returns that window's SessionLeg line and starts the next.
func _fold_session(window: FrameStatsWindow, frame: int, unix_ms: int) -> String:
	if window == null:
		return ""
	var sums := window.sums
	if sums.size() < FrameStats.SLOT_COUNT or sums[FrameStats.WORLD_RUNTIME] <= 0:
		return ""
	if _session_sums.size() != FrameStats.SLOT_COUNT:
		_session_sums.resize(FrameStats.SLOT_COUNT)
		_session_sums.fill(0)
	for slot in _session_slots:
		_session_sums[slot] += sums[slot]
	_session_frames += window.frames
	if _session_sums[FrameStats.FRAME_WALL] < SESSION_WINDOW_US:
		return ""
	var line := session_leg_line(_session_sums, _session_frames, frame, unix_ms)
	_session_sums.fill(0)
	_session_frames = 0
	return line


## The slots a SessionLeg window sums: the wall, the session leg, every slot named with a
## SESSION_PREFIXES prefix (times and counts), in the board's order.
static func session_slots() -> PackedInt32Array:
	var slots := PackedInt32Array([FrameStats.FRAME_WALL, FrameStats.WORLD_RUNTIME])
	for slot in range(FrameStats.SLOT_COUNT):
		var slot_name := FrameStats.slot_name(slot).to_lower()
		for prefix in SESSION_PREFIXES:
			if slot_name.begins_with(prefix):
				slots.append(slot)
				break
	return slots


## One session window's line: its frames, the mean wall, updates, wire rows and the cold
## re-plans, then each session-leg time slot's mean per frame at SESSION_MIN_PART_US or more in
## the board's order (world_runtime first), then the SESSION_REMAINDERS, in milliseconds.
static func session_leg_line(sums: PackedInt64Array, frames: int, frame: int, unix_ms: int) -> String:
	if frames <= 0 or sums.size() < FrameStats.SLOT_COUNT:
		return ""
	var per_frame := func(amount: int) -> float: return float(amount) / float(frames)
	var parts := PackedStringArray()
	for slot in session_slots():
		if slot == FrameStats.FRAME_WALL or FrameStats.slot_description(slot).begins_with("VALUE"):
			continue
		var mean: float = per_frame.call(sums[slot])
		if mean >= SESSION_MIN_PART_US:
			parts.append("%s %.2f" % [FrameStats.slot_name(slot).to_lower(), mean / US_PER_MS])
	for remainder: Array in SESSION_REMAINDERS:
		var base := slot_id(remainder[1])
		if sums[base] <= 0:
			continue
		var rest := sums[base]
		for part: String in remainder[2]:
			rest -= sums[slot_id(part)]
		parts.append("%s %.2f" % [remainder[0], per_frame.call(rest) / US_PER_MS])
	return "%s: frame=%d unix_ms=%d over %d frame(s): wall %.1f ms, updates %.2f, wire rows %.0f, cold plans %d, per frame ms: %s" % [
		SESSION_LABEL, frame, unix_ms, frames, per_frame.call(sums[FrameStats.FRAME_WALL]) / US_PER_MS,
		per_frame.call(sums[FrameStats.SIM_TICKS]),
		per_frame.call(sums[FrameStats.PRESENT_WIRE_ROWS]),
		sums[FrameStats.PRESENT_WIRE_COLD_PLANS], ", ".join(parts)]


## A FrameStats slot by its constant's name.
static func slot_id(slot_name: String) -> int:
	return ClassDB.class_get_integer_constant("FrameStats", slot_name)


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
