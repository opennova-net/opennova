class_name GameWorld
extends Node3D

# Loads a playable world (terrain + environment + vegetation + foliage) from ONE
# resource root and wires it onto the engine nodes it contains (Terrain,
# MissionEnvironment, Water). The data core is shared engine code
# (TerrainData, EnvFile, FoliageDispatcher, VegAssets); this node is just
# runtime orchestration — one loader, one root, no fallbacks. The scene lives
# in game_world.tscn so the game shell and focused engine tests can instance it;
# production play mounts the selected runtime resource directory.
#
# EMBEDDER CONTRACT: a shell or focused test instances game_world.tscn, optionally injects a root,
# calls one load_* entry, then
#   * drives tick(camera_position) once per frame while playing (foliage ->
#     runtime logic+present -> audio, in that order; pausing = not ticking),
#   * provides the camera that position comes from,
#   * consumes mission_effects (HUD text / win / waypoints / dialog routing),
#   * calls unload() to tear the played world down (placed objects, runtime,
#     audio, env overrides) before loading another mission or leaving.

const VegAssets := preload("res://game/terrain/veg_assets.gd")
# Godot's per-geometry reservation in the global shader buffer (vec4 values) for
# a shader that declares instance uniforms; see _instance_uniform_geometry_estimate.
const INSTANCE_UNIFORM_VALUES_PER_GEOMETRY := 16
const GameFramePipelineScript := preload("res://game/world/game_frame_pipeline.gd")
const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const FirstPersonArmsWitness := preload(
		"res://game/world/first_person_arms_witness.gd")
const DebugViewStatus := preload(
		"res://game/debug/debug_view_status.gd")

signal world_loaded()
signal load_failed(reason: String)
## A joiner's authoritative session record (post-auth S2C 0x7B) resolved during
## the pre-load wait: server/mission names + the exact wire-header world about
## to be constructed. The shell refreshes its loading screen from this — retail's
## connect stream fills the same session vars before its header-backed load
## [orig: parse_server_session_variables @ 0x5202f0].
signal join_session_identified(info: Dictionary)
## A joiner crossed the authoritative admission edge. Wire-header world load
## completion is intentionally separate: the shell keeps the loading presentation
## raised until this edge (or until the host requests a deployment-zone pick).
signal join_admission_ready()
## A pick-required join reached the player-paced deployment stage: the host granted
## the loadouts and holds this player respawn-pending until a deploy pick. The shell
## opens the DEATH deploy screen; the join watchdog has stopped (everything past this
## point is player-paced). [orig: 0x0A flags1 bit1 -> the DEATH screen; net-re 5.61]
signal join_deploy_pick_required()
## An ESTABLISHED in-match session went silent past the witnessed connection reap
## window (JO cs_dir0.timeout_ms = 120000 ms). Retail does not raise an in-world
## dialog for this: its transport reaps the peer and the disconnect event maps an
## error code onto g_mission_exit_reason, i.e. it EXITS THE MISSION with a reason.
## The shell's analog is return-to-menu with `reason` surfaced the same way a join
## failure is. Emitted at most ONCE per session.
## [orig: CNapiNetwork_Init @ 0x4ca4a0 (timeout stores @ 0x4caa81/@ 0x4cab54) ->
##  CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0]
signal session_lost(reason: String)
# Mission-load progress, 0..100, emitted when each stage of the engine's
# mission load plan starts (MissionData.load_progress_percent — the
# witnessed per-stage anchors, engine/runtime/mission/mission_load_plan.h)
# and pulsed at the object stage's value from inside the placement loop.
signal load_progress(percent: int)
# Presentation side effects drained from the mission runtime's EffectLog each tick
# (kind: "text"/"debug_text"/"win"/"subgoal_*"/"show_waypoints"/"set_light"/"dialog").
# Player text is consumed by the HUD; debug_text remains a distinct unrouted channel.
# "dialog" is also routed straight to mission audio below.
signal mission_effects(effects: Array)
## The CPU-built gameplay-map depthspin water mask changed.
signal minimap_water_changed(mask: ImageTexture)

# A mission (.bms) to boot into. When set, the mission's header selects the
# terrain + environment (terrain_file/env_file below are ignored) and its placed
# objects are populated into the world. Empty = load bare terrain + environment.
@export var mission_file: String = ""

# The terrain + environment loaded, by name, from the resource directory. Used
# only when mission_file is empty.
@export var terrain_file: String = "Dvxi5.trn"
@export var env_file: String = "full_00.env"

@onready var _terrain: Terrain = $Terrain
@onready var _env: MissionEnvironment = get_node_or_null("MissionEnvironment")
@onready var _water: Water = get_node_or_null("Water")
@onready var _weather: Weather = get_node_or_null("Weather")
@onready var _precipitation: Precipitation = get_node_or_null("Precipitation")
@onready var _celestial: Celestial = get_node_or_null("Celestial")
@onready var _sky_dome: SkyDome = get_node_or_null("SkyDome")
@onready var _clear_color: WorldEnvironment = get_node_or_null("ClearColor")

var _dispatcher: FoliageDispatcher
var _sun_shadow: SunShadow
# True once the env presenters' own _process is off and this world advances
# them from render_environment_nodes_frame (the render diagnostics report it).
var _env_presenters_world_driven := false
var _terrain_data: TerrainData
var _resource_root: ResourceRoot
var _mission_tile_info: TerrainTileInfo
var _mission_til_bytes := PackedByteArray()
var _join_wire_assets_pending := false
var _join_wire_til_applied := false
var _join_wire_assets_failed := false
var _join_wire_asset_failure_emitted := false
var _world_ready := false
var _loaded_mission: MissionData
# The BMS clock the weather home is seeded with at the mission-start boundary.
var _mission_clock_start_q8_8: int = 0
var _mission_clock_minutes_per_day: int = MissionEnvironment.DEFAULT_MINUTES_PER_DAY
# The BMS argument that completed the active mission load. This is runtime
# state, deliberately separate from mission_file (the exported boot option).
var _loaded_mission_file: String = ""
var _runtime: MissionPresentation = null  # the one mission runtime driver (sim + present pass + index), DIVIDED cadence
var _panm_clock := PanmClock.new()
var _frame_pipeline: GameFramePipeline
var _mission_stats: Dictionary = {}
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database); untyped
             # because game_world_test's ViewmodelWorldHarness installs a RefCounted double
var _last_load_timeline: PerfTimeline = null  # the most recent load_mission timing
var _weapon_db: WeaponDatabase = null  # weapon.def, lazy per mounted root (FP viewmodel)
var _local_weapon_dict := {}  # the resolved weapon's raw dict (FSM setup transport, ADR 0017 edge)
var _mission_audio: MissionAudio
var _effect_world: EffectWorld  # the runtime .ptl effect world (render-only, per mission)
# GameWorld-owned first-person presentation seam. MissionPresentation invokes GameWorld
# once per completed fixed tick; this callback consumes that tick's weapon
# events before EffectWorld advances, matching retail's action -> particle-pass
# order without coupling the simulation to LocalPlayerPresenter Nodes.
var _local_player_weapon_tick_consumer := Callable()
# Frame-clear cache (divergence #21): recompute only when the env generation
# moves or the camera crosses the water plane.
var _clear_env_generation: int = -1
var _clear_above_water := true
# The render-occlusion frame pass (occlusion_frame_pass.gd): the blink letter
# gates, the per-frame section-mask/portal apply, and the two visibility
# dictionaries the mission present pass shares BY REFERENCE. Constructed once
# in _init; tick() calls it directly (hot path — no Callables).
var _occlusion: OcclusionFramePass
# The Godot frame device legs (ADR 0035): the per-frame camera/timing latch
# and the renderer/audio/environment leg bodies GameFramePipeline orders
# around the session tick, extracted to world_device_frame.gd on the same
# plain-RefCounted pattern. One-line delegates below keep every leg name on
# GameWorld — the pipeline and its FakeWorld test pin the duck-typed contract
# here.
var _device_frame: WorldDeviceFrame
# The local-player visuals (world_player_visuals.gd): the FP viewmodel/arms
# composition, the third-person avatar + held-gun builders, the armory weapon
# apply/clear and spawn-loadout projection, and the typed local-player view
# decodes, on the same pattern. Delegates below keep the names on GameWorld.
var _player_visuals: WorldPlayerVisuals
# The mission-effect/fixed-tick presentation router
# (world_effect_router.gd): the WAC/BMS effect fan-out, the impact/scorch
# drains, and the runtime signal handler bodies, on the same pattern.
var _effect_router: WorldEffectRouter
# The mission attribute that forces the indoors accum bit every frame. Stays
# on the world (mission state, test-pinned by name); handed to the pass's
# entries as an argument. [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8-0x5ca1cd]
var _mission_forces_indoors := false
var _idle_frame_clear_color := Color.BLACK
# A shell-injected resource root (main_game hands its boot mount over; tests
# hand fixture roots). When set, the load_*
# entries skip the settings lookup + their own mount and resolve through it; the
# game path (no injection) still mounts from the persisted resource directory.
var _injected_root: ResourceRoot = null
# Debug: hide the scattered foliage (the dev tools' "Hide foliage"). Off by default.
var _foliage_hidden := false
var _playable := true
# The net-session drive: typed request staging, the joiner preload/admission
# state machines, the ESC aborts, and NovaWorld gate registration (see
# net_session_drive.gd). The session signals live on THIS node — the shell
# contract pins them here — and the drive emits them through its world reference.
var _net_drive: NetSessionDrive
# The F3 debug-view set: the world-space debug views + the pick stack, an
# internal child node on the same pattern (see debug_view_set.gd). The views
# it builds attach to THIS world node — hosts and tests pin them as
# world-relative lookups — and the moved public toggles keep one-line
# delegates below so the shell-facing surface never moved.
var _debug_views: DebugViewSet
# The per-item ITEMS.DEF effect director (item_effect_director.gd): the
# attached/static/controller item emitters, the effect-anchor resolvers, and
# the retail master particle switch, on the same internal pattern (plain
# RefCounted — it owns no Nodes). Public delegates below keep the shell-facing
# names on GameWorld. ALSO the sanctioned test-injection seam: like _runtime,
# harnesses may swap in a director double (see game_world_test.gd).
var _item_fx: ItemEffectDirector
var _light_director: EffectLightDirector
var _local_player_spawn_loadout: Dictionary = {}
# The local player's two per-side character selections + classes projected for
# the sim (the listen host's own type-2 connection / a joiner's ClientAuth).
var _local_character_profile: Dictionary = {}
var _perf_tick_us: int = 0
var _perf_foliage_us: int = 0
var _perf_runtime_us: int = 0
var _perf_audio_us: int = 0


## Inject the resource root the next load resolves through. Runtime hosts and
## focused tests use this to keep one already-mounted resource session. Null
## returns to the game's settings-driven mount.
func set_resource_root(root: ResourceRoot) -> void:
	_injected_root = root


## Configure the local player's profile for the next mission runtime start. The
## value is consumed once the runtime exists (or discarded by unload after a
## failed/abandoned load). An empty dictionary preserves the historical fallback;
## a profile carrying empty slot names explicitly requests an all-NONE kit.
func set_local_player_spawn_loadout(loadout: Dictionary) -> void:
	_local_player_spawn_loadout = loadout.duplicate(true)


