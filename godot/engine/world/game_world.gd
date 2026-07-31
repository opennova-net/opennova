class_name GameWorld
extends Node3D

# Loads a playable world (terrain + environment + vegetation + foliage) from ONE
# resource root and wires it onto the engine nodes it contains (NovaTerrain,
# NovaEnvironment, NovaWater). The data core is shared engine code
# (NovaTerrainData, EnvFile, NovaFoliageDispatcher, VegAssets); this node is just
# runtime orchestration — one loader, one root, no fallbacks. The scene lives
# in game_world.tscn so the game shell and focused engine tests can instance it;
# production play mounts the selected runtime resource directory.
#
# HOST CONTRACT: a shell or focused test instances game_world.tscn, optionally injects a root,
# calls one load_* entry, then
#   * drives tick(camera_position) once per frame while playing (foliage ->
#     runtime logic+present -> audio, in that order; pausing = not ticking),
#   * provides the camera that position comes from,
#   * consumes mission_effects (HUD text / win / waypoints / dialog routing),
#   * calls unload() to tear the played world down (placed objects, runtime,
#     audio, env overrides) before loading another mission or leaving.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const NovaSunShadowScript := preload("res://engine/environment/nova_sun_shadow.gd")
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const PanmClockScript := preload("res://engine/world/panm_clock.gd")
const NovaModelResolver := preload("res://engine/mission/nova_model_resolver.gd")
const NetWorldView := preload("res://engine/world/net_world_view.gd")
const NetEventView := preload("res://engine/world/net_event_view.gd")
const NovaDebugViewStatus := preload(
		"res://engine/debug/nova_debug_view_status.gd")
const NET_CONTAINER_NAME := "NetObjects"
const TICK_DT := MissionRuntime.TICK_DT  # one source; default for tick()'s delta param
const WEATHER_TICK_HZ := NovaWeather.WEATHER_TICK_HZ  # one source (the weather core's cadence)
const MAX_WEATHER_CATCHUP_TICKS := 31

signal world_loaded()
signal load_failed(reason: String)
## A joiner's authoritative session record (post-auth S2C 0x7B) resolved during
## the pre-load wait: server/mission names + the local mission file about to
## load. The shell refreshes its loading screen from this — retail's connect
## stream fills the same session vars before its local load
## [orig: parse_server_session_variables @ 0x5202f0].
signal join_session_identified(info: Dictionary)
## A joiner crossed the authoritative admission edge. Local terrain/mission load
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
# Mission-load progress, 0..100, emitted at the stage boundaries below and
# pulsed (at the stage's constant value) from inside the object-placement loop.
# The values are the witnessed schedule's anchor points; the original pumps its
# loading screen the same way — constant per-stage percentages, re-presented
# from inside the model-load loops [orig: Game_StartMission's
# LoadingScreen_UpdateAndPresent calls @ 0x52498f..0x525d29].
signal load_progress(percent: int)
# Host-presentation side effects drained from the mission runtime's EffectLog each tick
# (kind: "text"/"debug_text"/"win"/"subgoal_*"/"show_waypoints"/"set_light"/"dialog").
# Player text is consumed by the HUD; debug_text remains a distinct unrouted channel.
# "dialog" is also routed straight to mission audio below.
signal mission_effects(effects: Array)

# A mission (.bms) to boot into. When set, the mission's header selects the
# terrain + environment (terrain_file/env_file below are ignored) and its placed
# objects are populated into the world. Empty = load bare terrain + environment.
@export var mission_file: String = ""

# The terrain + environment loaded, by name, from the resource directory. Used
# only when mission_file is empty.
@export var terrain_file: String = "Dvxi5.trn"
@export var env_file: String = "full_00.env"

@onready var _terrain: NovaTerrain = $NovaTerrain
@onready var _env: Node = get_node_or_null("NovaEnvironment")
@onready var _water: Node = get_node_or_null("NovaWater")
@onready var _clear_color: WorldEnvironment = get_node_or_null("ClearColor")

var _dispatcher: NovaFoliageDispatcher
var _tile_overlay: NovaTerrainTileOverlay
var _sun_shadow: NovaSunShadow
var _static_sun_shadow: NovaSunShadow
var _terrain_data: NovaTerrainData
var _resource_root: NovaResourceRoot
var _mission_tile_info: NovaTerrainTileInfo
var _mission_til_bytes := PackedByteArray()
var _loaded: bool = false
var _loaded_mission: NovaMissionData
# The BMS argument that completed the active mission load. This is runtime
# state, deliberately separate from mission_file (the exported boot option).
var _loaded_mission_file: String = ""
var _runtime  # MissionRuntime: the one mission runtime driver (sim + present pass + index), DIVIDED cadence
var _panm_clock = PanmClockScript.new()
var _mission_stats: Dictionary = {}
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database)
var _weapon_db: NovaWeaponDatabase = null  # weapon.def, lazy per mounted root (FP viewmodel)
var _local_weapon_dict := {}  # the resolved weapon's raw dict (FSM setup transport, ADR 0017 edge)
var _mission_audio: NovaMissionAudio
var _effect_world: NovaEffectWorld  # the runtime .ptl effect world (render-only, per mission)
# Host-owned first-person presentation seam. MissionRuntime invokes GameWorld
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
# The mission attribute that forces the indoors accum bit every frame. Stays
# on the world (mission state, test-pinned by name); handed to the pass's
# entries as an argument. [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8-0x5ca1cd]
var _mission_forces_indoors := false
var _idle_frame_clear_color := Color.BLACK
var _net_client     # NovaNetClient: the in-match wire client (replay or live)
var _net_view       # NetWorldView: spawns + drives models from the decoded world
var _net_event_view # NetEventView: draws the decoded event stream over the world
# A host-injected resource root (main_game hands its boot mount over; tests
# hand fixture roots). When set, the load_*
# entries skip the settings lookup + their own mount and resolve through it; the
# game path (no injection) still mounts from the persisted resource directory.
var _injected_root: NovaResourceRoot = null
# Debug: hide the scattered foliage (F3 overlay's "Hide foliage"). Off by default.
var _foliage_hidden := false
var _playable := true
# The net-session drive: typed request staging, the joiner preload/admission
# coroutines, the ESC aborts, and NovaWorld gate registration (see
# net_session_drive.gd). The session signals live on THIS node — the shell
# contract pins them here — and the drive emits them through its world reference.
var _net_drive: NetSessionDrive
# The F3 debug-view set: the world-space debug views + the pick stack, an
# internal child node on the same pattern (see debug_view_set.gd). The views
# it builds attach to THIS world node — hosts and tests pin them as
# world-relative lookups — and the moved public toggles keep one-line
# delegates below so the host-facing surface never moved.
var _debug_views: DebugViewSet
# The per-item ITEMS.DEF effect director (item_effect_director.gd): the
# attached/static/controller item emitters, the effect-anchor resolvers, and
# the retail master particle switch, on the same internal pattern (plain
# RefCounted — it owns no Nodes). Public delegates below keep the host-facing
# names on GameWorld. ALSO the sanctioned test-injection seam: like _runtime,
# harnesses may swap in a director double (see game_world_test.gd).
var _item_fx: ItemEffectDirector
var _local_player_spawn_loadout: Dictionary = {}
var _perf_tick_us: int = 0
var _perf_foliage_us: int = 0
var _perf_runtime_us: int = 0
var _weather_tick_credit := 0.0
var _perf_audio_us: int = 0


## Inject the resource root the next load resolves through. Runtime hosts and
## focused tests use this to keep one already-mounted resource session. Null
## returns to the game's settings-driven mount.
func set_resource_root(root: NovaResourceRoot) -> void:
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
func _resolve_root(dir: String) -> NovaResourceRoot:
	if _injected_root != null:
		return _injected_root
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		load_failed.emit("no resource directory set")
		return null
	return _mount_runtime_root(dir)


func _init() -> void:
	# The drive's preload/admission waits are coroutines that await process_frame,
	# so it must be in the tree before load_mission_as_joiner runs: constructed
	# here, it enters the tree with the world itself, and every load_* entry runs
	# on an in-tree world. The drive holds this world for its public load surface
	# + signal emission; the three Callables lend it the private internals the
	# preload path needs without widening GameWorld's API.
	_net_drive = NetSessionDrive.new()
	_net_drive.name = "NetSessionDrive"
	_net_drive.setup(self,
			Callable(self, "_load_mission_internal"),
			Callable(self, "_resolve_root"),
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
	var effect_world_getter := func():
		var world = ref.get_ref()
		return world.get_effect_world() if world != null else null
	_debug_views.setup(self, user_point_sources, effect_world_getter)
	add_child(_debug_views)
	# The render-occlusion frame pass: plain RefCounted (no tree presence),
	# direct-called from tick() every frame. Constructed exactly once — its two
	# shared dictionaries must keep their identity for the mission present pass.
	_occlusion = OcclusionFramePass.new()
	_occlusion.setup(self)
	# The item-effect director, wired like the debug-view set: its two lent
	# privates are the placer's static item-effect sources and its item
	# database, null-guarded here. The db seam stays duck-typed on purpose —
	# the public get_item_db() keeps its NovaItemDatabase contract while
	# harness worlds serve value-only db doubles.
	_item_fx = ItemEffectDirector.new()
	_item_fx.setup(self,
			func() -> Array:
				return _placer.get_static_item_effect_sources() if _placer != null else [],
			func() -> Variant:
				return _placer.get_item_db() if _placer != null else null)


func _ready() -> void:
	if _clear_color != null and _clear_color.environment != null:
		_idle_frame_clear_color = _clear_color.environment.background_color
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher") as NovaFoliageDispatcher
		_tile_overlay = _terrain.get_node_or_null("TileOverlay") as NovaTerrainTileOverlay
	_sun_shadow = NovaSunShadowScript.new()
	_sun_shadow.name = "NovaSunShadow"
	_sun_shadow.projection_mode = NovaSunShadow.PROJECTION_DYNAMIC
	add_child(_sun_shadow)
	_sun_shadow.set_environment_node(_env)
	_static_sun_shadow = NovaSunShadowScript.new()
	_static_sun_shadow.name = "NovaStaticSunShadow"
	_static_sun_shadow.projection_mode = NovaSunShadow.PROJECTION_STATIC_TERRAIN
	add_child(_static_sun_shadow)
	_static_sun_shadow.set_environment_node(_env)
	# Both retained render systems start dormant until a successful load chooses
	# their host mode. In particular, do not let an authored scene height make
	# initial/menu frames look underwater.
	_set_water_world_rendering_enabled(false)
	# Freeze the retained weather node until a load selects autonomous bare/net
	# rendering or prepares a mission-owned fixed tick.
	_set_weather_world_tick_driven(true)


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		_stop_water_render_stats()
		return
	if what != NOTIFICATION_VISIBILITY_CHANGED or not is_node_ready():
		return
	if _loaded and is_visible_in_tree():
		_clear_env_generation = -1
		_update_frame_clear_color()
	else:
		_restore_idle_frame_clear_color()


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

	_loaded = true
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
			bms_name, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY):
		load_failed.emit("%s not found in %s" % [bms_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(
			resource_root, bms_name, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY) != OK:
		load_failed.emit("failed to parse %s: %s" % [bms_name, mission.get_last_error()])
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, bms_name, resource_root)


## Load the exact saved, top-level loose BMS from the selected resource root.
## This is ONED's standalone F6 path: it deliberately differs from load_mission(),
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
	var mission := NovaMissionData.new()
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
## staged and driven by NetSessionDrive (authenticate before the local load; S2C
## 0x7B supplies the mission). Returns the same codes as load_mission.
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


## Load an in-memory mission through the shared world pipeline. This is retained
## as a focused engine-test/tool seam; normal game and ONED launches always use
## a saved .bms through load_mission() or load_loose_mission().
func load_mission_data(mission: NovaMissionData, bms_name: String, dir: String = "") -> int:
	if mission == null or not mission.is_loaded():
		load_failed.emit("no mission document to load")
		return ERR_INVALID_PARAMETER
	var resource_root := _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, bms_name, resource_root)


