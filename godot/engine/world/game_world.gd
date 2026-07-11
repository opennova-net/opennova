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
const CollisionDebugView := preload("res://engine/debug/collision_debug_view.gd")
# The shared interactive-music service script (constants) — the live service is the
# NovaMusicService autoload, resolved via the tree in _music_service() so the engine
# layer never names the shell autoload identifier (host-neutral; -s probes and
# headless flows run without autoloads and must still compile).
const MusicServiceScript := preload("res://engine/world/nova_music_service.gd")
const NET_CONTAINER_NAME := "NetObjects"
const SKELETON_DEBUG_NAME := "SkeletonDebug"
const COLLISION_DEBUG_NAME := "CollisionDebug"
const TICK_DT := 1.0 / 62.5  # mirrors MissionRuntime.TICK_DT; default for tick()'s delta param

signal world_loaded()
signal load_failed(reason: String)
# Host-presentation side effects drained from the mission runtime's EffectLog each tick
# (kind: "text"/"win"/"subgoal_*"/"show_waypoints"/"set_light"/"dialog"). Consumed by the HUD;
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
@onready var _music: Node = get_node_or_null(^"/root/NovaMusicService")
@onready var _clear_color: WorldEnvironment = get_node_or_null("ClearColor")

var _dispatcher: NovaFoliageDispatcher
var _tile_overlay: NovaTerrainTileOverlay
var _terrain_data: NovaTerrainData
var _resource_root: NovaResourceRoot
var _loaded: bool = false
var _loaded_mission: NovaMissionData
var _runtime  # MissionRuntime: the one mission runtime driver (sim + present pass + index), DIVIDED cadence
var _mission_stats: Dictionary = {}
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database)
var _weapon_db: NovaWeaponDatabase = null  # weapon.def, lazy per mounted root (FP viewmodel)
var _local_weapon_dict := {}  # the resolved weapon's raw dict (FSM setup transport, ADR 0017 edge)
var _mission_audio: NovaMissionAudio
var _effect_world: NovaEffectWorld  # the runtime .ptl effect world (render-only, per mission)
# Frame-clear cache (divergence #21): recompute only when the env generation
# moves or the camera crosses the water plane.
var _clear_env_generation: int = -1
var _clear_above_water := true
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
# Debug: draw the collision volumes + player capsule (F3 overlay's "Show collision"). Off by default.
var _collision_debug := false
# Debug: hide the scattered foliage (F3 overlay's "Hide foliage"). Off by default.
var _foliage_hidden := false
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
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher") as NovaFoliageDispatcher
		_tile_overlay = _terrain.get_node_or_null("TileOverlay") as NovaTerrainTileOverlay


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

	_resource_root = resource_root
	if not _load_environment(env_file):
		load_failed.emit("failed to load %s" % env_file)
		return ERR_CANT_OPEN
	if not _load_terrain(terrain_file):
		load_failed.emit("failed to load %s" % terrain_file)
		return ERR_CANT_OPEN

	_loaded = true
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
	timeline.span("environment")
	if not _load_environment(env_name):
		load_failed.emit("failed to load %s" % env_name)
		return ERR_CANT_OPEN
	_apply_mission_environment_overrides(mission)
	timeline.end_span()
	timeline.span("terrain")
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		return ERR_CANT_OPEN
	timeline.end_span()

	_loaded_mission = mission
	timeline.span("objects")
	_place_mission_objects(mission, timeline)
	timeline.end_span()
	timeline.span("runtime")
	_start_runtime(mission, bms_name)
	timeline.end_span()
	timeline.span("audio")
	_start_mission_audio(mission, bms_name)
	timeline.end_span()
	timeline.span("effects")
	_start_effect_world()
	timeline.end_span()
	timeline.finish()
	_loaded = true
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
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	if container != null:
		container.queue_free()
	# Skeleton / collision debug overlays, if summoned (harmless to leave, but free for a
	# clean teardown).
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
	if _effect_world != null:
		_effect_world.queue_free()
		_effect_world = null
	# Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	# The game shell re-opens menu music on its return to the front end.
	if _music != null:
		_music.stop_context()
	if _env != null and _env.environment_data != null:
		_env.environment_data.clear_mission_overrides()
	_loaded = false
	_loaded_mission = null
	if _runtime != null:
		_runtime.queue_free()  # frees its off-tree sim too (MissionRuntime._exit_tree)
	_runtime = null
	_mission_audio = null
	_placer = null
	_weapon_db = null  # re-resolves against the next load's mounted root
	_local_weapon_dict = {}
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


