class_name GameDebugAdapter
extends GameMcpAdapter

## Game-shell adapter for F3 and the ephemeral runtime MCP endpoint.
##
## This is deliberately a thin composition root: it resolves the current live
## world/runtime on every call and forwards reads and mutations through their
## public APIs. It owns no duplicate simulation state and receives no editor
## document state.

const MCP_ENTITY_LIMIT_MAX := 128

var _service: GameMcpService = null
# The one typed record of shell seams (GameShellSeams): suppliers, state
# reads, action legs, and capture presentation, all resolved live per call.
var _seams: GameShellSeams = null
# The typed debug-control table (ADR 0042 d5), built once over the adopted
# seams; every row re-resolves its live owner per call.
var _controls: DebugControls = null


func configure(seams: GameShellSeams) -> void:
	_seams = seams
	_controls = DebugControls.new(seams, self)


## Release the shell-capturing Callable graph before its script teardown.
func release_shell_seams() -> void:
	if _controls != null:
		_controls.clear()
	if _seams != null:
		_seams.clear()
	_controls = null
	_seams = null


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


func get_debug_controls() -> DebugControls:
	return _controls


## The same record configure() adopted; the probe runner drives its suppliers
## and mission verbs (ADR 0041).
func get_shell_seams() -> GameShellSeams:
	return _seams


## Curated transport snapshot. Dictionaries begin here because this is the
## JSON-facing game boundary; the underlying runtime contracts stay typed.
func get_mcp_game_state() -> Variant:
	var world := _current_world()
	var runtime := _current_runtime()
	var sim: Simulation = runtime.get_sim() if runtime != null else null
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
		player["weapon_state"] = sim.get_local_player_weapon_state().to_json_value()
	return {
		"shell": {
			"state": String(_seams.shell_state_source.call()),
			"world_loading": bool(_seams.world_loading_source.call()),
			"world_loaded": world != null and world.is_loaded(),
			"mission_file": world.get_loaded_mission_file() \
					if world != null else "",
			"dev_tools_open": bool(_seams.dev_tools_open_source.call()),
		},
		"session": _session_facts(sim),
		"runtime": runtime_state,
		"player": player,
		"mission": (world.get_mission_stats().to_json_value()
				if world != null and world.get_mission_stats() != null else {}),
		"performance": world.get_runtime_perf_counters() if world != null else {},
		"audio_buses": _audio_bus_state(),
	}


## The in-match session facts (ADR 0042: Simulation.session_state()/
## session_role() re-export the portable inmatch::Session — the authority,
## never the transport flags), as transport labels.
func _session_facts(sim: Simulation) -> Dictionary:
	if sim == null:
		return {}
	return {
		"state": _session_state_name(int(sim.session_state())),
		"role": _session_role_name(int(sim.session_role())),
	}


func _session_state_name(state: int) -> String:
	match state:
		MissionFrameOutcome.STATE_UNLOADED: return "unloaded"
		MissionFrameOutcome.STATE_CONNECTING: return "connecting"
		MissionFrameOutcome.STATE_LOADING: return "loading"
		MissionFrameOutcome.STATE_RUNNING: return "running"
		MissionFrameOutcome.STATE_PAUSED: return "paused"
		MissionFrameOutcome.STATE_STOPPING: return "stopping"
		MissionFrameOutcome.STATE_FAILED: return "failed"
	return "unknown(%d)" % state


func _session_role_name(role: int) -> String:
	match role:
		Simulation.ROLE_SINGLE_PLAYER: return "single_player"
		Simulation.ROLE_LISTEN_HOST: return "listen_host"
		Simulation.ROLE_JOINER: return "joiner"
		Simulation.ROLE_DEDICATED_HOST: return "dedicated_host"
	return "unknown(%d)" % role


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
		"state": String(_seams.shell_state_source.call()),
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
			presentation_begin = _seams.render_capture_begin_action
			presentation_finish = _seams.render_capture_end_action
		"hud_hidden":
			presentation_begin = _seams.hud_hidden_capture_begin_action
			presentation_finish = _seams.hud_hidden_capture_end_action
			if not _seams.hud_hidden_capture_witness_source.is_valid():
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


## Bounded MCP counterpart to F3's entity discovery, over the engine's typed
## entity directory (Simulation.entity_directory, ADR 0042 d5). `index`
## addresses the current discovery view; authoritative edits still require a
## row's ai_index. JSON conversion happens here, at the MCP boundary, via the
## records' own to_json_value() — the wire shape is the legacy key set.
func get_mcp_game_entities(offset: int, limit: int) -> Variant:
	var runtime := _current_runtime()
	var sim: Simulation = runtime.get_sim() if runtime != null else null
	if sim == null:
		return {}
	var rows: Array = sim.entity_directory()
	var total := rows.size()
	var first := clampi(offset, 0, total)
	var count := clampi(limit, 1, MCP_ENTITY_LIMIT_MAX)
	var last := mini(total, first + count)
	var entities: Array = []
	for index in range(first, last):
		entities.append((rows[index] as EntityRow).to_json_value())
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
	var runtime := _current_runtime()
	var sim: Simulation = runtime.get_sim() if runtime != null else null
	if index < 0 or sim == null:
		return {}
	var rows: Array = sim.entity_directory()
	if index >= rows.size():
		return {}
	var row: EntityRow = rows[index]
	var result: Dictionary = {}
	var card: EntityCard = sim.entity_card(row.get_wire_handle())
	if card != null:
		result = card.to_json_value()
	result.merge(row.to_json_value(), true)
	return result


