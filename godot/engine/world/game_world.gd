class_name GameWorld
extends Node3D

# Loads a playable world (terrain + environment + vegetation + foliage) from ONE
# resource root and wires it onto the engine nodes it contains (NovaTerrain,
# NovaEnvironment, NovaWater). The data core is shared engine code
# (NovaTerrainData, EnvFile, NovaFoliageDispatcher, VegAssets); this node is just
# the orchestration both the runtime (game/main_game.tscn) and the editor's Play
# mode go through — one loader, one root, no fallbacks. The scene lives in
# game_world.tscn so hosts instance it; the game mounts its root from the
# persisted resource directory, the editor injects its own via
# set_resource_root() and plays the live document via load_mission_data().
#
# HOST CONTRACT (duck-typed on purpose — two hosts do not justify a formal
# interface): a host instances game_world.tscn, optionally injects a root,
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
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const NovaModelResolver := preload("res://engine/mission/nova_model_resolver.gd")
const NetWorldView := preload("res://engine/world/net_world_view.gd")
const NetEventView := preload("res://engine/world/net_event_view.gd")
const SkeletonDebugView := preload("res://engine/debug/skeleton_debug_view.gd")
const UserPointDebugView := preload("res://engine/debug/user_point_debug_view.gd")
const CollisionDebugView := preload("res://engine/debug/collision_debug_view.gd")
const ParticleDebugView := preload("res://engine/debug/particle_debug_view.gd")
const NET_CONTAINER_NAME := "NetObjects"
const SKELETON_DEBUG_NAME := "SkeletonDebug"
const USER_POINT_DEBUG_NAME := "UserPointDebug"
const COLLISION_DEBUG_NAME := "CollisionDebug"
const PARTICLE_DEBUG_NAME := "ParticleDebug"
const TICK_DT := 1.0 / 62.5  # mirrors MissionRuntime.TICK_DT; default for tick()'s delta param
# [orig: ItemDef_GetBoneMaskByName @ 0x49ea40 scans the first 16 points.]
const ITEM_EFFECT_USER_POINT_SCAN_LIMIT := 16

signal world_loaded()
signal load_failed(reason: String)
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
var _mission_stats: Dictionary = {}
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database)
var _weapon_db: NovaWeaponDatabase = null  # weapon.def, lazy per mounted root (FP viewmodel)
var _local_weapon_dict := {}  # the resolved weapon's raw dict (FSM setup transport, ADR 0017 edge)
var _mission_audio: NovaMissionAudio
var _effect_world: NovaEffectWorld  # the runtime .ptl effect world (render-only, per mission)
# Host-owned first-person presentation seam. MissionRuntime invokes GameWorld
# once per completed fixed tick; this callback consumes that tick's weapon
# events before EffectWorld advances, matching retail's action -> particle-pass
# order without coupling the simulation to LocalPlayerHost Nodes.
var _local_player_weapon_tick_consumer := Callable()
# Frame-clear cache (divergence #21): recompute only when the env generation
# moves or the camera crosses the water plane.
var _clear_env_generation: int = -1
var _clear_above_water := true
# The local player's applied blink letter gates (render-occlusion-re.md §4):
# accum bit 0x2 hides the terrain render (near detail + far foliage ride the
# terrain node) and the sky dome + celestials; bit 0x8 hides the water passes.
var _blink_indoors := false
var _blink_water_suppressed := false
var _idle_frame_clear_color := Color.BLACK
var _net_client     # NovaNetClient: the in-match wire client (replay or live)
var _net_view       # NetWorldView: spawns + drives models from the decoded world
var _net_event_view # NetEventView: draws the decoded event stream over the world
# NovaWorldHost: registers a LAN/co-op listen host with the NovaWorld gate so a
# retail client can browse + join it (F1). Only created when a gate was supplied
# (via _host_config["nw_gate_host"]); absent for pure-LAN play. Fed the live
# player count from tick(), torn down in unload().
var _nw_host
# A host-injected resource root (the editor's mounted VFS). When set, the load_*
# entries skip the settings lookup + their own mount and resolve through it; the
# game path (no injection) still mounts from the persisted resource directory.
var _injected_root: NovaResourceRoot = null
# Debug: draw character bones over the world (F3 overlay's "Show skeletons"). Off by default.
var _skeleton_debug := false
# Debug: draw named model user points, including static-batched objects. Off by default.
var _user_point_debug := false
# Debug: draw the collision volumes + player capsule (F3 overlay's "Show collision"). Off by default.
var _collision_debug := false
# Debug: hide the scattered foliage (F3 overlay's "Hide foliage"). Off by default.
var _foliage_hidden := false
# Debug: hide every particle effect (F3 overlay's "Hide particles" — the retail
# master particle switch, mimicked). Off by default; survives mission reloads.
var _particles_hidden := false
# Debug: draw live emitter bounds + effect names (F3 overlay's "Show effect boxes").
var _particle_debug := false
var _playable := true
var _host_config: Dictionary = {}  # set by load_mission_as_host; consumed once by _start_runtime
var _perf_tick_us: int = 0
var _perf_foliage_us: int = 0
var _perf_runtime_us: int = 0
var _perf_audio_us: int = 0


## Inject the resource root the next load resolves through (play-in-editor hands
## the editor's root over so play uses exactly the assets being authored). Null
## returns to the game's settings-driven mount.
func set_resource_root(root: NovaResourceRoot) -> void:
	_injected_root = root


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


func _ready() -> void:
	if _clear_color != null and _clear_color.environment != null:
		_idle_frame_clear_color = _clear_color.environment.background_color
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher") as NovaFoliageDispatcher
		_tile_overlay = _terrain.get_node_or_null("TileOverlay") as NovaTerrainTileOverlay


func _notification(what: int) -> void:
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

	_clear_mission_tile_info()
	_resource_root = resource_root
	if not _load_environment(env_file):
		load_failed.emit("failed to load %s" % env_file)
		return ERR_CANT_OPEN
	if not _load_terrain(terrain_file):
		load_failed.emit("failed to load %s" % terrain_file)
		return ERR_CANT_OPEN

	_loaded = true
	if _user_point_debug:
		_refresh_user_point_debug()
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
	if not resource_root.has_file(bms_name):
		load_failed.emit("%s not found in %s" % [bms_name, resource_root.get_root_dir()])
		return ERR_FILE_NOT_FOUND
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(resource_root, bms_name) != OK:
		load_failed.emit("failed to parse %s: %s" % [bms_name, mission.get_last_error()])
		return ERR_CANT_OPEN
	return _load_mission_internal(mission, bms_name, resource_root)


## Load a mission as a LAN co-op HOST. Same load path as load_mission, but the runtime
## starts the in-process listen server (ADR 0011) — bound to a real socket transport
## (Phase 2) and advertised on the LAN (Phase 3) once those land. `config` is the mp.mnu
## host screen's co-op-minimal readback: { mission|missions[], server_name, max_players,
## game_type, net_transport, bind_port, ... }. Returns the same codes as load_mission.
func load_mission_as_host(config: Dictionary) -> int:
	_host_config = config.duplicate()
	var bms := String(config.get("mission", ""))
	if bms.is_empty():
		var missions: Array = config.get("missions", [])
		if missions.size() > 0:
			bms = String(missions[0])
	if bms.is_empty():
		_host_config = {}
		load_failed.emit("host start: no mission selected")
		return ERR_INVALID_PARAMETER
	return load_mission(bms, String(config.get("dir", "")))


## Load a mission as a LAN co-op JOINER (a non-authority client). Same load path as a host
## (terrain + environment from the .bms header), but the runtime dials the host and runs the
## witnessed in-match JOIN instead of starting a listen server; dynamic entities (the host,
## other joiners, NPCs) render WIRE-DIRECT (no .bms placement), so _place_mission_objects is
## skipped for dynamics (statics/buildings arrive via S2C 0x10 in a follow-up). `server` is the
## discovered/selected row { host_ip, port, mission }, `player_name` rides the ClientHello.co
## (the host echoes it back so we self-identify by name-match). Returns the load_mission codes.
func load_mission_as_joiner(server: Dictionary, player_name: String) -> int:
	_host_config = {
		"net_transport": "lan-join",
		"host_ip": String(server.get("host_ip", "127.0.0.1")),
		"port": int(server.get("port", 32768)),
		"player_name": player_name,
	}
	var bms := String(server.get("mission", ""))
	if bms.is_empty():
		bms = mission_file
	if bms.is_empty():
		_host_config = {}
		load_failed.emit("join: no mission name (the host's mission must be known)")
		return ERR_INVALID_PARAMETER
	# NovaWorld's host row carries the retail basename (e.g. ASH_I5A), while the
	# VFS load requires the resource filename. LAN callers that already supply the
	# extension pass through unchanged.
	if not bms.to_lower().ends_with(".bms"):
		bms += ".bms"
	return load_mission(bms, String(server.get("dir", "")))