func _load_terrain(trn_path: String) -> bool:
	var data := NovaTerrainData.new()
	if data.load_from_resource_root(_resource_root, trn_path) != OK:
		return false
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


# Runtime foliage: feed the dispatcher NovaTerrainData directly (C++ fast path).
# The dispatcher runs the witnessed retail coverage: the 42u near-cell pool
# for FAR plus the sector-entity model walk (docs/foliage/foliage-re.md).
func _configure_foliage() -> void:
	if _dispatcher == null or _terrain_data == null:
		return
	_dispatcher.terrain_data = _terrain_data
	var defs: Array = _terrain_data.get_foliage_defs()
	_dispatcher.foliage_defs = defs
	_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(_resource_root, defs)
	# The ":fd" bake both tiers bind [orig: Foliage_LoadDefAssets @ 0x601260].
	_dispatcher.slot_fd_textures = VegAssets.resolve_slot_fd_textures(_resource_root, defs)
	if _tile_overlay != null:
		# NovaTerrain composites the tile overlay into its own material; the scene
		# TileOverlay node is only an authoring override provider here.
		if _terrain.tile_info_override == null and _tile_overlay.tile_info != null:
			_terrain.tile_info_override = _tile_overlay.tile_info
		_tile_overlay.clear()
		_tile_overlay.visible = false


func get_terrain_data() -> NovaTerrainData:
	return _terrain_data


func get_resource_root() -> NovaResourceRoot:
	return _resource_root


func is_loaded() -> bool:
	return _loaded


## The host per-frame order, faithful to the original main loop's server-tick-then-client-render:
## foliage coverage around the viewer, then the mission runtime (MissionRuntime.tick advances the
## logic at the 62-frame cadence, presents entity state onto the placed nodes, and drains side
## effects), then the audio render pass. Effects come back through MissionRuntime.effects_drained.
func tick(camera_pos: Vector3, camera_xform: Transform3D = Transform3D(), delta: float = TICK_DT) -> void:
	var tick_start := Time.get_ticks_usec()
	var foliage_start := tick_start
	_perf_foliage_us = 0
	_perf_runtime_us = 0
	_perf_audio_us = 0
	if _loaded and _dispatcher != null:
		# Near-tier anchors: the witnessed driver is per-SECTOR-ENTITY (the
		# .trn/.bms-placed world models) [orig: Terrain_RenderSectorEntitiesBySide
		# @ 0x5c7d50]; placed mission objects are the host equivalent (host
		# mapping). No entities means no model clusters - retail has no
		# camera-carpet model dispatch.
		var model_anchors := PackedVector3Array()
		if _placer != null and _placer.has_method("get_placed_world_positions"):
			model_anchors = _placer.get_placed_world_positions()
		_dispatcher.set_model_anchors(model_anchors)
		# FAR runs the witnessed 42u near-cell pool around the camera; the
		# MODEL walk gates on view depth + the view frustum via camera_xform
		# (retail dispatches only VISIBLE sector entities [orig:
		# Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]).
		var cam := get_viewport().get_camera_3d() if get_viewport() != null else null
		if cam != null:
			var vp_size := get_viewport().get_visible_rect().size
			var aspect := (vp_size.x / vp_size.y) if vp_size.y > 0.0 else 1.777
			_dispatcher.set_model_view_fov(cam.fov, aspect)
		_dispatcher.dispatch(camera_pos, camera_xform)
		_perf_foliage_us = Time.get_ticks_usec() - foliage_start
	var runtime_start := Time.get_ticks_usec()
	# Gate on the runtime transport so MissionRuntime._playing is THE play flag
	# in both hosts: the debug overlay's Pause/Step work in the game too, not
	# just the editor preview. _start_runtime calls play(), so normal missions
	# run exactly as before.
	if _loaded and _runtime != null and _runtime.is_playing():
		# Fixed-timestep accumulator: the sim runs at a constant 62.5 Hz regardless of render rate.
		# Guard keeps the duck-typed test stubs (game_world_test.gd) that only implement tick() green.
		if _runtime.has_method("tick_realtime"):
			_runtime.tick_realtime(delta)
		else:
			_runtime.tick()
		_perf_runtime_us = Time.get_ticks_usec() - runtime_start
		# Keep the gate's advertised occupancy current (host + admitted joiners).
		# set_player_count self-dedupes, so this is a no-op until the count changes.
		if _nw_host != null and _runtime.has_method("get_sim"):
			var sim = _runtime.get_sim()
			if sim != null and sim.has_method("get_host_peer_count"):
				_nw_host.set_player_count(1 + sim.get_host_peer_count())
	var audio_start := Time.get_ticks_usec()
	if _loaded and _mission_audio != null:
		# The env clock drives the marker soundloop time-of-day slots
		# [orig: Entity_CalcTimeOfDayRegion @ 0x408110 reads the env time].
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
		"foliage": _dispatcher.get_dispatch_stats() if _dispatcher != null and _dispatcher.has_method("get_dispatch_stats") else {},
		"audio": _mission_audio.get_perf_counters() if _mission_audio != null and _mission_audio.has_method("get_perf_counters") else {},
	}


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