func _hud_hidden_capture_witness_json() -> Dictionary:
	var value: Variant = _seams.hud_hidden_capture_witness_source.call()
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
	}


func _menu_shell() -> MenuShell:
	if _seams == null or _seams.menu_shell_source.is_null():
		return null
	return _seams.menu_shell_source.call() as MenuShell


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


## Narrow transport used by GameMcpTools. Pause and step report the engine
## session's own refusal for multiplayer roles (the network pump must keep
## running); the adapter adds no shell-side role gate.
func mcp_game_control(action: String) -> Error:
	var runtime := _current_runtime()
	match action:
		"pause":
			# The engine refuses net-role pauses (inmatch::Session::pause is
			# SinglePlayer-only: the network pump must keep running); the
			# runtime reports that verdict instead of a shell-side gate.
			if runtime == null or not runtime.pause():
				return ERR_UNAVAILABLE
		"resume":
			if runtime == null:
				return ERR_UNAVAILABLE
			runtime.play()
			# The armory rides the same resume leg as the pause overlay
			# (_on_resume closes whichever is up and hands play back).
			if String(_seams.shell_state_source.call()) in ["paused", "armory"]:
				_seams.resume_action.call()
		"step":
			# Same engine verdict as "pause": a net-role session declines the
			# manual step natively.
			if runtime == null or not runtime.step_once():
				return ERR_UNAVAILABLE
		"open_ingame_menu":
			if not _seams.open_ingame_menu_action.is_valid():
				return ERR_UNAVAILABLE
			var menu_err: Error = _seams.open_ingame_menu_action.call()
			if menu_err != OK:
				return menu_err
		"open_armory":
			if not _seams.open_armory_action.is_valid():
				return ERR_UNAVAILABLE
			var armory_err: Error = _seams.open_armory_action.call()
			if armory_err != OK:
				return armory_err
		"return_to_menu":
			# The shell's one gated return leg (MainGame.return_to_menu: busy
			# while a load is pending, unavailable without a loaded world).
			if not _seams.return_to_menu.is_valid():
				return ERR_UNAVAILABLE
			var menu_err: Error = _seams.return_to_menu.call()
			if menu_err != OK:
				return menu_err
		"quit":
			_deferred_quit.call_deferred()
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
			or volume_db < DebugControls.AUDIO_BUS_VOLUME_MIN_DB \
			or volume_db > DebugControls.AUDIO_BUS_VOLUME_MAX_DB:
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
	# Authority is the session-role fact: ROLE_JOINER is the one
	# non-authoritative role; every other session role owns the world.
	var world := _current_world()
	var sim: Simulation = world.get_sim() if world != null else null
	return sim != null and int(sim.session_role()) != Simulation.ROLE_JOINER


func runtime_status() -> Dictionary:
	var runtime := _current_runtime()
	var sim: Simulation = runtime.get_sim() if runtime != null else null
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
	if int(sim.session_role()) == Simulation.ROLE_JOINER:
		role = "joiner"
	elif bool(sim.is_host_listening()):
		# The session role is configured at mission load; the transport flag
		# also labels a listen socket that has not loaded yet.
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


func _network_state(sim: Simulation) -> Dictionary:
	if sim == null:
		return {"role": "none"}
	if int(sim.session_role()) == Simulation.ROLE_JOINER:
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
		var session := sim.get_host_session_config().to_json_value()
		session["mission_header_size"] = sim.get_mission_header_size()
		return {
			"role": "host",
			"port": sim.get_host_listen_port(),
			"peers": sim.get_host_peer_count(),
			"session": session,
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
	var value: Variant = _seams.world_source.call()
	return value as GameWorld


func _current_runtime() -> MissionPresentation:
	if _seams == null or not _seams.runtime_source.is_valid():
		return null
	var value: Variant = _seams.runtime_source.call()
	if value is MissionPresentation and is_instance_valid(value):
		return value
	return null


func _is_world_loading() -> bool:
	return _seams != null and _seams.world_loading_source.is_valid() \
			and bool(_seams.world_loading_source.call())


func _current_viewport() -> Viewport:
	return get_viewport() if is_inside_tree() else null


func _deferred_quit() -> void:
	_seams.quit_action.call()