## Spectate a NET-driven session: a NovaNetClient connects to the source (the
## replay tool today, a real server later), and entities come from the LIVE WIRE
## stream — not the .bms placements, not the AI sim. The map name rides the wire
## (S2C 0x7B), so when the client learns it we load that mission's terrain +
## environment; meanwhile NetWorldView renders the decoded .3di models each frame.
## Only the resource dir + the endpoint are needed.
## opts: { dir, loose (bool), replay_host, replay_port, items (optional items.def
## path override), camera (Camera3D for the spectator overview) }.
func load_net_session(opts: Dictionary) -> int:
	# Resource root: a `loose` dir (a flat extract — e.g. an authored probe folder)
	# mounts via set_root_dir; otherwise the normal PFF-install resolution.
	var resource_root: NovaResourceRoot
	var dir := String(opts.get("dir", ""))
	if bool(opts.get("loose", false)) and not dir.is_empty():
		resource_root = NovaResourceRoot.new()
		resource_root.set_root_dir(dir)
		set_resource_root(resource_root)
	else:
		resource_root = _resolve_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	_clear_mission_tile_info()
	_resource_root = resource_root

	# Item database for BOTH the §5.10b wire dispatch-class table and model
	# resolution. Normally resolved from the mounted root; an explicit `items` path
	# overrides it (e.g. when a probe's items.def lives outside the install).
	var item_db := NovaItemDatabase.new()
	var items_path := String(opts.get("items", ""))
	var item_err := item_db.load(items_path) if not items_path.is_empty() \
		else item_db.load_from_resource_root(resource_root, "items.def")
	if item_err != OK:
		push_warning("net session: items.def not loaded (%s) — entities won't resolve" % item_db.get_last_error())

	var resolver = NovaModelResolver.new()
	resolver.setup(resource_root, item_db)

	# The in-match spectator client (replay vs real differ only by the endpoint).
	_net_client = NovaNetClient.new()
	_net_client.name = "NovaNetClient"
	_net_client.set_item_database(item_db)
	_net_client.replay_host = String(opts.get("replay_host", "127.0.0.1"))
	_net_client.replay_port = int(opts.get("replay_port", 42000))
	_net_client.mission_known.connect(_on_net_mission)
	add_child(_net_client)

	var container := Node3D.new()
	container.name = NET_CONTAINER_NAME
	add_child(container)

	_net_view = NetWorldView.new()
	_net_view.name = "NetWorldView"
	_net_view.setup(_net_client, resolver, container, _env, opts.get("camera", null))
	add_child(_net_view)

	# Draw the decoded event stream (fire / hits / kills / capture zones) over the
	# rendered world — the 3D replacement for the standalone viewer's 2D markers.
	_net_event_view = NetEventView.new()
	_net_event_view.name = "NetEventView"
	_net_event_view.setup(_net_client)
	add_child(_net_event_view)

	_net_client.connect_to_replay()
	_loaded = true
	_debug_views.on_loaded()
	world_loaded.emit()
	return OK


# The map name arrived on the wire (S2C 0x7B). Load that mission's terrain +
# environment so the streamed entities have ground to stand on. Entities are NOT
# placed from the .bms and the AI sim never runs — they come from the wire.
func _on_net_mission(mission_name: String) -> void:
	if _loaded_mission != null or _resource_root == null:
		return
	# Wire-selected missions use the same witnessed archive-only BMS path.
	# [orig: Mission_LoadBMSFromPFF @ 0x40d43c]
	if not _resource_root.has_file(
			mission_name, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY):
		push_warning("net session: map '%s' (from the wire) not in %s" % [mission_name, _resource_root.get_root_dir()])
		return
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(
			_resource_root, mission_name, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY) != OK:
		push_warning("net session: failed to parse %s: %s" % [mission_name, mission.get_last_error()])
		return
	# A wire map is a small transaction over both required render resources.
	# Keep retained consumers dormant and the BMS retryable until ENV + TRN have
	# both loaded; otherwise a valid terrain could resurrect stale atmosphere.
	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	_load_mission_tile_info(mission_name, _resource_root)
	var env_name := mission.get_environment_ref() + ".env"
	if not _resource_root.has_file(env_name) or not _load_environment(env_name):
		push_warning("net session: environment %s.env not loaded" % mission.get_environment_ref())
		return
	var trn := mission.get_terrain_ref() + ".trn"
	if not _resource_root.has_file(trn) or not _load_terrain(trn):
		push_warning("net session: terrain %s.trn not loaded" % mission.get_terrain_ref())
		return
	# EnvFile overrides and the distinct BMS water-height rung commit together
	# only after the complete map is renderable. A partial load publishes neither.
	_apply_mission_environment_overrides(mission)
	_loaded_mission = mission
	_mission_forces_indoors = (int(mission.get_info().get("attrib_flags", 0)) & NovaMissionData.ATTRIB_FORCE_INDOORS) != 0
	_prepare_autonomous_weather()
	_set_water_world_rendering_enabled(true)
	print_verbose("GameWorld(net): map %s -> terrain %s loaded" % [mission_name, mission.get_terrain_ref()])


# The ONE mission path — the file entry (load_mission) and the in-memory
# entry (load_mission_data) converge here: resolve the header's terrain +
# environment from `resource_root`, apply the mission's env overrides, build the
# world, place objects, start the runtime + audio.
func _load_mission_internal(mission: NovaMissionData, bms_name: String, resource_root: NovaResourceRoot) -> int:
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
	_resource_root = resource_root
	# Game_StartMission destroys the previous shared .3DI definition cache before
	# reloading this mission's render resources. Reset before environment/terrain:
	# NovaCelestial resolves its models from _load_environment, and foliage loaded
	# by terrain must remain present-but-excluded in the same generation.
	# [orig: sub_5B5710 @0x524A6F]
	NovaObjectData.reset_network_challenge_model_registry()
	_load_mission_tile_info(bms_name, resource_root)
	# Progress values are anchor points from the witnessed schedule (2..100);
	# our pipeline has fewer stages than the original's ~30 call sites, so each
	# boundary reports the nearest witnessed value
	# (docs/interface/loading-screen-re.md D-LOADSCR-1)
	# [orig: Game_StartMission @ 0x524360 progress schedule
	# 2,3,4,6,20,26,...,41,45,50,60,70,90,95,100].
	load_progress.emit(2)
	timeline.span("environment")
	if not _load_environment(env_name):
		load_failed.emit("failed to load %s" % env_name)
		timeline.finish()
		return ERR_CANT_OPEN
	_apply_mission_environment_overrides(mission)
	timeline.end_span()
	load_progress.emit(6)
	timeline.span("terrain")
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		timeline.finish()
		return ERR_CANT_OPEN
	timeline.end_span()
	load_progress.emit(26)

	_loaded_mission = mission
	# The mission attribute that forces the indoors accum bit every frame.
	# [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8]
	_mission_forces_indoors = (int(mission.get_info().get("attrib_flags", 0)) & NovaMissionData.ATTRIB_FORCE_INDOORS) != 0
	timeline.span("objects")
	_place_mission_objects(mission, timeline)
	timeline.end_span()
	load_progress.emit(41)
	timeline.span("runtime")
	var runtime_error := _start_runtime(mission, bms_name)
	timeline.end_span()
	if runtime_error != OK:
		timeline.finish()
		unload()
		return runtime_error
	# Retail freezes its non-foliage loaded-.3DI page once, after the entity,
	# celestial, HUD, and renderer resource loads and before the loading screen
	# drops. MissionRuntime.setup has now resolved the placed/wire mission models
	# (including collision/husk definitions); late network spawns must not change
	# this page. [orig: sub_5B3A80 @0x5871CF from Game_StartMission @0x525A6E]
	var challenge_sim: NovaSimulation = _runtime.get_sim()
	if challenge_sim != null and challenge_sim.is_joiner():
		_prewarm_loaded_model_challenge_definitions()
	if challenge_sim != null:
		challenge_sim.finalize_loaded_model_challenge_snapshot()
	load_progress.emit(70)
	timeline.span("audio")
	_start_mission_audio(mission, bms_name)
	timeline.end_span()
	_prepare_world_driven_weather()
	load_progress.emit(90)
	timeline.span("effects")
	_start_effect_world()
	timeline.end_span()
	# Warm the effect catalog while the loading screen still covers the frame:
	# the first live spawn otherwise pays the deferred texture resolves + the
	# renderer's first-draw pipeline compiles as a ~90 ms hitch on the player's
	# first shot (measured: first-fire tap 92.9 ms -> repeat 12.5 ms). Retail
	# pays this at load [orig: CEffectSystem_Init @ 0x5f6070 loads every .ptl
	# and its textures at Game_StartMission].
	timeline.span("effects_warm")
	_warm_effect_world_catalog()
	timeline.end_span()
	load_progress.emit(95)
	timeline.finish()
	_loaded_mission_file = bms_name
	_loaded = true
	_set_water_world_rendering_enabled(true)
	load_progress.emit(100)
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
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var resource_root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if resource_root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		load_failed.emit(resource_root.get_last_error())
		return null
	return resource_root


# Populate the world with the mission's placed objects under a MissionObjects node.
# Shares the shell-agnostic placer with the editor Mission workspace.
func _place_mission_objects(mission: NovaMissionData, timeline: PerfTimeline = null) -> void:
	if _resource_root == null or mission == null:
		return
	_placer = MissionObjectPlacer.new(_resource_root)
	_panm_clock.sample_frame()
	_placer.set_panm_clock(_panm_clock)
	var options := { "environment_node": _env }
	# A joiner places the mission like any other client of it — the retail client
	# loads and renders its local .bms through the normal pipeline, applying net
	# state on top — MINUS the organics: players and streamed AI have no stable
	# .bms identity on the wire and render wire-direct. Placed pools 1-3 share the
	# host's pool/slot handle space (promote order mirrors Mission_LoadBMSFile
	# @0x40f4e0 on both sides), so the wire present pass defers their rows onto
	# these placed nodes by identity, restoring MultiMesh batching, occlusion,
	# and registry resolution to the joiner.
	if _net_drive.is_join_pending():
		options["skip_kinds"] = [NovaMissionData.KIND_ORGANIC]
	if timeline != null:
		options["timeline"] = timeline
	# Pulse the load-progress screen from inside the model-load loop at the
	# stage's constant value — the original re-presents its loading screen the
	# same way, with a constant percentage from within the per-model loops
	# [orig: the paired constant-value LoadingScreen_UpdateAndPresent calls
	# inside Game_StartMission's model loops @ 0x524d9c/0x524e09, 0x524f32/0x524fe0].
	options["progress"] = func() -> void: load_progress.emit(26)
	_mission_stats = _placer.place(mission, self, options)
	print_verbose("GameWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
		int(_mission_stats.get("placed", 0)),
		int(_mission_stats.get("batched", 0)),
		int(_mission_stats.get("animated", 0)),
		int(_mission_stats.get("unresolved", 0)),
		int(_mission_stats.get("markers", 0)),
	])