func set_local_player_input(forward: bool, back: bool, left: bool, right: bool, run: bool, crouch: bool, prone: bool, jump: bool, look_yaw_deg: float, look_pitch_deg: float) -> void:
	if _runtime != null:
		_runtime.set_player_input(forward, back, left, right, run, crouch, prone, jump, look_yaw_deg, look_pitch_deg)

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
	# Install the new weapon's FSM on the sim NOW — the mount is not hostage to the FP
	# model load [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -> Player_MountWeaponSlot
	# @0x4dfa40 installs the action table regardless of the render model]. Clip lengths
	# bake in again when the rebuilt viewmodel resolves (_setup_local_player_weapon); a
	# model that never loads leaves 'auto' delays collapsed instead of leaving the OLD
	# weapon's FSM live under the new entity stamp.
	_local_weapon_dict = _weapon_db.get_weapon(index)
	var sim := get_sim()
	if sim != null:
		sim.set_local_player_weapon(_local_weapon_dict, {})
	return true

func build_local_player_viewmodel() -> Node3D:
	if _placer == null:
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
	var arms = _placer.build_model_from_graphic(arms_name, adm_name, container, "anim_wpn_idle", _env)  # _placer untyped -> no :=
	var gun = _placer.build_model_from_graphic(gun_name, adm_name, container, "anim_wpn_idle", _env)
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
				clip_seconds[k] = float(skeletal.get_clip_length(k))
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


## The resolved weapon.def record driving the FP viewmodel: model/adm names plus the
## witnessed view-bias fields (pos/tpos raw units + rot degrees, renderfov horizontal
## degrees) LocalPlayerHost consumes — decoded from NovaWeaponDatabase's transport dict
## at this edge (ADR 0017). Null when the mounted root has no weapon.def or the weapon
## name is absent — callers keep their witnessed JOX AK-47 defaults then. The weapon is
## DEFAULT_VIEWMODEL_WEAPON until equipped-weapon resolution lands; NOVA_VM_WEAPON
## overrides the name (debug: rig A/B against another SKU's def).
func local_player_viewmodel_def() -> PlayerViewmodelDef:
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
			# [orig: WacScript_SpawnSoundAtEntity @ 0x4f23a0 — kong "sound" misnomer,
			# it spawns a particle emitter]. The original orients the emitter to the
			# terrain surface normal at the entity's grid cell; ported as up-vector
			# until the terrain-normal read lands (ptl-format-re §8 follow-up).
			if _effect_world != null and _runtime != null:
				var ssn := int(eff.get("b", 0))
				var pos = _runtime.entity_position_for_ssn(ssn)
				if pos != null:
					_effect_world.spawn_effect_owned(ssn, String(eff.get("str", "")), pos)
		# fx2tgt (spawn at a placed type-6088 target marker
		# [orig: WacScript_PlaySoundAtEmitter @ 0x4f7fd0 — same misnomer family])
		# stays unrouted: which .bms record field carries the 1..99 target number
		# is unwitnessed — ptl-format-re §8 follow-up.


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
		"tick_mode": NovaSimulation.TICK_DIVIDED,
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
	# [orig: Terrain_LoadTileInfoFile @0x5CA730 loads <mission>.til]. Read it raw from the resource root;
	# absent when the mission has none.
	if _resource_root != null:
		var til_name := mission_file.get_basename() + ".til"
		var til_bytes: PackedByteArray = _resource_root.read_file(til_name) if _resource_root.has_file(til_name) else PackedByteArray()
		if til_bytes.size() > 0:
			opts["terrain_til"] = til_bytes
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
	_runtime.setup(mission, container, opts)
	if _runtime.get_sim() == null:
		push_warning("GameWorld: failed to start mission runtime")
	_runtime.effects_drained.connect(_on_runtime_effects)
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


