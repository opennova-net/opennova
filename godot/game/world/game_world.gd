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
#   * calls _load_stages.unload() to tear the played world down (placed objects, runtime,
#     audio, env overrides) before loading another mission or leaving.

const VegAssets := preload("res://game/terrain/veg_assets.gd")
# Godot's per-geometry reservation in the global shader buffer (vec4 values) for
# a shader that declares instance uniforms; see _instance_uniform_geometry_estimate.
const GameFramePipelineScript := preload("res://game/world/game_frame_pipeline.gd")
const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const FirstPersonArmsWitness := preload(
		"res://game/world/first_person_arms_witness.gd")

signal world_loaded()
signal load_failed(reason: String)
## A joiner's authoritative session record (post-auth S2C 0x7B) resolved during
## the pre-load wait: server/mission names + the exact wire-header world about
## to be constructed. The shell refreshes its loading screen from this — retail's
## connect stream fills the same session vars before its header-backed load
## [orig: parse_server_session_variables @ 0x5202f0].
signal join_session_identified(info: LoadingScreenInfo)
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
@onready var _environment_cube: EnvironmentCubeCapture = \
		get_node_or_null("EnvironmentCubeCapture")
@onready var _framefx: FrameFx = \
		get_node_or_null("FrameFx")
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
var _slot_shadow: SlotShadow
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
var _mission_stats: MissionPlacementStats = null
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database); untyped
             # because game_world_test's ViewmodelWorldHarness installs a RefCounted double
var _last_load_timeline: PerfTimeline = null  # the most recent load_mission timing
var _weapon_db: WeaponDatabase = null  # weapon.def, lazy per mounted root (FP viewmodel)
var _local_weapon: WeaponDef = null  # the resolved weapon.def row (the viewmodel/HUD slices decode it)
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
# The load-plan stage bodies (WorldLoadStages), reached through this handle.
var _load_stages: WorldLoadStages
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
var _local_character_profile: CharacterJoinProfile = null
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
## value is consumed once the runtime exists (or discarded by _load_stages.unload after a
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
	return _load_stages._mount_runtime_root(dir)


func _init() -> void:
	# The load-plan stage bodies; first, because the net drive below takes
	# the internal-load Callable from this handle.
	load_stages()
	# The drive's preload wait is stepped by its own synchronous _process, so it
	# must be in the tree before load_mission_as_joiner runs: constructed here, it
	# enters the tree with the world itself, and every load_* entry runs on an
	# in-tree world. The drive holds this world for its public load surface
	# + signal emission; the three Callables lend it the private internals the
	# preload path needs without widening GameWorld's API.
	_net_drive = NetSessionDrive.new()
	_net_drive.name = "NetSessionDrive"
	_net_drive.setup(self,
			_load_stages._load_mission_internal,
			_resolve_root,
			func() -> Dictionary: return _local_player_spawn_loadout)
	add_child(_net_drive)
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
				return _placer.get_static_item_effect_sources() if _placer != null else [],
			func() -> Array:
				return _placer.get_static_light_draw_sources() if _placer != null else [],
			func() -> int:
				return int(_placer.get_static_light_draw_source_revision()) \
						if _placer != null else 0)


func _ready() -> void:
	set_process(false)
	_frame_pipeline = GameFramePipelineScript.new()
	_frame_pipeline.setup(self)
	if _clear_color != null and _clear_color.environment != null:
		_idle_frame_clear_color = _clear_color.environment.background_color
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher")
		if _dispatcher != null:
			# The applier reads the native detail-cell handoff and the composed
			# surface textures through this wired owner (never a parent probe).
			_dispatcher.set_terrain(_terrain)
			# The detail sway phase reads the weather oscillator's ring slot 0
			# (retail Env_WaveOscRing[0] in Foliage_SetupVertexShaderConstants).
			_dispatcher.set_weather(_weather)
	_sun_shadow = SunShadow.new()
	_sun_shadow.name = "SunShadow"
	_sun_shadow.projection_mode = SunShadow.PROJECTION_DYNAMIC
	add_child(_sun_shadow)
	_sun_shadow.set_environment_node(_env)
	# The render-slot entity ground shadows: the per-slot silhouette capture
	# device + the terrain drape publisher (retail's per-entity RT pipeline —
	# engine/runtime/renderer/render_slot_shadow.h carries the witness map).
	_slot_shadow = SlotShadow.new()
	_slot_shadow.name = "SlotShadow"
	# The highest selectable retail profile is SHADOWQUALITY=3. Detail 4 is an
	# internal oversample tier (1024px slot 0 and all slots every frame), not the
	# shipped maximum; profile 3 uses 512px captures and retail's half-rate
	# stagger for non-player slots.
	_slot_shadow.set_shadow_detail(3)
	add_child(_slot_shadow)
	_slot_shadow.set_environment_node(_env)
	# The retained water renderer starts dormant until a successful load chooses
	# its runtime mode. In particular, do not let an authored scene height make
	# initial/menu frames look underwater.
	_load_stages._set_water_world_rendering_enabled(false)
	# Freeze the retained weather node until a load selects autonomous bare/net
	# rendering or prepares a mission-owned fixed tick.
	_load_stages._set_weather_world_tick_driven(true)
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
		_device_frame.apply_scene_environment_frame()
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

	_load_stages._set_water_world_rendering_enabled(false)
	_load_stages._set_mission_water_height_override(NAN)
	_load_stages._clear_mission_tile_info()
	_resource_root = resource_root
	if not _load_stages._load_environment(env_file):
		load_failed.emit("failed to load %s" % env_file)
		return ERR_CANT_OPEN
	if not _load_stages._load_terrain(terrain_file):
		load_failed.emit("failed to load %s" % terrain_file)
		return ERR_CANT_OPEN

	_world_ready = true
	_load_stages._prepare_autonomous_weather()
	_load_stages._set_water_world_rendering_enabled(true)
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
	return _load_stages._load_mission_internal(mission, bms_name, resource_root)


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
	return _load_stages._load_mission_internal(mission, mission_name, resource_root)


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
	return _load_stages._load_mission_internal(mission, bms_name, resource_root)

