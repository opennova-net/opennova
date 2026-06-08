class_name NovaWorld
extends Node3D

# Loads a playable world (terrain + environment + vegetation + foliage) from ONE
# user-chosen resource directory and wires it onto the engine nodes it contains
# (NovaTerrain, NovaEnvironment, NovaWater). The data core is shared engine code
# (NovaTerrainData, EnvFile, NovaFoliageDispatcher, VegAssets); this node is just
# the orchestration both the runtime (game/main_game.tscn) and a future editor
# "Play" mode go through — one loader, one resource dir, no fallbacks.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
const MissionCommandHost := preload("res://engine/world/mission_command_host.gd")

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

var _dispatcher: NovaFoliageDispatcher
var _tile_overlay: NovaTerrainTileOverlay
var _terrain_data: NovaTerrainData
var _resource_root: NovaResourceRoot
var _loaded: bool = false
var _loaded_mission: NovaMissionData
var _sim: NovaSimulation  # the one mission runtime (World + WAC + BMS + AI), driven at 62-frame cadence
var _mission_stats: Dictionary = {}
var _placer  # MissionObjectPlacer (kept so mission audio reuses its item database)
var _mission_audio: NovaMissionAudio
var _entity_registry
var _command_host


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
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		load_failed.emit("no resource directory set")
		return ERR_FILE_NOT_FOUND
	var resource_root := _mount_runtime_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN
	if not resource_root.has_file(terrain_file):
		load_failed.emit("%s not found in %s" % [terrain_file, dir])
		return ERR_FILE_NOT_FOUND
	if not resource_root.has_file(env_file):
		load_failed.emit("%s not found in %s" % [env_file, dir])
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
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		load_failed.emit("no resource directory set")
		return ERR_FILE_NOT_FOUND
	var resource_root := _mount_runtime_root(dir)
	if resource_root == null:
		return ERR_CANT_OPEN

	if not resource_root.has_file(bms_name):
		load_failed.emit("%s not found in %s" % [bms_name, dir])
		return ERR_FILE_NOT_FOUND
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(resource_root, bms_name) != OK:
		load_failed.emit("failed to parse %s: %s" % [bms_name, mission.get_last_error()])
		return ERR_CANT_OPEN

	var trn := mission.get_terrain_ref() + ".trn"
	if not resource_root.has_file(trn):
		load_failed.emit("%s.trn (from %s) not found in %s" % [mission.get_terrain_ref(), bms_name, dir])
		return ERR_FILE_NOT_FOUND
	var env_name := mission.get_environment_ref() + ".env"
	if not resource_root.has_file(env_name):
		load_failed.emit("%s.env (from %s) not found in %s" % [mission.get_environment_ref(), bms_name, dir])
		return ERR_FILE_NOT_FOUND

	_resource_root = resource_root
	if not _load_environment(env_name):
		load_failed.emit("failed to load %s" % env_name)
		return ERR_CANT_OPEN
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		return ERR_CANT_OPEN

	_loaded_mission = mission
	_place_mission_objects(mission)
	_start_mission_sim(mission)
	_start_command_host(mission)
	_start_mission_audio(mission, bms_name)
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
func _place_mission_objects(mission: NovaMissionData) -> void:
	if _resource_root == null or mission == null:
		return
	_placer = MissionObjectPlacer.new(_resource_root)
	_mission_stats = _placer.place(mission, self, { "environment_node": _env })
	print("NovaWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
		int(_mission_stats.get("placed", 0)),
		int(_mission_stats.get("batched", 0)),
		int(_mission_stats.get("animated", 0)),
		int(_mission_stats.get("unresolved", 0)),
		int(_mission_stats.get("markers", 0)),
	])


func get_loaded_mission() -> NovaMissionData:
	return _loaded_mission


func get_sim() -> NovaSimulation:
	return _sim


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
	if _mission_audio != null:
		_mission_audio.teardown()
	_loaded = false
	_loaded_mission = null
	if _sim != null:
		_sim.free()
	_sim = null
	_mission_audio = null
	_placer = null
	if _command_host != null:
		_command_host.queue_free()
	_command_host = null
	_entity_registry = null
	_mission_stats = {}


func _load_environment(env_path: String) -> bool:
	if _env == null:
		return true
	var env := EnvFile.new()
	if env.load_from_resource_root(_resource_root, env_path) != OK:
		push_warning("NovaWorld: failed to load environment '%s'" % env_path)
		return false
	# NovaEnvironment's setter reloads + pushes shader globals on assignment.
	_env.environment_data = env
	return true


