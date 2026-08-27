class_name GameDebugAdapter
extends GameMcpAdapter

## Game-shell adapter for F3 and the ephemeral runtime MCP endpoint.
##
## This is deliberately a thin composition root: it resolves the current live
## world/runtime on every call and forwards reads and mutations through their
## public APIs. It owns no duplicate simulation state and receives no editor
## document state.

const DebugEntities := preload("res://game/debug/debug_entities.gd")

const MCP_ENTITY_LIMIT_MAX := 128

var _session := DebugSession.new()
var _service: GameMcpService = null
var _runtime_source: Callable
var _world_source: Callable
var _player_source: Callable
var _shell_state_source: Callable
var _world_loading_source: Callable
var _dev_tools_open_source: Callable
var _resume_action: Callable
var _return_to_menu_action: Callable
var _quit_action: Callable
var _menu_shell_source: Callable
var _open_ingame_menu_action: Callable
var _open_armory_action: Callable
var _render_capture_begin_action: Callable
var _render_capture_end_action: Callable
var _hud_hidden_capture_begin_action: Callable
var _hud_hidden_capture_end_action: Callable
var _hud_hidden_capture_witness_source: Callable


func configure(
		runtime_source: Callable,
		world_source: Callable,
		player_source: Callable,
		shell_state_source: Callable,
		world_loading_source: Callable,
		dev_tools_open_source: Callable,
		resume_action: Callable,
		return_to_menu_action: Callable,
		quit_action: Callable) -> void:
	_runtime_source = runtime_source
	_world_source = world_source
	_player_source = player_source
	_shell_state_source = shell_state_source
	_world_loading_source = world_loading_source
	_dev_tools_open_source = dev_tools_open_source
	_resume_action = resume_action
	_return_to_menu_action = return_to_menu_action
	_quit_action = quit_action
	DebugCatalog.install(_session)
	DebugCatalog.bind_runtime_targets(
			_session,
			_runtime_source,
			_world_source,
			_player_source,
			_current_viewport,
			_current_scene_tree,
			func(): return self)
	_session.set_authority_source(has_debug_authority)
	_session.set_status_source(runtime_status)


## The runtime MCP endpoint rides `--mcp-port <n>` (LaunchFlags); an unflagged
## launch runs none.
func start_runtime_endpoint() -> void:
	var port := LaunchFlags.mcp_port()
	if port <= 0:
		return
	_service = GameMcpService.new()
	_service.name = "RuntimeMcpService"
	add_child(_service)
	var err := _service.setup(self, port)
	if err != OK:
		push_warning("Runtime MCP failed to start on port %d: %s" % [
				port, error_string(err)])


func get_debug_session() -> DebugSession:
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
			"dev_tools_open": bool(_dev_tools_open_source.call()),
		},
		"runtime": runtime_state,
		"player": player,
		"mission": world.get_mission_stats() if world != null else {},
		"performance": world.get_runtime_perf_counters() if world != null else {},
		"audio_buses": _audio_bus_state(),
	}


## One transport-ready view of the world's typed renderer snapshot. Shell
## ownership stays explicit because a loaded simulation can coexist briefly
## with the start-mission splash that still owns the viewport.
func get_mcp_render_diagnostics() -> Variant:
	var world := _current_world()
	if world == null or not world.is_loaded():
		return {}
	var viewport := _current_viewport()
	var camera := viewport.get_camera_3d() if viewport != null else null
	var snapshot: GameRenderDiagnostics = world.get_render_diagnostics(camera)
	var value := snapshot.to_json_value()
	value["shell"] = {
		"state": String(_shell_state_source.call()),
		"world_loading": _is_world_loading(),
		"gameplay_camera_available": camera != null and camera.is_current(),
	}
	return value