func set_playable(enabled: bool) -> void:
	_playable = enabled


func is_playable() -> bool:
	return _playable


# The root a load resolves through: the injected one, else a fresh runtime mount of
# `dir` (or the persisted resource directory when empty). Emits load_failed and
# returns null when nothing resolves.
func _resolve_root(dir: String) -> ResourceRoot:
	if _injected_root != null:
		return _injected_root
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		load_failed.emit("no resource directory set")
		return null
	return _mount_runtime_root(dir)


func _init() -> void:
	# The drive's preload wait is stepped by its own synchronous _process, so it
	# must be in the tree before load_mission_as_joiner runs: constructed here, it
	# enters the tree with the world itself, and every load_* entry runs on an
	# in-tree world. The drive holds this world for its public load surface
	# + signal emission; the three Callables lend it the private internals the
	# preload path needs without widening GameWorld's API.
	_net_drive = NetSessionDrive.new()
	_net_drive.name = "NetSessionDrive"
	_net_drive.setup(self,
			_load_mission_internal,
			_resolve_root,
			func() -> Dictionary: return _local_player_spawn_loadout)
	add_child(_net_drive)
	# The debug-view set follows the same internal-child pattern. Its two
	# Callables lend the private internals the views need without widening
	# GameWorld's API: the placer's grouped static user-point sources, and a
	# weakref-guarded effect-world getter (the ParticleDebugView outlives
	# mission reloads, so it must never hold this world strongly).
	_debug_views = DebugViewSet.new()
	_debug_views.name = "DebugViewSet"
	var ref: WeakRef = weakref(self)
	var user_point_sources := func() -> Array:
		return _placer.get_static_user_point_sources() if _placer != null else []
	var effect_world_getter := func() -> EffectWorld:
		var world: GameWorld = ref.get_ref()
		return world.get_effect_world() if world != null else null
	_debug_views.setup(self, user_point_sources, effect_world_getter)
	add_child(_debug_views)
	# The render-occlusion frame pass: plain RefCounted (no tree presence),
	# direct-called from tick() every frame. Constructed exactly once — its two
	# shared dictionaries must keep their identity for the mission present pass.
	_occlusion = OcclusionFramePass.new()
	_occlusion.setup(self)
	# The frame device legs share the pass's construction slot: plain
	# RefCounted, wired once, direct-called through the leg delegates below.
	_device_frame = WorldDeviceFrame.new()
	_device_frame.setup(self)
	_player_visuals = WorldPlayerVisuals.new()
	_player_visuals.setup(self)
	_effect_router = WorldEffectRouter.new()
	_effect_router.setup(self)
	# The item-effect director, wired like the debug-view set: its two lent
	# privates are the placer's static item-effect sources and its item
	# database, null-guarded here. The db seam stays duck-typed on purpose —
	# the public get_item_db() keeps its ItemDatabase contract while
	# harness worlds serve value-only db doubles.
	_item_fx = ItemEffectDirector.new()
	_item_fx.setup(self,
			func() -> Array:
				return _placer.get_static_item_effect_sources() if _placer != null else [],
			func() -> Variant:
				return _placer.get_item_db() if _placer != null else null)
	_light_director = EffectLightDirector.new()
	_light_director.setup(self,
			func() -> Array:
				return _placer.get_static_item_effect_sources() if _placer != null else [])


func _ready() -> void:
	set_process(false)
	_frame_pipeline = GameFramePipelineScript.new()
	_frame_pipeline.setup(self)
	if _clear_color != null and _clear_color.environment != null:
		_idle_frame_clear_color = _clear_color.environment.background_color
		# ADR 0043: the environment feeds the lit scene through this
		# Environment — the hemisphere-sky ambient (background stays BG_COLOR,
		# the witnessed frame clear) and the Environment fog.
		_env.set_scene_environment(_clear_color.environment)
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher")
		if _dispatcher != null:
			# The applier reads the native detail-cell handoff and the composed
			# surface textures through this wired owner (never a parent probe).
			_dispatcher.set_terrain(_terrain)
			# The detail sway phase reads the weather oscillator's ring slot 0
			# (retail Env_WaveOscRing[0] in Foliage_SetupVertexShaderConstants).
			_dispatcher.set_weather(_weather)
	# ADR 0043: the one scene sun — real color/energy and the CSM every lit
	# receiver takes (the render-slot capture/drape system is retired).
	_sun_shadow = SunShadow.new()
	_sun_shadow.name = "SunShadow"
	add_child(_sun_shadow)
	_sun_shadow.set_environment_node(_env)
	# The retained water renderer starts dormant until a successful load chooses
	# its runtime mode. In particular, do not let an authored scene height make
	# initial/menu frames look underwater.
	_set_water_world_rendering_enabled(false)
	# Freeze the retained weather node until a load selects autonomous bare/net
	# rendering or prepares a mission-owned fixed tick.
	_set_weather_world_tick_driven(true)
	# The env presenters self-clock through _process when they stand alone
	# (tests, standalone scenes). Under this world GameFramePipeline drives
	# their advance_frame at a defined ladder slot (render_environment_nodes_frame
	# and render_water_frame), so their idle callbacks stay off here: a process
	# callback races the camera placement and the legs that consume them.
	for presenter in [_weather, _sun_shadow, _sky_dome, _celestial, _water]:
		if presenter != null:
			(presenter as Node).set_process(false)
	_env_presenters_world_driven = true


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		_device_frame._stop_water_render_stats()
		return
	if what != NOTIFICATION_VISIBILITY_CHANGED or not is_node_ready():
		return
	if _world_ready and is_visible_in_tree():
		apply_scene_environment_frame()
		_clear_env_generation = -1
		_device_frame._update_frame_clear_color()
	else:
		if _env != null:
			_env.set_underwater_view(false)
			_env.set_underwater_overlay_view(false)
		_device_frame._restore_idle_frame_clear_color()


## Load the world from `dir`, or from the persisted resource directory when empty.
## Returns OK, or ERR_FILE_NOT_FOUND when the directory is unset/missing the
## terrain (the caller decides whether to prompt). No fallbacks: the chosen
## directory is the only place looked.
func load_world(dir: String = "") -> int:
	if not mission_file.is_empty():
		return load_mission(mission_file, dir)
	var resource_root := _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	if not resource_root.has_file(terrain_file):
		load_failed.emit("%s not found in %s" % [terrain_file, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	if not resource_root.has_file(env_file):
		load_failed.emit("%s not found in %s" % [env_file, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND

	_set_water_world_rendering_enabled(false)
	_set_mission_water_height_override(NAN)
	_clear_mission_tile_info()
	_resource_root = resource_root
	if not _load_environment(env_file):
		load_failed.emit("failed to load %s" % env_file)
		return ERR_CANT_OPEN
	if not _load_terrain(terrain_file):
		load_failed.emit("failed to load %s" % terrain_file)
		return ERR_CANT_OPEN

	_world_ready = true
	_prepare_autonomous_weather()
	_set_water_world_rendering_enabled(true)
	_debug_views.on_loaded()
	world_loaded.emit()
	return OK


## Load a mission (.bms): its header selects the terrain + environment, which are
## resolved from `dir` (or the persisted resource directory) and loaded through the
## same path as load_world, then the mission's placed objects are populated into the
## world. Returns OK, or the same error codes as load_world.
func load_mission(bms_name: String, dir: String = "") -> int:
	var resource_root := _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	# The runtime BMS path bypasses loose overrides even under /d.
	# [orig: Mission_LoadBMSFromPFF @ 0x40d43c]
	if not resource_root.has_file(
			bms_name, ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY):
		load_failed.emit("%s not found in %s" % [bms_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	var mission := MissionData.new()
	if mission.open_from_resource_root(
			resource_root, bms_name, ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY) != OK:
		load_failed.emit("failed to parse %s: %s" % [bms_name, mission.get_last_error()])
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, bms_name, resource_root)


## Load the exact saved, top-level loose BMS from the selected resource root.
## This tooling/test seam deliberately differs from load_mission(),
## whose retail contract remains archive-only even when the session has /d.
## Only the BMS itself is forced to disk; terrain, environment, objects and
## sidecars continue through the mounted runtime root and its normal /d policy.
func load_loose_mission(bms_name: String, dir: String = "") -> int:
	var mission_name := bms_name.strip_edges().replace("\\", "/")
	if mission_name.is_empty() or mission_name != mission_name.get_file() \
			or mission_name.get_extension().to_lower() != "bms":
		load_failed.emit("loose mission must be a top-level .bms file")
		return ERR_INVALID_PARAMETER
	var resource_root := _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	var mission_path := resource_root.get_root_dir().path_join(mission_name)
	if not FileAccess.file_exists(mission_path):
		load_failed.emit("%s not found in %s" % [mission_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	var mission := MissionData.new()
	if mission.open_file(mission_path) != OK:
		load_failed.emit("failed to parse %s: %s" % [mission_name, mission.get_last_error()])
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, mission_name, resource_root)


## Load a mission as a LAN co-op HOST: the typed session request (ADR 0017) is
## staged and driven by NetSessionDrive through the same load path as
## load_mission. Returns the same codes as load_mission.
func load_mission_as_host(config: HostSessionConfig) -> int:
	return _net_drive.load_as_host(config)


## Load as a LAN co-op JOINER (a non-authority client): the typed dial target is
## staged and driven by NetSessionDrive (authenticate before the wire-header world
## load; S2C 0x7B supplies the mission identity). Returns the same codes as load_mission.
func load_mission_as_joiner(target: JoinTarget) -> int:
	return _net_drive.load_as_joiner(target)


## ESC/abort for the joiner's pre-load connect/session wait. Returns true when an
## in-flight preload was aborted (see NetSessionDrive.cancel_preload).
func cancel_join_preload() -> bool:
	return _net_drive.cancel_preload()


## ESC/abort for the joiner's post-load admission wait. Returns true when a live
## admission wait was told to abort (see NetSessionDrive.cancel_admission_wait).
func cancel_join_admission() -> bool:
	return _net_drive.cancel_admission_wait()


## True while this world is a live network session (a co-op JOINER or a LISTEN HOST).
## The shell uses it to keep the world ticking through the in-game menu: the world tick
## is the only pump for the session socket, so freezing it silences the connection and a
## peer eventually drops us on its connection timeout. Retail multiplayer cannot pause at
## all — the ESC menu overlays a running match [orig: the pause path has no MP leg; the
## reaping side is cs_dir0.timeout_ms = 120000, CNapiNetwork_Init @0x4ca4a0].
func is_net_session() -> bool:
	var sim := get_sim()
	if sim == null:
		return false
	return bool(sim.is_joiner()) or bool(sim.is_host_listening())


## The shell's ESC-pause session leg: the engine session pauses and resumes
## WITH the shell's pause overlay, so the paused state is a session fact every
## reader agrees on, not a shell-only tick skip. No role gate here — the
## runtime reports the engine's own verdict (inmatch::Session::pause is
## SinglePlayer-only: retail multiplayer cannot pause, its ESC menu overlays a
## running match, and the world tick is the net session's only socket pump).
## Resuming an unpaused session is a NoOp, so every shell resume leg (ESC,
## armory close, menu RESUME, the MCP resume verb) can call this safely.
func set_shell_paused(paused: bool) -> void:
	if _runtime == null:
		return
	if paused:
		_runtime.pause()
	else:
		_runtime.play()


## Load an in-memory mission through the shared world pipeline. This is retained
## as a focused engine-test/tool seam; normal game launches always use
## a saved .bms through load_mission() or load_loose_mission().
func load_mission_data(mission: MissionData, bms_name: String, dir: String = "") -> int:
	if mission == null or not mission.is_loaded():
		load_failed.emit("no mission document to load")
		return ERR_INVALID_PARAMETER
	var resource_root := _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, bms_name, resource_root)


# The ONE mission path — the file entry (load_mission) and the in-memory
# entry (load_mission_data) converge here: resolve the header's terrain +
# environment from `resource_root`, apply the mission's env overrides, build the
# world, place objects, start the runtime + audio.
func _load_mission_internal(mission: MissionData, bms_name: String,
		resource_root: ResourceRoot) -> int:
	var wire_header_join := mission.is_wire_header_only()
	_join_wire_assets_pending = wire_header_join
	_join_wire_til_applied = false
	_join_wire_assets_failed = false
	_join_wire_asset_failure_emitted = false
	var trn := mission.get_terrain_ref() + ".trn"
	if not resource_root.has_file(trn):
		load_failed.emit("%s.trn (from %s) not found in %s" % [mission.get_terrain_ref(), bms_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	var env_name := mission.get_environment_ref() + ".env"
	if not resource_root.has_file(env_name):
		load_failed.emit("%s.env (from %s) not found in %s" % [mission.get_environment_ref(), bms_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND

	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	# Keep stage attribution stable so load timelines remain comparable.
	var timeline := PerfTimeline.begin("Mission load %s" % bms_name)
	_last_load_timeline = timeline
	_resource_root = resource_root
	# The shared .3DI definition cache resets before the environment and
	# terrain stages (the load plan's first order witness): Celestial resolves
	# its models from _load_environment, and foliage loaded by terrain must
	# remain present-but-excluded in the same generation.
	ObjectData.reset_network_challenge_model_registry()
	_load_mission_tile_info(
			bms_name, resource_root, PackedByteArray(),
			mission.is_wire_header_only())
	# Each stage below presents the plan's anchor when it starts.
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_ENVIRONMENT))
	timeline.span("environment")
	if not _load_environment(env_name):
		load_failed.emit("failed to load %s" % env_name)
		timeline.finish()
		return ERR_CANT_OPEN
	_apply_mission_environment_overrides(mission)
	# Initialize the exact mission clock and the reset weather owner before the
	# runtime is constructed. The authority publishes this T0 sample after setup
	# but before play, so its first network tick cannot observe stale/default data.
	var mission_info: Dictionary = mission.get_info()
	_mission_clock_start_q8_8 = int(mission_info.get("start_time", 0))
	_mission_clock_minutes_per_day = int(mission_info.get(
			"minutes_per_day", MissionEnvironment.DEFAULT_MINUTES_PER_DAY))
	if _env != null:
		_env.configure_mission_clock(_mission_clock_start_q8_8, _mission_clock_minutes_per_day)
	_prepare_world_driven_weather()
	timeline.end_span()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_TERRAIN))
	timeline.span("terrain")
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		timeline.finish()
		return ERR_CANT_OPEN
	timeline.end_span()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_OBJECTS))

	_loaded_mission = mission
	# The mission attribute that forces the indoors accum bit every frame.
	# [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8]
	_mission_forces_indoors = (int(mission.get_info().get("attrib_flags", 0)) & MissionData.ATTRIB_FORCE_INDOORS) != 0
	timeline.span("objects")
	_place_mission_objects(mission, timeline)
	timeline.end_span()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_RUNTIME))
	timeline.span("runtime")
	var runtime_error := _start_runtime(mission, bms_name)
	timeline.end_span()
	if runtime_error != OK:
		timeline.finish()
		unload()
		return runtime_error
	# The non-foliage loaded-.3DI page freezes once here (the load plan's
	# freeze witness): MissionPresentation.setup has now resolved the placed/wire
	# mission models (including collision/husk definitions); late network spawns
	# must not change this page.
	var challenge_sim: Simulation = _runtime.get_sim()
	if challenge_sim != null and not wire_header_join:
		if challenge_sim.is_joiner():
			_prewarm_loaded_model_challenge_definitions()
		challenge_sim.finalize_loaded_model_challenge_snapshot()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_AUDIO))
	timeline.span("audio")
	_start_mission_audio(mission, bms_name)
	timeline.end_span()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_EFFECTS))
	timeline.span("effects")
	_start_effect_world()
	timeline.end_span()
	# Warm the effect catalog while the loading screen still covers the frame:
	# the first live spawn otherwise pays the deferred texture resolves + the
	# renderer's first-draw pipeline compiles as a ~90 ms hitch on the player's
	# first shot (measured: first-fire tap 92.9 ms -> repeat 12.5 ms). Retail
	# pays this at load (the load plan's effect-system witness).
	timeline.span("effects_warm")
	_warm_effect_world_catalog()
	timeline.end_span()
	load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_FINISH))
	timeline.finish()
	_loaded_mission_file = bms_name
	_world_ready = true
	_set_water_world_rendering_enabled(true)
	_build_minimap_water_mask()
	load_progress.emit(MissionData.LOAD_PROGRESS_COMPLETE)
	_debug_views.on_loaded()
	world_loaded.emit()
	return OK


