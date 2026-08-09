class_name DebugCatalog
## Built-in, shell-neutral debug-control catalog.
##
## Definitions name only public owner methods/properties. Target adapters bind
## the corresponding target sources on DebugSession.

const MissionRuntime := preload("res://game/world/mission_runtime.gd")

const TARGET_WORLD := &"world"
const TARGET_PLAYER := &"player"
const TARGET_RUNTIME := &"runtime"
const TARGET_SIM := &"simulation"
const TARGET_TERRAIN := &"terrain"
const TARGET_VIEWPORT := &"viewport"
const TARGET_SCENE_TREE := &"scene_tree"
const TARGET_GAME_SHELL := &"game_shell"
const TARGET_ENVIRONMENT := &"environment"
const TARGET_WEATHER := &"weather"

const MISSION_COORD_MIN := -32768.0
const MISSION_COORD_MAX := 32767.999
const ENTITY_HEALTH_MIN := -(1 << 15)
const ENTITY_HEALTH_MAX := (1 << 15) - 1
const AUDIO_BUS_VOLUME_MIN_DB := -60.0
const AUDIO_BUS_VOLUME_MAX_DB := 6.0

const VIEWPORT_DRAW_CHOICES: Array[String] = [
	"Normal",
	"Unshaded",
	"Lighting only",
	"Overdraw",
	"Wireframe",
	"Normal buffer",
	"Voxel GI albedo",
	"Voxel GI lighting",
	"Voxel GI emission",
	"Shadow atlas",
	"Directional shadow atlas",
	"Scene luminance",
	"SSAO",
	"SSIL",
	"PSSM splits",
	"Decal atlas",
	"SDFGI",
	"SDFGI probes",
	"GI buffer",
	"Disable LOD",
	"Cluster omni lights",
	"Cluster spot lights",
	"Cluster decals",
	"Cluster reflection probes",
	"Occluders",
	"Motion vectors",
	"Internal buffer",
]


static func install(session: DebugSession) -> void:
	if session == null:
		return
	for row in DebugOptions.OPTIONS:
		session.register_control(_options_definition(row))
	_install_terrain(session)
	_install_rendering(session)
	_install_edit_actions(session)
	_install_audio_actions(session)
	_install_authoritative_runtime_controls(session)


## Bind the standard runtime targets once for an eagerly owned session. All
## callables are suppliers, not retained objects. This is the MainGame/MCP
## entry point; DebugOverlay uses the same helper for its adapter.
static func bind_runtime_targets(
		session: DebugSession,
		runtime_source: Callable,
		world_source: Callable,
		player_source: Callable = Callable(),
		viewport_source: Callable = Callable(),
		scene_tree_source: Callable = Callable(),
		game_shell_source: Callable = Callable()) -> void:
	if session == null:
		return
	session.set_target_source(TARGET_RUNTIME, runtime_source,
			"No mission runtime is active.")
	session.set_target_source(TARGET_SIM, func():
		var runtime := _resolve_runtime(runtime_source)
		return runtime.get_sim() if runtime != null else null,
		"No simulation is active.")
	session.set_target_source(TARGET_WORLD, world_source,
			"No game world is loaded.")
	session.set_target_source(TARGET_TERRAIN, func():
		var world := _resolve_world(world_source)
		return world.get_terrain_node() if world != null else null,
		"The current world has no terrain.")
	session.set_target_source(TARGET_PLAYER, player_source,
			"No local player presenter is active.")
	session.set_target_source(TARGET_VIEWPORT, viewport_source,
			"No render viewport is available.")
	session.set_target_source(TARGET_SCENE_TREE, scene_tree_source,
			"No scene tree is available.")
	# The world owns the mission-clock knobs, so the environment target IS the
	# resolved GameWorld.
	session.set_target_source(TARGET_ENVIRONMENT, func():
		return _resolve_world(world_source),
		"The current world has no environment.")
	session.set_target_source(TARGET_WEATHER, func():
		var world := _resolve_world(world_source)
		return world.get_weather_node() if world != null else null,
		"The current world has no weather controller.")
	# The overlay rebinds its live world targets when it is constructed. Do
	# not erase MainGame's process-level target when that adapter has no backing
	# source of its own.
	if game_shell_source.is_valid():
		session.set_target_source(TARGET_GAME_SHELL, game_shell_source,
				"The game shell is not available.")