# True between load_mission_as_joiner and _start_runtime's config consume: this load is a
# co-op joiner, so dynamic objects render from the wire rather than from local placement.
func _is_joiner() -> bool:
	return String(_host_config.get("net_transport", "")) == "lan-join"


## Load an IN-MEMORY mission (the editor's live document, unsaved edits included):
## terrain + environment resolve from the injected (or mounted) root by the
## mission's own header refs, then the one shared load path runs. `bms_name` is
## the document's file name, used for the co-named audio lookups (.DBF/.LWF) and
## error messages. This is the play-in-editor entry: the world renders exactly
## the document being authored.
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
	if _user_point_debug:
		_refresh_user_point_debug()
	world_loaded.emit()
	return OK


# The map name arrived on the wire (S2C 0x7B). Load that mission's terrain +
# environment so the streamed entities have ground to stand on. Entities are NOT
# placed from the .bms and the AI sim never runs — they come from the wire.
func _on_net_mission(mission_name: String) -> void:
	if _loaded_mission != null or _resource_root == null:
		return
	if not _resource_root.has_file(mission_name):
		push_warning("net session: map '%s' (from the wire) not in %s" % [mission_name, _resource_root.get_root_dir()])
		return
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(_resource_root, mission_name) != OK:
		push_warning("net session: failed to parse %s: %s" % [mission_name, mission.get_last_error()])
		return
	_loaded_mission = mission
	_load_mission_tile_info(mission_name, _resource_root)
	var env_name := mission.get_environment_ref() + ".env"
	if _resource_root.has_file(env_name) and _load_environment(env_name):
		_apply_mission_environment_overrides(mission)
	var trn := mission.get_terrain_ref() + ".trn"
	if _resource_root.has_file(trn) and _load_terrain(trn):
		print("GameWorld(net): map %s -> terrain %s loaded" % [mission_name, mission.get_terrain_ref()])
	else:
		push_warning("net session: terrain %s.trn not loaded" % mission.get_terrain_ref())


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

	# Same stage attribution as the editor's mission open, so the two hosts'
	# load costs stay comparable (one timeline ring serves both).
	var timeline := PerfTimeline.begin("Mission load %s" % bms_name)
	_resource_root = resource_root
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
		return ERR_CANT_OPEN
	_apply_mission_environment_overrides(mission)
	timeline.end_span()
	load_progress.emit(6)
	timeline.span("terrain")
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		return ERR_CANT_OPEN
	timeline.end_span()
	load_progress.emit(26)

	_loaded_mission = mission
	timeline.span("objects")
	_place_mission_objects(mission, timeline)
	timeline.end_span()
	load_progress.emit(41)
	timeline.span("runtime")
	_start_runtime(mission, bms_name)
	timeline.end_span()
	load_progress.emit(70)
	timeline.span("audio")
	_start_mission_audio(mission, bms_name)
	timeline.end_span()
	load_progress.emit(90)
	timeline.span("effects")
	_start_effect_world()
	timeline.end_span()
	load_progress.emit(95)
	timeline.finish()
	_loaded_mission_file = bms_name
	_loaded = true
	load_progress.emit(100)
	if _user_point_debug:
		_refresh_user_point_debug()
	world_loaded.emit()
	return OK


# Mount `dir` as the runtime resource root: PFF archives are the packed game data,
# the `/exp <name>` flag (or persisted setting) layers an expansion over the base,
# loose files override the archives only under the `/d` dev flag, and the `/game <code>`
# flag (or persisted setting, default "jo") selects the SCR decode key so demo data
# decodes correctly. Emits load_failed and returns null on a bad root.
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var resource_root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if resource_root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		load_failed.emit(resource_root.get_last_error())
		return null
	return resource_root