func _load_terrain(trn_path: String) -> bool:
	var data := NovaTerrainData.new()
	if data.load_from_resource_root(_resource_root, trn_path) != OK:
		return false
	_terrain_data = data
	_terrain.terrain_data = data
	_terrain.build()
	if _water != null:
		_water.set("terrain_data", data)
	_configure_foliage()
	return true


# Runtime foliage: feed the dispatcher NovaTerrainData directly (C++ fast path —
# no Callable round trip). Same block the editor preview shares minus the
# live-sculpt samplers.
func _configure_foliage() -> void:
	if _dispatcher == null or _terrain_data == null:
		return
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	_dispatcher.cell_grid_radius = 8
	var defs: Array = _terrain_data.get_foliage_defs()
	_dispatcher.foliage_defs = defs
	_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(_resource_root, defs)
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


## Drive per-frame foliage coverage around the viewer, advance the mission runtime at the faithful
## 62-frame cadence, render its part animations, and route its presentation effects.
func tick(camera_pos: Vector3) -> void:
	if _loaded and _dispatcher != null:
		_dispatcher.dispatch(camera_pos)
	if _loaded and _sim != null:
		var ticked := _sim.advance_frame() # fires one logic tick every 62 render frames
		if ticked:
			if _command_host != null:
				_command_host.render() # pose part animations from the engine-computed brain phase
			var effects := _sim.drain_effects()
			if not effects.is_empty():
				_route_mission_effects(effects)
				mission_effects.emit(effects)
	if _loaded and _mission_audio != null:
		_mission_audio.tick(camera_pos)


# Fire mission audio for presentation effects. PlayWavList actions surface as "dialog" effects
# carrying the dialog/wav id in `a`; route them to the mission audio (which resolves the id through
# the co-named .DBF and plays the LWF set). Other kinds are still emitted via mission_effects for
# host consumers (HUD, etc.).
func _route_mission_effects(effects: Array) -> void:
	if _mission_audio == null:
		return
	for e in effects:
		var eff: Dictionary = e
		if String(eff.get("kind", "")) == "dialog":
			_mission_audio.play_dialog(int(eff.get("a", 0)))


func _start_mission_sim(mission: NovaMissionData) -> void:
	_sim = NovaSimulation.new()
	_sim.set_tick_mode(NovaSimulation.TICK_DIVIDED) # game cadence: one logic tick per 62 frames
	if not _sim.load_from_mission_data(mission):
		_sim.free()
		_sim = null
		push_warning("NovaWorld: failed to start mission runtime")
		return
	add_child(_sim) # parented for lifetime only; NovaWorld drives it via advance_frame() (not playing)


# Build the entity registry from the placed MissionObjects and wire the command host to the sim, so
# the host can pose each entity's part animations (PLAYPARTANIM, applied in-engine) onto its model
# every tick. No sim -> nothing to render.
func _start_command_host(mission: NovaMissionData) -> void:
	if _sim == null:
		return
	# A reload reuses this NovaWorld: free any command host + registry left over from the previous
	# mission first, mirroring unload(), so they are not orphaned driving freed entity nodes.
	if _command_host != null:
		_command_host.queue_free()
		_command_host = null
	_entity_registry = null
	var container := get_node_or_null(NodePath(MissionObjectPlacer.CONTAINER_NAME))
	_entity_registry = MissionEntityRegistry.new()
	_entity_registry.build(container, mission)
	_command_host = MissionCommandHost.new()
	_command_host.name = "MissionCommandHost"
	add_child(_command_host)
	_command_host.setup(_entity_registry, _sim)


# Place real ambient sounds at the mission's sound markers: load the co-named .LWF
# + gamelocl.LWF, resolve each marker to a sound set by name, and spawn looping 3D
# voices. Reuses the placer's item database for the item_id -> sound_profile lookup.
func _start_mission_audio(mission: NovaMissionData, bms_name: String) -> void:
	var item_db = _placer.get_item_db() if _placer != null else null
	_mission_audio = NovaMissionAudio.new(_resource_root, item_db)
	var stats := _mission_audio.setup(mission, bms_name, self)
	print("NovaWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d voice(s)" % [
		int(stats.get("markers_resolved", 0)),
		int(stats.get("markers_total", 0)),
		int(stats.get("banks_loaded", 0)),
		int(stats.get("voices", 0)),
	])


func get_mission_audio() -> NovaMissionAudio:
	return _mission_audio