func get_loaded_mission() -> NovaMissionData:
	return _loaded_mission


func get_loaded_mission_file() -> String:
	return _loaded_mission_file


## The active net spectator client (NovaNetClient), or null outside a net session.
## Hosts use it to drive a kill-feed / event HUD off the same decoded stream.
func get_net_client():
	return _net_client


func get_sim() -> NovaSimulation:
	return _runtime.get_sim() if _runtime != null else null


## The mounted world's shared weapon.def database. ArmoryPresenter consumes this on
## first open so its canonical parent tuples and its visible rows resolve against
## the same catalog; the FP viewmodel reuses it below (ADR 0018 resource seam).
func get_weapon_database() -> NovaWeaponDatabase:
	if _weapon_db == null:
		if _resource_root == null:
			return null
		_weapon_db = NovaWeaponDatabase.new()
		if _weapon_db.load_from_resource_root(_resource_root, "weapon.def") != OK:
			push_warning("GameWorld: weapon.def unavailable (%s) — weapon presentation/loadout lookup disabled"
					% _weapon_db.get_last_error())
			return null
	return _weapon_db if _weapon_db.is_loaded() else null


func get_runtime():
	return _runtime


func get_mission_stats() -> Dictionary:
	return _mission_stats


## Tear down a loaded world so the host can return to the menu (or load a
## different mission) without the previous world lingering. Frees the dynamically
## placed MissionObjects subtree and resets the load state; the terrain /
## environment scene nodes are kept in place and rebuilt by the next load_*().
## Safe to call when nothing is loaded.
func unload() -> void:
	_loaded = false
	_stop_water_render_stats()
	# Net-session teardown: the preload sim/root, the notification latches, the
	# typed request staging, and the NovaWorld gate registration.
	_net_drive.reset()
	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	_local_player_spawn_loadout = {}
	_clear_mission_tile_info()
	_restore_idle_frame_clear_color()
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	if container != null:
		container.queue_free()
	# Per-item attached-effect owner keys reference nodes in that container —
	# never let a reload's provider resolve against freed instances.
	_item_fx.reset()
	# Debug-view teardown: the retain/free split (user-point re-arm vs freed
	# overlays vs the deliberately surviving particle/pick stack) lives in the set.
	_debug_views.on_unload()
	# Net session teardown (no-ops for a normal mission).
	if _net_event_view != null:
		_net_event_view.queue_free()
	if _net_view != null:
		_net_view.queue_free()
	if _net_client != null:
		_net_client.stop()
		_net_client.queue_free()
	var net_container := get_node_or_null(NodePath(NET_CONTAINER_NAME))
	if net_container != null:
		net_container.queue_free()
	_net_event_view = null
	_net_view = null
	_net_client = null
	if _mission_audio != null:
		_mission_audio.teardown()
	# Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	# The game shell re-opens menu music on its return to the front end.
	NovaMusicService.stop_context()
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
		_runtime.queue_free()  # frees its off-tree sim too (MissionRuntime._exit_tree)
	_runtime = null
	if _effect_world != null:
		_effect_world.queue_free()
		_effect_world = null
	_mission_audio = null
	_placer = null
	_weapon_db = null  # re-resolves against the next load's mounted root
	_local_weapon_dict = {}
	_local_weapon_preserve_slot_state = false
	# Armory selections belong to the entity from the mission being torn down.
	# A new spawn must resolve from its own equipped AdmDef instead of inheriting
	# either the previous mission's override or its authored NONE state.
	_viewmodel_weapon_override = ""
	_viewmodel_weapon_cleared = false
	_mission_stats = {}


func _load_environment(env_path: String) -> bool:
	if _env == null:
		return true
	var env := EnvFile.new()
	if env.load_from_resource_root(_resource_root, env_path) != OK:
		push_warning("GameWorld: failed to load environment '%s'" % env_path)
		return false
	# NovaEnvironment's setter reloads + pushes shader globals on assignment.
	_env.environment_data = env
	# GameWorld retains one NovaWeather node across loads. A replacement ENV is
	# a discrete state change: retail snaps every color block to the new mission
	# targets instead of easing over from the previous mission's currents.
	var weather := get_node_or_null("NovaWeather")
	if weather != null and weather.has_method("resync_colors"):
		weather.resync_colors()
	var celestial := get_node_or_null("NovaCelestial")
	if celestial != null and celestial.has_method("set_resource_root"):
		celestial.set_resource_root(_resource_root)
	return true


## Apply the mission's attrib-gated water/fog overrides onto the loaded env via
## EnvFile's non-persistent override layer [orig: Game_LoadTerrainDuringConnect
## @ 0x520710]. The base .env is never mutated.
func _apply_mission_environment_overrides(mission: NovaMissionData) -> void:
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
	if overrides.has("water_height"):
		# Mission header values are signed engine half-units.
		mission_water = float(overrides["water_height"]) * 0.5
	_set_mission_water_height_override(mission_water)


func _set_mission_water_height_override(world_height: float) -> void:
	if _water != null and _water.has_method("set_mission_water_height_override"):
		_water.set_mission_water_height_override(world_height)


func _set_water_world_rendering_enabled(enabled: bool) -> void:
	if _water != null and _water.has_method("set_world_rendering_enabled"):
		_water.set_world_rendering_enabled(enabled)


# Runtime water exposes a render-aware predicate so its retained authored
# height cannot leak into frame clear/occlusion while a load is absent or in
# progress. Keep the height-only fallback for compatible test/host doubles.
func is_water_render_active() -> bool:
	if _water == null:
		return false
	if _water.has_method("is_water_render_active"):
		return bool(_water.is_water_render_active())
	return not _water.has_method("is_water_active") or bool(_water.is_water_active())


func _set_weather_world_tick_driven(enabled: bool) -> void:
	_weather_tick_credit = 0.0
	var weather := get_node_or_null("NovaWeather")
	if weather != null and weather.has_method("set_world_tick_driven"):
		weather.set_world_tick_driven(enabled)


func _prepare_world_driven_weather() -> void:
	_weather_tick_credit = 0.0
	var weather := get_node_or_null("NovaWeather")
	if weather != null and weather.has_method("prepare_world_driven"):
		weather.prepare_world_driven()
	else:
		_set_weather_world_tick_driven(true)


func _prepare_autonomous_weather() -> void:
	_weather_tick_credit = 0.0
	var weather := get_node_or_null('NovaWeather')
	if weather != null and weather.has_method('prepare_autonomous'):
		weather.prepare_autonomous()
	else:
		_set_weather_world_tick_driven(false)


func _advance_world_driven_weather(delta: float) -> void:
	_weather_tick_credit += maxf(delta, 0.0) * WEATHER_TICK_HZ
	var tick_count := int(floor(_weather_tick_credit + 1.0e-9))
	if tick_count <= 0:
		return
	_weather_tick_credit = maxf(
			0.0, _weather_tick_credit - float(tick_count))
	if tick_count > MAX_WEATHER_CATCHUP_TICKS:
		tick_count = MAX_WEATHER_CATCHUP_TICKS
		_weather_tick_credit = 0.0
	var weather := get_node_or_null("NovaWeather")
	for _tick in range(tick_count):
		_env.advance_mission_clock(1)
		if weather != null and weather.has_method("tick_fixed"):
			weather.tick_fixed()


