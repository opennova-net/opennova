class_name GameDebugAdapter
extends GameMcpAdapter

## Game-shell adapter for F3 and the ephemeral runtime MCP endpoint.
##
## This is deliberately a thin composition root: it resolves the current live
## world/runtime on every call and forwards reads and mutations through their
## public APIs. It owns no duplicate simulation state and receives no editor
## document state.

const DebugEntities := preload("res://engine/debug/nova_debug_entities.gd")

const MCP_ENTITY_LIMIT_MAX := 128

var _session := NovaDebugSession.new()
var _service: GameMcpService = null
var _runtime_source: Callable
var _world_source: Callable
var _player_source: Callable
var _shell_state_source: Callable
var _world_loading_source: Callable
var _overlay_open_source: Callable
var _resume_action: Callable
var _return_to_menu_action: Callable
var _quit_action: Callable


func configure(
		runtime_source: Callable,
		world_source: Callable,
		player_source: Callable,
		shell_state_source: Callable,
		world_loading_source: Callable,
		overlay_open_source: Callable,
		resume_action: Callable,
		return_to_menu_action: Callable,
		quit_action: Callable) -> void:
	_runtime_source = runtime_source
	_world_source = world_source
	_player_source = player_source
	_shell_state_source = shell_state_source
	_world_loading_source = world_loading_source
	_overlay_open_source = overlay_open_source
	_resume_action = resume_action
	_return_to_menu_action = return_to_menu_action
	_quit_action = quit_action
	NovaDebugCatalog.install(_session)
	NovaDebugCatalog.bind_runtime_targets(
			_session,
			_runtime_source,
			_world_source,
			_player_source,
			_current_viewport,
			_current_scene_tree,
			func(): return self)
	_session.set_authority_source(has_debug_authority)
	_session.set_status_source(runtime_status)


func start_runtime_endpoint() -> void:
	if not GameMcpService.should_start():
		return
	_service = GameMcpService.new()
	_service.name = "RuntimeMcpService"
	add_child(_service)
	var err := _service.setup(self)
	if err != OK:
		push_warning("Runtime debug connection unavailable: %s" % error_string(err))


func get_debug_session() -> NovaDebugSession:
	return _session


## Curated transport snapshot. Dictionaries begin here because this is the
## JSON-facing game boundary; the underlying runtime contracts stay typed.
func get_mcp_game_state() -> Variant:
	var world := _current_world()
	var runtime: Variant = _current_runtime()
	var sim: Variant = runtime.get_sim() if runtime != null else null
	var runtime_state := runtime_status()
	if sim != null:
		runtime_state["entity_count"] = int(sim.get_entity_count())
		runtime_state["spawned_count"] = int(sim.get_spawned_count())
		runtime_state["brain_count"] = int(sim.get_brain_count())
		runtime_state["network"] = _network_state(sim)
	var player := {
		"present": sim != null and sim.has_local_player(),
	}
	if bool(player["present"]):
		player["position"] = sim.get_local_player_position()
		player["health"] = sim.get_local_player_health()
		player["max_health"] = sim.get_local_player_max_health()
		player["team"] = sim.get_local_player_team()
		player["class"] = sim.get_local_player_class()
		player["weapon"] = sim.get_local_player_weapon_name()
		player["weapon_state"] = sim.get_local_player_weapon_state()
	return {
		"shell": {
			"state": String(_shell_state_source.call()),
			"world_loading": bool(_world_loading_source.call()),
			"world_loaded": world != null and world.is_loaded(),
			"mission_file": world.get_loaded_mission_file() \
					if world != null else "",
			"debug_overlay_open": bool(_overlay_open_source.call()),
		},
		"runtime": runtime_state,
		"player": player,
		"mission": world.get_mission_stats() if world != null else {},
		"performance": world.get_runtime_perf_counters() if world != null else {},
		"audio_buses": _audio_bus_state(),
	}


## Bounded MCP counterpart to F3's entity discovery. `index` addresses the
## current discovery view; authoritative edits still require a row's ai_index.
func get_mcp_game_entities(offset: int, limit: int) -> Variant:
	var runtime: Variant = _current_runtime()
	var sim: Variant = runtime.get_sim() if runtime != null else null
	if sim == null:
		return {}
	var discovered: Array[Dictionary] = DebugEntities.list(sim)
	var total := discovered.size()
	var first := clampi(offset, 0, total)
	var count := clampi(limit, 1, MCP_ENTITY_LIMIT_MAX)
	var last := mini(total, first + count)
	var entities: Array = []
	for index in range(first, last):
		var summary: Dictionary = discovered[index].duplicate(false)
		summary.erase("detail")
		entities.append(summary)
	return {
		"total": total,
		"offset": first,
		"limit": count,
		"returned": entities.size(),
		"truncated": last < total,
		"next_offset": last if last < total else null,
		"entities": entities,
	}


func get_mcp_game_entity(index: int) -> Variant:
	var runtime: Variant = _current_runtime()
	var sim: Variant = runtime.get_sim() if runtime != null else null
	if index < 0 or sim == null:
		return {}
	var discovered: Array[Dictionary] = DebugEntities.list(sim)
	if index >= discovered.size():
		return {}
	var row: Dictionary = discovered[index]
	var detail_value: Variant = row.get("detail", {})
	var result: Dictionary = detail_value.duplicate(true) \
			if detail_value is Dictionary else {}
	for key in row:
		if key != "detail":
			result[key] = row[key]
	return result