# Route the runtime's drained side effects: "dialog" actions to mission audio (resolved through the
# co-named .DBF + LWF set), everything else out to host consumers (HUD) via mission_effects.
func _on_runtime_effects(effects: Array) -> void:
	_route_mission_effects(effects)
	mission_effects.emit(effects)


# Place real ambient sounds at the mission's sound markers: load the co-named .LWF
# + gamelocl.LWF, resolve each marker to a sound set by name, and spawn looping 3D
# voices. Reuses the placer's item database for the item_id -> sound_profile lookup.
func _start_mission_audio(mission: NovaMissionData, bms_name: String) -> void:
	var item_db = _placer.get_item_db() if _placer != null else null
	_mission_audio = NovaMissionAudio.new(_resource_root, item_db)
	var stats := _mission_audio.setup(mission, bms_name, self)
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
	if _music != null:
		_music.open_game_context(_resource_root)


func get_mission_audio() -> NovaMissionAudio:
	return _mission_audio


# Build the runtime particle-effect world over the mounted root: parse every
# .ptl reachable in the mounts (loose overrides + all PFF volumes) into one
# effect registry, ready to spawn by name/handle [orig: CEffectSystem_Init
# @ 0x5f6070, called from Game_StartMission @ 0x524360].
func _start_effect_world() -> void:
	_effect_world = NovaEffectWorld.new()
	_effect_world.name = "EffectWorld"
	add_child(_effect_world)
	var count := _effect_world.load_from_resource_root(_resource_root)
	print("GameWorld: effect world — %d effect(s) across %d .ptl file(s)" % [
		count, _effect_world.file_count()])


func get_effect_world() -> NovaEffectWorld:
	return _effect_world


# Re-drive the gamemus vars from the local player each frame, the way the
# original does from the local player's body update [orig:
# Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
# g_local_player_entity @ 0x4b6234; full map docs/audio/mus-sbf-re.md §Game
# music driving]. Pumped here: Var7 = health % (cur*100/max, 100 when max <=
# cur [orig: @ 0x4b6315-0x4b6324]) and Var10 = team [orig: @ 0x4b62fc].
# Witnessed-but-unpumped seams (the shipped gamemus reads none of them):
# Var2 view pitch (the original writes raw engine angle units, unwitnessed
# conversion), Var5/Var6 threat distance / threat-targets-me
# (Entity_FindNearestThreat @ 0x4b0990 unported), Var3/Var4 (low-confidence),
# Var8 game type (retail scoring-mode ids not yet mapped to our sessions).
func _music_var_pump() -> void:
	if _music == null or not has_local_player():
		return
	var max_h := local_player_max_health()
	var cur_h := local_player_health()
	_music.set_var(MusicServiceScript.VAR_HEALTH_PCT,
		(cur_h * 100 / max_h) if max_h > cur_h else 100)
	_music.set_var(MusicServiceScript.VAR_TEAM, local_player_team())


# --- Frame clear color (env divergence #21, closed) ----------------------------

func _process(_delta: float) -> void:
	_update_frame_clear_color()


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