func missing_mission_asset_reason(asset: String, bms_name: String,
		resource_root: ResourceRoot, wire_header_join: bool) -> String:
	return _load_stages.missing_mission_asset_reason(asset, bms_name, resource_root, wire_header_join)


func unload() -> void:
	_load_stages.unload()


func settle_join_wire_assets() -> bool:
	return _load_stages.settle_join_wire_assets()


func is_join_wire_present_drained() -> bool:
	return _load_stages.is_join_wire_present_drained()


func report_join_wire_asset_failure(reason: String) -> void:
	_load_stages.report_join_wire_asset_failure(reason)


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
## and an _load_stages.unload both re-apply the default; the debug row switches the pass
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
	return _mission_stats.authored_occluder_models if _mission_stats != null else 0


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


func get_mission_stats() -> MissionPlacementStats:
	return _mission_stats


## The placer's static populations that currently carry at least one live
## row (the per-frame RLOD selection empties and refills them); 0 before a
## mission is placed.
func get_static_live_population_count() -> int:
	return int(_placer.get_static_live_population_count()) \
			if _placer != null else 0


## Process-exit-only release for renderer resources intentionally retained by
## _load_stages.unload() so world-to-menu and mission-to-mission transitions stay warm.
func release_runtime_renderer_resources() -> void:
	# FrameFx publishes Q3 frames that retain sampled producer resources. Drain
	# its compositor callback before Water releases those source textures.
	if _framefx != null:
		_framefx.shutdown()
	var runtime_water := _water as Water
	if runtime_water != null:
		runtime_water.release_runtime_renderer_resources()


# Runtime water exposes a render-aware predicate so its retained authored
# height cannot leak into frame clear/occlusion while a load is absent or in
# progress.
func is_water_render_active() -> bool:
	return _water != null and _water.is_water_render_active()


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
# GameFramePipeline invokes the concrete renderer/audio/environment operations on
# the WorldDeviceFrame this accessor hands it (no per-leg delegators here); the
# pipeline's typed frame outcome and the local-view presenter handoff stay on
# this node.
# one visible order around inmatch::Session::advance(). The leg bodies and the
# per-frame camera/timing latch live in WorldDeviceFrame
# (world_device_frame.gd); these one-line delegates keep every leg name on
# GameWorld for the pipeline's duck-typed contract, the probes, and the tests.

# The shell-owned local-player presenter whose camera/viewmodel placement the
# local-view device leg runs inside the frame (null in worlds without one —
# tests, dedicated). D-RORD-8: placing it before the occlusion/iris/particle
# legs lets them read the camera THIS frame's tick produced, not last frame's.
var _local_view_presenter: LocalPlayerPresenter = null


## The device legs GameFramePipeline orders each frame (world_device_frame.gd).
func device_frame() -> WorldDeviceFrame:
	return _device_frame


## The load-plan stage bodies (world_load_stages.gd); the harnesses that drive
## one stage directly reach it here. Built on first use so a harness that
## overrides _init without chaining still gets one.
func load_stages() -> WorldLoadStages:
	if _load_stages == null:
		_load_stages = WorldLoadStages.new()
		_load_stages.setup(self)
	return _load_stages


## One-time handoff from the shell that owns the local-player presenter.
func set_local_view_presenter(presenter: LocalPlayerPresenter) -> void:
	_local_view_presenter = presenter


func get_effect_light_report() -> EffectLightReport:
	return _light_director.get_report() if _light_director != null else null


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
	return _device_frame.get_runtime_perf_counters()


# Listener position for the fire present pass — the same camera position the audio
# render pass ticks with (INF until the first tick).
var _last_tick_camera_pos := Vector3.INF


func _fire_listener_position() -> Vector3:
	return _last_tick_camera_pos


# Fire-presentation counters (FirePresentPass.Stats, typed per ADR 0017;
# null until a mission runs).
func get_fire_present_stats() -> RefCounted:
	return _runtime.get_fire_present_stats() if _runtime != null else null


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


# _load_stages._start_runtime's signal connects bind these GameWorld methods (a future
# harness can override them here); the handler bodies live in
# WorldEffectRouter (world_effect_router.gd).
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
