class_name FrameStatsBoard
extends RefCounted
## The per-system frame-stats accumulator behind the F3 Stats tab. Hosts feed
## fixed integer slots (microseconds, or plain counts for the value slots)
## every frame while capture is active; the Stats pane drains a multi-frame window at
## its refresh cadence and shows mean-per-frame + worst-frame numbers.
##
## Cost contract: while the tab is closed capture stays inactive and every feed
## site guards on it — no clock reads, no writes. While open, a feed is one
## `add()` into preallocated packed arrays: no per-frame Strings, Dictionaries
## or allocations anywhere on the hot path. Window math (means, copies) runs
## only at the pane's refresh cadence.

signal capture_changed(active: bool)


class CaptureWindow:
	extends RefCounted

	var frames: int
	var sums: PackedInt64Array
	var peaks: PackedInt64Array
	var sample_frames: PackedInt32Array

	func _init(p_frames: int, p_sums: PackedInt64Array,
			p_peaks: PackedInt64Array, p_sample_frames: PackedInt32Array) -> void:
		frames = p_frames
		sums = p_sums
		peaks = p_peaks
		sample_frames = p_sample_frames


# --- Slots -------------------------------------------------------------------
# Times are microseconds unless the name says otherwise. One slot per span the
# hosts measure; the pane owns labels/grouping, the board owns only sums.
enum {
	# main_game frame legs (game shell _process)
	FRAME_WALL,            # true wall time between consecutive shell frames
	FRAME_PLAYER_BEFORE,   # LocalPlayerPresenter.before_world_tick
	FRAME_WORLD,           # GameWorld.tick total
	FRAME_PLAYER_AFTER,    # LocalPlayerPresenter.after_world_tick
	FRAME_HUD,             # GameHudPresenter.tick total
	# GameWorld.tick legs
	WORLD_FOLIAGE,
	WORLD_RUNTIME,
	WORLD_WEATHER,
	WORLD_BLINK,
	WORLD_IRIS,
	WORLD_AUDIO,
	# The render-occlusion frame, split build/probe/apply
	OCCL_BUILD,            # native OcclusionWorld::build_frame (portal walk)
	OCCL_PROBE,            # native per-entity render-gate loop
	OCCL_APPLY,            # GDScript node application (masks + culled set + water)
	OCCL_GLUE,             # run_occlusion_frame call minus build+probe (marshalling)
	# MissionRuntime legs
	SIM_STEP,              # sim.step() total, summed over the frame's logic ticks
	SIM_NET,               # native wire leg of step: joiner recv/uplink pump, or the
	                       # host's ClientState fold (S2C emit stays inside SIM_STEP
	                       # until npruntime grows a phase seam)
	SIM_TICKS,             # VALUE: logic ticks run this frame
	TRACE_TERRAIN,         # projectile trace terrain leg
	TRACE_STATIC,          # projectile trace static-entity leg
	TRACE_DYNAMIC,         # projectile trace dynamic-entity leg
	TRACE_PERSON,          # projectile trace person leg
	TRACE_CALLS,           # VALUE: projectile traces
	TRACE_STATIC_SURVIVORS,# VALUE: static broad-phase survivors
	TRACE_DYNAMIC_SURVIVORS,# VALUE: dynamic broad-phase survivors
	TRACE_PERSON_SURVIVORS,# VALUE: person broad-phase survivors
	TRACE_STATIC_FACES,    # VALUE: static survivor face-set sizes
	TRACE_DYNAMIC_FACES,   # VALUE: dynamic survivor face-set sizes
	EFFECTS_DRAIN,         # sim.drain_effects, summed over ticks
	EFFECTS_TICK,          # EffectWorld.advance_fixed_tick, summed over ticks
	PRESENT_SNAPSHOT,      # native get_present_snapshot build
	PRESENT_MISSION,       # MissionPresentPass.present_snapshot
	PRESENT_WIRE,          # WirePresentPass.present_snapshot
	PRESENT_FIRE,
	PRESENT_DESTRUCTION,
	PRESENT_THROWABLE,
	# GameHudPresenter.tick legs
	HUD_SCALARS,
	HUD_ATTACH,
	HUD_WAYPOINT,
	HUD_INFO,
	HUD_FLUSH,
	# Measured render times (RenderingServer, previous frame), stored as us
	RENDER_ROOT_CPU,
	RENDER_ROOT_GPU,
	RENDER_WATER_CPU,
	RENDER_WATER_GPU,
	SLOT_COUNT,
}

var _sums := PackedInt64Array()
var _peaks := PackedInt64Array()
var _sample_frames := PackedInt32Array()
var _last_frame := PackedInt64Array()
var _frame_totals := PackedInt64Array()
var _window_start_frame := 0
var _capture_active := false


func _init() -> void:
	_sums.resize(SLOT_COUNT)
	_peaks.resize(SLOT_COUNT)
	_sample_frames.resize(SLOT_COUNT)
	_last_frame.resize(SLOT_COUNT)
	_frame_totals.resize(SLOT_COUNT)
	_reset_window()


## Open or close the one capture window. Hosts observe this same edge for
## auxiliary diagnostics such as native trace timing and viewport measurement.
func set_capture_active(active: bool) -> void:
	if active == _capture_active:
		return
	_capture_active = active
	_reset_window()
	capture_changed.emit(active)


func is_capture_active() -> bool:
	return _capture_active


## Hot-path feed: amount is microseconds (or a count for the VALUE slots).
## Callers still guard on is_capture_active() before taking timestamps; this
## defensive gate prevents a stale producer from contaminating a closed window.
## Multiple writes to one slot in one render frame are summed before the peak is
## compared, so "peak" really means the worst render frame.
func add(slot: int, amount: int) -> void:
	if not _capture_active:
		return
	var frame := Engine.get_process_frames()
	_sums[slot] += amount
	if _last_frame[slot] != frame:
		_last_frame[slot] = frame
		_frame_totals[slot] = amount
		_sample_frames[slot] += 1
	else:
		_frame_totals[slot] += amount
	if _frame_totals[slot] > _peaks[slot]:
		_peaks[slot] = _frame_totals[slot]


## Atomically copy and reset the current capture window. The allocation/copies
## happen only at the pane's divided refresh cadence, never on producer paths.
func drain() -> CaptureWindow:
	var window := CaptureWindow.new(
			maxi(Engine.get_process_frames() - _window_start_frame, 0),
			_sums.duplicate(), _peaks.duplicate(), _sample_frames.duplicate())
	_reset_window()
	return window


func _reset_window() -> void:
	_sums.fill(0)
	_peaks.fill(0)
	_sample_frames.fill(0)
	_last_frame.fill(-1)
	_frame_totals.fill(0)
	_window_start_frame = Engine.get_process_frames()