static func _options_definition(row: Dictionary) -> DebugControlDef:
	var definition := DebugControlDef.check(
			StringName(row["id"]),
			StringName(row["page"]),
			String(row["label"]),
			String(row.get("tooltip", "")),
			StringName(row["target"]),
			StringName(row.get("getter", &"")),
			StringName(row["setter"]),
			bool(row.get("default", false)))
	definition.expensive = bool(row.get("expensive", false))
	# OPTIONS rows are applied by the game reading recorded intents, so they
	# stay togglable before their target world binds.
	definition.allow_unresolved_intent = true
	return definition


static func _install_terrain(session: DebugSession) -> void:
	var draw_mode := DebugControlDef.enum_control(
			&"terrain_draw_mode", &"Terrain", "Draw mode",
			"Color terrain by renderer decisions instead of textures.",
			TARGET_TERRAIN, &"get_debug_mode", &"set_debug_mode", 0,
			["Normal", "Detail levels", "Sector colors", "Surface angle", "Height map"])
	draw_mode.expensive = true
	session.register_control(draw_mode)
	session.register_control(DebugControlDef.slider(
			&"terrain_lod_quality", &"Terrain", "Terrain detail",
			"Scale how aggressively terrain refines toward the camera.",
			TARGET_TERRAIN, &"get_lod_quality", &"set_lod_quality",
			1.0, 0.1, 4.0, 0.1))
	_add_terrain_check(session, &"terrain_no_frustum", "Disable all culling",
			"Show terrain patches that the camera would normally reject.",
			&"get_debug_no_frustum", &"set_debug_no_frustum")
	_add_terrain_check(session, &"terrain_no_nearfar", "Disable distance culling",
			"Ignore the camera's near and far terrain planes.",
			&"get_debug_no_nearfar", &"set_debug_no_nearfar")
	_add_terrain_check(session, &"terrain_no_sideplanes", "Disable side-plane culling",
			"Ignore the left, right, top and bottom terrain planes.",
			&"get_debug_no_sideplanes", &"set_debug_no_sideplanes")
	_add_terrain_check(session, &"terrain_no_partial_subdiv", "Disable partial subdivision",
			"Require complete terrain subdivision decisions.",
			&"get_debug_no_partial_subdiv", &"set_debug_no_partial_subdiv")
	_add_terrain_check(session, &"terrain_force_leaves", "Force leaf patches",
			"Render only terrain quadtree leaves.",
			&"get_debug_force_leaves", &"set_debug_force_leaves")
	_add_terrain_check(session, &"terrain_force_lod0", "Force highest detail",
			"Force terrain patches to the highest available detail level.",
			&"get_debug_force_lod0", &"set_debug_force_lod0")


static func _add_terrain_check(
		session: DebugSession,
		id: StringName,
		label: String,
		description: String,
		getter: StringName,
		setter: StringName) -> void:
	var definition := DebugControlDef.check(
			id, &"Terrain", label, description, TARGET_TERRAIN,
			getter, setter, false)
	definition.expensive = true
	session.register_control(definition)


static func _install_rendering(session: DebugSession) -> void:
	var draw_mode := DebugControlDef.enum_control(
			&"viewport_debug_draw", &"Rendering", "Viewport view",
			"Show the renderer's built-in diagnostic buffers.",
			TARGET_VIEWPORT, &"", &"", 0, VIEWPORT_DRAW_CHOICES)
	draw_mode.property_name = &"debug_draw"
	draw_mode.expensive = true
	session.register_control(draw_mode)


