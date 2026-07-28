class_name FrameStatsBoard
extends RefCounted
## The per-system frame-stats accumulator behind the F3 Stats tab. Hosts feed
## fixed integer slots (microseconds, or plain counts for the value slots)
## every frame WHILE `enabled`; the Stats pane drains a multi-frame window at
## its refresh cadence and shows mean-per-frame + worst-frame numbers.
##
## Cost contract: while the tab is closed `enabled` stays false and every feed
## site guards on it — no clock reads, no writes. While open, a feed is one
## `add()` into preallocated packed arrays: no per-frame Strings, Dictionaries
## or allocations anywhere on the hot path. Window math (means, copies) runs
## only at the pane's refresh cadence.

# --- Slots -------------------------------------------------------------------
# Times are microseconds unless the name says otherwise. One slot per span the
# hosts measure; the pane owns labels/grouping, the board owns only sums.
enum {
	# main_game frame legs (game shell _process)
	FRAME_WALL,            # true wall time between consecutive shell frames
	FRAME_PLAYER_BEFORE,   # LocalPlayerHost.before_world_tick
	FRAME_WORLD,           # GameWorld.tick total
	FRAME_PLAYER_AFTER,    # LocalPlayerHost.after_world_tick
	FRAME_HUD,             # GameHudHost.tick total
	# GameWorld.tick legs
	WORLD_FOLIAGE,
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
	EFFECTS_DRAIN,         # sim.drain_effects, summed over ticks
	EFFECTS_TICK,          # EffectWorld.advance_fixed_tick, summed over ticks
	PRESENT_SNAPSHOT,      # native get_present_snapshot build
	PRESENT_MISSION,       # MissionPresentPass.present_snapshot
	PRESENT_WIRE,          # WirePresentPass.present_snapshot
	PRESENT_FIRE,
	PRESENT_DESTRUCTION,
	PRESENT_THROWABLE,
	# GameHudHost.tick legs
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

## Feed gate. The pane owns it: true only while the Stats tab is the visible
## overlay tab. Every host feed site checks this ONCE per frame and skips all
## measurement when false.
var enabled := false

var _sums := PackedInt64Array()
var _maxes := PackedInt64Array()
var _counts := PackedInt32Array()
var _window_start_frame := 0


func _init() -> void:
	_sums.resize(SLOT_COUNT)
	_maxes.resize(SLOT_COUNT)
	_counts.resize(SLOT_COUNT)
	reset_window()


## Hot-path feed: amount is microseconds (or a count for the VALUE slots).
## Callers guard on `enabled`; add() itself stays branch-light.
func add(slot: int, amount: int) -> void:
	_sums[slot] += amount
	_counts[slot] += 1
	if amount > _maxes[slot]:
		_maxes[slot] = amount


## Frames elapsed since the window opened (render frames, host-independent).
func window_frames() -> int:
	return maxi(Engine.get_process_frames() - _window_start_frame, 0)


func window_sums() -> PackedInt64Array:
	return _sums.duplicate()


func window_maxes() -> PackedInt64Array:
	return _maxes.duplicate()


func window_counts() -> PackedInt32Array:
	return _counts.duplicate()


func reset_window() -> void:
	_sums.fill(0)
	_maxes.fill(0)
	_counts.fill(0)
	_window_start_frame = Engine.get_process_frames()