## Lossless comparison capture. Fail closed while a loading/splash layer owns
## the viewport and when no current Camera3D can identify a gameplay frame.
## Capture presentation delegates through injected shell actions and is armed
## only after settle frames. The adapter never reaches into HUD/MenuLayer
## children itself; HUD-hidden evidence is sampled on the correlated draw.
func capture_mcp_render_bundle(
		args: Dictionary,
		cancel_requested: Callable = Callable()) -> Variant:
	var world := _current_world()
	if _is_world_loading():
		return {"error": (
				"The mission is still loading or the start-mission splash still owns "
				+ "the viewport; dismiss it and wait for MainGame.is_world_loading() "
				+ "to become false before capture.")}
	if world == null or not world.is_loaded():
		return {"error": "No loaded render world is available; start a playable mission first."}
	var viewport := _current_viewport()
	var camera := viewport.get_camera_3d() if viewport != null else null
	if camera == null or not camera.is_inside_tree() or not camera.is_current():
		return {"error": (
				"No current gameplay camera is available; wait for the local-player "
				+ "presenter before capture.")}

	var world_only := bool(args.get("world_only", true))
	var presentation_mode := String(args.get(
			"presentation_mode", "world_only" if world_only else "full_frame"))
	if presentation_mode not in ["world_only", "full_frame", "hud_hidden"]:
		return {"error": "Unknown render capture presentation mode '%s'." \
				% presentation_mode}
	var presentation_begin := Callable()
	var presentation_finish := Callable()
	var diagnostics_source := get_mcp_render_diagnostics
	match presentation_mode:
		"world_only":
			presentation_begin = _render_capture_begin_action
			presentation_finish = _render_capture_end_action
		"hud_hidden":
			presentation_begin = _hud_hidden_capture_begin_action
			presentation_finish = _hud_hidden_capture_end_action
			if not _hud_hidden_capture_witness_source.is_valid():
				return {"error": "HUD-hidden render capture witness is unavailable in this game shell."}
			diagnostics_source = func() -> Variant:
				var diagnostics_value: Variant = get_mcp_render_diagnostics()
				if not (diagnostics_value is Dictionary):
					return {}
				var witness_value := _hud_hidden_capture_witness_json()
				if witness_value.is_empty():
					return {}
				var diagnostics := (diagnostics_value as Dictionary).duplicate(true)
				diagnostics["presentation"] = {
					"mode": "hud_hidden",
					"hud_hidden_capture": witness_value,
				}
				return diagnostics
		"full_frame":
			pass
	if presentation_mode != "full_frame" and (not presentation_begin.is_valid() \
			or not presentation_finish.is_valid()):
		return {"error": "%s render capture is unavailable in this game shell." \
				% presentation_mode.capitalize()}

	var capture_args := args.duplicate(false)
	if presentation_mode != "full_frame":
		capture_args[GameRenderCapture.PRESENTATION_BEGIN_OPTION] = presentation_begin
		capture_args[GameRenderCapture.PRESENTATION_FINISH_OPTION] = presentation_finish
	return await GameRenderCapture.capture(
			viewport, diagnostics_source, capture_args, cancel_requested)


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
# Additive seam (keeps configure()'s arity stable): the menu shell the
# game_menu tool drives.
func set_menu_shell_source(source: Callable) -> void:
	_menu_shell_source = source


# Additive seam (same arity contract): the in-world screen verbs game_control
# routes — the ESC pause overlay and the armory's direct-open. Each returns the
# shell's Error verdict; unset callables report the verb unavailable.
func set_ingame_screen_actions(
		open_ingame_menu: Callable,
		open_armory: Callable) -> void:
	_open_ingame_menu_action = open_ingame_menu
	_open_armory_action = open_armory


## Additive shell-owned presentation seam for `world_only` captures. The begin
## action returns Error after snapshotting/hiding presentation; end restores the
## exact prior state after the async readback, including failure/cancellation.
func set_render_capture_actions(
		begin_action: Callable,
		end_action: Callable) -> void:
	_render_capture_begin_action = begin_action
	_render_capture_end_action = end_action


## Screenshot-scoped counterpart to world-only presentation. The witness is
## sampled inside the correlated completed-draw callback while the transaction
## is active; begin/finish never span settle frames or bundle persistence.
func set_hud_hidden_capture_actions(
		begin_action: Callable,
		end_action: Callable,
		witness_source: Callable) -> void:
	_hud_hidden_capture_begin_action = begin_action
	_hud_hidden_capture_end_action = end_action
	_hud_hidden_capture_witness_source = witness_source