# Populate the world with the mission's placed objects under a MissionObjects node.
# Shares the host-agnostic placer with the editor Mission workspace.
func _place_mission_objects(mission: NovaMissionData, timeline: PerfTimeline = null) -> void:
	if _resource_root == null or mission == null:
		return
	_placer = MissionObjectPlacer.new(_resource_root)
	# A co-op joiner renders all dynamic entities WIRE-DIRECT (the faithful client model), so
	# it does NOT place the .bms organics/vehicles — they would be frozen duplicates of the
	# wire avatars. Still build the placer (the local-player avatar + the wire present pass
	# resolve models through it) and an empty MissionObjects container (the wire avatars'
	# parent + the unload() teardown target). Statics/buildings (S2C 0x10) are a follow-up.
	if _is_joiner():
		var wire_container := Node3D.new()
		wire_container.name = MissionObjectPlacer.CONTAINER_NAME
		add_child(wire_container)
		_mission_stats = {}
		print("GameWorld(joiner): mission objects render wire-direct — local placement skipped.")
		return
	var options := { "environment_node": _env }
	if timeline != null:
		options["timeline"] = timeline
	# Pulse the load-progress screen from inside the model-load loop at the
	# stage's constant value — the original re-presents its loading screen the
	# same way, with a constant percentage from within the per-model loops
	# [orig: the paired constant-value LoadingScreen_UpdateAndPresent calls
	# inside Game_StartMission's model loops @ 0x524d9c/0x524e09, 0x524f32/0x524fe0].
	options["progress"] = func() -> void: load_progress.emit(26)
	_mission_stats = _placer.place(mission, self, options)
	print("GameWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
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
	_host_config = {}
	_clear_mission_tile_info()
	_restore_idle_frame_clear_color()
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	if container != null:
		container.queue_free()
	# Per-item attached-effect owner keys reference nodes in that container —
	# never let a reload's provider resolve against freed instances.
	_item_fx_nodes.clear()
	_item_fx_owner_refs.clear()
	_item_fx_registered_nodes.clear()
	_item_fx_pending_nodes.clear()
	_item_fx_registered_static.clear()
	_item_fx_pending_static.clear()
	_item_fx_control_active.clear()
	_item_fx_control_nodes.clear()
	_item_fx_control_instances.clear()
	# The user-point view retains its toggle and re-arms on the next successful load.
	_remove_user_point_debug_view()
	# Skeleton/collision overlays are also freed for a clean teardown.
	var skel_debug := get_node_or_null(NodePath(SKELETON_DEBUG_NAME))
	if skel_debug != null:
		skel_debug.queue_free()
	var col_debug := get_node_or_null(NodePath(COLLISION_DEBUG_NAME))
	if col_debug != null:
		col_debug.queue_free()
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
	# Gate registration teardown: tells the gate to drop the host row (ClientStopHosting).
	if _nw_host != null:
		_nw_host.stop()
		_nw_host.queue_free()
		_nw_host = null
	if _mission_audio != null:
		_mission_audio.teardown()
	# Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	# The game shell re-opens menu music on its return to the front end.
	NovaMusicService.stop_context()
	# Blink frame gates reset with the mission [orig: the letter-bit clear
	# @ 0x525c45 at mission start] — an unload while indoors must not leave the
	# next mission's terrain/sky/water hidden.
	_reset_blink_frame_gates()
	if _env != null and _env.environment_data != null:
		_env.environment_data.clear_mission_overrides()
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
	var celestial := get_node_or_null("NovaCelestial")
	if celestial != null and celestial.has_method("set_resource_root"):
		celestial.set_resource_root(_resource_root)
	return true


## Apply the mission's attrib-gated water/fog overrides onto the loaded env via
## EnvFile's non-persistent override layer [orig: Game_LoadTerrainDuringConnect
## @ 0x520710]. The base .env is never mutated.
func _apply_mission_environment_overrides(mission: NovaMissionData) -> void:
	if _env == null or mission == null:
		return
	var env_data: EnvFile = _env.environment_data
	if env_data == null:
		return
	var overrides: Dictionary = mission.get_environment_overrides()
	if overrides.is_empty():
		env_data.clear_mission_overrides()
	else:
		env_data.apply_mission_overrides(overrides)


# Retail loads <mission>.til into one shared g_TerrainTileArray used by
# terrain overlays/surface overrides, network initial state, and both foliage
# generators' radius-2 blocker.
# [orig: Terrain_LoadFoliageFile @ 0x60a740;
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
	if not resource_root.has_file(til_name):
		return
	var til_bytes := resource_root.read_file(til_name)
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
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.colormap_source = _terrain_data
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


## The host per-frame order, faithful to the original main loop's server-tick-then-client-render:
## foliage coverage around the viewer, then the mission runtime (MissionRuntime.tick advances the
## logic at the 62-frame cadence, presents entity state onto the placed nodes, and drains side
## effects), then the audio render pass. Effects come back through MissionRuntime.effects_drained.
func tick(camera_pos: Vector3, camera_xform: Transform3D = Transform3D(), delta: float = TICK_DT) -> void:
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
	# Gate on the runtime transport so MissionRuntime._playing is THE play flag
	# in both hosts: the debug overlay's Pause/Step work in the game too, not
	# just the editor preview. _start_runtime calls play(), so normal missions
	# run exactly as before.
	if _loaded and _runtime != null and _runtime.is_playing():
		# Fixed-timestep accumulator: the sim runs at a constant 62.5 Hz regardless of render rate.
		# Guard keeps the duck-typed test stubs (game_world_test.gd) that only implement tick() green.
		if _runtime.has_method("tick_realtime"):
			runtime_ticks = int(_runtime.tick_realtime(delta))
		else:
			runtime_ticks = 1 if bool(_runtime.tick()) else 0
		_perf_runtime_us = Time.get_ticks_usec() - runtime_start
		# Keep the gate's advertised occupancy current (host + admitted joiners).
		# set_player_count self-dedupes, so this is a no-op until the count changes.
		if _nw_host != null and _runtime.has_method("get_sim"):
			var sim = _runtime.get_sim()
			if sim != null and sim.has_method("get_host_peer_count"):
				_nw_host.set_player_count(1 + sim.get_host_peer_count())
	# The environment owns the one mission clock and advances only for fixed
	# simulation ticks [orig: Environment_SetTodAdvanceRate @ 0x57d170].
	if _loaded and runtime_ticks > 0 and _env != null:
		_env.advance_mission_clock(runtime_ticks)
	# Blink flags only change on sim ticks; re-apply the frame gates then.
	if _loaded and runtime_ticks > 0:
		_apply_blink_frame_gates()
	var audio_start := Time.get_ticks_usec()
	if _loaded and _mission_audio != null:
		# Ambient soundloop regions read that same clock [orig:
		# Entity_CalcTimeOfDayRegion @ 0x408110].
		if _env != null and _env.get("time_of_day") != null:
			_mission_audio.set_time_of_day_hhmm(float(_env.get("time_of_day")))
		_mission_audio.tick(camera_pos)
		_music_var_pump()
		_perf_audio_us = Time.get_ticks_usec() - audio_start
	_perf_tick_us = Time.get_ticks_usec() - tick_start


func get_runtime_perf_counters() -> Dictionary:
	return {
		"tick_us": _perf_tick_us,
		"foliage_us": _perf_foliage_us,
		"runtime_us": _perf_runtime_us,
		"audio_us": _perf_audio_us,
		"runtime": _runtime.get_perf_counters() if _runtime != null and _runtime.has_method("get_perf_counters") else {},
		"foliage": _dispatcher.get_frame_stats() if _dispatcher != null and _dispatcher.has_method("get_frame_stats") else {},
		"audio": _mission_audio.get_perf_counters() if _mission_audio != null and _mission_audio.has_method("get_perf_counters") else {},
	}


# Listener position for the fire present pass — the same camera position the audio
# render pass ticks with (INF until the first tick).
var _last_tick_camera_pos := Vector3.INF


func _fire_listener_position() -> Vector3:
	return _last_tick_camera_pos


# Fire-presentation counters (probe/diagnostic seam; empty until a mission runs).
func get_fire_present_stats() -> Dictionary:
	return _runtime.get_fire_present_stats() if _runtime != null and _runtime.has_method("get_fire_present_stats") else {}


# --- the local player (Phase 2; ADR 0012). Host delegates to the mission runtime. ---
func has_local_player() -> bool:
	return _runtime != null and _runtime.has_player()

func local_player_position() -> Vector3:
	return _runtime.local_player_position() if _runtime != null else Vector3.ZERO

func local_player_yaw_deg() -> float:
	return _runtime.local_player_yaw_deg() if _runtime != null else 0.0

func local_player_pitch_deg() -> float:
	return _runtime.local_player_pitch_deg() if _runtime != null else 0.0

func local_player_body_anim_slot() -> int:
	return _runtime.local_player_body_anim_slot() if _runtime != null else -1

func local_player_anim_key() -> String:
	return _runtime.local_player_anim_key() if _runtime != null else ""

func local_player_anim_phase_ticks() -> int:
	return _runtime.local_player_anim_phase_ticks() if _runtime != null else 0

func local_player_aim_overlay() -> PlayerAimOverlay:
	return _runtime.local_player_aim_overlay() if _runtime != null else null

func local_player_health() -> int:
	return _runtime.local_player_health() if _runtime != null else 0

func local_player_max_health() -> int:
	return _runtime.local_player_max_health() if _runtime != null else 100

func local_player_team() -> int:
	return _runtime.local_player_team() if _runtime != null else 0

func set_local_player_input(forward: bool, back: bool, left: bool, right: bool, lean_left: bool, lean_right: bool, jump: bool) -> void:
	if _runtime != null:
		_runtime.set_player_input(forward, back, left, right, lean_left, lean_right, jump)

## One frame of raw mouse pixels -> the sim-owned look (see mission_runtime).
func add_local_player_look(dx_px: float, dy_px: float) -> void:
	if _runtime != null:
		_runtime.add_player_look(dx_px, dy_px)

## Stance SELECT request: 0 stand / 1 crouch / 2 prone.
func request_local_player_stance(stance: int) -> bool:
	return _runtime.request_player_stance(stance) if _runtime != null else false

## Build a host-managed avatar model for the local player (which has no BMS placement of its
## own). The caller (LocalPlayerHost) positions it and swaps its visual layer per first/third
## person: in first person the body stays renderable on the reflection-only layer, because the
## witnessed water mirror re-renders the world scene, local body included
## [orig: Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene @ 0x5c1240]. Null
## when the resource root / item graphic is unavailable. 0x14B9 = player infantry [net-re §5.2b].
func build_local_player_avatar() -> Node3D:
	if _placer == null:
		return null
	# _env wires the TOD-reactive lighting/fog stamp — without it the avatar
	# freezes at the noon preview defaults (retail relights every entity per
	# frame [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]).
	return _placer.build_player_animated_model(0x14B9, self, _env)

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

## Armory apply, host side: point the FP viewmodel + action FSM at `weapon_name`.
## Validates against weapon.def; the caller (main_game) drops the old viewmodel so the
## per-frame pass rebuilds gun/arms/FSM from the new def [orig: the ACCEPT re-mount,
## WeaponLoadout_ApplyFromBuffer @0x565cd0 -> Player_MountWeaponSlot @0x4dfa40].
func set_local_player_weapon_by_name(weapon_name: String) -> bool:
	if weapon_name.is_empty():
		return false
	if _weapon_db == null:
		local_player_viewmodel_def()  # lazily loads weapon.def into _weapon_db
	var index: int = _weapon_db.find_weapon(weapon_name) \
			if _weapon_db != null and _weapon_db.is_loaded() else -1
	if index < 0:
		push_warning("GameWorld: armory weapon '%s' not in weapon.def — keeping current" % weapon_name)
		return false
	_viewmodel_weapon_override = weapon_name
	_viewmodel_weapon_cleared = false
	# Install the new weapon's FSM on the sim NOW — the mount is not hostage to the FP
	# model load [orig: the ACCEPT chain rebuilds the slot table + mounts with no
	# render dependency — WeaponSlotTable_LoadAllFromDefs @0x5414e0 +
	# Player_MountWeaponSlot @0x4dfa40 (camera/scope/switch-queue state only); the FP
	# model resolve is a separate per-frame consumer @0x4ded60]. Clip lengths bake in
	# again when the rebuilt viewmodel resolves (_setup_local_player_weapon); a model
	# that never loads leaves 'auto' delays collapsed instead of leaving the OLD
	# weapon's FSM live under the new entity stamp.
	_local_weapon_dict = _weapon_db.get_weapon(index)
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_weapon(_local_weapon_dict, {})
	return true

## Armory NONE: clear the equipped render/FSM state instead of falling back to the
## pre-armory default model on the next frame.
func clear_local_player_weapon() -> void:
	_viewmodel_weapon_override = ""
	_viewmodel_weapon_cleared = true
	_local_weapon_dict = {}
	var sim := get_sim()
	if sim != null:
		sim.clear_local_player_weapon()

func build_local_player_viewmodel() -> Node3D:
	if _placer == null:
		return null
	if _viewmodel_weapon_cleared:
		return null
	var container := Node3D.new()
	container.name = "PlayerViewmodel"
	add_child(container)
	# anim_wpn_idle = the FP holding pose; without it the arms sit in their bind/T-pose.
	# _env: the viewmodel lights/fogs with the live TOD like every entity
	# (retail draws the FP model through the same lighting constants
	# [orig: Player_RenderFirstPersonViewModel @ 0x4ded60 -> the ctx block]).
	var def := local_player_viewmodel_def()
	var gun_name := def.gfx1 if def != null and not def.gfx1.is_empty() else "ak47_1st"
	var arms_name := def.gfx1a if def != null and not def.gfx1a.is_empty() else "armsG"
	var adm_name := def.animadm if def != null and not def.animadm.is_empty() else "ak47_1st"
	# Both submits reuse the equipped GUN's model table, while `adm_name` supplies the clips.
	# Some valid retail sets differ (M21B_1st: 42 parts, M21_1st: 40); sizing from the ADM
	# basename truncates late animated parts such as the M14 magazine. [orig: @0x4ded60]
	var arms = _placer.build_model_from_graphic(arms_name, adm_name, container, "anim_wpn_idle", _env, gun_name)  # _placer untyped -> no :=
	var gun = _placer.build_model_from_graphic(gun_name, adm_name, container, "anim_wpn_idle", _env, gun_name)
	if arms == null:
		push_warning("GameWorld: FP arms model '%s' failed to load from the resource root" % arms_name)
	if gun == null:
		push_warning("GameWorld: FP gun model '%s' failed to load from the resource root" % gun_name)
	if arms == null and gun == null:
		container.queue_free()
		return null
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
	sim.set_local_player_weapon(_local_weapon_dict, clip_seconds)


## Per-frame weapon trigger state from the host: fire held + edge and the RAW reload
## edge — the dispatch gates (full-magazine/empty-reserve refusal) run in the sim
## [orig: the binding-149/reload input dispatch, Input_HandleActionBinding_0 @0x4e0420].
func set_local_player_weapon_input(fire_held: bool, fire_pressed: bool, reload_pressed: bool) -> void:
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_weapon_input(fire_held, fire_pressed, reload_pressed)


## The ADS toggle request; the sim applies the dispatcher gates and owns the engaged
## state [orig: input case 6 @0x4e0420; Player_ToggleWeaponScope @0x4df0c0].
func request_local_player_scope_toggle() -> bool:
	var sim := get_sim()
	return sim != null and bool(sim.request_local_player_scope_toggle())


## The host camera mode, driving the sim-side fov suppression + anchor chase
## [orig: g_camera_mode @0xA890C8].
func set_local_player_camera_third_person(third_person: bool) -> void:
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_camera_third_person(third_person)


## The host-sampled head-bone eye (Godot space) — the sim's 3P anchor-chase target
## [orig: ThirdPersonCamera_Update @0x437b70 target = Position + CameraOffset].
func set_local_player_eye(eye: Vector3, valid: bool) -> void:
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_eye(eye, valid)


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


## Register the host-side presenter for fixed-tick weapon events. The game and
## ONED both install LocalPlayerHost here; headless/runtime-only hosts leave it
## invalid and may drain the typed event queue explicitly.
func set_local_player_weapon_tick_consumer(consumer: Callable) -> void:
	_local_player_weapon_tick_consumer = consumer


## The resolved weapon.def record driving the FP viewmodel: model/adm names plus the
## witnessed view-bias fields (pos/tpos raw units + rot degrees, renderfov horizontal
## degrees) LocalPlayerHost consumes — decoded from NovaWeaponDatabase's transport dict
## at this edge (ADR 0017). Null when the mounted root has no weapon.def or the weapon
## name is absent — callers keep their witnessed JOX AK-47 defaults then. The weapon is
## DEFAULT_VIEWMODEL_WEAPON until equipped-weapon resolution lands; NOVA_VM_WEAPON
## overrides the name (debug: rig A/B against another SKU's def).
func local_player_viewmodel_def() -> PlayerViewmodelDef:
	if _viewmodel_weapon_cleared:
		return null
	if _weapon_db == null:
		if _resource_root == null:
			return null
		_weapon_db = NovaWeaponDatabase.new()
		if _weapon_db.load_from_resource_root(_resource_root, "weapon.def") != OK:
			push_warning("GameWorld: weapon.def unavailable (%s) — FP viewmodel keeps built-in defaults"
					% _weapon_db.get_last_error())
			return null
	if not _weapon_db.is_loaded():
		return null
	# Precedence: the armory-equipped weapon, else the NOVA_VM_WEAPON debug override,
	# else the fixed default until first equip.
	var weapon_name := _viewmodel_weapon_override
	if weapon_name.is_empty():
		weapon_name = OS.get_environment("NOVA_VM_WEAPON")
	if weapon_name.is_empty():
		weapon_name = DEFAULT_VIEWMODEL_WEAPON
	var index: int = _weapon_db.find_weapon(weapon_name)
	if index < 0:
		push_warning("GameWorld: weapon '%s' not in weapon.def — FP viewmodel keeps built-in defaults" % weapon_name)
		return null
	_local_weapon_dict = _weapon_db.get_weapon(index)
	return PlayerViewmodelDef.from_weapon_dict(_local_weapon_dict)


# --- Skeleton debug view (F3 overlay's "Show skeletons") ---------------------
# Build / free a child SkeletonDebugView that draws every character's bones over the world.
# Mirrors the editor's set_pick_debug -> _refresh_pick_debug build/free toggle flow.

func set_skeleton_debug(enabled: bool) -> void:
	_skeleton_debug = enabled
	_refresh_skeleton_debug()

func is_skeleton_debug() -> bool:
	return _skeleton_debug

func _refresh_skeleton_debug() -> void:
	var existing := get_node_or_null(NodePath(SKELETON_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not _skeleton_debug:
		return
	var view := SkeletonDebugView.new()
	view.name = SKELETON_DEBUG_NAME
	add_child(view)
	view.setup(self)  # walks this GameWorld's subtree for Skeleton3D nodes each frame


# --- User-point debug view (F3 overlay's "Show user points") ------------------
# Live models are discovered under this world. Static mission objects have no
# per-entity nodes after batching, so the placer supplies the exact grouped
# placement-time sources that successfully rendered.

func set_user_point_debug(enabled: bool) -> void:
	_user_point_debug = enabled
	_refresh_user_point_debug()


func is_user_point_debug() -> bool:
	return _user_point_debug


func _refresh_user_point_debug() -> void:
	_remove_user_point_debug_view()
	if not _user_point_debug:
		return
	var view := UserPointDebugView.new()
	view.name = USER_POINT_DEBUG_NAME
	add_child(view)
	var static_sources: Array = []
	if _placer != null:
		static_sources = _placer.get_static_user_point_sources()
	view.setup(self, static_sources)


func _remove_user_point_debug_view() -> void:
	var existing := get_node_or_null(NodePath(USER_POINT_DEBUG_NAME))
	if existing == null:
		return
	# Detach before deferred destruction so an unload+reload in one frame can
	# create the same stable child name without stale-source/name collisions.
	remove_child(existing)
	existing.queue_free()


# --- Collision debug view (F3 overlay's "Show collision") --------------------
# Build / free a child CollisionDebugView drawing the sim's collision volumes +
# the local player's capsule test points over the world. Same build/free toggle
# flow as the skeleton view; the view re-resolves the sim through this
# GameWorld every frame, so mission reloads never leave it stale.

func set_collision_debug(enabled: bool) -> void:
	_collision_debug = enabled
	_refresh_collision_debug()

func is_collision_debug() -> bool:
	return _collision_debug


## The F3 overlay's Particles tab seams (the existing get_effect_world() is
## the data source; these are the two debug toggles).
func set_particles_hidden(hidden: bool) -> void:
	var was_hidden := _particles_hidden
	_particles_hidden = hidden
	if _effect_world != null:
		_effect_world.set_particles_hidden(hidden)
	if was_hidden and not hidden:
		_retry_pending_item_effects()


# Build / free a child ParticleDebugView drawing every live emitter's bounds +
# effect name, on the overlay's "Show effect boxes" toggle (the collision-view
# contract; survives mission reloads by re-resolving the effect world).
func set_particle_debug(enabled: bool) -> void:
	_particle_debug = enabled
	var existing := get_node_or_null(NodePath(PARTICLE_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled:
		return
	var view := ParticleDebugView.new()
	view.name = PARTICLE_DEBUG_NAME
	add_child(view)
	var ref: WeakRef = weakref(self)
	view.setup(func():
		var world = ref.get_ref()
		return world.get_effect_world() if world != null else null)

func _refresh_collision_debug() -> void:
	var existing := get_node_or_null(NodePath(COLLISION_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not _collision_debug:
		return
	var view := CollisionDebugView.new()
	view.name = COLLISION_DEBUG_NAME
	add_child(view)
	view.setup(self)  # duck-typed get_sim(), re-resolved per frame


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
# drives it explicitly from tick() (self_tick off); its drained side effects route through
# _on_runtime_effects. A reload reuses this GameWorld, so any prior runtime is freed in unload() first.
func _start_runtime(mission: NovaMissionData, bms_name: String) -> void:
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	_runtime = MissionRuntime.new()
	_runtime.name = "MissionRuntime"
	add_child(_runtime)
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
	opts["playable"] = _playable and not bool(_host_config.get("dedicated", false))
	# A LAN host start threads its config (server name, mission rotation, player cap, and the
	# socket transport mode) through to the listen server. A LAN JOINER threads the dial target
	# (host_ip/port/player_name) and is NOT a listen server. Consumed once per load; absent for
	# a normal single-player start, which keeps the in-process (socketless) listen server.
	if not _host_config.is_empty():
		if String(_host_config.get("net_transport", "")) != "lan-join":
			opts["listen_server"] = true
		for k in ["server_name", "max_players", "game_type", "gametype", "net_transport", "bind_port",
				"advertise", "host_ip", "port", "player_name",
				"nw_gate_host", "nw_gate_port", "region", "dedicated", "channel"]:
			if _host_config.has(k):
				opts[k] = _host_config[k]
		_host_config = {}
	# The placer + environment node let the joiner's wire present pass resolve + light its
	# remote-entity avatars (build_player_animated_model); unused by the host present path.
	opts["placer"] = _placer
	opts["env_node"] = _env
	# The fire present pass's providers (AI/remote fire sound + muzzle + tracers): audio
	# and effect world resolve lazily (mission audio is set up after the runtime), the
	# listener is the same camera position the audio render pass ticks with.
	opts["fire_audio"] = Callable(self, "get_mission_audio")
	opts["fire_fx"] = Callable(self, "get_effect_world")
	opts["fire_listener"] = Callable(self, "_fire_listener_position")
	_runtime.setup(mission, container, opts)
	if _runtime.get_sim() == null:
		push_warning("GameWorld: failed to start mission runtime")
	_runtime.effects_drained.connect(_on_runtime_effects)
	_runtime.fixed_tick_completed.connect(_on_runtime_fixed_tick)
	_runtime.simulation_restarted.connect(_on_runtime_simulation_restarted)
	# A browsable listen host: register it with the NovaWorld gate (F1), if one was
	# configured. No-op for single-player, joiners, and pure-LAN play.
	_maybe_start_nw_host(opts, bms_name)
	# The game starts running (tick() gates on is_playing, so the overlay's
	# transport can pause/step a live mission).
	_runtime.play()


# Register a browsable listen host with the NovaWorld gate (F1, ADR 0010). The host-direction
# sibling of the joiner's NovaWorldClient: it runs the NWU lobby handshake to the gate, then
# ClientHostRequest + ClientHostUpdate heartbeats so the host shows in /api/hosts + the retail
# server browser. Gated so it only fires for a real LAN listen server WITH a gate configured —
# single-player, joiners, and pure-LAN play (no nw_gate_host) all skip it, unchanged.
func _maybe_start_nw_host(opts: Dictionary, bms_name: String) -> void:
	if not bool(opts.get("listen_server", false)):
		return
	if String(opts.get("net_transport", "")) != "lan":
		return
	# Register with the NovaWorld gate when the host picked the NovaWorld channel (a browsable
	# online host) or when a gate was injected via env (NW_GATE_HOST). The in-match wire is the
	# SAME either way (net_transport "lan"); only discovery/registration differs (LAN vs NovaWorld).
	var channel := String(opts.get("channel", "LAN"))
	var gate_host := String(opts.get("nw_gate_host", ""))
	if gate_host.is_empty():
		if channel == "NovaWorld":
			push_warning("GameWorld: NovaWorld host requested but no gate address (nw_gate_host) — gate registration skipped; host is LAN-reachable only")
		return  # no gate configured -> pure LAN, nothing to register with
	if not ClassDB.class_exists("NovaWorldHost"):
		push_warning("GameWorld: NovaWorldHost unavailable; host is LAN-only (not browsable)")
		return
	var sim = _runtime.get_sim() if _runtime != null and _runtime.has_method("get_sim") else null
	if sim == null or not sim.has_method("is_host_listening") or not sim.is_host_listening():
		return  # the listen socket never came up; nothing reachable to advertise
	_nw_host = ClassDB.instantiate("NovaWorldHost")
	add_child(_nw_host)
	_nw_host.host = gate_host
	_nw_host.gate_port = int(opts.get("nw_gate_port", 7597))
	_nw_host.server_name = String(opts.get("server_name", "OpenNova Host"))
	_nw_host.mission_name = bms_name.get_basename()
	_nw_host.max_players = int(opts.get("max_players", 32))
	# The actually-bound game port the joiner will dial (not the requested bind_port).
	_nw_host.game_port = sim.get_host_listen_port() if sim.has_method("get_host_listen_port") else int(opts.get("bind_port", 32768))
	_nw_host.region = String(opts.get("region", "us"))
	_nw_host.player_name = String(opts.get("player_name", "Host"))
	var adv := String(opts.get("advertise", ""))
	if not adv.is_empty():
		_nw_host.advertise_ip = adv
	if _nw_host.has_signal("registered"):
		_nw_host.registered.connect(_on_nw_host_registered)
	if _nw_host.has_signal("error_occurred"):
		_nw_host.error_occurred.connect(_on_nw_host_error)
	_nw_host.start()


func _on_nw_host_registered() -> void:
	print("GameWorld: listen host registered with the NovaWorld gate (browsable)")


func _on_nw_host_error(message: String) -> void:
	push_warning("GameWorld: NovaWorld host registration error: %s" % message)


# Consume render-internal lifecycle effects first, route "dialog" actions to
# mission audio (resolved through the co-named .DBF + LWF set), then expose only
# the remaining host-facing effects to HUD consumers.
func _on_runtime_effects(effects: Array) -> void:
	var routed: Array = []
	for effect_v in effects:
		if effect_v is Dictionary:
			var effect: Dictionary = effect_v
			if _consume_item_fx_control_effect(effect):
				continue
		routed.append(effect_v)
	if routed.is_empty():
		return
	_route_mission_effects(routed)
	mission_effects.emit(routed)



func _on_runtime_fixed_tick(_logic_tick: int) -> void:
	# Retail executes local weapon actions and physical impacts before the same
	# frame's global particle update. Consume each source tick synchronously so
	# admission slots, first emission, and catch-up chronology are exact; only
	# mission render Nodes remain batched until tick_realtime() returns.
	if _local_player_weapon_tick_consumer.is_valid():
		_local_player_weapon_tick_consumer.call(drain_local_player_weapon_events())
	_route_round_impacts()
	if _effect_world != null and _effect_world.has_method("advance_fixed_tick"):
		_effect_world.advance_fixed_tick(MissionRuntime.TICK_DT)


func _on_runtime_simulation_restarted() -> void:
	if _effect_world == null:
		return
	_effect_world.reset_runtime_state()
	# Persistent item effects belong to the restored entity set, not the scene
	# that was just discarded. Re-register their admission and owner identities;
	# restore emits fresh controller-start lifecycle events for occupied baselines.
	_attach_item_effects()


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
	# [orig: Sound_ApplyOcclusionDistance @ 0x529970]; PIE/menu hosts without a
	# sim mix unoccluded.
	_mission_audio.set_simulation(get_sim())
	var stats := _mission_audio.setup(mission, bms_name, self)
	if _env != null and _env.get("time_of_day") != null:
		_mission_audio.set_time_of_day_hhmm(float(_env.get("time_of_day")))
	print("GameWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d voice(s)" % [
		int(stats.get("markers_resolved", 0)),
		int(stats.get("markers_total", 0)),
		int(stats.get("banks_loaded", 0)),
		int(stats.get("voices", 0)),
	])
	# Open the GAME music context + seed the witnessed vars [orig: Game_StartMission
	# @ 0x525581-0x52561b]. Retail gates the open on is_mp_session_peer and STOPS
	# music in single-player; ours opens in ALL sessions — D-MUS-SPGATE
	# (docs/audio/mus-sbf-re.md §Game music driving; SP-as-listen-server, ADR
	# 0009/0011/0012). gamemus's discriminator Var1 stays 0 (never written in
	# retail), so the Multiplayerstart P0 loop plays.
	NovaMusicService.open_game_context(_resource_root)


# Mission-start load of EVERY mounted .ptl into the runtime effect world
# [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
# file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
func _start_effect_world() -> void:
	_effect_world = NovaEffectWorld.new()
	_effect_world.name = "EffectWorld"
	add_child(_effect_world)
	_effect_world.set_environment_source(_env)
	if _particles_hidden:
		_effect_world.set_particles_hidden(true)
	var count := _effect_world.load_from_resource_root(_resource_root)
	if _water != null:
		_effect_world.set_water_height(float(_water.water_height))
	# One provider for every owned/attached group: int keys are WAC fx2ssn SSNs
	# (resolved through the runtime), String keys are the per-item effect attaches
	# (resolved to the placed node's live transform).
	_effect_world.set_owner_position_provider(Callable(self, "_effect_owner_transform"))
	print("GameWorld: effect world — %d effect(s) across %d .ptl file(s)" % [
		count, _effect_world.file_count()])
	_attach_item_effects()
	if _runtime != null and _runtime.has_method("set_wire_node_spawned_callback"):
		_runtime.set_wire_node_spawned_callback(Callable(self, "_on_wire_node_spawned"))


func get_effect_world() -> NovaEffectWorld:
	return _effect_world


# Owner-transform provider for the effect world's owned/attached groups. Int keys are
# WAC fx2ssn SSNs (the runtime resolves the live entity transform; null = entity gone,
# the group detaches [orig: CEffect_UpdateEmitterTransform @ 0x5f7410]); String keys
# are the per-item effect attaches registered by _attach_item_effects (the placed
# entity's current value snapshot. The Node remains only as a pre-first-tick
# seed and lifetime fallback for non-sim-owned callers.
func _effect_owner_transform(owner_key: Variant) -> Variant:
	# Host-owned live anchors first (the local weapon flash follows its viewmodel
	# userpoint for the emitter group's whole life [orig: the actionEffectHandle
	# per-tick tracker in WeaponAction_ProcessFrame @ 0x540edf]).
	var anchor: Variant = _effect_anchor_resolvers.get(owner_key)
	if anchor is Callable:
		var resolver := anchor as Callable
		if resolver.is_valid():
			return resolver.call()
		_effect_anchor_resolvers.erase(owner_key)
		return null
	if owner_key is String:
		# Untyped on purpose: assigning a FREED instance to a typed Node3D var raises
		# before any is_instance_valid guard could run.
		var node: Variant = _item_fx_nodes.get(owner_key)
		if node is Node3D and is_instance_valid(node) and node.is_inside_tree():
			var entity_ref: Dictionary = _item_fx_owner_refs.get(owner_key, {})
			if not entity_ref.is_empty() and _runtime != null \
					and _runtime.has_method("has_current_present_effect_snapshot") \
					and _runtime.has_current_present_effect_snapshot() \
					and _runtime.has_method("presented_entity_effect_transform"):
				# Null here means the identity left THIS tick's client view. Do
				# not fall back to the one-frame-old Node or the group would emit
				# once more from stale state before the batched present frees it.
				return _runtime.presented_entity_effect_transform(entity_ref)
			return (node as Node3D).global_transform
		_item_fx_nodes.erase(owner_key)
		_item_fx_owner_refs.erase(owner_key)
		return null
	if _runtime != null and _runtime.has_method("entity_effect_transform_for_ssn"):
		return _runtime.entity_effect_transform_for_ssn(owner_key)
	return null


# owner key -> Callable returning the live anchor Transform3D (or null once
# stale) for host-owned owner-bound effect groups; consulted before the
# item-fx/SSN legs by _effect_owner_transform.
var _effect_anchor_resolvers: Dictionary = {}


## A host registers a live pose resolver for an owner-bound effect group it
## spawned (e.g. the local muzzle flash riding the viewmodel userpoint). The
## resolver is polled by the effect world's owner-pose sync while any group
## bound to owner_key is alive; re-registering the same key overwrites.
func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
	_effect_anchor_resolvers[owner_key] = resolver


func unregister_effect_anchor(owner_key: Variant) -> void:
	_effect_anchor_resolvers.erase(owner_key)


# owner key (String) -> presented Node3D, for the per-item attached effect groups.
var _item_fx_nodes: Dictionary = {}
# owner key -> copied entity_ref value identity (bms/origin or wire handle).
var _item_fx_owner_refs: Dictionary = {}
var _item_fx_registered_nodes: Dictionary = {}
var _item_fx_pending_nodes: Dictionary = {}
var _item_fx_registered_static: Dictionary = {}
var _item_fx_pending_static: Dictionary = {}
# Controller/Driver-only PlayerControl item effects are dormant at mission
# startup. Portable lifecycle events activate them without polling.
# identity alias -> true while the vehicle has any controlling occupant
var _item_fx_control_active: Dictionary = {}
# Node instance id -> {node, kind, item_id, aliases}
var _item_fx_control_nodes: Dictionary = {}
# Node instance id -> {group_ids, owner_keys}
var _item_fx_control_instances: Dictionary = {}


# Mission-start attach of the per-item ITEMS.DEF effects — slot A ('particlefx
# <effect> <userpoint>') only: for every presented animated entity whose item def
# authors it, spawn one entity-attached emitter at EVERY model userpoint matching
# the authored name — exact case-insensitive match over the model's first 16
# userpoints, duplicate names all match (a 16-bit mask in the original). Static
# MultiMesh entities use the placer's value descriptors and spawn the same authored
# effects world-bound at their final placement transform; no owner/render node is
# synthesized for them.
# Pool gates are kind-sensitive: pool 0 organics are excluded; pool 1 items skip
# attrib 0x42; pools 2/3 skip attrib 0x2. The fxs/fxw1..4 wake tiers and the
# death/fire/other family are movement/damage-state driven and stay unrouted
# (ptl-format-re.md §8).
# [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 (mission start,
#  entity pools 1-3) -> ItemDef_GetBoneMaskByName @ 0x49ea40 (first 16, stricmp) ->
#  Entity_SpawnBoneTrailEffect @ 0x43bef0 (one mode-2 attached emitter per masked
#  userpoint: pos = the userpoint, forward = its direction)]
func _attach_item_effects() -> void:
	_item_fx_nodes.clear()
	_item_fx_owner_refs.clear()
	_item_fx_registered_nodes.clear()
	_item_fx_pending_nodes.clear()
	_item_fx_registered_static.clear()
	_item_fx_pending_static.clear()
	_item_fx_control_active.clear()
	_item_fx_control_nodes.clear()
	_item_fx_control_instances.clear()
	if _effect_world == null or _placer == null:
		return
	var item_db = _placer.get_item_db()
	if item_db == null:
		return
	var attached := 0
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	if container != null:
		for child in container.get_children():
			var node := child as Node3D
			if node == null or not node.has_meta("entity_ref"):
				continue
			var ref: Dictionary = node.get_meta("entity_ref")
			attached += _attach_item_effect_to_node(node, int(ref.get("kind", -1)),
					int(ref.get("item_id", 0)), item_db)
	var static_sources: Array = _placer.get_static_item_effect_sources()
	for source_index in range(static_sources.size()):
		attached += _attach_item_effect_to_static(
				static_sources[source_index], source_index, item_db)
	if attached > 0:
		print("GameWorld: item effects — %d emitter(s)" % attached)


func _on_wire_node_spawned(node: Node3D, kind: int, item_id: int) -> void:
	_attach_item_effect_to_node(node, kind, item_id)


# The original walks entity pools 1-3 with distinct attrib masks. The imported
# mission kind enum is Marker=0, Item=1, Building=2, Organic=3.
func _item_effect_pool_allows(kind: int, attrib: int) -> bool:
	if kind == NovaMissionData.KIND_ITEM:
		return (attrib & 0x42) == 0
	if kind == NovaMissionData.KIND_BUILDING or kind == NovaMissionData.KIND_MARKER:
		return (attrib & 0x2) == 0
	return false


func _item_effect_controller_allows(kind: int, attrib: int) -> bool:
	# The occupied-controller pass bypasses only PlayerControl (0x40). The
	# independent 0x2 exclusion remains intact.
	return kind == NovaMissionData.KIND_ITEM and (attrib & 0x42) == 0x40


func _item_fx_identity_aliases(net_id: int, bms_id: int,
		spawn_origin: int) -> Array[String]:
	var aliases: Array[String] = []
	if net_id > 0:
		aliases.append("net:%d" % net_id)
	if bms_id > 0:
		aliases.append("bms:%d" % bms_id)
	if spawn_origin > 0:
		aliases.append("origin:%d" % spawn_origin)
	return aliases


func _item_fx_control_event_aliases(effect: Dictionary) -> Array[String]:
	return _item_fx_identity_aliases(
			int(effect.get("a", 0)),
			int(effect.get("b", 0)),
			int(effect.get("c", 0)))


func _item_fx_control_node_aliases(node: Node3D) -> Array[String]:
	if node == null:
		return []
	var ref: Dictionary = node.get_meta("entity_ref", {})
	var net_id := int(ref.get("net_id", 0)) if ref.has("net_id") else 0
	var bms_id := int(ref.get("bms_id", 0))
	var origin_kind := int(ref.get("origin_kind", ref.get("kind", -1)))
	var index := int(ref.get("index", -1))
	var spawn_origin := 0
	if origin_kind >= 0 and index >= 0:
		spawn_origin = ((origin_kind & 0xff) << 24) | (index & 0xffffff)
	return _item_fx_identity_aliases(net_id, bms_id, spawn_origin)


func _item_fx_aliases_intersect(left: Array, right: Array) -> bool:
	for alias_v in left:
		if right.has(alias_v):
			return true
	return false


func _item_fx_control_node_is_active(entry: Dictionary) -> bool:
	for alias_v in entry.get("aliases", []):
		if _item_fx_control_active.has(String(alias_v)):
			return true
	return false


func _register_item_fx_control_node(node: Node3D, kind: int,
		item_id: int) -> Dictionary:
	var aliases := _item_fx_control_node_aliases(node)
	if aliases.is_empty():
		return {}
	var entry := {
		"node": node,
		"kind": kind,
		"item_id": item_id,
		"aliases": aliases,
	}
	_item_fx_control_nodes[node.get_instance_id()] = entry
	return entry


func _track_item_fx_control_spawn(node_id: int, owner_key: String,
		receipt: Dictionary) -> void:
	var instance: Dictionary = _item_fx_control_instances.get(node_id, {
		"group_ids": [],
		"owner_keys": [],
	})
	var group_ids: Array = instance.get("group_ids", [])
	var group_id := int(receipt.get("group_id", 0))
	if group_id > 0 and not group_ids.has(group_id):
		group_ids.append(group_id)
	var owner_keys: Array = instance.get("owner_keys", [])
	if not owner_keys.has(owner_key):
		owner_keys.append(owner_key)
	instance["group_ids"] = group_ids
	instance["owner_keys"] = owner_keys
	_item_fx_control_instances[node_id] = instance


func _stop_item_fx_control_node(node_id: int) -> void:
	var instance: Dictionary = _item_fx_control_instances.get(node_id, {})
	if _effect_world != null:
		for group_id_v in instance.get("group_ids", []):
			var group_id := int(group_id_v)
			if group_id > 0:
				_effect_world.stop_group(group_id)
	for owner_key_v in instance.get("owner_keys", []):
		var owner_key := String(owner_key_v)
		_item_fx_nodes.erase(owner_key)
		_item_fx_owner_refs.erase(owner_key)
	_item_fx_control_instances.erase(node_id)
	_item_fx_registered_nodes.erase(node_id)
	_item_fx_pending_nodes.erase(node_id)


func _activate_item_fx_control_nodes(event_aliases: Array) -> void:
	for node_id_v in _item_fx_control_nodes.keys().duplicate():
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
		if not _item_fx_aliases_intersect(entry.get("aliases", []), event_aliases):
			continue
		var node_v: Variant = entry.get("node")
		if not is_instance_valid(node_v) or not (node_v is Node3D):
			_stop_item_fx_control_node(node_id)
			_item_fx_control_nodes.erase(node_id)
			continue
		if not _item_fx_control_node_is_active(entry):
			continue
		_attach_item_effect_to_node(
				node_v as Node3D,
				int(entry.get("kind", -1)),
				int(entry.get("item_id", 0)),
				null,
				true)


func _deactivate_item_fx_control_nodes(event_aliases: Array) -> void:
	for node_id_v in _item_fx_control_nodes.keys().duplicate():
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
		if not _item_fx_aliases_intersect(entry.get("aliases", []), event_aliases):
			continue
		if not _item_fx_control_node_is_active(entry):
			_stop_item_fx_control_node(node_id)


func _consume_item_fx_control_effect(effect: Dictionary) -> bool:
	var kind := String(effect.get("kind", ""))
	if kind != "vehicle_control_started" and kind != "vehicle_control_stopped":
		return false
	var aliases := _item_fx_control_event_aliases(effect)
	if kind == "vehicle_control_started":
		for alias in aliases:
			_item_fx_control_active[alias] = true
		_activate_item_fx_control_nodes(aliases)
	else:
		for alias in aliases:
			_item_fx_control_active.erase(alias)
		_deactivate_item_fx_control_nodes(aliases)
	return true


func _attach_item_effect_to_node(node: Node3D, kind: int, item_id: int,
		item_db_override: Variant = null,
		controller_active: bool = false) -> int:
	if _effect_world == null or _placer == null or node == null or item_id <= 0:
		return 0
	if not node.has_method("get_object_data"):
		return 0
	var node_id := node.get_instance_id()
	var registered: Variant = _item_fx_registered_nodes.get(node_id)
	if registered is Node and is_instance_valid(registered):
		return 0
	var item_db: Variant = item_db_override
	if item_db == null:
		item_db = _placer.get_item_db()
	if item_db == null:
		return 0
	var attrib := int(item_db.get_attrib(item_id))
	if controller_active:
		if not _item_effect_controller_allows(kind, attrib):
			return 0
	else:
		if not _item_effect_pool_allows(kind, attrib):
			if not _item_effect_controller_allows(kind, attrib):
				return 0
			var control_entry := _register_item_fx_control_node(node, kind, item_id)
			if not control_entry.is_empty() and _item_fx_control_node_is_active(control_entry):
				return _attach_item_effect_to_node(node, kind, item_id, item_db, true)
			return 0
	var fx: Dictionary = item_db.get_particle_effects(item_id).get("particlefx", {})
	var effect := String(fx.get("effect", ""))
	var userpoint := String(fx.get("userpoint", ""))
	if effect.is_empty():
		return 0
	var data = node.get_object_data()
	if data == null:
		return 0
	if _effect_world.are_particles_hidden():
		# The retail master switch makes every spawn facade a no-op. Remember
		# persistent item attachments so re-enabling after a hidden mission load
		# creates them exactly once instead of losing them for the mission.
		_item_fx_pending_nodes[node_id] = {
			"node": node,
			"kind": kind,
			"item_id": item_id,
			"controller_active": controller_active,
		}
		return 0
	var attached := 0
	var matched := 0
	var entity_ref: Dictionary = node.get_meta("entity_ref", {}).duplicate()
	if not userpoint.is_empty():
		var points := mini(data.get_user_point_count(), ITEM_EFFECT_USER_POINT_SCAN_LIMIT)
		for i in range(points):
			var info: Dictionary = data.get_user_point_info(i)
			if String(info.get("name", "")).nocasecmp_to(userpoint) != 0:
				continue
			var key := "itemfx:%d:%d" % [node_id, i]
			var receipt: Dictionary = _effect_world.spawn_effect_attached_request(
					key, effect, node.global_transform,
					Vector3(info.get("position", Vector3.ZERO)),
					Vector3(info.get("rotation", Vector3.ZERO)))
			if bool(receipt.get("spawned", false)):
				_item_fx_nodes[key] = node
				_item_fx_owner_refs[key] = entity_ref
				if controller_active:
					_track_item_fx_control_spawn(node_id, key, receipt)
				attached += 1
			matched += 1
	if matched == 0:
		# No matched point (or no authored point name): the original still spawns
		# ONE emitter at the entity origin — the spawn_count==0 leg
		# [orig: Entity_SpawnBoneTrailEffect @ 0x43c097 -> submit_effect_descriptor
		#  @ 0x43c0a4 at entity->Position].
		var key := "itemfx:%d:origin" % node_id
		var receipt: Dictionary = _effect_world.spawn_effect_attached_request(
				key, effect, node.global_transform, Vector3.ZERO, Vector3.ZERO)
		if bool(receipt.get("spawned", false)):
			_item_fx_nodes[key] = node
			_item_fx_owner_refs[key] = entity_ref
			if controller_active:
				_track_item_fx_control_spawn(node_id, key, receipt)
			attached += 1
	if attached > 0:
		_item_fx_registered_nodes[node_id] = node
		_item_fx_pending_nodes.erase(node_id)
	return attached


# Convert the authored userpoint forward vector into the same local pose used by
# NovaEffectWorld.spawn_effect_attached. Static sources then compose this once
# with their placement transform and submit it as a World-bound request.
func _item_effect_local_pose(position: Vector3, forward_value: Vector3) -> Transform3D:
	if forward_value.length_squared() <= 0.000001:
		return Transform3D(Basis.IDENTITY, position)
	var forward := forward_value.normalized()
	var up_hint := Vector3.UP
	if absf(forward.dot(up_hint)) > 0.999:
		up_hint = Vector3.RIGHT
	var right := up_hint.cross(forward).normalized()
	var up := forward.cross(right).normalized()
	return Transform3D(Basis(right, up, forward), position)


func _spawn_static_item_effect(effect: String, transform: Transform3D) -> bool:
	var receipt: Dictionary = _effect_world.spawn_effect_request(effect, transform, {
		"admission": NovaEffectScene.ADMISSION_ALWAYS,
		"binding": NovaEffectScene.BINDING_WORLD,
		"render_domain": NovaEffectScene.RENDER_DOMAIN_WORLD,
	})
	return bool(receipt.get("spawned", false))


func _attach_item_effect_to_static(source: Dictionary, source_index: int,
		item_db_override: Variant = null) -> int:
	if _effect_world == null or _placer == null or source_index < 0:
		return 0
	if _item_fx_registered_static.has(source_index):
		return 0
	var item_id := int(source.get("item_id", 0))
	var kind := int(source.get("kind", -1))
	if item_id <= 0:
		return 0
	var item_db: Variant = item_db_override
	if item_db == null:
		item_db = _placer.get_item_db()
	if item_db == null or not _item_effect_pool_allows(kind, item_db.get_attrib(item_id)):
		return 0
	var fx: Dictionary = item_db.get_particle_effects(item_id).get("particlefx", {})
	var effect := String(fx.get("effect", ""))
	var userpoint := String(fx.get("userpoint", ""))
	var data: Variant = source.get("object_data")
	if effect.is_empty() or data == null:
		return 0
	if _effect_world.are_particles_hidden():
		_item_fx_pending_static[source_index] = source.duplicate()
		return 0
	var entity_transform: Transform3D = source.get(
			"world_transform", Transform3D.IDENTITY)
	var attached := 0
	var matched := 0
	if not userpoint.is_empty():
		var points := mini(data.get_user_point_count(), ITEM_EFFECT_USER_POINT_SCAN_LIMIT)
		for i in range(points):
			var info: Dictionary = data.get_user_point_info(i)
			if String(info.get("name", "")).nocasecmp_to(userpoint) != 0:
				continue
			var local_pose := _item_effect_local_pose(
					Vector3(info.get("position", Vector3.ZERO)),
					Vector3(info.get("rotation", Vector3.ZERO)))
			if _spawn_static_item_effect(effect, entity_transform * local_pose):
				attached += 1
			matched += 1
	if matched == 0:
		# The spawn_count==0 leg uses the entity origin. Preserve the entity basis,
		# matching an attached origin pose at the moment it becomes world-bound.
		if _spawn_static_item_effect(effect, entity_transform):
			attached += 1
	if attached > 0:
		_item_fx_registered_static[source_index] = true
		_item_fx_pending_static.erase(source_index)
	return attached


func _retry_pending_item_effects() -> void:
	if _effect_world == null or _effect_world.are_particles_hidden():
		return
	var pending_ids := _item_fx_pending_nodes.keys().duplicate()
	for node_id_v in pending_ids:
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_pending_nodes.get(node_id, {})
		# A wire node may have despawned while particles were disabled. Keep the
		# freed-object Variant untyped until after the validity guard; a typed cast
		# can raise before is_instance_valid gets a chance to reject it.
		var node_v: Variant = entry.get("node")
		if not is_instance_valid(node_v) or not (node_v is Node3D):
			_item_fx_pending_nodes.erase(node_id)
			continue
		var node := node_v as Node3D
		var controller_active := bool(entry.get("controller_active", false))
		if controller_active:
			var control_entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
			if control_entry.is_empty() or not _item_fx_control_node_is_active(control_entry):
				_item_fx_pending_nodes.erase(node_id)
				continue
		_attach_item_effect_to_node(node, int(entry.get("kind", -1)),
				int(entry.get("item_id", 0)), null, controller_active)
	var static_ids := _item_fx_pending_static.keys().duplicate()
	for source_index_v in static_ids:
		var source_index := int(source_index_v)
		var source: Dictionary = _item_fx_pending_static.get(source_index, {})
		_attach_item_effect_to_static(source, source_index)


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
	if not has_local_player():
		return
	var max_h := local_player_max_health()
	var cur_h := local_player_health()
	NovaMusicService.set_var(NovaMusicService.VAR_HEALTH_PCT,
		(cur_h * 100 / max_h) if max_h > cur_h else 100)
	NovaMusicService.set_var(NovaMusicService.VAR_TEAM, local_player_team())


# --- Blink frame gates (docs/render/render-occlusion-re.md §4) -----------------
# The local player's accumulated blink letters gate whole render passes. The
# letters are authored PER BOX (init 0x3E; letters clear bits), so windowed
# buildings simply don't carry the indoors letter and keep the outside world
# rendering — no portal special-casing at the gate level. The per-section
# interior visibility masks (the portal traversal) are the next occlusion slice.

func _apply_blink_frame_gates() -> void:
	# Duck-typed like the silhouette-anchor pull above: harness runtimes supply
	# value-only sims without weakening get_sim()'s NovaSimulation contract.
	if _runtime == null or not _runtime.has_method("get_sim"):
		return
	var sim = _runtime.get_sim()
	if sim == null or not sim.has_method("local_player_blink_flags"):
		return
	var flags := int(sim.local_player_blink_flags())
	# Accum bit 0x2 (indoors): the terrain render is skipped entirely — the
	# near-detail and far-foliage tiers are terrain children here, matching
	# retail where the detail cells ride the skipped terrain traversal and the
	# far patches carry their own bit-2 gate — and the skybox pass (dome +
	# celestials) is skipped [orig: render_main_scene @ 0x5c1353 (PolyTrn
	# skip), terrain_scene_render @ 0x5d0570, Terrain_RenderSkyboxPass skip
	# @ 0x5ca84f, Foliage_RenderFarPatchesPass skips @ 0x5c95bf/0x5c9665].
	var indoors := (flags & 0x2) != 0
	if indoors != _blink_indoors:
		_blink_indoors = indoors
		if _terrain != null:
			_terrain.visible = not indoors
		var sky := get_node_or_null("NovaSky")
		if sky != null:
			sky.visible = not indoors
		var celestial := get_node_or_null("NovaCelestial")
		if celestial != null:
			celestial.visible = not indoors
	# Accum bit 0x8 (the authored water letter): both water passes skipped.
	# Letter bits only accumulate while inside a box, so the outdoors leg of
	# retail's override is implicit; the remaining g_BlinkWaterVisible legs
	# (a camera building straddling the water plane, the window latch) ride
	# the section-mask slice [orig: Terrain_RenderSceneWithReflection
	# @ 0x5c93cb / @ 0x5c95d2-0x5c95ea].
	var water_off := (flags & 0x8) != 0
	if water_off != _blink_water_suppressed:
		_blink_water_suppressed = water_off
		if _water != null:
			_water.visible = not water_off


func _reset_blink_frame_gates() -> void:
	if _blink_indoors:
		if _terrain != null:
			_terrain.visible = true
		var sky := get_node_or_null("NovaSky")
		if sky != null:
			sky.visible = true
		var celestial := get_node_or_null("NovaCelestial")
		if celestial != null:
			celestial.visible = true
	if _blink_water_suppressed and _water != null:
		_water.visible = true
	_blink_indoors = false
	_blink_water_suppressed = false


# --- Frame clear color (env divergence #21, closed) ----------------------------

func _process(_delta: float) -> void:
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
	if _blink_indoors:
		if _clear_env_generation != -2:
			_clear_env_generation = -2
			_clear_color.environment.background_color = Color.BLACK
		return
	var above := true
	var cam := get_viewport().get_camera_3d() if is_inside_tree() else null
	if cam != null and _water != null:
		above = cam.global_position.y > float(_water.water_height)
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