static func _install_edit_actions(session: DebugSession) -> void:
	var teleport := DebugControlDef.action_control(
			&"teleport_local_player", &"Player", "Teleport player",
			"Move the local player to a mission-space position.",
			TARGET_SIM, &"debug_teleport_local_player")
	teleport.requires_unlock = true
	teleport.authority = DebugControlDef.Authority.HOST_ONLY
	teleport.action_validator = _valid_teleport_args
	teleport.action_returns_error = true
	session.register_control(teleport)

	var health := DebugControlDef.action_control(
			&"set_entity_health", &"Entities", "Set health",
			"Set the selected simulation entity's health.",
			TARGET_SIM, &"debug_set_entity_health")
	health.requires_unlock = true
	health.authority = DebugControlDef.Authority.HOST_ONLY
	health.action_validator = _valid_health_args
	health.action_returns_error = true
	session.register_control(health)

	var position := DebugControlDef.action_control(
			&"set_entity_position", &"Entities", "Move entity",
			"Move the selected simulation entity to a mission-space position.",
			TARGET_SIM, &"debug_set_entity_position")
	position.requires_unlock = true
	position.authority = DebugControlDef.Authority.HOST_ONLY
	position.action_validator = _valid_entity_position_args
	position.action_returns_error = true
	session.register_control(position)


static func _install_audio_actions(session: DebugSession) -> void:
	var volume := DebugControlDef.action_control(
			&"set_audio_bus_volume", &"Audio", "Set bus volume",
			"Set one named audio bus volume in decibels.",
			TARGET_GAME_SHELL, &"debug_set_audio_bus_volume")
	volume.action_validator = _valid_audio_bus_volume_args
	volume.action_returns_error = true
	session.register_control(volume)

	for row in [
		[&"set_audio_bus_mute", "Set bus mute", &"debug_set_audio_bus_mute"],
		[&"set_audio_bus_solo", "Set bus solo", &"debug_set_audio_bus_solo"],
		[&"set_audio_bus_bypass", "Set bus effect bypass",
				&"debug_set_audio_bus_bypass"],
	]:
		var definition := DebugControlDef.action_control(
				row[0], &"Audio", row[1],
				"Set one named audio bus %s state." % String(row[1]).trim_prefix("Set bus "),
				TARGET_GAME_SHELL, row[2])
		definition.action_validator = _valid_audio_bus_switch_args
		definition.action_returns_error = true
		session.register_control(definition)


static func _install_authoritative_runtime_controls(
		session: DebugSession) -> void:
	var transport := DebugControlDef.action_control(
			&"runtime_transport", &"Sim", "Runtime transport",
			"Resume, pause, or single-step the real game runtime.",
			TARGET_GAME_SHELL, &"mcp_game_control")
	_authoritative(transport)
	transport.action_validator = _valid_transport_args
	transport.action_returns_error = true
	session.register_control(transport)

	var return_to_menu := DebugControlDef.action_control(
			&"runtime_return_to_menu", &"Sim", "Return to menu",
			"Leave the current world locally and return to the game menu.",
			TARGET_GAME_SHELL, &"debug_return_to_menu")
	return_to_menu.action_returns_error = true
	session.register_control(return_to_menu)

	var scripts_paused := DebugControlDef.check(
			&"runtime_wac_paused", &"Sim", "Pause mission scripts",
			"Pause WAC scripts while the rest of the world continues.",
			TARGET_SIM, &"is_wac_paused", &"set_wac_paused", false)
	_authoritative(scripts_paused)
	session.register_control(scripts_paused)

	var mission_variable := DebugControlDef.action_control(
			&"set_mission_variable", &"Vars", "Set mission variable",
			"Set one live V0..V511 mission-script variable.",
			TARGET_SIM, &"set_mission_variable")
	_authoritative(mission_variable)
	mission_variable.action_validator = _valid_mission_variable_args
	session.register_control(mission_variable)

	var time_of_day := DebugControlDef.slider(
			&"environment_time_of_day", &"Environment", "Time of day",
			"Scrub the mission clock by minute of day (0 = 00:00, 1439 = 23:59).",
			TARGET_ENVIRONMENT, &"get_debug_mission_minute_of_day",
			&"debug_set_mission_minute_of_day", 720.0, 0.0, 1439.0, 1.0)
	time_of_day.setter_returns_error = true
	_authoritative(time_of_day)
	session.register_control(time_of_day)

	var wind := DebugControlDef.slider(
			&"environment_wind_strength", &"Environment", "Wind strength",
			"Scale the wind driving foliage sway and weather gusts.",
			TARGET_WEATHER, &"", &"", 100.0, 0.0, 100.0, 1.0)
	wind.property_name = &"wind_strength"
	_authoritative(wind)
	session.register_control(wind)

	var lightning_short := DebugControlDef.action_control(
			&"environment_lightning_short", &"Environment", "Lightning (short)",
			"Trigger the weather controller's short lightning strike.",
			TARGET_WEATHER, &"trigger_lightning_short")
	_authoritative(lightning_short)
	session.register_control(lightning_short)

	var lightning_long := DebugControlDef.action_control(
			&"environment_lightning_long", &"Environment", "Lightning (long)",
			"Trigger the weather controller's long lightning strike.",
			TARGET_WEATHER, &"trigger_lightning_long")
	_authoritative(lightning_long)
	session.register_control(lightning_long)