# Retail loads <mission>.til into one shared g_TerrainTileArray used by
# terrain overlays/surface overrides, network initial state, and both foliage
# generators' radius-2 blocker.
# Its file probe/read force loose-first around this one load.
# [orig: Terrain_LoadFoliageFile @ 0x60a740, policy force @ 0x60a74e;
# Terrain_GetSurfaceTypeAtPosition @ 0x606510;
# Foliage_PathBlockedByPlacedTile @ 0x606490]
func _load_mission_tile_info(bms_name: String, resource_root: NovaResourceRoot) -> void:
	_clear_mission_tile_info()
	if resource_root == null:
		return
	var mission_name := bms_name.get_file()
	if mission_name.is_empty():
		mission_name = bms_name
	var til_name := mission_name.get_basename() + ".til"
	if not resource_root.has_file(
			til_name, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return
	var til_bytes := resource_root.read_file(
			til_name, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
	if til_bytes.is_empty():
		return
	var tile_info := NovaTerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		push_warning("GameWorld: failed to parse mission tile file '%s'." % til_name)
		return
	_mission_tile_info = tile_info
	_mission_til_bytes = til_bytes


func _clear_mission_tile_info() -> void:
	_mission_tile_info = null
	_mission_til_bytes = PackedByteArray()
	if _terrain != null:
		_terrain.tile_info_override = null
	if _dispatcher != null:
		_dispatcher.tile_info = null


func _load_terrain(trn_path: String) -> bool:
	var data := NovaTerrainData.new()
	if data.load_from_resource_root(_resource_root, trn_path) != OK:
		return false
	var tile_info := _mission_tile_info
	if tile_info == null and _tile_overlay != null:
		tile_info = _tile_overlay.tile_info
	_terrain.tile_info_override = tile_info
	_terrain_data = data
	_terrain.terrain_data = data
	_terrain.build()
	if _water != null:
		_water.set("terrain_data", data)
	var celestial_node := get_node_or_null("NovaCelestial")
	if celestial_node != null:
		# The glare occlusion rays march this terrain (env #14).
		celestial_node.set("terrain_data", data)
	_configure_foliage()
	return true


# Runtime foliage: NovaTerrain supplies the retail 16-unit detail-cell set;
# the sim's crouched/prone infantry supply the distant silhouette anchors
# (see tick()). Sampling and deterministic candidate generation stay in the
# fresh native runtime.
func _configure_foliage() -> void:
	if _dispatcher == null or _terrain_data == null:
		return
	# The runtime source already supplies height, detail/model foliage indices,
	# colormap, and change invalidation. Binding the same NovaTerrainData again as
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
	if _tile_overlay != null:
		# NovaTerrain composites the tile overlay into its own material; the scene
		# TileOverlay node is only the authoring fallback selected before build.
		_tile_overlay.clear()
		_tile_overlay.visible = false


func get_terrain_data() -> NovaTerrainData:
	return _terrain_data


func get_resource_root() -> NovaResourceRoot:
	return _resource_root


func is_loaded() -> bool:
	return _loaded


func get_current_frame_clear_color() -> Color:
	if _clear_color == null or _clear_color.environment == null:
		return Color.BLACK
	return _clear_color.environment.background_color


func _sample_panm_clock() -> void:
	_panm_clock.sample_frame()
	if _runtime != null and _runtime.has_method("set_presentation_time_ms"):
		_runtime.set_presentation_time_ms(_panm_clock.time_ms)


## The host per-frame order, faithful to the original main loop's server-tick-then-client-render:
## foliage coverage around the viewer, then the mission runtime (MissionRuntime.tick advances the
## logic at the 62-frame cadence, presents entity state onto the placed nodes, and drains side
## effects), then the audio render pass. Effects come back through MissionRuntime.effects_drained.
var _perf_probe_enabled := false
var _perf_probe_spans: Dictionary = {}
var _perf_probe_skip_occl := false
var _perf_probe_skip_effect_tick := false
var _perf_probe_skip_fixed_handlers := false
var _perf_probe_occlusion_skipped := false

# The shared F3 frame-stats board (null outside the game shell). Feeds gate on
# board capture so a closed Stats tab costs nothing; the occlusion split spans
# land from OcclusionFramePass.apply_frame, the tick legs from tick() below.
var _frame_stats: FrameStatsBoard = null
# Weakref edge latch for measured render time on the water reflection RTT.
var _stats_water_vp_ref: WeakRef = null


## The game shell hands its FrameStatsBoard here; the world re-hands it to
## every MissionRuntime it creates and feeds its own tick legs.
func set_frame_stats_board(board: FrameStatsBoard) -> void:
	if board == _frame_stats:
		return
	if _frame_stats != null:
		var old_capture_changed := Callable(self, "_on_frame_stats_capture_changed")
		if _frame_stats.capture_changed.is_connected(old_capture_changed):
			_frame_stats.capture_changed.disconnect(old_capture_changed)
	_stop_water_render_stats()
	_frame_stats = board
	if _frame_stats != null:
		var capture_changed := Callable(self, "_on_frame_stats_capture_changed")
		if not _frame_stats.capture_changed.is_connected(capture_changed):
			_frame_stats.capture_changed.connect(capture_changed)
	_occlusion.set_frame_stats_board(board)
	if _runtime != null and _runtime.has_method("set_frame_stats_board"):
		_runtime.set_frame_stats_board(board)


func _on_frame_stats_capture_changed(active: bool) -> void:
	if not active:
		_stop_water_render_stats()


func is_water_render_stats_measured() -> bool:
	return _stats_water_vp_ref != null \
			and is_instance_valid(_stats_water_vp_ref.get_ref())


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


func tick(camera_pos: Vector3, camera_xform: Transform3D = Transform3D(), delta: float = TICK_DT) -> void:
	_sample_panm_clock()
	var probe_enabled := _perf_probe_enabled
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	# One shared gate for the per-leg clock reads: the manual A/B probe and the
	# F3 Stats capture consume the same measurements.
	var timing := probe_enabled or stats_on
	var skip_occlusion := probe_enabled and _perf_probe_skip_occl
	if probe_enabled:
		_perf_probe_spans.clear()
	var tick_start := Time.get_ticks_usec()
	_last_tick_camera_pos = camera_pos  # the fire present pass's listener (audio-tick source)
	var foliage_start := tick_start
	_perf_foliage_us = 0
	_perf_runtime_us = 0
	_perf_audio_us = 0
	if _loaded and _dispatcher != null:
		# The silhouette tier is the hide-in-grass mechanic: retail's sector-entity
		# walk generates model foliage only around CROUCHED/PRONE infantry standing
		# on terrain — never around placed objects, whose MoveOrder stays 0
		# [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
		# (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7].
		var silhouette_anchors := PackedVector3Array()
		if _runtime != null and _runtime.has_method("get_sim"):
			var anchor_sim = _runtime.get_sim()
			if anchor_sim != null and anchor_sim.has_method("get_foliage_mask_anchor_positions"):
				silhouette_anchors = anchor_sim.get_foliage_mask_anchor_positions()
		_dispatcher.silhouette_anchors = silhouette_anchors
		_dispatcher.render_frame(camera_xform)
		_perf_foliage_us = Time.get_ticks_usec() - foliage_start
	var runtime_start := Time.get_ticks_usec()
	var runtime_ticks := 0
	# Occlusion no longer restores-then-rehides per frame: the apply below is
	# diff-based and the present pass consults the shared occlusion-hidden set,
	# so steady verdicts leave nodes untouched. Only the A/B seam edges do bulk
	# work: entering the skip releases every occlusion override (mission
	# blink/indoors semantics remain authoritative; iris keeps sampling below),
	# leaving it re-arms a full re-emit from the sim's delta baseline.
	if probe_enabled and _loaded:
		if skip_occlusion != _perf_probe_occlusion_skipped:
			if skip_occlusion:
				_occlusion.enter_probe_skip()
			else:
				_occlusion.leave_probe_skip()
		_perf_probe_occlusion_skipped = skip_occlusion
	elif probe_enabled:
		_perf_probe_occlusion_skipped = false
	var probe_phase_start := 0
	# Gate on the runtime transport so MissionRuntime._playing is THE play flag
	# for the real game: F3 and runtime MCP Pause/Step share this public flag.
	# _start_runtime calls play(), so normal missions run exactly as before.
	if _loaded and _runtime != null and _runtime.is_playing():
		# Fixed-timestep accumulator: the sim runs at a constant 62.5 Hz regardless of render rate.
		# Guard keeps the duck-typed test stubs (game_world_test.gd) that only implement tick() green.
		if _runtime.has_method("tick_realtime"):
			runtime_ticks = int(_runtime.tick_realtime(delta))
		else:
			runtime_ticks = 1 if bool(_runtime.tick()) else 0
		_perf_runtime_us = Time.get_ticks_usec() - runtime_start
		# Net-session edges (admission/deploy/loss) + the gate's occupancy report.
		_net_drive.observe_tick(_runtime)
	# Weather/TOD is a distinct 62 Hz fixed clock; the mission simulation above
	# remains 62.5 Hz. Each weather quantum advances integer fixed24 time, which
	# recomputes TOD targets, then ticks every weather block exactly once
	# [orig: Environment_UpdateWeatherTick @ 0x57e9b0].
	probe_phase_start = Time.get_ticks_usec() if timing else 0
	if (_loaded and _runtime != null and _runtime.is_playing()
			and _env != null):
		_advance_world_driven_weather(delta)
	if timing:
		var weather_us := Time.get_ticks_usec() - probe_phase_start
		if probe_enabled:
			_perf_probe_spans["weather"] = weather_us
		if stats_on:
			_frame_stats.add(FrameStatsBoard.WORLD_WEATHER, weather_us)
	# Blink flags only change on sim ticks; re-apply the frame gates then.
	probe_phase_start = Time.get_ticks_usec() if timing else 0
	if _loaded and runtime_ticks > 0:
		_occlusion.apply_blink_gates(_mission_forces_indoors)
	if timing:
		var blink_us := Time.get_ticks_usec() - probe_phase_start
		if probe_enabled:
			_perf_probe_spans["blink"] = blink_us
		if stats_on:
			_frame_stats.add(FrameStatsBoard.WORLD_BLINK, blink_us)
	# The render-occlusion frame is camera-driven: it runs every render frame
	# (retail collects visible entities per scene render, not per sim tick).
	# [orig: Terrain_CollectVisibleEntities @ 0x5c9160 from
	# Terrain_RenderSceneWithReflection @ 0x5c94f0]
	if _loaded:
		probe_phase_start = Time.get_ticks_usec() if timing else 0
		if not skip_occlusion:
			_occlusion.apply_frame(camera_xform, _mission_forces_indoors)
		if probe_enabled:
			_perf_probe_spans["occl_frame"] = (0 if skip_occlusion
					else Time.get_ticks_usec() - probe_phase_start)
		probe_phase_start = Time.get_ticks_usec() if timing else 0
		_stamp_iris_samples(camera_xform)
		if timing:
			var iris_us := Time.get_ticks_usec() - probe_phase_start
			if probe_enabled:
				_perf_probe_spans["iris"] = iris_us
			if stats_on:
				_frame_stats.add(FrameStatsBoard.WORLD_IRIS, iris_us)
	elif probe_enabled:
		_perf_probe_spans["occl_frame"] = 0
		_perf_probe_spans["iris"] = 0
	var audio_start := Time.get_ticks_usec()
	if _loaded and _mission_audio != null:
		# Ambient soundloop regions read that same clock [orig:
		# Entity_CalcTimeOfDayRegion @ 0x408110].
		if _env != null and _env.get("time_of_day") != null:
			_mission_audio.set_time_of_day_hhmm(float(_env.get("time_of_day")))
		# Marker eval/registration rides the sim's logic-tick clock — the witnessed
		# pool-2 stagger [orig: Entity_UpdateAllEntities @ 0x4c225a]; the per-frame
		# call below is only the live-slot mix + voice binds [orig:
		# SoundEmitter_UpdateAndMixTop8 @ 0x521341]. A host with no ticking runtime
		# (editor idle) free-runs the eval clock off render delta instead.
		if runtime_ticks > 0 and _runtime != null and _runtime.has_method("get_sim"):
			var audio_sim = _runtime.get_sim()
			if audio_sim != null and audio_sim.has_method("get_logic_tick"):
				_mission_audio.advance_ticks(int(audio_sim.get_logic_tick()))
		_mission_audio.tick(camera_pos, delta)
		_music_var_pump()
		_perf_audio_us = Time.get_ticks_usec() - audio_start
	_perf_tick_us = Time.get_ticks_usec() - tick_start
	if stats_on:
		_frame_stats.add(FrameStatsBoard.WORLD_FOLIAGE, _perf_foliage_us)
		_frame_stats.add(FrameStatsBoard.WORLD_RUNTIME, _perf_runtime_us)
		_frame_stats.add(FrameStatsBoard.WORLD_AUDIO, _perf_audio_us)
	_sample_water_render_stats(stats_on)


# Water-reflection RTT sampling for the Stats tab: flip measured render time on
# the reflection SubViewport only while the tab captures, then land the
# previous frame's CPU/GPU times on the board. Weakref-latched so a freed
# viewport never sees a stale-RID RenderingServer call.
func _sample_water_render_stats(stats_on: bool) -> void:
	var viewport: SubViewport = null
	if stats_on and _water != null:
		var viewport_v: Variant = _water.get("reflection_viewport")
		if viewport_v is SubViewport and is_instance_valid(viewport_v):
			viewport = viewport_v
	var previous: Object = _stats_water_vp_ref.get_ref() if _stats_water_vp_ref != null else null
	if previous != viewport:
		if previous is SubViewport:
			RenderingServer.viewport_set_measure_render_time(
					(previous as SubViewport).get_viewport_rid(), false)
		_stats_water_vp_ref = weakref(viewport) if viewport != null else null
		if viewport != null:
			RenderingServer.viewport_set_measure_render_time(
					viewport.get_viewport_rid(), true)
	if viewport == null:
		return
	var rid := viewport.get_viewport_rid()
	_frame_stats.add(FrameStatsBoard.RENDER_WATER_CPU,
			int(RenderingServer.viewport_get_measured_render_time_cpu(rid) * 1000.0))
	_frame_stats.add(FrameStatsBoard.RENDER_WATER_GPU,
			int(RenderingServer.viewport_get_measured_render_time_gpu(rid) * 1000.0))


func _stop_water_render_stats() -> void:
	var previous: Object = (
			_stats_water_vp_ref.get_ref() if _stats_water_vp_ref != null else null)
	if previous is SubViewport:
		RenderingServer.viewport_set_measure_render_time(
				(previous as SubViewport).get_viewport_rid(), false)
	_stats_water_vp_ref = null


func _sync_runtime_profiling() -> void:
	if _runtime != null and _runtime.has_method(
			"set_runtime_profiling_enabled"):
		_runtime.set_runtime_profiling_enabled(_perf_probe_enabled)


func get_runtime_perf_counters() -> Dictionary:
	return {
		"tick_us": _perf_tick_us,
		"foliage_us": _perf_foliage_us,
		"runtime_us": _perf_runtime_us,
		"audio_us": _perf_audio_us,
		"runtime": _runtime.get_perf_counters() if _runtime != null and _runtime.has_method("get_perf_counters") else {},
		"foliage": _dispatcher.get_frame_stats() if _dispatcher != null else {},
		"audio": _mission_audio.get_perf_counters() if _mission_audio != null else {},
	}


# Listener position for the fire present pass — the same camera position the audio
# render pass ticks with (INF until the first tick).
var _last_tick_camera_pos := Vector3.INF


func _fire_listener_position() -> Vector3:
	return _last_tick_camera_pos


# Fire-presentation counters (probe/diagnostic seam; empty until a mission runs).
func get_fire_present_stats() -> Dictionary:
	return _runtime.get_fire_present_stats() if _runtime != null and _runtime.has_method("get_fire_present_stats") else {}


# Destruction-presentation counters (DestructionPresentPass.Stats, typed per
# ADR 0017; null until a host mission runs with the pass).
func get_destruction_present_stats() -> RefCounted:
	return _runtime.get_destruction_present_stats() if _runtime != null and _runtime.has_method("get_destruction_present_stats") else null


## Build a host-managed avatar model for the local player (which has no BMS placement of its
## own). The caller (LocalPlayerPresenter) positions it and swaps its visual layer per first/third
## person: in first person the body stays renderable on the reflection-only layer, because the
## witnessed water mirror re-renders the world scene, local body included
## [orig: Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene @ 0x5c1240]. Null
## when the resource root / item graphic is unavailable. 0x14B9 = player infantry [net-re §5.2b].
## The soldier's THIRD-PERSON gun. Built as a SIBLING of the avatar rather than a child:
## NovaObjectModel.rebuild() frees all of its children, so a weapon parented under the
## avatar would silently vanish whenever the body model rebuilds. It carries no skeleton
## and no clip — the original stamps ONE matrix into every bone slot of this model, i.e.
## it is drawn rigid, posed entirely by its attach basis.
## [orig: BoneCallback_org0_World draw 5 @0x4e3c87..0x4e3d99; model = WeaponDef.tpModel
##  (+0x170, weapon.def gfx3) @0x4e3cd3]
func build_local_player_held_weapon(graphic: String) -> Node3D:
	if _placer == null or graphic.is_empty():
		return null
	var model: Node3D = _placer.build_model_from_graphic(
			graphic, "", self, "", _env)
	if model != null:
		model.set_shadow_caster_enabled(true)
	return model


func build_local_player_avatar() -> Node3D:
	if _placer == null:
		return null
	# _env wires the TOD-reactive lighting/fog stamp — without it the avatar
	# freezes at the noon preview defaults (retail relights every entity per
	# frame [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]).
	return _placer.build_player_animated_model(0x14B9, self, _env)


# Resolve the .3DI definitions that LocalPlayerPresenter would otherwise load only on
# its first visible frame. Retail's Game_ReloadEntityModelsAndCallbacks and HUD
# model pass load the player + current weapon overlay before sub_5B3A80 freezes
# the C2S 0x3D source; doing the lightweight data lookup here gives our snapshot
# the same boundary without constructing hidden scene nodes. Later builders hit
# the placer's cache, so they cannot introduce a definition just after freeze.
func _prewarm_loaded_model_challenge_definitions() -> void:
	if _placer == null:
		return
	var visual_item_id := int(_placer.resolve_player_visual_item_id(0x14B9))
	var avatar_graphic := String(_placer.graphic_for(visual_item_id))
	if not avatar_graphic.is_empty():
		_placer.object_data_for(avatar_graphic)

	if _viewmodel_weapon_cleared:
		return
	var def := local_player_viewmodel_def()
	var gun_name := def.gfx1 if def != null else "ak47_1st"
	var arms_name := "armsG"
	if def != null and not def.gfx1a.is_empty():
		arms_name = def.gfx1a
	var show_arms := def == null or (def.flags & NovaWeaponDatabase.FLAG_EMPLACED) == 0
	if not gun_name.is_empty():
		_placer.object_data_for(gun_name)
	if show_arms and not arms_name.is_empty():
		_placer.object_data_for(arms_name)


## Build a host-managed FIRST-PERSON weapon viewmodel for the local player (shown in 1st person; the
## inverse of the 3rd-person avatar). Faithful composition: the equipped weapon's FP gun model PLUS
## the character arms, sharing one skeleton [orig: Player_RenderFirstPersonViewModel @0x4ded60 draws
## the weapon FP model + arms with shared bone matrices]. The models come from the mounted root's
## weapon.def — gfx1 (gun), gfx1a (arms; gfx1b alternate skin unused until team/skin selection),
## animadm (the shared animation set) [orig: WeaponDef_ParseProperty @0x54d730 rows] — for the
## DEFAULT_VIEWMODEL_WEAPON entry until the player's equipped weapon resolves it per-weapon
## (NOVA_VM_WEAPON overrides the name for rig A/B checks). The witnessed JOX values stay as the
## no-def fallback. Camera sway / fire-kick / ADS [orig: Player_UpdateFirstPersonCamera @0x4dd380]
## are follow-ups. Null when the placer or both models fail to resolve.
const DEFAULT_VIEWMODEL_WEAPON := "WPN_AK47AUTO"

# The armory-equipped weapon name; overrides DEFAULT_VIEWMODEL_WEAPON/env once the
# player accepts a loadout [orig: the equipped AdmDef drives the FP model pick,
# Player_RenderFirstPersonViewModel @0x4ded60 via the mounted slot].
var _viewmodel_weapon_override := ""
# NONE is distinct from the pre-armory empty override, which falls back to the
# witnessed bring-up default until an equipped weapon is resolved.
var _viewmodel_weapon_cleared := false
# A UseGun presentation rebuild follows a slot-pointer commit that has already
# selected a persistent parent/personal slot. Both the dict-only install and the
# later ADM-duration rebake must preserve that slot's action/ammo state.
var _local_weapon_preserve_slot_state := false

## Armory apply, host side: point the FP viewmodel + action FSM at `weapon_name`.
## Validates against weapon.def; the caller (main_game) drops the old viewmodel so the
## per-frame pass rebuilds gun/arms/FSM from the new def [orig: the ACCEPT re-mount,
## WeaponLoadout_ApplyFromBuffer @0x565cd0 -> Player_MountWeaponSlot @0x4dfa40].
func set_local_player_weapon_by_name(weapon_name: String,
		preserve_slot_state: bool = false) -> bool:
	if weapon_name.is_empty():
		return false
	var weapon_db := get_weapon_database()
	var index: int = weapon_db.find_weapon(weapon_name) if weapon_db != null else -1
	if index < 0:
		push_warning("GameWorld: armory weapon '%s' not in weapon.def — keeping current" % weapon_name)
		return false
	_viewmodel_weapon_override = weapon_name
	_viewmodel_weapon_cleared = false
	_local_weapon_preserve_slot_state = preserve_slot_state
	# Install the new weapon's FSM on the sim NOW — the mount is not hostage to the FP
	# model load [orig: the ACCEPT chain rebuilds the slot table + mounts with no
	# render dependency — WeaponSlotTable_LoadAllFromDefs @0x5414e0 +
	# Player_MountWeaponSlot @0x4dfa40 (camera/scope/switch-queue state only); the FP
	# model resolve is a separate per-frame consumer @0x4ded60]. Clip lengths bake in
	# again when the rebuilt viewmodel resolves (_setup_local_player_weapon); a model
	# that never loads leaves 'auto' delays collapsed instead of leaving the OLD
	# weapon's FSM live under the new entity stamp.
	_local_weapon_dict = weapon_db.get_weapon(index)
	var sim := get_sim()
	if sim != null:
		_set_local_player_first_person_model_available(false)
		sim.set_local_player_weapon(
				_local_weapon_dict, {}, _local_weapon_preserve_slot_state)
	return true


func _apply_local_player_spawn_loadout() -> void:
	var loadout := _local_player_spawn_loadout
	_local_player_spawn_loadout = {}
	var sim := get_sim()
	if sim == null:
		return
	var has_loadout := false
	for slot_key in ["primary", "secondary", "accessory"]:
		if loadout.has(slot_key):
			has_loadout = true
			break
	if loadout.has("player_class"):
		sim.set_local_player_class(int(loadout.get("player_class", 0)))
	# Mission-authored kits outrank the profile selection. Unlike the inventory
	# itself, this source bit stays false for load_weapon_table's WPN_M4AUTO
	# fallback, so a real default weapon cannot masquerade as mission policy.
	if bool(sim.has_explicit_spawn_loadout()):
		_sync_local_player_weapon_from_inventory(sim)
		return
	if not has_loadout:
		return
	var kit: Array[Dictionary] = []
	for slot_key in ["primary", "secondary", "accessory"]:
		var weapon_name := String(loadout.get(slot_key, ""))
		if weapon_name.is_empty():
			continue
		kit.append({
			"name": weapon_name,
			"ammo_primary": int(loadout.get(slot_key + "_clips", -1)),
			"ammo_secondary": -1,
			"flags": -1,
		})
	if not bool(sim.apply_local_player_loadout(kit, int(loadout.get("player_class", 0)))):
		return
	if kit.is_empty():
		clear_local_player_weapon()
		return
	_sync_local_player_weapon_from_inventory(sim)


func _sync_local_player_weapon_from_inventory(sim: NovaSimulation) -> void:
	var inventory: Dictionary = sim.get_local_player_inventory()
	if not bool(inventory.get("valid", false)):
		return
	var equipped := String(inventory.get("equipped_name", ""))
	# A syntactically nonempty kit can still be rejected by mission/class rules.
	# Keep the presentation aligned with the resulting authoritative inventory.
	if equipped.is_empty():
		clear_local_player_weapon()
		return
	set_local_player_weapon_by_name(equipped)


## Armory NONE: clear the equipped render/FSM state instead of falling back to the
## pre-armory default model on the next frame.
func clear_local_player_weapon() -> void:
	_viewmodel_weapon_override = ""
	_viewmodel_weapon_cleared = true
	_local_weapon_dict = {}
	_local_weapon_preserve_slot_state = false
	var sim := get_sim()
	if sim != null:
		_set_local_player_first_person_model_available(false)
		sim.clear_local_player_weapon()


func _set_local_player_first_person_model_available(available: bool) -> void:
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_first_person_model_available(available)

func build_local_player_viewmodel() -> Node3D:
	if _placer == null:
		_set_local_player_first_person_model_available(false)
		return null
	if _viewmodel_weapon_cleared:
		_set_local_player_first_person_model_available(false)
		return null
	var container := Node3D.new()
	container.name = "PlayerViewmodel"
	add_child(container)
	# anim_wpn_idle = the FP holding pose; without it the arms sit in their bind/T-pose.
	# _env: the viewmodel lights/fogs with the live TOD like every entity
	# (retail draws the FP model through the same lighting constants
	# [orig: Player_RenderFirstPersonViewModel @ 0x4ded60 -> the ctx block]).
	var def := local_player_viewmodel_def()
	# The AK is only the no-definition bring-up fallback. A resolved retail Def
	# with no fpModel intentionally submits no first-person gun.
	var gun_name := def.gfx1 if def != null else "ak47_1st"
	var arms_name := def.gfx1a if def != null and not def.gfx1a.is_empty() else "armsG"
	var adm_name := def.animadm if def != null and not def.animadm.is_empty() else "ak47_1st"
	# Emplaced (Flags 0x80) mounts render their own FP gun but omit the carried
	# character-arms model. [orig: Player_RenderFirstPersonViewModel @0x4dedc7]
	var show_arms := def == null or (def.flags & NovaWeaponDatabase.FLAG_EMPLACED) == 0
	# Both submits reuse the equipped GUN's model table, while `adm_name` supplies the clips.
	# Some valid retail sets differ (M21B_1st: 42 parts, M21_1st: 40); sizing from the ADM
	# basename truncates late animated parts such as the M14 magazine. [orig: @0x4ded60]
	var arms = _placer.build_model_from_graphic(arms_name, adm_name, container,
			"anim_wpn_idle", _env, gun_name) if show_arms else null  # _placer untyped -> no :=
	var gun = _placer.build_model_from_graphic(gun_name, adm_name, container,
			"anim_wpn_idle", _env, gun_name) if not gun_name.is_empty() else null
	_set_local_player_first_person_model_available(gun != null)
	if show_arms and arms == null:
		push_warning("GameWorld: FP arms model '%s' failed to load from the resource root" % arms_name)
	if gun == null and not gun_name.is_empty():
		push_warning("GameWorld: FP gun model '%s' failed to load from the resource root" % gun_name)
	if arms == null and gun == null:
		# A valid definition with no resolved fpModel is a stable, intentionally
		# empty presentation epoch. Returning its container prevents the host from
		# retrying every frame or substituting a different weapon.
		if def == null:
			container.queue_free()
			return null
		return container
	_setup_local_player_weapon(gun if gun != null else arms)
	return container


## Install the equipped weapon's action FSM on the sim: the weapon dict's ACTION rows +
## flags/clipsize/startrounds plus the loaded .adm clip lengths (seconds) the bake turns
## into 62.5 Hz delays [orig: Anim_InitActions @0x541fa0 binds the rows and bakes 'auto'
## delays via Anim_GetDurationTicks @0x53ee10; net-re §5.62]. The arms ride the same
## animadm, so one part's clip table covers both.
func _setup_local_player_weapon(model) -> void:
	var sim := get_sim()
	if sim == null:
		return
	if _local_weapon_dict.is_empty() or model == null or not model.has_method("get_skeletal_anim"):
		sim.clear_local_player_weapon()
		return
	var skeletal = model.get_skeletal_anim()
	var clip_seconds := {}
	if skeletal != null:
		var keys := ["anim_wpn_idle", "anim_wpn_empty_idle"]
		for a in _local_weapon_dict.get("actions", []):
			var k := String(a.get("anim", ""))
			if not k.is_empty() and not keys.has(k):
				keys.append(k)
		for k in keys:
			if skeletal.has_clip(k):
				# EVERY variant's length, .adm file order — the sim seeds its slot
				# rings from these and consumes them serve-then-advance (bake reads
				# and play latches) [orig: the animState slot heads +72;
				# Anim_GetDurationTicks @0x53ee10 / AnimMap_PlayAnimBySlot @0x40bda0].
				clip_seconds[k] = skeletal.get_clip_variant_lengths(k)
	sim.rebake_local_player_weapon(
			_local_weapon_dict, clip_seconds, _local_weapon_preserve_slot_state)


## The installed FP weapon dict's name (empty when none) — the switch-event guard
## against redundant viewmodel reinstalls.
func local_player_weapon_name() -> String:
	return String(_local_weapon_dict.get("name", ""))


## Feed only the first-person-visible NVG state into world lighting. The raw
## active state deliberately survives third person in the simulation.
func set_local_player_nvg_view(active: bool, gain: int) -> void:
	if _env != null and _env.has_method("set_nvg_view"):
		_env.set_nvg_view(active, gain)


## The 62.5 Hz view state (ADS ease, fov policy, 3P anchor), decoded once at this
## edge (ADR 0017); null without a sim.
func local_player_view() -> PlayerLocalView:
	var sim := get_sim()
	if sim == null:
		return null
	return PlayerLocalView.from_view_dict(sim.get_local_player_view())


## The equipped weapon's HUD slice (error table, HUDCLIPGFX/HUDRNDGFX, clipsize, name),
## decoded from NovaWeaponDatabase's transport dict at this edge (ADR 0017) — the HUD
## reads it per frame, mirroring the original HUD info struct's weapon-def pointer
## [orig: HUD_BuildEntityInfo @0x4b8561 -> hudInfo+552]. Null until a weapon resolves.
func local_player_hud_weapon_def() -> PlayerHudWeaponDef:
	return PlayerHudWeaponDef.from_weapon_dict(_local_weapon_dict)


## The equipped-weapon FSM view, decoded once at this edge (ADR 0017); null when no
## weapon FSM is installed.
func local_player_weapon_view() -> PlayerWeaponView:
	var sim := get_sim()
	if sim == null:
		return null
	return PlayerWeaponView.from_state_dict(sim.get_local_player_weapon_state())


## Destructively drain the equipped FSM's ordered presentation batch, decoding the
## C++ transport Dictionaries at this one adapter edge (ADR 0017).
func drain_local_player_weapon_events() -> Array[PlayerWeaponEvent]:
	var out: Array[PlayerWeaponEvent] = []
	# Keep this transport adapter duck-typed like _route_round_impacts so host
	# harnesses can supply value-only drains without weakening get_sim()'s
	# public NovaSimulation contract.
	var sim = (_runtime.get_sim()
			if _runtime != null and _runtime.has_method("get_sim") else null)
	if sim == null:
		return out
	for row in sim.drain_local_player_weapon_events():
		out.append(PlayerWeaponEvent.from_event_dict(row as Dictionary))
	return out


## Register the host-side presenter for fixed-tick weapon events. The game
## installs LocalPlayerPresenter here; headless/runtime-only hosts leave it
## invalid and may drain the typed event queue explicitly.
func set_local_player_weapon_tick_consumer(consumer: Callable) -> void:
	_local_player_weapon_tick_consumer = consumer


## The resolved weapon.def record driving the FP viewmodel: model/adm names plus the
## witnessed view-bias fields (pos/tpos raw units + rot degrees, renderfov horizontal
## degrees) LocalPlayerPresenter consumes — decoded from NovaWeaponDatabase's transport dict
## at this edge (ADR 0017). Null when the mounted root has no weapon.def or the weapon
## name is absent — callers keep their witnessed JOX AK-47 defaults then. The weapon is
## DEFAULT_VIEWMODEL_WEAPON until equipped-weapon resolution lands; NOVA_VM_WEAPON
## overrides the name (debug: rig A/B against another SKU's def).
func local_player_viewmodel_def() -> PlayerViewmodelDef:
	if _viewmodel_weapon_cleared:
		return null
	var weapon_db := get_weapon_database()
	if weapon_db == null:
		return null
	# Precedence: the armory-equipped weapon, else the NOVA_VM_WEAPON debug override,
	# else the fixed default until first equip.
	var weapon_name := _viewmodel_weapon_override
	if weapon_name.is_empty():
		weapon_name = OS.get_environment("NOVA_VM_WEAPON")
	if weapon_name.is_empty():
		weapon_name = DEFAULT_VIEWMODEL_WEAPON
	var index: int = weapon_db.find_weapon(weapon_name)
	if index < 0:
		push_warning("GameWorld: weapon '%s' not in weapon.def — FP viewmodel keeps built-in defaults" % weapon_name)
		return null
	_local_weapon_dict = weapon_db.get_weapon(index)
	return PlayerViewmodelDef.from_weapon_dict(_local_weapon_dict)


# --- F3 debug views (world-space overlays + the pick stack) ------------------
# The build/teardown lifecycle lives in DebugViewSet (debug_view_set.gd), an
# internal child constructed in _init; the views it builds still attach under
# THIS node, so world-relative lookups (SkeletonDebug/PickDebug/...) are
# unchanged. These one-line delegates keep the host-facing names on GameWorld:
# the F3 option registry dispatches its setters against the world script
# (nova_debug_options), and probes duck-find the world by these methods.

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


func set_particle_debug(enabled: bool) -> void:
	_debug_views.set_particle_debug(enabled)


func is_particle_debug() -> bool:
	return _debug_views.is_particle_debug()


func set_round_debug(enabled: bool) -> void:
	_debug_views.set_round_debug(enabled)


func is_round_debug() -> bool:
	return _debug_views.is_round_debug()


func set_hitbox_debug(enabled: bool) -> void:
	_debug_views.set_hitbox_debug(enabled)


func is_hitbox_debug() -> bool:
	return _debug_views.is_hitbox_debug()


func set_pick_debug(pick_list: NovaDebugPickList) -> void:
	_debug_views.set_pick_debug(pick_list)


func set_pick_click_enabled(enabled: bool) -> void:
	_debug_views.set_pick_click_enabled(enabled)


func set_occlusion_debug(enabled: bool) -> void:
	_debug_views.set_occlusion_debug(enabled)


func is_occlusion_debug() -> bool:
	return _debug_views.is_occlusion_debug()


func get_debug_view_statuses() -> Array[NovaDebugViewStatus]:
	return _debug_views.get_debug_view_statuses()


## The F3 overlay's Particles tab seams (the existing get_effect_world() is
## the data source; these are the two debug toggles).
## Delegates to the item-effect director; the name stays on GameWorld for the
## F3 option registry dispatch (nova_debug_options) + probe duck-calls.
func set_particles_hidden(hidden: bool) -> void:
	_item_fx.set_particles_hidden(hidden)


func is_particles_hidden() -> bool:
	return _item_fx.particles_hidden()


# --- Hide foliage (F3 overlay's "Hide foliage") ------------------------------
# The dispatcher renders the scattered vegetation through child MultiMeshInstance3D slots,
# so hiding the dispatcher node hides all foliage at once -- without touching the placement
# caches, so re-showing is instant and the next dispatch is already current.

func set_foliage_hidden(hidden: bool) -> void:
	_foliage_hidden = hidden
	if _dispatcher != null:
		_dispatcher.visible = not hidden

func is_foliage_hidden() -> bool:
	return _foliage_hidden


# Fire mission audio + particle effects for presentation. PlayWavList actions surface as "dialog"
# effects carrying the dialog/wav id in `a`; route them to the mission audio (which resolves the id
# through the co-named .DBF and plays the LWF set). WAC fx commands surface with the effect name in
# `str`; route them to the effect world. Other kinds are still emitted via mission_effects for host
# consumers (HUD, etc.).
func _route_mission_effects(effects: Array) -> void:
	for e in effects:
		var eff: Dictionary = e
		var kind := String(eff.get("kind", ""))
		if kind == "dialog":
			# BMS PlayWavList: dialog id resolved through the co-named .DBF (queued).
			if _mission_audio != null:
				_mission_audio.play_dialog(int(eff.get("a", 0)))
		elif kind == "dialog_wav":
			# WAC wave/pwave: a scripted voice .wav by filename on its own channel.
			if _mission_audio != null:
				_mission_audio.play_wac_wave(String(eff.get("str", "")))
		elif kind == "fx2ssn":
			# WAC fx2ssn: spawn the named effect at the SSN entity's position with
			# the emitter handle owned per entity — a scripted re-trigger detaches
			# the previous group (spawn_effect_owned), so loops/respawns never stack
			# emitters and FOREVEREMIT effects never accumulate
			# [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0 — renamed from the
			# kong "sound" misnomer, it spawns a particle emitter]. The original
			# orients the emitter to the terrain surface normal at the entity's
			# grid cell; ported as up-vector until the terrain-normal read lands
			# (ptl-format-re.md §8, D-PTL-7).
			if _effect_world != null and _runtime != null:
				var ssn := int(eff.get("b", 0))
				var pos = _runtime.entity_position_for_ssn(ssn)
				if pos != null:
					_effect_world.spawn_effect_owned(ssn, String(eff.get("str", "")), pos, Vector3.UP)
		# fx2tgt (spawn at a placed type-6088 target marker
		# [orig: WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0 — same misnomer
		# family]) stays unrouted: which .bms record field carries the 1..99
		# target number is unwitnessed — ptl-format-re.md §8.


# Drain the flight sim's resolved round impacts and present both descriptor legs.
# Impact particles are generic Always transients in the world domain; their
# production tick/order and catch-up age survive a multi-tick host frame.
# [orig: Projectile_UpdatePhysics @ 0x4e9d70 -> the type-specific impact
#  handler -> Projectile_SpawnImpactEffect @ 0x4e9b80]
func _route_round_impacts() -> void:
	if _runtime == null or not _runtime.has_method("get_sim"):
		return
	var sim = _runtime.get_sim()
	if sim == null or not sim.has_method("drain_round_impacts"):
		return
	for row_v in sim.drain_round_impacts():
		var row: Dictionary = row_v
		var pos := Vector3(row.get("position", Vector3.ZERO))
		var effect := String(row.get("effect", ""))
		if _effect_world != null and not effect.is_empty():
			_effect_world.spawn_effect_transient(effect, pos,
					Vector3(row.get("direction", Vector3.ZERO)),
					maxi(int(row.get("age_ticks", 0)), 0),
					NovaEffectScene.RENDER_DOMAIN_WORLD,
					int(row.get("source_tick", 0)),
					int(row.get("source_order", 0)))
		var sound := String(row.get("sound", ""))
		if _mission_audio != null and not sound.is_empty():
			_mission_audio.fire_soundset(sound, pos)


# Start the shared mission runtime driver: it promotes the mission, builds the present index over the
# placed MissionObjects, and each tick applies every entity's transform + part animations (PLAYPARTANIM,
# applied in-engine) + visibility onto its model. The game runs it at the faithful 62-frame cadence and
# drives it explicitly from tick(); its drained side effects route through
# _on_runtime_effects. A reload reuses this GameWorld, so any prior runtime is freed in unload() first.
func _start_runtime(mission: NovaMissionData, bms_name: String) -> int:
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	_runtime = MissionRuntime.new()
	_runtime.name = "MissionRuntime"
	add_child(_runtime)
	if _frame_stats != null:
		_runtime.set_frame_stats_board(_frame_stats)
	var mission_file := bms_name.get_file()
	if mission_file.is_empty():
		mission_file = bms_name
	var mission_label := mission.get_mission_name().strip_edges()
	if mission_label.is_empty():
		mission_label = mission_file.get_basename()
	# A mission with no AI still ticks (BMS events / WAC); only a promote failure leaves a null sim.
	# Hand the loaded terrain to the runtime so promoted AI grounds on it (entities hug the terrain),
	# and the resource root so soldiers resolve their .adm/.bad root-motion clips.
	var opts := {
		"terrain": _terrain_data,
		"resource_root": _resource_root,
		"wac_basename": bms_name.get_basename(),
		"mission_file": mission_file,
		"mission_name": mission_label,
		"spawn_names": [mission_label],
		# The placer's item database (item_id -> anim_def), so each soldier grounds off its own
		# model's .adm clip set (per-entity capsule_bottom), not the shared default. [D-INF-6]
		"item_db": _placer.get_item_db() if _placer != null else null,
	}
	# Serve-and-play hosts run the listen server AND spawn their own player (ADR 0011/0012, net-re
	# §5.2b/§5.38). A DEDICATED host (config "dedicated") serves WITHOUT a local player — same listen
	# server, just no own-player spawn; main_game skips the HUD when there is no local player. Diagnostic
	# previews opt out via _playable.
	# Terrain-tile (.til) bytes for the S2C 0x45 terrain-tile load a listen host streams to joiners so
	# their g_loading_progress climbs 5 -> 6 and terrain finishes loading (net-re §5.37). The tile-overlay
	# .til is named after the MISSION (localres.pff: ASH_I5A.til), not the terrain tileinfo
	# [orig: Terrain_LoadFoliageFile @ 0x60a740;
	# serialize_terrain_tiles @ 0x6080f0]. Reuse the payload parsed before terrain build.
	if not _mission_til_bytes.is_empty():
		opts["terrain_til"] = _mission_til_bytes
	opts["playable"] = _playable and not _net_drive.pending_dedicated()
	# Spread the staged net-session request (typed record + derived staging +
	# the surrendered preload sim, consumed once per load) into the runtime's
	# options — MissionRuntime alone adopts opts["simulation"] (ADR 0011/0012).
	_net_drive.stage_runtime_options(opts)
	# The placer + environment node let the joiner's wire present pass resolve + light its
	# remote-entity avatars (build_player_animated_model); unused by the host present path.
	opts["placer"] = _placer
	opts["env_node"] = _env
	# The occlusion-claim set the present pass consults (two-bit visibility
	# ownership; see OcclusionFramePass._set_occlusion_hidden). Shared by
	# reference: the pass created these dictionaries once and mutates them in
	# place across the mission's occlusion frames — hand the SAME instances.
	opts["present_options"] = {
		"occlusion_hidden_ids": _occlusion.occlusion_hidden_ids(),
		"present_visibility": _occlusion.present_visibility(),
	}
	# The fire present pass's providers (AI/remote fire sound + muzzle + tracers): audio
	# and effect world resolve lazily (mission audio is set up after the runtime), the
	# listener is the same camera position the audio render pass ticks with.
	opts["fire_audio"] = Callable(self, "get_mission_audio")
	opts["fire_fx"] = Callable(self, "get_effect_world")
	opts["fire_listener"] = Callable(self, "_fire_listener_position")
	# The destruction present pass anchors its wreck/piece effect groups through
	# register_effect_anchor and swaps husk models via the placer.
	opts["game_world"] = self
	_runtime.setup(mission, container, opts)
	if _runtime.get_sim() == null:
		var setup_error := int(_runtime.get_setup_error())
		var lan_bind_failure := String(opts.get("net_transport", "")) == "lan"
		var bind_port := int(opts.get("bind_port", 32768))
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
	_sync_runtime_profiling()
	# The player profile's saved weapon kits, loaded before ANY kit is applied or
	# submitted: in a net session the original's spawn kit is a page of this file,
	# selected by the very class byte it also puts on the wire
	# [orig: Game_StartMission @ 0x525767-0x525836].
	_load_player_weapon_profile()
	_apply_local_player_spawn_loadout()
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
func _weapon_profile_path(resource_root: NovaResourceRoot) -> String:
	if resource_root == null:
		return ""
	var dir := String(resource_root.get_root_dir())
	if dir.is_empty():
		return ""
	var expansion := String(resource_root.get_expansion())
	if expansion.is_empty():
		return dir.path_join("weapon.sav")
	return dir.path_join("expansion").path_join(expansion).path_join("weapon.sav")


# Load weapon.sav onto the sim: five profile-slot records, each carrying a per-side
# class byte and the five 2048-byte class kit pages the MP loadout submit indexes BY
# that class byte [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 — header check
# @ 0x54f586 ("FPBC"/"0211"), the 5 x 0x1080C record reads]. This is a plain disk
# file, not archive content, so it is read through the mount's directory rather than
# the VFS. A file that is absent or not a profile is NOT a load failure: retail's
# own miss leaves PlayerProfile_InitDefaults' shipped defaults in place (BLUE/RED
# class 8, one weapon name per class page) [orig: @ 0x54bb40].
func _load_player_weapon_profile() -> void:
	# Probed rather than called straight through, like the other optional sim seams
	# in this file: the profile reader is a native method, so a build whose
	# GDExtension predates it must degrade to the defaults instead of failing to
	# parse this script.
	var sim: Variant = get_sim()
	if sim == null or not sim.has_method("load_weapon_profile"):
		return
	var path := _weapon_profile_path(_resource_root)
	if path.is_empty():
		return
	if not FileAccess.file_exists(path):
		print_verbose("GameWorld: no weapon.sav at %s — keeping the shipped profile defaults" % path)
		return
	var err := int(sim.load_weapon_profile(path))
	if err != OK:
		push_warning("GameWorld: weapon.sav at %s not accepted (error %d) — keeping the shipped profile defaults"
				% [path, err])


# Consume render-internal lifecycle effects first, route "dialog" actions to
# mission audio (resolved through the co-named .DBF + LWF set), then expose only
# the remaining host-facing effects to HUD consumers.
func _on_runtime_effects(effects: Array) -> void:
	var routed: Array = []
	for effect_v in effects:
		if effect_v is Dictionary:
			var effect: Dictionary = effect_v
			if _item_fx.consume_control_effect(effect):
				continue
		routed.append(effect_v)
	if routed.is_empty():
		return
	_route_mission_effects(routed)
	mission_effects.emit(routed)


func _on_runtime_fixed_tick(_logic_tick: int) -> void:
	var probe_enabled := _perf_probe_enabled
	var skip_fixed_handlers := probe_enabled and _perf_probe_skip_fixed_handlers
	if skip_fixed_handlers:
		return
	# Retail executes local weapon actions and physical impacts before the same
	# frame's global particle update. Consume each source tick synchronously so
	# admission slots, first emission, and catch-up chronology are exact; only
	# mission render Nodes remain batched until tick_realtime() returns.
	if _local_player_weapon_tick_consumer.is_valid():
		_local_player_weapon_tick_consumer.call(drain_local_player_weapon_events())
	_route_round_impacts()
	var skip_effect_tick := probe_enabled and _perf_probe_skip_effect_tick
	if _effect_world != null and not skip_effect_tick:
		if _frame_stats != null and _frame_stats.is_capture_active():
			var fx_start := Time.get_ticks_usec()
			_effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)
			_frame_stats.add(FrameStatsBoard.EFFECTS_TICK,
					Time.get_ticks_usec() - fx_start)
		else:
			_effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)