func _hud_hidden_capture_witness_json() -> Dictionary:
	var value: Variant = _hud_hidden_capture_witness_source.call()
	if not (value is HudHiddenCaptureWitness):
		return {}
	var witness := value as HudHiddenCaptureWitness
	if not witness.is_valid():
		return {}
	return {
		"hud_detail_level": witness.hud_detail_level,
		"gameplay_hud_visible": witness.gameplay_hud_visible,
		"player_view_effects_active": witness.player_view_effects_active,
		"ads_active": witness.ads_active,
		"big_map_active": witness.big_map_active,
		"hud_canvas_layer_active": witness.hud_canvas_layer_active,
		"fps_counter_visible": witness.fps_counter_visible,
	}


func _menu_shell() -> MenuShell:
	if _menu_shell_source.is_null():
		return null
	return _menu_shell_source.call() as MenuShell


func mcp_game_menu(args: Dictionary) -> Variant:
	var shell := _menu_shell()
	if shell == null:
		return {"error": "no menu shell"}
	var op := String(args.get("op", ""))
	match op:
		"state":
			return shell.menu_snapshot(bool(args.get("widgets", true)))
		"press":
			var target := String(args.get("name", ""))
			if target.is_empty():
				return {"error": "press requires name"}
			if not shell.menu_press(target):
				return {"error": "no widget named '%s'" % target}
			return shell.menu_snapshot(false)
		"press_at":
			if not args.has("x") or not args.has("y"):
				return {"error": "press_at requires x and y (design coords)"}
			var hit := shell.menu_press_at(
					Vector2(float(args["x"]), float(args["y"])))
			var out := shell.menu_snapshot(false)
			out["hit_index"] = hit
			return out
		"click_at":
			if not args.has("x") or not args.has("y"):
				return {"error": "click_at requires x and y (design coords)"}
			if not shell.menu_click_at(Vector2(float(args["x"]), float(args["y"]))):
				return {"error": "menu not visible"}
			return shell.menu_snapshot(false)
		"key":
			if not args.has("keycode"):
				return {"error": "key requires keycode"}
			var handled := shell.menu_key(
					int(args["keycode"]), int(args.get("unicode", 0)))
			var out2 := shell.menu_snapshot(false)
			out2["handled"] = handled
			return out2
		"screen":
			var screen := String(args.get("name", ""))
			if screen.is_empty():
				return {"error": "screen requires name"}
			if not shell.menu_show_screen(screen):
				return {"error": "no screen named '%s'" % screen}
			return shell.menu_snapshot(false)
		"open":
			var file := String(args.get("file", ""))
			if file.is_empty():
				return {"error": "open requires file"}
			if not shell.open_menu(file, String(args.get("target_screen", ""))):
				return {"error": "could not open '%s'" % file}
			return shell.menu_snapshot(false)
	return {"error": "unknown game_menu op '%s'" % op}


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
			# The armory rides the same resume leg as the pause overlay
			# (_on_resume closes whichever is up and hands play back).
			if String(_shell_state_source.call()) in ["paused", "armory"]:
				_resume_action.call()
		"step":
			if runtime == null or (world != null and world.is_net_session()):
				return ERR_UNAVAILABLE
			runtime.step_once()
		"open_ingame_menu":
			if not _open_ingame_menu_action.is_valid():
				return ERR_UNAVAILABLE
			var menu_err: Error = _open_ingame_menu_action.call()
			if menu_err != OK:
				return menu_err
		"open_armory":
			if not _open_armory_action.is_valid():
				return ERR_UNAVAILABLE
			var armory_err: Error = _open_armory_action.call()
			if armory_err != OK:
				return armory_err
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
			or volume_db < DebugCatalog.AUDIO_BUS_VOLUME_MIN_DB \
			or volume_db > DebugCatalog.AUDIO_BUS_VOLUME_MAX_DB:
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


func _is_world_loading() -> bool:
	return _world_loading_source.is_valid() \
			and bool(_world_loading_source.call())


func _current_viewport() -> Viewport:
	return get_viewport() if is_inside_tree() else null


func _current_scene_tree() -> SceneTree:
	return get_tree() if is_inside_tree() else null


func _deferred_quit() -> void:
	_quit_action.call()