static func _authoritative(definition: DebugControlDef) -> void:
	definition.requires_unlock = true
	definition.authority = DebugControlDef.Authority.HOST_ONLY


## Convert a target-source result once at this boundary (ADR 0034): the
## Callable seam is untyped, everything downstream is the concrete class.
static func _resolve_world(source: Callable) -> GameWorld:
	if not source.is_valid():
		return null
	var world: Variant = source.call()
	if world is GameWorld and is_instance_valid(world):
		return world
	return null


static func _resolve_runtime(source: Callable) -> MissionRuntime:
	if not source.is_valid():
		return null
	var runtime: Variant = source.call()
	if runtime is MissionRuntime and is_instance_valid(runtime):
		return runtime
	return null


static func _valid_transport_args(args: Array) -> bool:
	return args.size() == 1 \
			and typeof(args[0]) == TYPE_STRING \
			and String(args[0]) in ["resume", "pause", "step"]


static func _valid_mission_variable_args(args: Array) -> bool:
	if args.size() != 2 or not _is_integer_number(args[0]) \
			or not _is_integer_number(args[1]):
		return false
	var index := int(args[0])
	var value := int(args[1])
	return index >= 0 and index < 512 \
			and value >= -2147483648 and value <= 2147483647


static func _valid_health_args(args: Array) -> bool:
	if args.size() != 2 or not _is_integer_number(args[0]) \
			or not _is_integer_number(args[1]):
		return false
	var index := int(args[0])
	var health := int(args[1])
	return index >= 0 and health >= ENTITY_HEALTH_MIN \
			and health <= ENTITY_HEALTH_MAX


static func _valid_entity_position_args(args: Array) -> bool:
	return args.size() == 2 and _is_integer_number(args[0]) \
			and int(args[0]) >= 0 \
			and args[1] is Vector3 and _valid_mission_position(args[1])


static func _valid_teleport_args(args: Array) -> bool:
	if args.size() != 3 or not (args[0] is Vector3) \
			or not _valid_mission_position(args[0]) \
			or not _is_finite_number(args[1]) \
			or not _is_finite_number(args[2]):
		return false
	var yaw := float(args[1])
	var pitch := float(args[2])
	return is_finite(yaw) and yaw >= -360.0 and yaw <= 360.0 \
			and is_finite(pitch) and pitch >= -90.0 and pitch <= 90.0


static func _valid_audio_bus_volume_args(args: Array) -> bool:
	return args.size() == 2 \
			and typeof(args[0]) == TYPE_STRING \
			and not String(args[0]).is_empty() \
			and _is_finite_number(args[1]) \
			and float(args[1]) >= AUDIO_BUS_VOLUME_MIN_DB \
			and float(args[1]) <= AUDIO_BUS_VOLUME_MAX_DB


static func _valid_audio_bus_switch_args(args: Array) -> bool:
	return args.size() == 2 \
			and typeof(args[0]) == TYPE_STRING \
			and not String(args[0]).is_empty() \
			and typeof(args[1]) == TYPE_BOOL


static func _valid_mission_position(position: Vector3) -> bool:
	return position.is_finite() \
			and position.x >= MISSION_COORD_MIN and position.x <= MISSION_COORD_MAX \
			and position.y >= MISSION_COORD_MIN and position.y <= MISSION_COORD_MAX \
			and position.z >= MISSION_COORD_MIN and position.z <= MISSION_COORD_MAX


static func _is_finite_number(value: Variant) -> bool:
	if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
		return false
	return is_finite(float(value))


static func _is_integer_number(value: Variant) -> bool:
	return _is_finite_number(value) and float(value) == floorf(float(value))