func _on_runtime_simulation_restarted() -> void:
	# A Stop/restart can restore the saved personal slot while the presenter still
	# owns an emplaced model. Consume that control event synchronously; no fixed
	# tick runs while stopped.
	if _local_player_weapon_tick_consumer.is_valid():
		_local_player_weapon_tick_consumer.call(
				drain_local_player_weapon_events())
	if _effect_world == null:
		return
	_effect_world.reset_runtime_state()
	# Persistent item effects belong to the restored entity set, not the scene
	# that was just discarded. Re-register their admission and owner identities;
	# restore emits fresh controller-start lifecycle events for occupied baselines.
	_item_fx.reattach()


# Place real ambient sounds at the mission's sound markers: load the co-named .LWF
# + gamelocl.LWF, resolve each marker to a sound set by name, and spawn looping 3D
# voices. Reuses the placer's item database for the item_id -> soundloop_1..4 lookup.
func _start_mission_audio(mission: NovaMissionData, bms_name: String) -> void:
	var item_db = _placer.get_item_db() if _placer != null else null
	var mission_info: Dictionary = mission.get_info()
	if _env != null:
		_env.configure_mission_clock(
			int(mission_info.get("start_time", 0)),
			int(mission_info.get("minutes_per_day", NovaEnvironment.DEFAULT_MINUTES_PER_DAY)))
	_mission_audio = NovaMissionAudio.new(_resource_root, item_db)
	# Sound occlusion runs LOS through the sim's collision world + terrain
	# [orig: Sound_ApplyOcclusionDistance @ 0x529970]; hosts without a sim mix
	# unoccluded.
	_mission_audio.set_simulation(get_sim())
	var stats := _mission_audio.setup(mission, bms_name, self)
	if _env != null and _env.get("time_of_day") != null:
		_mission_audio.set_time_of_day_hhmm(float(_env.get("time_of_day")))
	print_verbose("GameWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d ambient candidate(s), %d/%d physical channel(s) allocated" % [
		int(stats.get("markers_resolved", 0)),
		int(stats.get("markers_total", 0)),
		int(stats.get("banks_loaded", 0)),
		int(stats.get("ambient_candidates", 0)),
		int(stats.get("physical_channels", 0)),
		int(stats.get("channel_budget", NovaMissionAudio.MIX_CHANNELS)),
	])
	# Open the GAME music context + seed the witnessed vars [orig: Game_StartMission
	# @ 0x525581-0x52561b]. Retail gates the open on is_mp_session_peer and STOPS
	# music in single-player; ours opens in ALL sessions — D-MUS-SPGATE
	# (docs/audio/mus-sbf-re.md §Game music driving; SP-as-listen-server, ADR
	# 0009/0011/0012). gamemus's discriminator Var1 stays 0 (never written in
	# retail), so the Multiplayerstart P0 loop plays.
	NovaMusicService.open_game_context(_resource_root)


