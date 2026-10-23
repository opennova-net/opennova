class_name SlowFrameReport
extends Node

## The slow-frame readout under --verbose (an instrument with no behaviour). It arms the
## shell's FrameStats board, drains it once a frame after the other idle callbacks, and
## prints one line for each drained window whose wall time reached SLOW_FRAME_US, naming
## the time slots that took the most of it. A joiner's verbose log then carries, beside its
## LoadTimeline and JoinWindow lines, where every heavy frame of the join went (the frames
## after the S2C 0x0F included); unix_ms lines it up with the capture. The open dev tools
## own the board: no drain while they are up.

const SLOW_FRAME_US := 100_000
const TOP_SLOTS := 10
const MIN_SLOT_US := 1_000

var _board: FrameStats = null
var _tools_open := Callable()
var _time_slots := PackedInt32Array()


func _init() -> void:
	name = "SlowFrameReport"
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 2_147_483_647


## `tools_open` answers whether the dev tools are up (they drain the board then).
func setup(board: FrameStats, tools_open: Callable) -> void:
	_board = board
	_tools_open = tools_open
	_time_slots = time_slots()


func _process(_delta: float) -> void:
	if _board == null or (_tools_open.is_valid() and bool(_tools_open.call())):
		return
	if not _board.is_capture_active():
		_board.set_capture_active(true)
		return
	var line := report_line(_board.drain(), _time_slots, Engine.get_process_frames(),
			int(Time.get_unix_time_from_system() * 1000.0))
	if not line.is_empty():
		print_verbose(line)


## The board's time slots (every slot whose description does not mark it a VALUE).
static func time_slots() -> PackedInt32Array:
	var slots := PackedInt32Array()
	for slot in range(FrameStats.SLOT_COUNT):
		if not FrameStats.slot_description(slot).begins_with("VALUE"):
			slots.append(slot)
	return slots


## One window's line, or "" when its FRAME_WALL sum stays under SLOW_FRAME_US: the wall,
## the render frames the window spans, then up to TOP_SLOTS of `slots` at MIN_SLOT_US or
## more, largest first, in milliseconds. Slots nest (a GameWorld leg sits inside
## frame_world), so the parts do not sum to the wall.
static func report_line(window: FrameStatsWindow, slots: PackedInt32Array, frame: int,
		unix_ms: int) -> String:
	if window == null:
		return ""
	var sums := window.sums
	if sums.size() <= FrameStats.FRAME_WALL:
		return ""
	var wall := sums[FrameStats.FRAME_WALL]
	if wall < SLOW_FRAME_US:
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
	return "SlowFrame: frame=%d unix_ms=%d wall %d ms over %d frame(s): %s" % [
		frame, unix_ms, wall / 1000, window.frames, ", ".join(parts)]