# Mount `dir` as the runtime resource root: PFF archives are the packed game data,
# the `/exp <name>` flag (or persisted setting) layers an expansion over the base,
# loose files override the archives only under the `/d` dev flag, and the `/game <code>`
# flag (or persisted setting, default "jo") selects the SCR decode key so demo data
# decodes correctly. Emits load_failed and returns null on a bad root.
# The expansion here is the LOCAL choice, which is only authoritative for single-player and
# for hosting. A joiner's is the HOST's, learned after this mount and reconciled by
# NetSessionDrive._reconcile_join_expansion before any host data is read (D-NET-178).
func _mount_runtime_root(dir: String) -> ResourceRoot:
	var resource_root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := LaunchFlags.game(ResourceDirSettings.get_game())
	if resource_root.mount_runtime(dir, expansion, LaunchFlags.loose_override_enabled(), game) != OK:
		load_failed.emit(resource_root.get_last_error())
		return null
	return resource_root


# Populate the world with the mission's placed objects under a MissionObjects node.
# Uses the same shell-agnostic placer as every other mission load path.
func _place_mission_objects(mission: MissionData, timeline: PerfTimeline = null) -> void:
	if _resource_root == null or mission == null:
		return
	_placer = MissionObjectPlacer.create(_resource_root, null)
	# The placer's Avatars.def is the one registry every player visual resolves
	# against; the same table projects the local profile the sim stamps.
	var avatar_db: AvatarDatabase = _placer.get_avatar_db()
	var sim := get_sim()
	if sim != null:
		sim.set_character_avatar_database(avatar_db)
	if avatar_db != null:
		_local_character_profile = NetSessionDrive.character_join_profile_from_database(
				avatar_db, _local_player_spawn_loadout)
	else:
		push_warning("GameWorld: Avatars.def unavailable; players draw their item model")
		_local_character_profile = {}
	_panm_clock.sample_frame()
	_placer.set_panm_clock(_panm_clock)
	var options := {}
	# A wire-header join deliberately has no authored body records. The load stream
	# creates native pools 2/1/3 from S2C 0x10/0x0D/0x20 at exact handles; remote
	# pool-0 organics arrive in 0x0C and every live pose advances through 0x0A.
	# MissionPresentation presents those decoded rows directly instead of deferring them
	# onto nonexistent local BMS placements (D-NET-194). Explicit-mission/debug
	# joins still use their complete document.
	if timeline != null:
		options["timeline"] = timeline
	# Pulse the load-progress screen from inside the model-load loop at the
	# object stage's constant value (the load plan's per-model pulse witness).
	options["progress"] = func() -> void: load_progress.emit(
			MissionData.load_progress_percent(MissionData.LOAD_STAGE_OBJECTS))
	_mission_stats = _placer.place(mission, self, options)
	_apply_occlusion_culling_policy()
	print_verbose("GameWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
		int(_mission_stats.placed),
		int(_mission_stats.batched),
		int(_mission_stats.animated),
		int(_mission_stats.unresolved),
		int(_mission_stats.markers),
	])


## Godot's occlusion consumer is a world-level decision (a conservative second
## layer under the retail section/portal verdict, docs/render/
## render-occlusion-re.md "Conservative device occluders"); no ObjectModel
## flips viewport state. It stays OFF by default: measured 2026-08-30 through
## the "occlusion_culling" debug row (1600x900, Ryzen 7735HS iGPU, medians of
## p50 over two runs, the missions carrying 19 / 26 buildings with authored
## occluders), the occluder pass cost 0.64 / 0.59 ms of render_root_cpu
## (frame 14.47 -> 13.55 ms on 00TRa, 14.50 -> 13.68 ms on CP01) and culled
## nothing (487 -> 489 and 416 -> 416 root draw calls) because the retail
## section verdict already hides what the OOBJ faces would. A mission load
## and an unload both re-apply the default; the debug row switches the pass
## on live while the mission carries occluders.
func _apply_occlusion_culling_policy() -> void:
	set_occlusion_culling_enabled(false)


## The occlusion consumer as a live device switch (the F3/MCP
## "occlusion_culling" row): it reads and writes the world viewport directly,
## never on a viewport without a RenderingDevice (headless and Compatibility
## expose no occlusion path), and the next mission load re-applies the policy
## default above (a fresh mission gets fresh debug state; nothing replays).
func set_occlusion_culling_enabled(enabled: bool) -> void:
	var viewport := get_viewport() if is_inside_tree() else null
	if viewport == null:
		return
	viewport.use_occlusion_culling = enabled \
			and RenderingServer.get_rendering_device() != null


func is_occlusion_culling_enabled() -> bool:
	var viewport := get_viewport() if is_inside_tree() else null
	return viewport != null and viewport.use_occlusion_culling


## How many placed buildings carry authored OOBJ occluders in the loaded
## mission (0 = the occluder pass has nothing to cull with).
func get_authored_occluder_model_count() -> int:
	return int(_mission_stats.get("authored_occluder_models", 0))


func get_loaded_mission() -> MissionData:
	return _loaded_mission


func get_loaded_mission_file() -> String:
	return _loaded_mission_file


func get_sim() -> Simulation:
	return _runtime.get_sim() if _runtime != null else null


## The mounted world's shared weapon.def database. ArmoryPresenter consumes this on
## first open so its canonical parent tuples and its visible rows resolve against
## the same catalog; the FP viewmodel reuses it below (ADR 0018 resource seam).
func get_weapon_database() -> WeaponDatabase:
	if _weapon_db == null:
		if _resource_root == null:
			return null
		_weapon_db = WeaponDatabase.new()
		if _weapon_db.load_from_resource_root(_resource_root, "weapon.def") != OK:
			push_warning("GameWorld: weapon.def unavailable (%s) — weapon presentation/loadout lookup disabled"
					% _weapon_db.get_last_error())
			return null
	return _weapon_db if _weapon_db.is_loaded() else null


## The timing of the most recent mission load (null before the first load).
func last_load_timeline() -> PerfTimeline:
	return _last_load_timeline


func get_runtime() -> MissionPresentation:
	return _runtime


func get_mission_stats() -> Dictionary:
	return _mission_stats