# The load-time effect warm pass (see the load-path call site): spawn every
# catalog effect in front of the load camera, advance the fixed tick so fresh
# emitters actually emit, force-draw two frames SYNCHRONOUSLY so every new
# material/pipeline draws once (no coroutine — the load path stays callable
# without await), then clear the warm spawns exactly like the sim-restart
# path (reset + re-register the persistent item effects). Returns the count.
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
	if _runtime != null and _runtime.has_method("warm_present_pipelines"):
		_runtime.warm_present_pipelines(warm_pos)
	_effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)
	_effect_world.render_now()
	# Pipeline compiles need real draws. Skip the forced frames inside the
	# editor host (re-entrant editor drawing); the texture warm above still
	# runs there, and the shipped game is what the full warm protects.
	if is_inside_tree() and not Engine.is_editor_hint():
		# MainGame keeps World hidden behind the opaque loading CanvasLayer.
		# Temporarily expose it so the particle domains, tracer MeshInstance,
		# and deterministic helper quads are actually submitted to force_draw.
		var was_visible := visible
		visible = true
		RenderingServer.force_draw(true)
		_effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)
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
	var unresolved := PackedStringArray(
			_effect_world.get_unresolved_texture_names()).size()
	print_verbose("GameWorld: effect warm pass — %d effect(s) precompiled, %d unresolved texture(s)" % [
			spawned, unresolved])
	return spawned


