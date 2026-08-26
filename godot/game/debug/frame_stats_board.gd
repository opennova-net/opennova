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
	FRAME_STATS_SAMPLE,    # root render counters + menu-video timing collection
	FRAME_SHELL_CONTROL,   # shell state/input/end-screen preamble before gameplay devices
	FRAME_ROUND_FLOW,      # post-HUD round-cycle/session transition checks
	FRAME_MENU_SHELL,      # visible MenuShell driver + portrait model frame
	FRAME_MENU_VIDEO,      # native Bink decode + texture upload
	FRAME_DEBUG_REFRESH,   # active F3 page refresh at the overlay timer cadence
	FRAME_PROCESS_CALLBACKS,# earliest-to-latest idle Node callback window
	FRAME_PHYSICS_CALLBACKS,# summed earliest-to-latest physics callback windows
	# The engine time outside every Node callback, split at Godot's draw
	# signals (RootFramePhaseSampler): what used to be one residual row.
	FRAME_DEFERRED_FLUSH,   # latest idle callback -> frame_pre_draw: MessageQueue flush (call_deferred, queue_redraw -> _draw), transform flush, SceneTree tail, RS sync
	FRAME_DRAW,             # frame_pre_draw -> frame_post_draw: RenderingServer.draw for every viewport (cull, draw lists, submit, present)
	FRAME_PACING_INPUT,     # frame_post_draw -> next earliest idle callback: audio/script frame hooks, input pump, physics servers, SceneTree head
	FRAME_TIME_PROCESS,     # VALUE (us): Performance.TIME_PROCESS of the previous frame (process + flush + sync + draw), the split's cross-check
	FRAME_PHYSICS_SERVER,   # VALUE (us): Performance.TIME_PHYSICS_PROCESS (max over the frame's physics iterations, servers included)
	FRAME_PHYSICS_ITERATIONS,# VALUE: physics iterations run before this render frame
	# GameWorld.tick legs
	WORLD_FOLIAGE,
	WORLD_RUNTIME,
	WORLD_WEATHER,
	WORLD_BLINK,
	WORLD_IRIS,
	WORLD_AUDIO,
	WORLD_LOCAL_VIEW,       # local-player camera/viewmodel publication
	WORLD_FRAMEFX,          # auxiliary-view pose synchronization
	WORLD_SCENE_ENV,        # render-eye fog/ambient + water classifier
	WORLD_ENV_NODES,        # weather smoothing, sun direction, sky dome, celestial bodies
	WORLD_WATER,            # water strip march + mirror camera
	WORLD_TERRAIN,          # terrain draw-list and MATCHTERRAIN publication
	WORLD_NETWORK_FRAME,    # net-session edge observation/environment apply
	WORLD_SUN_VEIL,         # celestial exposure feed
	WORLD_LIGHT,            # point-light selection and terrain/shadow context
	WORLD_MATERIAL,         # awake ObjectModel runtime/material/animation advance
	MODEL_CLOCK_ANIMATION,  # native model clocks + part/body animation
	MODEL_PANM,             # native PANM transform evaluation/publication
	MODEL_MATERIAL,         # native dynamic material generator/texture writes
	MODEL_ORDER_BOUNDS,     # alpha-strip ordering + changed model bounds
	MODEL_AWAKE_MODELS,     # VALUE: models visited by the shared awake walk
	MODEL_RENDERABLE_MODELS,# VALUE: visited models currently camera-submitted
	WORLD_SLOT_SHADOW,      # render-slot ground-shadow planning/publication
	WORLD_PARTICLES,        # EffectWorld render-frame publication
	WORLD_CLEAR,            # viewport clear-color publication
	WORLD_ENV_CUBE,         # environment-cube update submission
	# The render-occlusion frame, split native collection from shell publication.
	OCCL_BUILD,            # native OcclusionWorld::build_frame (portal walk)
	OCCL_PROBE,            # native per-entity render-gate loop
	OCCL_APPLY,            # complete post-run visibility/lighting publication
	OCCL_GLUE,             # run_occlusion_frame call minus build+probe (marshalling)
	OCCL_BUILDING_QUERY,   # native building visibility-delta build + marshalling
	OCCL_BUILDING_APPLY,   # section-mask and building visibility node writes
	OCCL_CULL_QUERY,       # native entity-cull delta build + marshalling
	OCCL_CULL_APPLY,       # entity-cull visibility node writes
	OCCL_LIGHT_QUERY,      # native per-drawn-entity sun-visibility queries
	OCCL_LIGHT_APPLY,      # placed/wire lighting-context writes
	OCCL_WATER_APPLY,      # final blink-water visibility write
	# MissionPresentation legs
	SIM_STEP,              # sim.step() total, summed over the frame's logic ticks
	SIM_NET,               # native wire leg of step: joiner recv/uplink pump, or the
	                       # host's local ClientState decode/fold
	SIM_HOST_PREP,         # viewport/input/request setup before the portable host pump
	SIM_HOST_PUMP,         # complete npruntime host owner iteration
	SIM_HOST_RECEIVE,      # recv drain + missing-sequence service
	SIM_HOST_CONNECTIONS,  # connection/spawn service
	SIM_HOST_ADAPTER,      # binding callback at the pre-server registration seam
	SIM_SERVER_TICK,       # authoritative C2S -> world -> rules -> replication loop
	SIM_SERVER_INPUT,      # C2S apply + human/breath prepass
	SIM_SERVER_WORLD,      # World::run_logic_tick total
	SIM_WORLD_SETUP,       # per-tick shared state/fire-sound setup
	SIM_WORLD_SCRIPTS,     # registered authored systems (WAC + BMS)
	SIM_WORLD_AI,          # AI/entity system
	SIM_AI_REACTIONS,      # damage-hit reaction queue
	SIM_AI_COLLISION,      # collision/proximity table rebuild
	SIM_AI_ENTITIES,       # brain + infantry body pass
	SIM_AI_INFANTRY,       # infantry/player body rows
	SIM_AI_INFANTRY_REMOTE,# authority animation/collision for remote players
	SIM_AI_INFANTRY_COMBAT,# NPC perception, reactions, and aim
	SIM_AI_INFANTRY_ANIMATION,# weapon/body channels + root-motion sampling
	SIM_AI_INFANTRY_COLLISION,# mounted and ordinary movement resolver calls
	SIM_AI_INFANTRY_COLLISION_CONTACTS,# candidate/model contact passes
	SIM_AI_INFANTRY_COLLISION_REPULSION,# person-sphere separation
	SIM_AI_INFANTRY_COLLISION_GROUND,# final terrain/model ground ray
	SIM_AI_OTHER_ENTITIES, # non-infantry state-machine rows
	SIM_AI_AUTH_VEHICLES,  # authority vehicle motor pass
	SIM_AI_VEHICLE_SCAN,   # vehicle-trait registry scan
	SIM_AI_VEHICLE_MOTORS, # selected family motor dispatch
	SIM_AI_VEHICLE_RIDERS, # final carrier-relative rider refresh
	SIM_AI_CLIENT_VEHICLES,# client vehicle prediction/presentation pass
	SIM_AI_EVENTS,         # timed AI event queue
	SIM_WORLD_ATTACHMENTS, # emplacement attachment posing
	SIM_ATTACHMENT_ORPHANS,# dead-parent chain cleanup
	SIM_ATTACHMENT_CHILDREN,# child userpoint/root posing
	SIM_ATTACHMENT_RIDERS, # riders refreshed from attached children
	SIM_WORLD_THROWABLES,
	SIM_WORLD_WEAPONS,     # mounted weapon action pump
	SIM_WORLD_PROJECTILES,
	SIM_WORLD_DESTRUCTION, # explosions, dead-item physics, death pieces
	SIM_WORLD_HOUSEKEEPING,# waypoint/recount/mailbox tail
	SIM_MATCH,
	SIM_SERVER_RULES,      # deaths, respawn, win/capture, maintenance events
	SIM_SERVER_REPLICATION,# snapshot + per-connection S2C fan + linger tail
	SIM_REPLICATION_QUERY_PREP,# stable post-movement solid candidate index build
	SIM_REPLICATION_QUERY_COLLECT,# effective-solid target/bound collection
	SIM_REPLICATION_QUERY_GRID,# stable spatial cell publication
	SIM_REPLICATION_QUERY_GRID_SPAN,# candidate -> covered-cell range calculation
	SIM_REPLICATION_QUERY_GRID_BUCKET,# dense/hash bucket clear/population
	SIM_REPLICATION_QUERY_GRID_WORKSPACE,# query marks/results workspace reset
	SIM_REPLICATION_SNAPSHOT,# registry -> wire snapshot build
	SIM_REPLICATION_FAN,   # complete per-recipient S2C fan
	SIM_REPLICATION_FAN_SETUP,# recipient anchor + frame header setup
	SIM_REPLICATION_ROUNDS,# round-event selection
	SIM_REPLICATION_ENTITIES,# entity priority/budget selection
	SIM_REPLICATION_ENTITY_SETUP,# age/self anchor preparation
	SIM_REPLICATION_ENTITY_SCORE,# distance/view/LOS priority scoring
	SIM_REPLICATION_ENTITY_LOS,# collision-world LOS raycasts inside scoring
	SIM_REPLICATION_ENTITY_LOS_TERRAIN,# heightfield march
	SIM_REPLICATION_ENTITY_LOS_SECTOR,# static/dynamic solid walk
	SIM_REPLICATION_ENTITY_SORT,# descending priority ordering
	SIM_REPLICATION_ENTITY_BUDGET,# byte-budget selection + cache stamps
	SIM_REPLICATION_ENCODE,# selected 0x0A body serialization
	SIM_REPLICATION_ENQUEUE,# semantic transport enqueue
	SIM_HOST_SEND,         # remote transport drain/frame/send
	SIM_HOST_PLAYER,       # local view/weapon/medic device tail
	SIM_CLIENT_SETUP,      # client role clock + keepalive setup
	SIM_CLIENT_RECEIVE,    # loopback/wire receive + state fold
	SIM_CLIENT_MAINTENANCE,# decoded-state timers and movers
	SIM_CLIENT_SEND,       # joiner-only C2S build/frame
	SIM_ADM_RESOLVE,       # late animation-registry resolution after the tick
	SIM_SINK,              # typed per-tick Godot presentation/effects callback
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
	PRESENT_MISSION_CORE,  # row scan, transform, and submission gate
	PRESENT_MISSION_AIM,   # right-hand collapse + aim-overlay publication
	PRESENT_MISSION_CONTROLS,# PANM phase and CTRL-bus publication
	PRESENT_MISSION_VISIBILITY,# sim visibility + section-mask publication
	PRESENT_MISSION_BODY,  # skeletal body-pose selection/publication
	PRESENT_MISSION_MUZZLE,# posed muzzle readback into the simulation
	PRESENT_MISSION_ROWS,  # VALUE: rows visited by the mission presenter
	PRESENT_MISSION_SUBMITTED_ROWS,# VALUE: camera-submitted rows
	PRESENT_MISSION_BODY_ROWS,# VALUE: rows eligible for a body pose
	PRESENT_MISSION_MUZZLE_ROWS,# VALUE: rows requiring muzzle readback
	PRESENT_WIRE,          # WirePresentPass.present_snapshot
	PRESENT_FIRE,
	PRESENT_DESTRUCTION,
	PRESENT_THROWABLE,
	PRESENT_SCARS,
	# GameHudPresenter.tick legs
	HUD_SCALARS,
	HUD_ATTACH,
	HUD_WAYPOINT,
	HUD_INFO,
	HUD_FLUSH,
	# HudOverlay._draw runs in the deferred flush, not the HUD tick: the native
	# compile + canvas emit of the previous frame's redraw (consumed per tick).
	HUD_DRAW_COMPILE,
	HUD_DRAW_EMIT,
	# Measured render times (RenderingServer, previous frame), stored as us
	RENDER_ROOT_CPU,
	RENDER_ROOT_GPU,
	RENDER_WATER_CPU,
	RENDER_WATER_GPU,
	RENDER_Q3_CPU,         # FrameFx's shared-world Q3 view
	RENDER_Q3_GPU,
	RENDER_SLOT_CPU,       # summed over the slot-shadow captures that rendered
	RENDER_SLOT_GPU,
	# Per-pass render counts (RenderingServer per-viewport render info for the
	# previous frame). VALUE slots: what each pass actually submitted, so pass
	# cost attribution (main view vs shadow maps vs the water mirror) is read
	# off the board instead of guessed.
	RENDER_MAIN_OBJECTS,   # VALUE: root viewport visible-pass objects
	RENDER_MAIN_DRAWS,     # VALUE: root viewport visible-pass draw calls
	RENDER_SHADOW_OBJECTS, # VALUE: root viewport shadow-pass objects
	RENDER_SHADOW_DRAWS,   # VALUE: root viewport shadow-pass draw calls
	RENDER_WATER_OBJECTS,  # VALUE: water mirror visible-pass objects
	RENDER_WATER_DRAWS,    # VALUE: water mirror visible-pass draw calls
	RENDER_Q3_OBJECTS,     # VALUE: Q3 view visible-pass objects
	RENDER_Q3_DRAWS,       # VALUE: Q3 view visible-pass draw calls
	RENDER_SLOT_OBJECTS,   # VALUE: slot captures' visible-pass objects (rendered slots only)
	RENDER_SLOT_DRAWS,     # VALUE: slot captures' visible-pass draw calls
	RENDER_SLOT_VIEWPORTS, # VALUE: slot captures that rendered
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