## The placer's static populations that currently carry at least one live
## row (the per-frame RLOD selection empties and refills them); 0 before a
## mission is placed.
func get_static_live_population_count() -> int:
	return int(_placer.get_static_live_population_count()) \
			if _placer != null else 0


## Tear down a loaded world so the shell can return to the menu (or load a
## different mission) without the previous world lingering. Frees the dynamically
## placed MissionObjects subtree and resets the load state; the terrain /
## environment scene nodes are kept in place and rebuilt by the next load_*().
## Safe to call when nothing is loaded.
func unload() -> void:
	_world_ready = false
	_minimap_water_mask = null
	minimap_water_changed.emit(null)
	_join_wire_assets_pending = false
	_join_wire_til_applied = false
	_join_wire_assets_failed = false
	_join_wire_asset_failure_emitted = false
	_device_frame._stop_water_render_stats()
	# Net-session teardown: the preload sim/root, the notification latches, the
	# typed request staging, and the NovaWorld gate registration.
	_net_drive.reset()
	# The environment's weather view points into the departing sim's World:
	# detach before the runtime (and its off-tree sim) is freed.
	if _weather != null:
		_weather.bind_simulation(null)
	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	if _env != null:
		_env.set_underwater_view(false)
		_env.set_underwater_overlay_view(false)
	_local_player_spawn_loadout = {}
	_local_character_profile = {}
	_clear_mission_tile_info()
	_device_frame._restore_idle_frame_clear_color()
	# Point-light output is a RenderingServer global, so retire it before the
	# placed nodes begin their deferred queue_free teardown. The director also
	# disconnects its wire-node exit hooks here; those hooks must not race the
	# whole-world reset or leak a prior mission's pool into the menu frame.
	if _light_director != null:
		_light_director.reset()
	_apply_occlusion_culling_policy()
	var container := get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		container.queue_free()
	# Per-item attached-effect owner keys reference nodes in that container —
	# never let a reload's provider resolve against freed instances.
	_item_fx.reset()
	# Debug-view teardown: the retain/free split (user-point re-arm vs freed
	# overlays vs the deliberately surviving particle/pick stack) lives in the set.
	_debug_views.on_unload()
	if _mission_audio != null:
		_mission_audio.teardown()
	# Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	# The game shell re-opens menu music on its return to the front end.
	MusicService.stop_context()
	# Blink frame gates and every occlusion override reset with the mission
	# [orig: the letter-bit clear @ 0x525c45 at mission start] — an unload while
	# indoors must not leave the next mission's terrain/sky/water hidden. The
	# pass clears the shared present-visibility intent FIRST (the pre-extraction
	# unload cleared it up top), so its release walk falls back to
	# sim.entity_present_visible — see OcclusionFramePass.reset.
	_occlusion.reset()
	_mission_forces_indoors = false
	set_local_player_nvg_view(false, 0)
	if _env != null and _env.environment_data != null:
		_env.environment_data.clear_mission_overrides()
	_set_mission_water_height_override(NAN)
	_loaded_mission = null
	_loaded_mission_file = ""
	if _runtime != null:
		_runtime.queue_free()  # frees its off-tree sim too (MissionPresentation._exit_tree)
	_runtime = null
	if _effect_world != null:
		_effect_world.release_runtime_renderer_resources()
		_effect_world.queue_free()
		_effect_world = null
	_mission_audio = null
	_placer = null
	_weapon_db = null  # re-resolves against the next load's mounted root
	_local_weapon_dict = {}
	# The decoded view record is keyed on the resolved name; the next mission
	# re-decodes from ITS weapon.def even when the name repeats, or the memo
	# would short-circuit with the dict above left empty.
	_viewmodel_def_name = ""
	_viewmodel_def = null
	_local_weapon_preserve_slot_state = false
	# Armory selections belong to the entity from the mission being torn down.
	# A new spawn must resolve from its own equipped AdmDef instead of inheriting
	# either the previous mission's override or its authored NONE state.
	_viewmodel_weapon_override = ""
	_viewmodel_weapon_cleared = false
	_mission_stats = {}


## Process-exit-only release for renderer resources intentionally retained by
## unload() so world-to-menu and mission-to-mission transitions stay warm.
func release_runtime_renderer_resources() -> void:
	var runtime_water := _water as Water
	if runtime_water != null:
		runtime_water.release_runtime_renderer_resources()


func _load_environment(env_path: String) -> bool:
	if _env == null:
		return true
	var env := EnvFile.new()
	if env.load_from_resource_root(_resource_root, env_path) != OK:
		push_warning("GameWorld: failed to load environment '%s'" % env_path)
		return false
	# MissionEnvironment's setter reloads + pushes shader globals on assignment.
	_env.environment_data = env
	# The overcast table the overcast blend cross-fades against: overcast.def
	# appended after the .trn pass (stock .trn files carry no TOD blocks)
	# (retail Environment_LoadTimeOfDayConfig @ 0x57db30).
	var overcast := EnvFile.new()
	if overcast.load_from_resource_root(_resource_root, "overcast.def") == OK:
		_env.overcast_data = overcast
	else:
		_env.overcast_data = null
	# GameWorld retains one Weather node across loads. A replacement ENV is
	# a discrete state change: retail snaps every color block to the new mission
	# targets instead of easing over from the previous mission's currents.
	var weather: Weather = _weather
	if weather != null:
		weather.resync_colors()
	if _celestial != null:
		_celestial.set_resource_root(_resource_root)
	if _precipitation != null:
		_precipitation.set_resource_root(_resource_root)
	return true


## Apply the mission's attrib-gated water/fog overrides onto the loaded env via
## EnvFile's non-persistent override layer [orig: Game_LoadTerrainDuringConnect
## @ 0x520710]. The base .env is never mutated.
func _apply_mission_environment_overrides(mission: MissionData) -> void:
	if mission == null:
		return
	var overrides: Dictionary = mission.get_environment_overrides()
	# EnvFile owns the other live-view overrides, while water keeps the BMS
	# rung distinct so a flagged zero still beats a nonzero TRN height.
	if _env != null:
		var env_data: EnvFile = _env.environment_data
		if env_data != null:
			if overrides.is_empty():
				env_data.clear_mission_overrides()
			else:
				env_data.apply_mission_overrides(overrides)
	var mission_water := NAN
	if overrides.has("water_height_world"):
		mission_water = float(overrides["water_height_world"])
	_set_mission_water_height_override(mission_water)


func _set_mission_water_height_override(world_height: float) -> void:
	if _water != null:
		_water.set_mission_water_height_override(world_height)


func _set_water_world_rendering_enabled(enabled: bool) -> void:
	if _water != null:
		_water.set_world_rendering_enabled(enabled)


# Runtime water exposes a render-aware predicate so its retained authored
# height cannot leak into frame clear/occlusion while a load is absent or in
# progress.
func is_water_render_active() -> bool:
	return _water != null and _water.is_water_render_active()


func _set_weather_world_tick_driven(enabled: bool) -> void:
	var weather: Weather = _weather
	if weather != null:
		weather.set_world_tick_driven(enabled)


func _prepare_world_driven_weather() -> void:
	var weather: Weather = _weather
	if weather != null:
		weather.prepare_world_driven()
	else:
		_set_weather_world_tick_driven(true)


func _prepare_autonomous_weather() -> void:
	var weather: Weather = _weather
	if weather != null:
		# A world without a mission runs the environment's standalone weather
		# home: drop any bound Simulation first.
		weather.bind_simulation(null)
		weather.prepare_autonomous()
	else:
		_set_weather_world_tick_driven(false)


# The witnessed mission-start environment boundary runs natively on the
# weather device (Weather.run_mission_start_boundary): the World's weather
# seed from the loaded .env + the BMS clock, the authority's WAC direct
# execution, the initializer + 255-tick settle, the baseline seal.
func _run_mission_start_environment_boundary() -> void:
	var weather: Weather = _weather
	if weather != null:
		weather.run_mission_start_boundary(get_sim(),
				_mission_clock_start_q8_8, _mission_clock_minutes_per_day)