## Narrow transport used by GameMcpTools. Pause and step reject multiplayer
## because the world's network pump must keep running.
func mcp_game_control(action: String) -> Error:
	var world := _current_world()
	var runtime: Variant = _current_runtime()
	match action:
		"pause":
			if runtime == null or (world != null and world.is_net_session()):
				return ERR_UNAVAILABLE
			runtime.pause()
		"resume":
			if runtime == null:
				return ERR_UNAVAILABLE
			runtime.play()
			if String(_shell_state_source.call()) == "paused":
				_resume_action.call()
		"step":
			if runtime == null or (world != null and world.is_net_session()):
				return ERR_UNAVAILABLE
			runtime.step_once()
		"return_to_menu":
			if world == null or not world.is_loaded() \
					or bool(_world_loading_source.call()):
				return ERR_UNAVAILABLE
			_return_to_menu_action.call()
		"quit":
			call_deferred("_deferred_quit")
		_:
			return ERR_INVALID_PARAMETER
	return OK


## F3's local Stop button shares the control without classifying leaving a
## multiplayer session as an authoritative world mutation.
func debug_return_to_menu() -> Error:
	return mcp_game_control("return_to_menu")


## Process-local audio knobs shared by F3 and runtime MCP.
func debug_set_audio_bus_volume(bus_name: String, volume_db: float) -> Error:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus < 0:
		return ERR_INVALID_PARAMETER
	if not is_finite(volume_db) \
			or volume_db < NovaDebugCatalog.AUDIO_BUS_VOLUME_MIN_DB \
			or volume_db > NovaDebugCatalog.AUDIO_BUS_VOLUME_MAX_DB:
		return ERR_INVALID_PARAMETER
	AudioServer.set_bus_volume_db(bus, volume_db)
	return OK


func debug_set_audio_bus_mute(bus_name: String, muted: bool) -> Error:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus < 0:
		return ERR_INVALID_PARAMETER
	AudioServer.set_bus_mute(bus, muted)
	return OK


func debug_set_audio_bus_solo(bus_name: String, soloed: bool) -> Error:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus < 0:
		return ERR_INVALID_PARAMETER
	AudioServer.set_bus_solo(bus, soloed)
	return OK


func debug_set_audio_bus_bypass(bus_name: String, bypassed: bool) -> Error:
	var bus := AudioServer.get_bus_index(bus_name)
	if bus < 0:
		return ERR_INVALID_PARAMETER
	AudioServer.set_bus_bypass_effects(bus, bypassed)
	return OK


func has_debug_authority() -> bool:
	var world := _current_world()
	var sim: Variant = world.get_sim() if world != null else null
	return sim != null and not bool(sim.is_joiner())


func runtime_status() -> Dictionary:
	var runtime: Variant = _current_runtime()
	var sim: Variant = runtime.get_sim() if runtime != null else null
	if runtime == null or sim == null:
		return {
			"label": "No mission",
			"detail": "F3 remains available for process-wide diagnostics.",
			"playing": false,
			"authority": false,
		}
	var mission_name := String(runtime.get_mission_name())
	if mission_name.is_empty():
		mission_name = String(runtime.get_mission_file()).get_basename().get_file()
	if mission_name.is_empty():
		mission_name = "Mission"
	var role := "local"
	if bool(sim.is_joiner()):
		role = "joiner"
	elif bool(sim.is_host_listening()):
		role = "host"
	var playing := bool(runtime.is_playing())
	return {
		"label": "%s | %s%s" % [
			mission_name, role, "" if playing else " | paused"],
		"detail": "Runtime targets are resolved live on every read and write.",
		"mission": mission_name,
		"role": role,
		"playing": playing,
		"logic_tick": sim.get_logic_tick(),
		"authority": has_debug_authority(),
	}


func _network_state(sim: Variant) -> Dictionary:
	if sim == null:
		return {"role": "none"}
	if bool(sim.is_joiner()):
		return {
			"role": "joiner",
			"phase": sim.get_joiner_phase(),
			"in_match": sim.is_joined_in_match(),
			"self_handle": sim.get_joiner_self_handle(),
			"server": sim.get_join_server_name(),
			"error": sim.get_join_error(),
			"diagnostics": sim.get_joiner_network_diagnostics(),
		}
	if bool(sim.is_host_listening()):
		return {
			"role": "host",
			"port": sim.get_host_listen_port(),
			"peers": sim.get_host_peer_count(),
			"session": sim.get_host_session_config(),
		}
	return {"role": "local"}


func _audio_bus_state() -> Array:
	var buses: Array = []
	for index in range(AudioServer.bus_count):
		buses.append({
			"index": index,
			"name": AudioServer.get_bus_name(index),
			"volume_db": AudioServer.get_bus_volume_db(index),
			"mute": AudioServer.is_bus_mute(index),
			"solo": AudioServer.is_bus_solo(index),
			"bypass_effects": AudioServer.is_bus_bypassing_effects(index),
			"peak_left_db": AudioServer.get_bus_peak_volume_left_db(index, 0),
			"peak_right_db": AudioServer.get_bus_peak_volume_right_db(index, 0),
		})
	return buses


func _current_world() -> GameWorld:
	var value: Variant = _world_source.call()
	return value as GameWorld


func _current_runtime() -> Variant:
	return _runtime_source.call()


func _current_viewport() -> Viewport:
	return get_viewport() if is_inside_tree() else null


func _current_scene_tree() -> SceneTree:
	return get_tree() if is_inside_tree() else null


func _deferred_quit() -> void:
	_quit_action.call()