# Mission-start load of EVERY mounted .ptl into the runtime effect world
# [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
# file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
func _start_effect_world() -> void:
	_effect_world = NovaEffectWorld.new()
	_effect_world.name = "EffectWorld"
	add_child(_effect_world)
	_effect_world.set_environment_source(_env)
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


func get_effect_world() -> NovaEffectWorld:
	return _effect_world


## The live terrain node, for the F3 Terrain & foliage page's counters/knobs.
func get_terrain_node() -> NovaTerrain:
	return _terrain


## The live foliage dispatcher (null until a mission builds one), same consumer.
func get_foliage_dispatcher() -> NovaFoliageDispatcher:
	return _dispatcher


## The mounted item database (null before a mission), for the F3 snapshot
## writer's display-name/graphic enrichment.
func get_item_db() -> NovaItemDatabase:
	return _placer.get_item_db() if _placer != null else null


## The live environment / weather / water nodes, for the F3 Environment
## page's readouts and scrub knobs.
func get_environment_node() -> Node:
	return _env


func get_weather_node() -> Node:
	return get_node_or_null("NovaWeather")


## Hosted mission-clock knob used by F3 and runtime MCP. NovaEnvironment owns
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
	var err: Error = _env.debug_set_mission_minute_of_day(minute_of_day)
	if err != OK:
		return err
	var weather := get_weather_node()
	if weather != null and weather.has_method("resync_colors_now"):
		weather.resync_colors_now()
	elif weather != null and weather.has_method("resync_colors"):
		weather.resync_colors()
	if _mission_audio != null:
		_mission_audio.set_time_of_day_hhmm(_env.time_of_day)
	return OK