# Retail loads <mission>.til into one shared g_TerrainTileArray used by
# terrain overlays/surface overrides, network initial state, and both foliage
# generators' radius-2 blocker.
# Its file probe/read force loose-first around this one load.
# [orig: Terrain_LoadTileInfoFile @ 0x60a740, policy force @ 0x60a74e;
# Terrain_GetSurfaceTypeAtPosition @ 0x606510;
# Foliage_PathBlockedByPlacedTile @ 0x606490]
func _load_mission_tile_info(bms_name: String, resource_root: ResourceRoot,
		wire_til_bytes: PackedByteArray = PackedByteArray(),
		wire_is_authoritative := false) -> void:
	_clear_mission_tile_info()
	if resource_root == null:
		return
	# A joining retail client consumes the host's paged S2C 0x45 bytes. An empty
	# payload means the host emitted no terrain overlay; it must not fall back to
	# a same-named local .til and accidentally render a different custom map.
	if wire_is_authoritative:
		if wire_til_bytes.is_empty():
			return
		var wire_tile_info := TerrainTileInfo.new()
		if wire_tile_info.load_from_bytes(wire_til_bytes) != OK:
			push_warning("GameWorld: failed to parse host S2C 0x45 terrain tile stream.")
			return
		_mission_tile_info = wire_tile_info
		_mission_til_bytes = wire_til_bytes
		return
	var mission_name := bms_name.get_file()
	if mission_name.is_empty():
		mission_name = bms_name
	var til_name := mission_name.get_basename() + ".til"
	if not resource_root.has_file(
			til_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return
	var til_bytes := resource_root.read_file(
			til_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
	if til_bytes.is_empty():
		return
	var tile_info := TerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		push_warning("GameWorld: failed to parse mission tile file '%s'." % til_name)
		return
	_mission_tile_info = tile_info
	_mission_til_bytes = til_bytes


func _apply_join_wire_til_if_ready() -> bool:
	if not _join_wire_assets_pending:
		return not _join_wire_assets_failed
	var sim: Simulation = _runtime.get_sim() if _runtime != null else null
	if sim == null or not sim.is_joiner():
		return true
	var til_state := sim.get_join_terrain_til_state()
	if til_state == Simulation.JOIN_TERRAIN_TIL_INVALID:
		_join_wire_assets_failed = true
		return false
	if til_state not in [
			Simulation.JOIN_TERRAIN_TIL_ABSENT,
			Simulation.JOIN_TERRAIN_TIL_RECEIVING,
			Simulation.JOIN_TERRAIN_TIL_COMPLETE]:
		_join_wire_assets_failed = true
		return false
	# Check Invalid before this latch: an extra semantic 0x45 after a completed
	# stream must not be hidden by an already-applied terrain override.
	if _join_wire_til_applied:
		return true
	if til_state in [Simulation.JOIN_TERRAIN_TIL_ABSENT,
			Simulation.JOIN_TERRAIN_TIL_RECEIVING]:
		return true
	var til_bytes := sim.get_join_terrain_til()
	if til_bytes.is_empty():
		_join_wire_assets_failed = true
		return false
	var tile_info := TerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		_join_wire_assets_failed = true
		return false
	_mission_tile_info = tile_info
	_mission_til_bytes = til_bytes
	_join_wire_til_applied = true
	# S2C 0x45 arrives only after the client releases the world-ready gate, so
	# terrain already exists. Both setters invalidate/rebuild their derived data;
	# the loading screen remains raised until settle_join_wire_assets below.
	if _terrain != null:
		_terrain.tile_info_override = tile_info
	if _dispatcher != null:
		_dispatcher.tile_info = tile_info
	_build_minimap_water_mask()
	return true


## Complete the wire-only part of a retail join once the protocol reaches its
## deployment/admission boundary. NetSessionDrive calls this before revealing
## the world (or deploy map), guaranteeing the optional 0x45 overlay and the
## renderer-backed C2S 0x3D snapshot reflect the completed initial stream.
func settle_join_wire_assets() -> bool:
	if _join_wire_assets_failed:
		return false
	if not _join_wire_assets_pending:
		return true
	if not _apply_join_wire_til_if_ready():
		return false
	var sim: Simulation = _runtime.get_sim() if _runtime != null else null
	if sim == null or not sim.is_joiner():
		_join_wire_assets_failed = true
		return false
	var til_state := sim.get_join_terrain_til_state()
	if til_state == Simulation.JOIN_TERRAIN_TIL_RECEIVING \
			or til_state == Simulation.JOIN_TERRAIN_TIL_INVALID \
			or (til_state == Simulation.JOIN_TERRAIN_TIL_COMPLETE \
					and not _join_wire_til_applied) \
			or til_state not in [
				Simulation.JOIN_TERRAIN_TIL_ABSENT,
				Simulation.JOIN_TERRAIN_TIL_COMPLETE]:
		_join_wire_assets_failed = true
		return false
	_prewarm_loaded_model_challenge_definitions()
	sim.finalize_loaded_model_challenge_snapshot()
	_join_wire_assets_pending = false
	return true


## The revealed world must never race the budgeted cold wire materialization:
## NetSessionDrive holds the join-admission edge until the wire presenter's
## deferred-spawn queue drains behind the loading/DEATH hold. Trivially true
## with no runtime, a harness stub runtime, or no wire presenter.
func is_join_wire_present_drained() -> bool:
	var runtime := _runtime as MissionPresentation
	return runtime == null or runtime.join_wire_present_pending() == 0


## Fail the streamed-asset leg once per join. Both the per-frame runtime driver
## and the frame-polled admission observer can observe the same protocol edge;
## routing them through one latch prevents duplicate load_failed emissions.
func report_join_wire_asset_failure(reason: String) -> void:
	_join_wire_assets_failed = true
	_join_wire_assets_pending = false
	if _join_wire_asset_failure_emitted:
		return
	_join_wire_asset_failure_emitted = true
	load_failed.emit(reason)


func _clear_mission_tile_info() -> void:
	_mission_tile_info = null
	_mission_til_bytes = PackedByteArray()
	if _terrain != null:
		_terrain.tile_info_override = null
	if _dispatcher != null:
		_dispatcher.tile_info = null


func _load_terrain(trn_path: String) -> bool:
	var data := TerrainData.new()
	if data.load_from_resource_root(_resource_root, trn_path) != OK:
		return false
	_terrain.tile_info_override = _mission_tile_info
	_terrain_data = data
	_terrain.terrain_data = data
	_terrain.build()
	if _water != null:
		_water.terrain_data = data
	if _celestial != null:
		# The glare occlusion rays march this terrain (env #14).
		_celestial.terrain_data = data
	_configure_foliage()
	return true


# Runtime foliage: Terrain supplies the retail 16-unit detail-cell set;
# the sim's crouched/prone infantry supply the distant silhouette anchors
# (see tick()). Sampling and deterministic candidate generation stay in the
# fresh native runtime.
func _configure_foliage() -> void:
	if _dispatcher == null or _terrain_data == null:
		return
	# The runtime source already supplies height, detail/model foliage indices,
	# colormap, and change invalidation. Binding the same TerrainData again as
	# the fallback colormap source attempts a duplicate terrain_changed connection
	# in Godot and makes mission reloads report ERR_INVALID_PARAMETER.
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.tile_info = _terrain.tile_info_override
	var defs: Array = _terrain_data.get_foliage_defs()
	_dispatcher.configure_slots(
		defs,
		VegAssets.resolve_slot_meshes(_resource_root, defs),
		VegAssets.resolve_slot_fd_textures(_resource_root, defs)
	)
	for diagnostic_value in _dispatcher.get_slot_diagnostics():
		var diagnostic := diagnostic_value as Dictionary
		var status := String(diagnostic.get('status', ''))
		if status == 'missing_mesh' or status == 'invalid_mesh':
			push_warning(
				"GameWorld: foliage slot %d graphic '%s' disabled (%s)." % [
					int(diagnostic.get('slot', -1)),
					String(diagnostic.get('graphic', '')),
					status,
				]
			)
		elif status == 'enabled' and not bool(diagnostic.get('fd_texture_loaded', false)):
			push_warning(
				"GameWorld: foliage slot %d graphic '%s' has no :fd texture; appearance is degraded." % [
					int(diagnostic.get('slot', -1)),
					String(diagnostic.get('graphic', '')),
				]
			)
func get_terrain_data() -> TerrainData:
	return _terrain_data


func get_resource_root() -> ResourceRoot:
	return _resource_root


func is_loaded() -> bool:
	return _world_ready


func get_current_frame_clear_color() -> Color:
	if _clear_color == null or _clear_color.environment == null:
		return Color.BLACK
	return _clear_color.environment.background_color


func _sample_panm_clock() -> void:
	_panm_clock.sample_frame()
	if _runtime != null:
		_runtime.set_presentation_time_ms(_panm_clock.time_ms)


## The Godot per-frame order, faithful to the original main loop's server-tick-then-client-render:
## foliage coverage around the viewer, then the mission runtime (MissionPresentation.tick advances the
## logic at the 62-frame cadence, presents entity state onto the placed nodes, and drains side
## effects), then the audio render pass. Effects come back through MissionPresentation.effects_drained.
var _perf_probe_enabled := false
var _perf_probe_spans: Dictionary = {}
var _perf_probe_skip_occl := false
var _perf_probe_skip_effect_tick := false
var _perf_probe_skip_fixed_handlers := false
var _perf_probe_occlusion_skipped := false

# The shared F3 frame-stats board (null outside the game shell). Feeds gate on
# board capture so a closed Stats tab costs nothing; the occlusion split spans
# land from OcclusionFramePass.apply_frame, the tick legs from tick() below.
var _frame_stats: FrameStats = null


## The game shell hands its FrameStats here; the world re-hands it to
## every MissionPresentation it creates and feeds its own tick legs.
func set_frame_stats(board: FrameStats) -> void:
	if board == _frame_stats:
		return
	if _frame_stats != null:
		var old_capture_changed := _on_frame_stats_capture_changed
		if _frame_stats.capture_changed.is_connected(old_capture_changed):
			_frame_stats.capture_changed.disconnect(old_capture_changed)
	_device_frame._stop_water_render_stats()
	_frame_stats = board
	if _frame_stats != null:
		var capture_changed := _on_frame_stats_capture_changed
		if not _frame_stats.capture_changed.is_connected(capture_changed):
			_frame_stats.capture_changed.connect(capture_changed)
	_occlusion.set_frame_stats(board)
	if _runtime != null:
		_runtime.set_frame_stats(board)


## GameFramePipeline's capture-only timing seam. The active value is latched by
## begin_device_frame(), so every leg in one frame writes to the same board even
## if the overlay changes page during that frame.
func get_active_frame_stats() -> FrameStats:
	return _frame_stats if _device_frame._frame_stats_on else null


func is_device_frame_timing_enabled() -> bool:
	return _device_frame._frame_timing


func record_runtime_frame(elapsed_us: int) -> void:
	_perf_runtime_us = elapsed_us
	if _device_frame._frame_stats_on:
		_frame_stats.add(FrameStats.WORLD_RUNTIME, elapsed_us)


func _on_frame_stats_capture_changed(active: bool) -> void:
	if not active:
		_device_frame._stop_water_render_stats()


func is_water_render_stats_measured() -> bool:
	return _device_frame.is_water_render_stats_measured()


## Enables the manual frame-span/A-B probe. Disabling restores every skip
## request to its retail default and drops any sampled frame transport.
func set_perf_probe_enabled(enabled: bool) -> void:
	_perf_probe_enabled = enabled
	# The pass shares the probe's timing gate (see OcclusionFramePass.probe_timing).
	_occlusion.probe_timing = enabled
	_perf_probe_spans.clear()
	if not enabled:
		_perf_probe_skip_occl = false
		_perf_probe_skip_effect_tick = false
		_perf_probe_skip_fixed_handlers = false
		_perf_probe_occlusion_skipped = false
	_sync_runtime_profiling()


## The perf probe's A/B switches (only honored while the probe is enabled).
func set_perf_probe_skip_occlusion(skip: bool) -> void:
	_perf_probe_skip_occl = skip


func set_perf_probe_skip_effect_tick(skip: bool) -> void:
	_perf_probe_skip_effect_tick = skip


func set_perf_probe_skip_fixed_handlers(skip: bool) -> void:
	_perf_probe_skip_fixed_handlers = skip


## The last frame's world-tick leg spans in microseconds (probe-enabled only).
func get_perf_probe_spans() -> Dictionary:
	return _perf_probe_spans.duplicate()


# --- Godot frame device legs (ADR 0035) --------------------------------------
# GameFramePipeline invokes these concrete renderer/audio/environment operations in
# one visible order around inmatch::Session::advance(). The leg bodies and the
# per-frame camera/timing latch live in WorldDeviceFrame
# (world_device_frame.gd); these one-line delegates keep every leg name on
# GameWorld for the pipeline's duck-typed contract, the probes, and the tests.

# The shell-owned local-player presenter whose camera/viewmodel placement the
# local-view device leg runs inside the frame (null in worlds without one —
# tests, dedicated). D-RORD-8: placing it before the occlusion/iris/particle
# legs lets them read the camera THIS frame's tick produced, not last frame's.
var _local_view_presenter: LocalPlayerPresenter = null


func begin_device_frame(camera_pos: Vector3, camera_xform: Transform3D,
		delta: float) -> void:
	_device_frame.begin_device_frame(camera_pos, camera_xform, delta)


func finish_device_frame() -> void:
	_device_frame.finish_device_frame()


func render_terrain_frame() -> void:
	_device_frame.render_terrain_frame()


func render_foliage_frame() -> void:
	_device_frame.render_foliage_frame()


func drive_network_frame() -> bool:
	return _device_frame.drive_network_frame()


func render_precipitation_frame() -> void:
	_device_frame.render_precipitation_frame()


func apply_blink_frame() -> void:
	_device_frame.apply_blink_frame()


func present_local_view_frame() -> void:
	_device_frame.present_local_view_frame()


func render_environment_nodes_frame() -> void:
	_device_frame.render_environment_nodes_frame()


func render_water_frame() -> void:
	_device_frame.render_water_frame()


func apply_scene_environment_frame() -> void:
	_device_frame.apply_scene_environment_frame()


## One-time handoff from the shell that owns the local-player presenter.
func set_local_view_presenter(presenter: LocalPlayerPresenter) -> void:
	_local_view_presenter = presenter


func apply_occlusion_frame() -> void:
	_device_frame.apply_occlusion_frame()


func sample_iris_frame() -> void:
	_device_frame.sample_iris_frame()


func render_sun_veil_frame() -> void:
	_device_frame.render_sun_veil_frame()


func mix_audio_frame(ticks_run: int) -> void:
	_device_frame.mix_audio_frame(ticks_run)


func render_material_frame() -> void:
	_device_frame.render_material_frame()


func render_particle_frame() -> void:
	_device_frame.render_particle_frame()


func render_light_frame() -> void:
	_device_frame.render_light_frame()


func get_effect_light_report() -> EffectLightReport:
	return _light_director.get_report() if _light_director != null else null


func update_clear_frame() -> void:
	_device_frame.update_clear_frame()


func session_frame_failed(reason: String) -> void:
	var message := reason if not reason.is_empty() else "mission session lost"
	session_lost.emit(message)


func tick(camera_pos: Vector3, camera_xform: Transform3D = Transform3D(),
		delta: float = -1.0, frame_input: MissionFrameInput = null) -> void:
	if delta < 0.0:
		delta = Simulation.tick_dt()
	if _frame_pipeline == null:
		_frame_pipeline = GameFramePipelineScript.new()
		_frame_pipeline.setup(self)
	_frame_pipeline.advance(camera_pos, camera_xform, delta, frame_input)


func _sync_runtime_profiling() -> void:
	if _runtime != null:
		_runtime.set_runtime_profiling_enabled(_perf_probe_enabled)


func get_runtime_perf_counters() -> Dictionary:
	var foliage_backend: Dictionary = (
			_dispatcher.get_backend_report() if _dispatcher != null else {})
	return {
		"tick_us": _perf_tick_us,
		"foliage_us": _perf_foliage_us,
		"runtime_us": _perf_runtime_us,
		"audio_us": _perf_audio_us,
		"runtime": _runtime.get_perf_counters() if _runtime != null else {},
		"foliage": _dispatcher.get_frame_stats() if _dispatcher != null else {},
		"foliage_backend": foliage_backend,
		"mission_placement": _mission_stats.duplicate(true),
		"static_live_populations": get_static_live_population_count(),
		"audio": _mission_audio.get_perf_counters() if _mission_audio != null else {},
		"instance_uniform_geometry_estimate":
				_instance_uniform_geometry_estimate(foliage_backend),
	}


# Godot reserves INSTANCE_UNIFORM_VALUES_PER_GEOMETRY vec4 values of the global
# shader buffer for every geometry instance whose shader declares instance
# uniforms, visible or not, and prints "Too many instances using shader
# instance variables. Increase buffer size in Project Settings." once the
# buffer_size budget is exhausted (16384 instances with the project's setting;
# shader_resource_validation_test.gd pins it). Godot does not expose the live
# allocation, so this sums the retained instance-uniform geometry the shell
# itself owns: the foliage draw pools (FoliageDispatcher), the placer's static
# populations (visible batches plus their shadow twins), and every surface
# instance of every live ObjectModel scene. Terrain patches, water, and the
# per-model shadow twins the placer parents under animated models are not
# counted: read the total as a floor on the allocation, not the exact figure.
func _instance_uniform_geometry_estimate(foliage_backend: Dictionary) -> Dictionary:
	var foliage_pool := int(foliage_backend.get("pool_size", 0))
	var static_populations := (int(_mission_stats.get("batches", 0))
			+ int(_mission_stats.get("static_shadow_batches", 0)))
	var object_geometry := int(ObjectModel.get_live_geometry_instance_count())
	var buffer_size := int(ProjectSettings.get_setting(
			"rendering/limits/global_shader_variables/buffer_size", 0))
	return {
		"total": foliage_pool + static_populations + object_geometry,
		"budget": buffer_size / INSTANCE_UNIFORM_VALUES_PER_GEOMETRY,
		"foliage_pool": foliage_pool,
		"static_populations": static_populations,
		"object_geometry": object_geometry,
	}


# Listener position for the fire present pass — the same camera position the audio
# render pass ticks with (INF until the first tick).
var _last_tick_camera_pos := Vector3.INF


func _fire_listener_position() -> Vector3:
	return _last_tick_camera_pos


# Fire-presentation counters (FirePresentPass.Stats, typed per ADR 0017;
# null until a mission runs).
func get_fire_present_stats() -> RefCounted:
	return _runtime.get_fire_present_stats() if _runtime != null else null


# Destruction-presentation counters (DestructionPresentPass.Stats, typed per
# ADR 0017; null until a loaded mission runs with the pass).
func get_destruction_present_stats() -> RefCounted:
	return _runtime.get_destruction_present_stats() if _runtime != null else null


# Scar-presentation counters (ScarPresentPass.Stats, typed per ADR 0017).
func get_scar_present_stats() -> RefCounted:
	return _runtime.get_scar_present_stats() if _runtime != null else null


# --- Local-player visuals (viewmodel / avatar / loadout) ----------------------
# The builder/apply/decode bodies live in WorldPlayerVisuals
# (world_player_visuals.gd); these one-line delegates keep the
# presenter/probe/test names on GameWorld, and the subclass override points
# (local_player_viewmodel_def, local_player_character_id,
# _set_local_player_first_person_model_available, the armory apply/clear pair)
# stay overridable here — the component always calls back through _world so
# harness overrides keep binding.

func build_local_player_held_weapon(graphic: String) -> ObjectModel:
	return _player_visuals.build_local_player_held_weapon(graphic)


func build_local_player_avatar() -> Node3D:
	return _player_visuals.build_local_player_avatar()


## The packed character id the authority stamped on the local player (the
## host's own spawn from its installed profile, a joiner's named 0x0C record) --
## the one word its third-person body/head and first-person arms key on, read
## from the sim rather than re-derived from team + profile here.
func local_player_character_id() -> int:
	var sim := get_sim()
	return int(sim.get_local_player_character_id()) if sim != null else 0


func _prewarm_loaded_model_challenge_definitions() -> void:
	_player_visuals._prewarm_loaded_model_challenge_definitions()


# The armory-equipped weapon name; overrides the bring-up fallback/env once the
# player accepts a loadout [orig: the equipped AdmDef drives the FP model pick,
# Player_RenderFirstPersonViewModel @0x4ded60 via the mounted slot].
var _viewmodel_weapon_override := ""
# NONE is distinct from the pre-armory empty override, which falls back to the
# witnessed bring-up default until an equipped weapon is resolved.
var _viewmodel_weapon_cleared := false
# The decoded weapon.def view record and the resolved name it was built from
# (the mounted slot's def pointer; re-decoded only when the name changes).
var _viewmodel_def_name := ""
var _viewmodel_def: PlayerViewmodelDef = null
# A UseGun presentation rebuild follows a slot-pointer commit that has already
# selected a persistent parent/personal slot. Both the dict-only install and the
# later ADM-duration rebake must preserve that slot's action/ammo state.
var _local_weapon_preserve_slot_state := false


func set_local_player_weapon_by_name(weapon_name: String,
		preserve_slot_state: bool = false) -> bool:
	return _player_visuals.set_local_player_weapon_by_name(
			weapon_name, preserve_slot_state)


func clear_local_player_weapon() -> void:
	_player_visuals.clear_local_player_weapon()


func _set_local_player_first_person_model_available(available: bool) -> void:
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_first_person_model_available(available)


func build_local_player_viewmodel() -> Node3D:
	return _player_visuals.build_local_player_viewmodel()


## The FP viewmodel's typed model parts (arms/gun), rebuilt with the
## container — the rig consumes this instead of scanning children.
var _local_viewmodel_parts: Array[ObjectModel] = []


func local_player_viewmodel_parts() -> Array[ObjectModel]:
	return _local_viewmodel_parts


func local_player_first_person_arms_witness() -> FirstPersonArmsWitness:
	return _player_visuals.local_player_first_person_arms_witness()


func local_player_weapon_name() -> String:
	return _player_visuals.local_player_weapon_name()


func set_local_player_nvg_view(active: bool, gain: int) -> void:
	_player_visuals.set_local_player_nvg_view(active, gain)


func local_player_view() -> PlayerLocalView:
	return _player_visuals.local_player_view()


func local_player_hud_weapon_def() -> PlayerHudWeaponDef:
	return _player_visuals.local_player_hud_weapon_def()


func local_player_weapon_view() -> PlayerWeaponView:
	return _player_visuals.local_player_weapon_view()


func drain_local_player_weapon_events() -> Array[PlayerWeaponEvent]:
	return _player_visuals.drain_local_player_weapon_events()


func set_local_player_weapon_tick_consumer(consumer: Callable) -> void:
	_player_visuals.set_local_player_weapon_tick_consumer(consumer)


func local_player_viewmodel_def() -> PlayerViewmodelDef:
	return _player_visuals.local_player_viewmodel_def()


# --- F3 debug views (world-space overlays + the pick stack) ------------------
# The build/teardown lifecycle lives in DebugViewSet (debug_view_set.gd), an
# internal child constructed in _init; the views it builds still attach under
# THIS node, so world-relative lookups (SkeletonDebug/PickDebug/...) are
# unchanged. These one-line delegates keep the presentation-facing names on GameWorld:
# the typed debug-control table (DebugControls) calls these public setters,
# and probes drive the world by the same methods.

func set_skeleton_debug(enabled: bool) -> void:
	_debug_views.set_skeleton_debug(enabled)


func is_skeleton_debug() -> bool:
	return _debug_views.is_skeleton_debug()


func set_user_point_debug(enabled: bool) -> void:
	_debug_views.set_user_point_debug(enabled)


func is_user_point_debug() -> bool:
	return _debug_views.is_user_point_debug()


func set_collision_debug(enabled: bool) -> void:
	_debug_views.set_collision_debug(enabled)


func is_collision_debug() -> bool:
	return _debug_views.is_collision_debug()


func collision_debug_drawable_count() -> int:
	return _debug_views.collision_debug_drawable_count()


func set_particle_debug(enabled: bool) -> void:
	_debug_views.set_particle_debug(enabled)


func is_particle_debug() -> bool:
	return _debug_views.is_particle_debug()


func set_round_debug(enabled: bool) -> void:
	_debug_views.set_round_debug(enabled)


func is_round_debug() -> bool:
	return _debug_views.is_round_debug()


func set_ray_debug(enabled: bool) -> void:
	_debug_views.set_ray_debug(enabled)


func is_ray_debug() -> bool:
	return _debug_views.is_ray_debug()


func set_hitbox_debug(enabled: bool) -> void:
	_debug_views.set_hitbox_debug(enabled)


func is_hitbox_debug() -> bool:
	return _debug_views.is_hitbox_debug()


func set_pick_debug(pick_list: DebugPickList) -> void:
	_debug_views.set_pick_debug(pick_list)


func set_pick_click_enabled(enabled: bool) -> void:
	_debug_views.set_pick_click_enabled(enabled)


func set_occlusion_debug(enabled: bool) -> void:
	_debug_views.set_occlusion_debug(enabled)


func is_occlusion_debug() -> bool:
	return _debug_views.is_occlusion_debug()


func set_ai_debug_option(id: StringName, enabled: bool) -> void:
	_debug_views.set_ai_debug_option(id, enabled)


func is_ai_debug() -> bool:
	return _debug_views.is_ai_debug()


func get_ai_view_state() -> Dictionary:
	return _debug_views.get_ai_view_state()


func set_ai_debug_selection_provider(provider: Callable) -> void:
	_debug_views.set_ai_debug_selection_provider(provider)


func get_debug_view_statuses() -> Array[DebugViewStatus]:
	return _debug_views.get_debug_view_statuses()


## The dev tools' Particles seams (the existing get_effect_world() is
## the data source; these are the two debug toggles).
## Delegates to the item-effect director; the name stays on GameWorld for the
## typed debug-control table (DebugControls) + probe calls.
func set_particles_hidden(hidden: bool) -> void:
	_item_fx.set_particles_hidden(hidden)


func is_particles_hidden() -> bool:
	return _item_fx.particles_hidden()


# --- Hide foliage (the dev tools' "Hide foliage") ----------------------------
# The dispatcher renders scattered vegetation through retained scenario instances;
# its visibility notification hides those instances without touching placement caches,
# so re-showing is instant and the next dispatch is already current.

func set_foliage_hidden(hidden: bool) -> void:
	_foliage_hidden = hidden
	if _dispatcher != null:
		_dispatcher.visible = not hidden

func is_foliage_hidden() -> bool:
	return _foliage_hidden


# Mission-effect routing — the WAC/BMS effect fan-out (dialog audio, fx2ssn
# emitters) and the per-source-tick impact/scorch drains — lives in
# WorldEffectRouter (world_effect_router.gd); this delegate keeps the routing
# name on GameWorld for the runtime handlers and the tests that drive it.
func route_mission_effects(effects: Array) -> void:
	_effect_router.route_mission_effects(effects)


# Start the shared mission runtime driver: it promotes the mission, builds the present index over the
# placed MissionObjects, and each tick applies every entity's transform + part animations (PLAYPARTANIM,
# applied in-engine) + visibility onto its model. The game runs it at the faithful 62-frame cadence and
# drives it explicitly from tick(); its drained side effects route through
# _on_runtime_effects. A reload reuses this GameWorld, so any prior runtime is freed in unload() first.
func _start_runtime(mission: MissionData, bms_name: String) -> int:
	var container := get_node_or_null(NodePath("MissionObjects"))
	_runtime = MissionPresentation.new()
	_runtime.name = "MissionPresentation"
	add_child(_runtime)
	if _frame_stats != null:
		_runtime.set_frame_stats(_frame_stats)
	var mission_file := bms_name.get_file()
	if mission_file.is_empty():
		mission_file = bms_name
	var mission_label := mission.get_mission_name().strip_edges()
	if mission_label.is_empty():
		mission_label = mission_file.get_basename()
	# A mission with no AI still ticks (BMS events / WAC); only a promote failure leaves a null sim.
	# Hand the loaded terrain to the runtime so promoted AI grounds on it (entities hug the terrain),
	# and the resource root so soldiers resolve their .adm/.bad root-motion clips.
	var opts := MissionSetupOptions.new()
	opts.terrain = _terrain_data
	opts.resource_root = _resource_root
	opts.wac_basename = bms_name.get_basename()
	opts.mission_file = mission_file
	opts.mission_name = mission_label
	opts.spawn_names = PackedStringArray([mission_label])
	# The placer's item database (item_id -> anim_def), so each soldier grounds off its own
	# model's .adm clip set (per-entity capsule_bottom), not the shared default. [D-INF-6]
	opts.item_db = _placer.get_item_db() if _placer != null else null
	if not _local_character_profile.is_empty():
		opts.local_character_profile = _local_character_profile.duplicate(true)
	# Serve-and-play hosts run the listen server AND spawn their own player (ADR 0011/0012, net-re
	# §5.2b/§5.38). A DEDICATED host (config "dedicated") serves WITHOUT a local player — same listen
	# server, just no own-player spawn; main_game skips the HUD when there is no local player. Diagnostic
	# previews opt out via _playable.
	# Terrain-tile (.til) bytes for the S2C 0x45 terrain-tile load a listen host streams to joiners so
	# their g_loading_progress climbs 5 -> 6 and terrain finishes loading (net-re §5.37). The tile-overlay
	# .til is named after the MISSION (localres.pff: ASH_I5A.til), not the terrain tileinfo
	# [orig: Terrain_LoadTileInfoFile @ 0x60a740;
	# serialize_terrain_tiles @ 0x6080f0]. Reuse the payload parsed before terrain build.
	if not _mission_til_bytes.is_empty():
		opts.terrain_til = _mission_til_bytes
	opts.playable = _playable and not _net_drive.pending_dedicated()
	# Stamp the staged net-session request (typed record + derived staging +
	# the surrendered preload sim, consumed once per load) onto the runtime's
	# options — MissionPresentation alone adopts opts.simulation (ADR 0011/0012).
	_net_drive.stage_runtime_options(opts)
	# The placer + environment node let the wire present pass resolve + light its
	# remote-entity avatars (build_player_animated_model): every remote row on a
	# joiner, and the admitted players' synthetic-origin rows on the host.
	opts.placer = _placer
	# The occlusion-claim set the present pass consults (two-bit visibility
	# ownership; see OcclusionFramePass._set_occlusion_hidden). Shared by
	# reference: the pass created these dictionaries once and mutates them in
	# place across the mission's occlusion frames — hand the SAME instances.
	opts.occlusion_hidden_ids = _occlusion.occlusion_hidden_ids()
	opts.present_visibility = _occlusion.present_visibility()
	# The fire present pass's providers (AI/remote fire sound + muzzle + tracers): audio
	# and effect world resolve lazily (mission audio is set up after the runtime), the
	# listener is the same camera position the audio render pass ticks with.
	opts.fire_audio = get_mission_audio
	opts.fire_fx = get_effect_world
	opts.fire_listener = _fire_listener_position
	# The destruction/throwable present passes anchor their wreck/piece/move
	# effect groups through the ItemEffectDirector's owner-anchor registry
	# (the typed seam; GameWorld's register_effect_anchor delegates to the
	# same instance).
	opts.effect_anchors = _item_fx
	# The scar present pass reads the fog distance + the combined terrain light
	# off the live environment node each present frame (world-wac-ai-re §24.9).
	opts.environment_node = get_environment_node
	# The dynamic light-pool routes (renderer/light_scene.h witness map): the
	# MF_Light muzzle glow per presented fire, the death flash per husk death.
	if _light_director != null:
		opts.muzzle_light = _light_director.on_muzzle_fire
		opts.death_light = _light_director.on_death_light
	_runtime.setup(mission, container, opts)
	if _runtime.get_sim() == null:
		var setup_error := int(_runtime.get_setup_error())
		var lan_bind_failure := opts.net_transport == "lan"
		var bind_port := opts.bind_port
		# Free before emitting: a load_failed handler may synchronously tear
		# the world down (the game shell returns to the menu via unload()),
		# and unload() frees _runtime — emitting first turned this leg into a
		# null-instance free on reentry.
		_runtime.free()
		_runtime = null
		if lan_bind_failure:
			load_failed.emit("host start: could not bind LAN UDP port %d" % bind_port)
		else:
			load_failed.emit("failed to start mission runtime")
		return setup_error if setup_error != OK else ERR_CANT_CREATE
	_run_mission_start_environment_boundary()
	_sync_runtime_profiling()
	# The player profile's saved weapon kits, loaded before ANY kit is applied or
	# submitted: in a net session the original's spawn kit is a page of this file,
	# selected by the very class byte it also puts on the wire
	# [orig: Game_StartMission @ 0x525767-0x525836].
	_load_player_weapon_profile()
	_player_visuals._apply_local_player_spawn_loadout()
	_runtime.set_presentation_time_ms(_panm_clock.time_ms)
	if _water != null:
		# Water may have been built before the runtime existed — re-push the
		# sim-side plane the footstep/landing legs compare feet against.
		_runtime.get_sim().set_water_z(float(_water.water_height))
	_runtime.effects_drained.connect(_on_runtime_effects)
	_runtime.fixed_tick_completed.connect(_on_runtime_fixed_tick)
	_runtime.simulation_restarted.connect(_on_runtime_simulation_restarted)
	# A browsable listen host: register it with the NovaWorld gate (F1), if one was
	# configured. No-op for single-player, joiners, and pure-LAN play.
	_net_drive.on_runtime_started(opts, bms_name)
	# The game starts running (tick() gates on is_playing, so the overlay's
	# transport can pause/step a live mission).
	_runtime.play()
	return OK


# The on-disk path of the player profile's weapon file. Retail builds it from the
# ACTIVE expansion name — with an expansion loaded it looks ONLY under that
# expansion's directory (there is no base-game fallback leg), otherwise it reads the
# game root's copy [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0, path build
# @ 0x54f68c-@ 0x54f6b7: g_ExpansionName[0] ? "expansion\<name>\weapon.sav" :
# "weapon.sav"]. The mount is the authority on both halves — for a joiner it has
# already been reconciled to the HOST's expansion (D-NET-178), which is what makes
# the profile's ADM index space agree with the host's.
# Load weapon.sav onto the sim: five profile-slot records, each carrying a per-side
# class byte and the five 2048-byte class kit pages the MP loadout submit indexes BY
# that class byte [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 — header check
# @ 0x54f586 ("FPBC"/"0211"), the 5 x 0x1080C record reads]. This is a plain disk
# file, not archive content, so it is read through the mount's directory rather than
# the VFS. A file that is absent or not a profile is NOT a load failure: retail's
# own miss leaves PlayerProfile_InitDefaults' shipped defaults in place (BLUE/RED
# class 8, one weapon name per class page) [orig: @ 0x54bb40].
func _load_player_weapon_profile() -> void:
	var sim := get_sim()
	if sim == null:
		return
	var path := PlayerProfile.weapon_profile_path(_resource_root)
	if path.is_empty():
		return
	if not FileAccess.file_exists(path):
		print_verbose("GameWorld: no weapon.sav at %s — keeping the shipped profile defaults" % path)
		return
	var err := int(sim.load_weapon_profile(path))
	if err != OK:
		push_warning("GameWorld: weapon.sav at %s not accepted (error %d) — keeping the shipped profile defaults"
				% [path, err])


# _start_runtime's signal connects bind these GameWorld methods (a future
# harness can override them here); the handler bodies live in
# WorldEffectRouter (world_effect_router.gd).
func _on_runtime_effects(effects: Array) -> void:
	_effect_router._on_runtime_effects(effects)


func _on_runtime_fixed_tick(logic_tick: int) -> void:
	_effect_router._on_runtime_fixed_tick(logic_tick)


func _on_runtime_simulation_restarted() -> void:
	_effect_router._on_runtime_simulation_restarted()


# Place real ambient sounds at the mission's sound markers: load the co-named .LWF
# + gamelocl.LWF, resolve each marker to a sound set by name, and spawn looping 3D
# voices. Reuses the placer's item database for the item_id -> soundloop_1..4 lookup.
func _start_mission_audio(mission: MissionData, bms_name: String) -> void:
	var item_db: ItemDatabase = _placer.get_item_db() if _placer != null else null
	_mission_audio = MissionAudio.new(_resource_root, item_db)
	# Sound occlusion runs LOS through the sim's collision world + terrain
	# [orig: Sound_ApplyOcclusionDistance @ 0x529970]; hosts without a sim mix
	# unoccluded.
	_mission_audio.set_simulation(get_sim())
	var stats := _mission_audio.setup(mission, bms_name, self)
	if _env != null:
		_mission_audio.set_time_of_day_hhmm(_env.time_of_day)
	print_verbose("GameWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d ambient candidate(s), %d/%d physical channel(s) allocated" % [
		int(stats.markers_resolved),
		int(stats.markers_total),
		int(stats.banks_loaded),
		int(stats.ambient_candidates),
		int(stats.physical_channels),
		int(stats.channel_budget),
	])
	# Open the GAME music context + seed the witnessed vars [orig: Game_StartMission
	# @ 0x525581-0x52561b]. Retail gates the open on is_mp_session_peer and STOPS
	# music in single-player; ours opens in ALL sessions — D-MUS-SPGATE
	# (docs/audio/mus-sbf-re.md §Game music driving; SP-as-listen-server, ADR
	# 0009/0011/0012). gamemus's discriminator Var1 stays 0 (never written in
	# retail), so the Multiplayerstart P0 loop plays.
	MusicService.open_game_context(_resource_root)


# The load-time effect warm pass (see the load-path call site): spawn every
# catalog effect in front of the load camera, advance the fixed tick so fresh
# emitters actually emit, force-draw two frames SYNCHRONOUSLY so every new
# material/pipeline draws once (no coroutine — the load path stays callable
# without await), then clear the warm spawns exactly like the sim-restart
# path (reset + re-register the persistent item effects). Returns the count.
# The 256x256 depthspin-equivalent shore mask. The base map always binds the
# original sharp colormap; this texture carries only transparent/blue water.
var _minimap_water_mask: ImageTexture = null


func get_minimap_water_mask() -> ImageTexture:
	return _minimap_water_mask


# Build retail's depthspin shore mask directly from the raw CPT height atlas.
# Streamed .til art does not participate in either the sharp colormap base or
# this independent water pass.
func _build_minimap_water_mask() -> void:
	_minimap_water_mask = null
	var terrain_data := get_terrain_data()
	if terrain_data != null and not Engine.is_editor_hint():
		var live_water := float(_water.water_height) if _water != null else NAN
		_minimap_water_mask = terrain_data.build_minimap_water_mask(live_water)
	minimap_water_changed.emit(_minimap_water_mask)


func _warm_effect_world_catalog() -> int:
	if _effect_world == null:
		return 0
	var warm_pos := Vector3.ZERO
	var cam := get_viewport().get_camera_3d() if is_inside_tree() else null
	if cam != null:
		warm_pos = cam.global_position - cam.global_transform.basis.z * 8.0
	# The persistent master switch is a gameplay preference, not a reason to
	# leave the catalog cold forever. Lift it only across the loading-screen
	# draws; keep the director's persisted switch unchanged and restore the
	# EffectWorld before persistent item effects are reattached.
	var restore_particles_hidden := _effect_world.are_particles_hidden()
	if restore_particles_hidden:
		_effect_world.set_particles_hidden(false)
	var spawned := int(_effect_world.warm_all_effects(warm_pos))
	if spawned <= 0:
		_effect_world.reset_runtime_state()
		if restore_particles_hidden:
			_effect_world.set_particles_hidden(true)
		return 0
	# The tracer ribbon pipelines compile in the same forced frames.
	if _runtime != null:
		_runtime.warm_present_pipelines(warm_pos)
	_effect_world.advance_fixed_tick(Simulation.tick_dt())
	_effect_world.render_now()
	# Pipeline compiles need real draws. Skip the forced frames inside the
	# editor embedder (re-entrant editor drawing); the texture warm above still
	# runs there, and the shipped game is what the full warm protects.
	if is_inside_tree() and not Engine.is_editor_hint():
		# MainGame keeps World hidden behind the opaque loading CanvasLayer.
		# Temporarily expose it so the particle domains, tracer MeshInstance,
		# and deterministic helper quads are actually submitted to force_draw.
		var was_visible := visible
		visible = true
		RenderingServer.force_draw(true)
		_effect_world.advance_fixed_tick(Simulation.tick_dt())
		_effect_world.render_now()
		RenderingServer.force_draw(true)
		# The reset below cancels any unserviced compositor warm request. Drain
		# the forced draws first so threaded renderers cannot race that cancel.
		RenderingServer.force_sync()
		visible = was_visible
	_effect_world.reset_runtime_state()
	if restore_particles_hidden:
		_effect_world.set_particles_hidden(true)
	_item_fx.reattach()
	if _light_director != null:
		_light_director.reattach()
	var unresolved := PackedStringArray(
			_effect_world.get_unresolved_texture_names()).size()
	print_verbose("GameWorld: effect warm pass — %d effect(s) precompiled, %d unresolved texture(s)" % [
			spawned, unresolved])
	return spawned


# Mission-start load of EVERY mounted .ptl into the runtime effect world
# [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
# file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
func _start_effect_world() -> void:
	_effect_world = EffectWorld.new()
	_effect_world.name = "EffectWorld"
	add_child(_effect_world)
	if _item_fx.particles_hidden():
		_effect_world.set_particles_hidden(true)
	var count := _effect_world.load_from_resource_root(_resource_root)
	if _water != null:
		_effect_world.set_water_height(float(_water.water_height))
		# The sim-side water plane (env.water_z): the footstep water pick, the
		# landing legs, AND the destruction paths (submerged wrecks skip pieces,
		# the wreck fire steams out) all gate on it [orig: Env_WaterHeightFixed
		# @ 0x26C6454; world-wac-ai-re §24]. Idempotent; re-pushed after runtime
		# start too (either side may come up first).
		var water_sim := get_sim()
		if water_sim != null:
			water_sim.set_water_z(float(_water.water_height))
	print_verbose("GameWorld: effect world — %d effect(s) across %d .ptl file(s)" % [
		count, _effect_world.file_count()])
	# Item-effect wiring — the owner-pose provider, the persistent per-item
	# attaches, and the wire-spawn callback — lives in the director.
	_item_fx.on_effect_world_started()
	if _light_director != null:
		_light_director.reattach()
	# One wire-spawn router for both directors: the runtime callback is
	# single-subscriber, so the world owns the fan-out.
	var wire_runtime: MissionPresentation = get_runtime()
	if wire_runtime != null and _light_director != null:
		wire_runtime.set_wire_node_spawned_callback(
				func(node: ObjectModel, kind: int, item_id: int) -> void:
					_item_fx.on_wire_node_spawned(node, kind, item_id)
					_light_director.on_wire_node_spawned(node, kind, item_id))


func get_effect_world() -> EffectWorld:
	return _effect_world


## The live terrain node, for the F3 Terrain & foliage page's counters/knobs.
func get_terrain_node() -> Terrain:
	return _terrain


## The mounted item database (null before a mission), for the F3 snapshot
## writer's display-name/graphic enrichment.
func get_item_db() -> ItemDatabase:
	return _placer.get_item_db() if _placer != null else null


## The live environment / weather / water nodes, for the F3 Environment
## page's readouts and scrub knobs.
func get_environment_node() -> MissionEnvironment:
	return _env


func get_weather_node() -> Weather:
	return _weather


## Hosted mission-clock knob used by F3 and runtime MCP. MissionEnvironment owns
## the fixed-point clock; GameWorld coordinates the weather/audio consumers so
## a paused scrub is an immediate visible state change rather than just a
## readback value waiting for the next simulation tick.
func get_debug_mission_minute_of_day() -> float:
	if _env == null or not _env.is_loaded():
		return 0.0
	return _env.get_mission_minute_of_day()


func debug_set_mission_minute_of_day(minute_of_day: float) -> Error:
	if _env == null or not _env.is_loaded():
		return ERR_UNAVAILABLE
	var sim: Simulation = get_sim()
	var err: Error = OK
	if sim != null and sim.weather_state_bound():
		if not sim.debug_set_time_of_day_minutes(minute_of_day):
			err = ERR_UNAVAILABLE
	else:
		err = _env.debug_set_mission_minute_of_day(minute_of_day)
	if err != OK:
		return err
	var weather := get_weather_node()
	if weather != null:
		weather.resync_colors_now()
	if _mission_audio != null:
		_mission_audio.set_time_of_day_hhmm(_env.time_of_day)
	return OK


## Re-evaluates only camera-dependent production render state for an exact-pose
## visual capture (the settle contract and the frozen-pose leg order live with
## the body in world_device_frame.gd).
func debug_refresh_render_pose(camera: Camera3D) -> Error:
	return _device_frame.debug_refresh_render_pose(camera)


func get_water_node() -> Water:
	return _water


func get_celestial_node() -> Celestial:
	return _celestial


func get_sky_dome_node() -> SkyDome:
	return _sky_dome


func get_sun_shadow_node() -> SunShadow:
	return _sun_shadow


## Whether this world advances the env presenters (weather, sun shadow, sky,
## celestial, water) itself instead of their own _process.
func drives_environment_presenters() -> bool:
	return _env_presenters_world_driven


func get_clear_color_node() -> WorldEnvironment:
	return _clear_color


## One typed read-only renderer snapshot for MCP, visual probes, and comparison
## tooling. Keeping camera/environment/water/shadow/pass sampling together
## guarantees every consumer sees the same fields and frame semantics.
func get_render_diagnostics(
		camera: Camera3D = null) -> GameRenderDiagnostics:
	var viewport := get_viewport() if is_inside_tree() else null
	if camera == null and viewport != null:
		camera = viewport.get_camera_3d()
	return GameRenderDiagnostics.from_world(self, camera, viewport)


## A caller registers a live pose resolver for an owner-bound effect group it
## spawned (e.g. the local muzzle flash riding the viewmodel userpoint). The
## resolver is polled by the effect world's owner-pose sync while any group
## bound to owner_key is alive; re-registering the same key overwrites.
## One-line delegates into the item-effect director (item_effect_director.gd):
## the names stay on GameWorld — LocalPlayerPresenter and the present passes
## register through the world, and harness worlds pin these methods.
func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
	_item_fx.register_effect_anchor(owner_key, resolver)


func unregister_effect_anchor(owner_key: Variant) -> void:
	_item_fx.unregister_effect_anchor(owner_key)


func get_mission_audio() -> MissionAudio:
	return _mission_audio


# Re-drive the gamemus vars from the local player each frame, the way the
# original does from the local player's body update [orig:
# Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
# g_local_player_entity @ 0x4b6234; full map docs/audio/mus-sbf-re.md §Game
# music driving]. Pumped here: Var7 = health % (cur*100/max, 100 when max <=
# cur [orig: @ 0x4b6315-0x4b6324]) and Var10 = team [orig: @ 0x4b62fc].
# Witnessed-but-unpumped seams (the shipped gamemus reads none of them —
# docs/audio/mus-sbf-re.md (D-MUS-VARPUMP)): Var2 view pitch (the original
# writes raw engine angle units, unwitnessed conversion), Var5/Var6 threat
# distance / threat-targets-me (Entity_FindNearestThreat @ 0x4b0990 unported),
# Var3/Var4 (low-confidence), Var8 game type (retail scoring-mode ids not yet
# mapped to our sessions).
func _music_var_pump() -> void:
	if _runtime == null or not _runtime.has_player():
		return
	var pump_sim := _runtime.get_sim()
	if pump_sim == null:
		return
	MusicService.set_var(MusicDirector.GAME_VAR_HEALTH_PCT,
		pump_sim.get_local_player_health_percent())
	MusicService.set_var(MusicDirector.GAME_VAR_TEAM, _runtime.local_player_team())