func get_water_node() -> Node:
	return _water


## A host registers a live pose resolver for an owner-bound effect group it
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


func get_mission_audio() -> NovaMissionAudio:
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
	var max_h: int = _runtime.local_player_max_health()
	var cur_h: int = _runtime.local_player_health()
	NovaMusicService.set_var(NovaMusicService.VAR_HEALTH_PCT,
		(cur_h * 100 / max_h) if max_h > cur_h else 100)
	NovaMusicService.set_var(NovaMusicService.VAR_TEAM, _runtime.local_player_team())


# The marched iris-exposure feed (D-RLIT-2): three camera-ray samples from the
# sim each render frame, consumed by NovaWeather's exposure re-target on its
# next tick [orig: Environment_ApplyFogAndAmbient @ 0x57e512 ->
# compute_ambient_light_along_direction @ 0x5c7a00 — retail re-targets from the
# local player's view every render pass]. The render-occlusion frame it used
# to share a section with (blink letter gates + the section-mask/portal apply)
# lives in occlusion_frame_pass.gd; the iris march stays here as the weather
# feed.
func _stamp_iris_samples(camera_xform: Transform3D) -> void:
	var weather := get_node_or_null("NovaWeather")
	if weather == null or _runtime == null or not _runtime.has_method("get_sim"):
		return
	var sim = _runtime.get_sim()
	if sim == null or not sim.has_method("compute_iris_samples"):
		return
	var light_dir := Vector3.UP
	if _env != null and _env.has_method("get_light_direction"):
		light_dir = _env.get_light_direction()
	weather.iris_samples = sim.compute_iris_samples(
			camera_xform.origin, -camera_xform.basis.z, light_dir)


# --- Frame clear color (env divergence #21, closed) ----------------------------

func _process(_delta: float) -> void:
	_sample_panm_clock()
	if not _loaded or not is_visible_in_tree():
		_restore_idle_frame_clear_color()
		return
	_update_frame_clear_color()


func _restore_idle_frame_clear_color() -> void:
	_clear_env_generation = -1
	if _clear_color == null or _clear_color.environment == null:
		return
	_clear_color.environment.background_color = _idle_frame_clear_color


# The witnessed frame clear: the horizon-blended skyfog above water, the lit
# water color underwater [orig: Render_ProcessMainSceneFrame @ 0x5ca776..
# 0x5ca792 - clear color = alternate_fog ? 0x808080 : cam above water ?
# skyfog[0] : Env_WaterColorLit; the vehicle alternate-fog view is not modeled
# yet]. Both branches serve RENDER-SPACE (x2-gained) colors, consumed VERBATIM
# by the modulate2x-path Clear this host reproduces (D-RMAT-7): above water the
# post-blend DOUBLED skyfog, underwater Env_WaterColorLit = water x light >> 7;
# the halving branch [orig: @ 0x67715d] is the non-modulate2x fallback with no
# host analog. The ClearColor Environment must stay BG_COLOR with ambient
# disabled - BG_SKY with no sky renders black and swallows these writes
# (GUT-pinned).
func _update_frame_clear_color() -> void:
	if _clear_color == null or _clear_color.environment == null or _env == null:
		return
	if not _env.has_method("get_frame_clear_color"):
		return
	# Indoors the frame clears BLACK, not skyfog [orig: render_main_scene
	# @ 0x5c1597 — the Env_SkyfogBlock clear runs only when the blink indoors
	# bit is clear; the sentinel generation forces a recompute on exit].
	if _occlusion.blink_indoors:
		if _clear_env_generation != -2:
			_clear_env_generation = -2
			_clear_color.environment.background_color = Color.BLACK
		return
	var above := true
	var cam := get_viewport().get_camera_3d() if is_inside_tree() else null
	if cam != null and is_water_render_active():
		# Camera3D h/v offsets move the rendered eye without changing the node
		# transform. Classify the same adjusted eye NovaWater marches from.
		above = cam.get_camera_transform().origin.y > float(_water.water_height)
	var gen := int(_env.get_env_generation())
	if gen == _clear_env_generation and above == _clear_above_water:
		return
	_clear_env_generation = gen
	_clear_above_water = above
	var rgb: Vector3
	if above:
		rgb = _env.get_frame_clear_color()
	else:
		# Underwater clear = the lit water color [orig: @ 0x5ca78b], the same
		# derived chain the water surface renders with.
		var combined := EnvFile.combine_terrain_light(
			_vec3_color(_env.get_sun_light()), _vec3_color(_env.get_sky_ambient()))
		var lit := EnvFile.lit_water_color(_vec3_color(_env.get_water_color()), combined)
		rgb = Vector3(lit.r, lit.g, lit.b)
	_clear_color.environment.background_color = Color(rgb.x, rgb.y, rgb.z)


static func _vec3_color(v: Vector3) -> Color:
	return Color(v.x, v.y, v.z)
