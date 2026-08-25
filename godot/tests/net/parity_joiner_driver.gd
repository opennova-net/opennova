extends SceneTree

# Tracked parity-capture driver for an OpenNova joiner. This is deliberately a
# frame-driven MainLoop rather than a coroutine: Simulation and other
# GDExtension objects are acquired only inside one synchronous frame and never
# survive an await/suspended stack into module teardown.

const TIMEOUT_MS := 240_000
const HEARTBEAT_INTERVAL_MS := 500
const MOTION_PRE_ROLL_MS := 250
const MOTION_INTER_PHASE_GAP_MS := 250
const LOOK_SAMPLES_PER_PHASE := 20

enum Phase {
	WAIT_READY,
	WAIT_MOTION_GATE,
	MOTION_PRE_ROLL,
	MOTION_FORWARD,
	MOTION_GAP_ONE,
	MOTION_STRAFE,
	MOTION_GAP_TWO,
	MOTION_RETURN,
	HEARTBEAT,
	SHUTDOWN_WORLD,
	SHUTDOWN_GAME,
	DONE,
}

var _ready_file := ""
var _readiness_mode := ""
var _auto_deploy := false
var _exercise_motion_enabled := false
var _pick_sent := false
var _motion_complete := false
var _heartbeat_sequence := 0
var _motion_gate_file := ""
var _stop_request_file := ""
var _shutdown_witness_file := ""
var _run_id := ""
var _topology := ""
var _motion_gate_observed := false
var _motion_gate_created_utc := ""
var _motion_gate_steady_started_utc := ""
var _motion_started_ticks_msec := 0
var _motion_completed_ticks_msec := 0

var _game: MainGame = null
var _world: Node = null
var _phase := Phase.WAIT_READY
var _phase_started_ms := 0
var _deadline_ms := 0
var _last_heartbeat_ms := 0
var _motion_key: Key = KEY_NONE
var _motion_duration_ms := 0
var _motion_total_look := 0
var _motion_sample_index := 0
var _shutdown_load_operation: WorldLoadOperation = null
var _shutdown_request: Dictionary = {}
var _shutdown_emit_witness := false
var _requested_exit_code := 1


func _initialize() -> void:
	var configuration_error := _configure()
	if not configuration_error.is_empty():
		push_error("[parity-joiner] %s" % configuration_error)
		_requested_exit_code = 8
		_phase = Phase.DONE
		quit(_requested_exit_code)
		return
	if not _boot_game():
		_requested_exit_code = 2
		_phase = Phase.DONE
		quit(_requested_exit_code)
		return
	var now_ms := Time.get_ticks_msec()
	_deadline_ms = now_ms + TIMEOUT_MS
	_last_heartbeat_ms = now_ms


func _process(_delta: float) -> bool:
	if _phase == Phase.DONE:
		return true
	if _phase not in [Phase.SHUTDOWN_WORLD, Phase.SHUTDOWN_GAME]:
		var stop_poll := _poll_stop_request()
		if not String(stop_poll.get("error", "")).is_empty():
			push_error("[parity-joiner] %s" % String(stop_poll.error))
			_begin_shutdown({}, 7, false)
		elif bool(stop_poll.get("present", false)):
			_begin_shutdown(stop_poll.request, 0, true)

	match _phase:
		Phase.WAIT_READY:
			_tick_wait_ready()
		Phase.WAIT_MOTION_GATE:
			_tick_wait_motion_gate()
		Phase.MOTION_PRE_ROLL:
			_tick_motion_pre_roll()
		Phase.MOTION_FORWARD, Phase.MOTION_STRAFE, Phase.MOTION_RETURN:
			_tick_motion_phase()
		Phase.MOTION_GAP_ONE, Phase.MOTION_GAP_TWO:
			_tick_motion_gap()
		Phase.HEARTBEAT:
			_tick_heartbeat()
		Phase.SHUTDOWN_WORLD:
			_tick_shutdown_world()
		Phase.SHUTDOWN_GAME:
			_tick_shutdown_game()
		Phase.DONE:
			return true
	return _phase == Phase.DONE


func _finalize() -> void:
	# WM_CLOSE and error/timeout fallbacks may bypass cooperative settlement. Free
	# the complete game synchronously while ScriptServer and the GDExtension are
	# still alive; no further frame is available from MainLoop finalization.
	_release_motion_keys()
	if is_instance_valid(_game):
		_game.begin_runtime_shutdown()
		_game.finish_runtime_shutdown()
		_game.free()
	_world = null
	_game = null


func _configure() -> String:
	_auto_deploy = OS.get_environment("NW_LAN_PARITY_AUTO_DEPLOY") == "1"
	_readiness_mode = OS.get_environment("NW_LAN_PARITY_READINESS_MODE")
	_exercise_motion_enabled = OS.get_environment("NW_LAN_PARITY_EXERCISE_MOTION") == "1"
	_motion_complete = not _exercise_motion_enabled
	_ready_file = OS.get_environment("NW_LAN_PARITY_READY_FILE")
	_motion_gate_file = OS.get_environment("NW_LAN_PARITY_MOTION_GATE_FILE")
	_stop_request_file = OS.get_environment("NW_LAN_PARITY_STOP_REQUEST_FILE")
	_shutdown_witness_file = OS.get_environment("NW_LAN_PARITY_SHUTDOWN_WITNESS_FILE")
	_run_id = OS.get_environment("NW_LAN_PARITY_RUN_ID")
	_topology = OS.get_environment("NW_LAN_PARITY_TOPOLOGY")
	if _ready_file.is_empty():
		return "NW_LAN_PARITY_READY_FILE is required"
	if _stop_request_file.is_empty() or _shutdown_witness_file.is_empty():
		return "cooperative stop request and shutdown witness paths are required"
	if _stop_request_file == _shutdown_witness_file:
		return "stop request and shutdown witness paths must be distinct"
	if _run_id.is_empty() or _topology not in ["RO", "OO"]:
		return "run ID and RO/OO topology are required"
	if _readiness_mode not in ["in_match", "deploy_hold"]:
		return "readiness mode must be in_match or deploy_hold"
	if _readiness_mode == "deploy_hold" and (_auto_deploy or _exercise_motion_enabled):
		return "deploy_hold must not auto-deploy or exercise motion"
	if _exercise_motion_enabled and _motion_gate_file.is_empty():
		return "exercised joiner requires a per-run motion gate"
	if not _exercise_motion_enabled and not _motion_gate_file.is_empty():
		return "non-exercised joiner must be motion-gate free"
	for path in [_ready_file, _stop_request_file, _shutdown_witness_file]:
		if FileAccess.file_exists(path):
			return "create-new parity artifact already exists: %s" % path
	return ""


func _boot_game() -> bool:
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[parity-joiner] failed to load main_game.tscn")
		return false
	_game = packed.instantiate() as MainGame
	packed = null
	if _game == null:
		push_error("[parity-joiner] failed to instantiate MainGame")
		return false
	root.add_child(_game)
	_world = _game.get_node_or_null("World")
	if _world == null:
		push_error("[parity-joiner] MainGame has no World")
		_game.free()
		_game = null
		return false
	return true


func _tick_wait_ready() -> void:
	var now_ms := Time.get_ticks_msec()
	if now_ms >= _deadline_ms:
		push_error("[parity-joiner] timed out waiting for deployment")
		_begin_shutdown({}, 4, false)
		return
	var state := _read_joiner_state(true)
	if state.is_empty():
		return
	var in_match_ready: bool = _readiness_mode == "in_match" \
			and bool(state.local_player) and bool(state.in_match) \
			and (not _auto_deploy or not bool(state.pick_pending))
	var deploy_hold_ready: bool = _readiness_mode == "deploy_hold" \
			and bool(state.pick_pending) and int(state.self_handle) > 0 \
			and bool(state.deploy_presented) \
			and String(state.join_admission_stage) \
					== "awaiting the player's deployment pick" \
			and String(state.join_error).is_empty() and not bool(state.session_lost)
	if not in_match_ready and not deploy_hold_ready:
		return
	if not _publish_state(state):
		_begin_shutdown({}, 6, false)
		return
	print("[parity-joiner] joiner ready mode=%s" % _readiness_mode)
	if _exercise_motion_enabled:
		_phase = Phase.WAIT_MOTION_GATE
		_deadline_ms = now_ms + TIMEOUT_MS
		_last_heartbeat_ms = now_ms
	else:
		_motion_complete = true
		if not _publish_current():
			_begin_shutdown({}, 6, false)
			return
		_phase = Phase.HEARTBEAT
		_last_heartbeat_ms = now_ms


func _tick_wait_motion_gate() -> void:
	var now_ms := Time.get_ticks_msec()
	if now_ms >= _deadline_ms:
		push_error("[parity-joiner] timed out waiting for steady-window motion gate")
		_begin_shutdown({}, 6, false)
		return
	if FileAccess.file_exists(_motion_gate_file):
		var gate_result := _read_motion_gate()
		var gate_error := String(gate_result.get("error", ""))
		if not gate_error.is_empty():
			push_error("[parity-joiner] %s" % gate_error)
			_begin_shutdown({}, 6, false)
			return
		_motion_gate_observed = true
		_motion_gate_created_utc = String(gate_result.gate.created_utc)
		_motion_gate_steady_started_utc = String(gate_result.gate.steady_started_utc)
		print("[parity-joiner] steady-window motion gate accepted")
		if not _publish_current():
			_begin_shutdown({}, 6, false)
			return
		_phase = Phase.MOTION_PRE_ROLL
		_phase_started_ms = now_ms
		return
	_publish_heartbeat_if_due(now_ms)


func _tick_motion_pre_roll() -> void:
	var now_ms := Time.get_ticks_msec()
	_publish_heartbeat_if_due(now_ms)
	if _is_shutting_down():
		return
	if now_ms - _phase_started_ms < MOTION_PRE_ROLL_MS:
		return
	_motion_started_ticks_msec = now_ms
	if not _publish_current():
		_begin_shutdown({}, 6, false)
		return
	_start_motion_phase(Phase.MOTION_FORWARD, KEY_W, 1800, 320)


func _start_motion_phase(phase: Phase, key: Key, duration_ms: int, total_look: int) -> void:
	_phase = phase
	_motion_key = key
	_motion_duration_ms = duration_ms
	_motion_total_look = total_look
	_motion_sample_index = 0
	_phase_started_ms = Time.get_ticks_msec()
	_key(key, true)


func _tick_motion_phase() -> void:
	var now_ms := Time.get_ticks_msec()
	var elapsed_ms := mini(_motion_duration_ms, int(now_ms - _phase_started_ms))
	var look_per_sample := int(float(_motion_total_look) / float(LOOK_SAMPLES_PER_PHASE))
	var final_remainder := _motion_total_look - look_per_sample * LOOK_SAMPLES_PER_PHASE
	while _motion_sample_index < LOOK_SAMPLES_PER_PHASE:
		var due_ms := float(_motion_duration_ms) * float(_motion_sample_index) \
				/ float(LOOK_SAMPLES_PER_PHASE)
		if float(elapsed_ms) < due_ms:
			break
		var look_sample := look_per_sample
		if _motion_sample_index == LOOK_SAMPLES_PER_PHASE - 1:
			look_sample += final_remainder
		var sim: Object = _current_sim()
		if sim == null or not sim.has_method("add_local_player_look"):
			_key(_motion_key, false)
			push_error("[parity-joiner] simulation disappeared during motion exercise")
			_begin_shutdown({}, 6, false)
			return
		sim.call("add_local_player_look", float(look_sample), 0.0)
		sim = null
		_motion_sample_index += 1
	_publish_heartbeat_if_due(now_ms)
	if _is_shutting_down():
		return
	if elapsed_ms < _motion_duration_ms:
		return
	_key(_motion_key, false)
	_motion_key = KEY_NONE
	if _motion_sample_index != LOOK_SAMPLES_PER_PHASE:
		push_error("[parity-joiner] motion phase emitted an incomplete look sample set")
		_begin_shutdown({}, 6, false)
		return
	_phase_started_ms = now_ms
	match _phase:
		Phase.MOTION_FORWARD:
			_phase = Phase.MOTION_GAP_ONE
		Phase.MOTION_STRAFE:
			_phase = Phase.MOTION_GAP_TWO
		Phase.MOTION_RETURN:
			_motion_completed_ticks_msec = now_ms
			_motion_complete = true
			print("[parity-joiner] walk/strafe/turn witness complete")
			if not _publish_current():
				_begin_shutdown({}, 6, false)
				return
			_phase = Phase.HEARTBEAT


func _tick_motion_gap() -> void:
	var now_ms := Time.get_ticks_msec()
	_publish_heartbeat_if_due(now_ms)
	if _is_shutting_down():
		return
	if now_ms - _phase_started_ms < MOTION_INTER_PHASE_GAP_MS:
		return
	if _phase == Phase.MOTION_GAP_ONE:
		_start_motion_phase(Phase.MOTION_STRAFE, KEY_D, 1200, -320)
	else:
		_start_motion_phase(Phase.MOTION_RETURN, KEY_A, 900, 160)


func _tick_heartbeat() -> void:
	_publish_heartbeat_if_due(Time.get_ticks_msec())


func _publish_heartbeat_if_due(now_ms: int) -> void:
	if now_ms - _last_heartbeat_ms < HEARTBEAT_INTERVAL_MS:
		return
	if not _publish_current():
		_begin_shutdown({}, 6, false)
		return
	_last_heartbeat_ms = now_ms


func _is_shutting_down() -> bool:
	return _phase in [Phase.SHUTDOWN_WORLD, Phase.SHUTDOWN_GAME, Phase.DONE]


func _current_sim() -> Object:
	if not is_instance_valid(_world) or not _world.has_method("get_sim"):
		return null
	return _world.call("get_sim")


func _is_deploy_presented() -> bool:
	if not is_instance_valid(_game):
		return false
	var presenter := _game.get_node_or_null("DeployScreenPresenter")
	return is_instance_valid(presenter) and presenter.has_method("is_open") \
			and bool(presenter.call("is_open"))


func _read_joiner_state(allow_deploy_pick: bool) -> Dictionary:
	var sim: Object = _current_sim()
	if sim == null:
		return {}
	var pick_pending: bool = sim.has_method("is_join_deploy_pick_pending") \
			and bool(sim.call("is_join_deploy_pick_pending"))
	if allow_deploy_pick and _auto_deploy and not _pick_sent and pick_pending \
			and sim.has_method("send_deployment_pick"):
		_pick_sent = bool(sim.call("send_deployment_pick", 0))
		print("[parity-joiner] default deployment pick sent=%s" % _pick_sent)
		pick_pending = sim.has_method("is_join_deploy_pick_pending") \
				and bool(sim.call("is_join_deploy_pick_pending"))
	var state := {
		"local_player": sim.has_method("has_local_player") and bool(sim.call("has_local_player")),
		"in_match": sim.has_method("is_joined_in_match") and bool(sim.call("is_joined_in_match")),
		"pick_pending": pick_pending,
		"deploy_presented": _is_deploy_presented(),
		"self_handle": int(sim.call("get_joiner_self_handle")) \
				if sim.has_method("get_joiner_self_handle") else 0,
		"join_error": String(sim.call("get_join_error")) \
				if sim.has_method("get_join_error") else "",
		"session_lost": bool(sim.call("is_session_lost")) \
				if sim.has_method("is_session_lost") else false,
		"joiner_phase": int(sim.call("get_joiner_phase")) \
				if sim.has_method("get_joiner_phase") else -1,
		"join_admission_stage": String(sim.call("get_join_admission_stage")) \
				if sim.has_method("get_join_admission_stage") else "",
	}
	sim = null
	return state


func _publish_current() -> bool:
	var state := _read_joiner_state(false)
	if state.is_empty():
		state = {
			"local_player": false,
			"in_match": false,
			"pick_pending": false,
			"deploy_presented": false,
			"self_handle": 0,
			"join_error": "",
			"session_lost": false,
			"joiner_phase": -1,
			"join_admission_stage": "",
		}
	return _publish_state(state)


func _publish_state(raw_state: Dictionary) -> bool:
	# Every witness field is the observed value: the deploy hold is the
	# WITNESSED split — the client enters its protocol InMatch phase and
	# creates the internal motor L after the first loadout grant so its
	# pre-pick C2S 0x0C can flow (net-re 5.61), while the player-paced DEATH
	# screen (deploy_presented) is the presentation hold. The verifier's
	# deploy_hold class asserts exactly that raw combination; nothing here
	# rewrites transport state into a synthesized presentation claim.
	var state := raw_state
	var deploy_hold_ready: bool = _readiness_mode == "deploy_hold" \
			and bool(state.pick_pending) and int(state.self_handle) > 0 \
			and bool(state.deploy_presented) \
			and String(state.join_error).is_empty() and not bool(state.session_lost)
	var next_sequence := _heartbeat_sequence + 1
	var witness := {
		"readiness_mode": _readiness_mode,
		"in_match": bool(state.in_match),
		"local_player": bool(state.local_player),
		"deploy_hold_ready": deploy_hold_ready,
		"auto_deploy": _auto_deploy,
		"deployment_pick_sent": _pick_sent,
		"deployment_pick_pending": bool(state.pick_pending),
		"deploy_presented": bool(state.deploy_presented),
		"joiner_phase": int(state.joiner_phase),
		"join_admission_stage": String(state.join_admission_stage),
		"self_handle": int(state.self_handle),
		"join_error": String(state.join_error),
		"session_lost": bool(state.session_lost),
		"exercise_motion": _exercise_motion_enabled,
		"motion_complete": _motion_complete,
		"motion_gate_required": _exercise_motion_enabled,
		"motion_gate_observed": _motion_gate_observed,
		"motion_gate_run_id": _run_id if _motion_gate_observed else "",
		"motion_gate_created_utc": _motion_gate_created_utc,
		"motion_gate_steady_started_utc": _motion_gate_steady_started_utc,
		"motion_started_ticks_msec": _motion_started_ticks_msec,
		"motion_completed_ticks_msec": _motion_completed_ticks_msec,
		"motion_pre_roll_ms": MOTION_PRE_ROLL_MS,
		"motion_inter_phase_gap_ms": MOTION_INTER_PHASE_GAP_MS,
		"look_samples_per_phase": LOOK_SAMPLES_PER_PHASE,
		"heartbeat_sequence": next_sequence,
		"ticks_msec": Time.get_ticks_msec(),
		"process_id": OS.get_process_id(),
	}
	if not _write_atomic_json_replace(_ready_file, witness):
		push_error("[parity-joiner] could not publish heartbeat witness")
		return false
	_heartbeat_sequence = next_sequence
	return true


func _read_motion_gate() -> Dictionary:
	var parsed := _read_json_object(_motion_gate_file)
	if not String(parsed.get("error", "")).is_empty():
		return parsed
	var gate: Dictionary = parsed.value
	var expected_keys := PackedStringArray([
		"created_utc", "forward_look", "forward_ms", "inter_phase_gap_ms",
		"joiner_pid", "look_samples_per_phase", "pre_roll_ms", "return_look",
		"return_ms", "run_id", "schema", "steady_started_utc", "strafe_look",
		"strafe_ms", "topology",
	])
	if not _has_exact_keys(gate, expected_keys):
		return {"error": "motion gate property set drift"}
	if int(gate.schema) != 1 or String(gate.run_id) != _run_id \
			or String(gate.topology) != _topology \
			or int(gate.joiner_pid) != OS.get_process_id() \
			or not _is_utc_timestamp(String(gate.steady_started_utc)) \
			or not _is_utc_timestamp(String(gate.created_utc)) \
			or int(gate.pre_roll_ms) != MOTION_PRE_ROLL_MS \
			or int(gate.inter_phase_gap_ms) != MOTION_INTER_PHASE_GAP_MS \
			or int(gate.look_samples_per_phase) != LOOK_SAMPLES_PER_PHASE \
			or int(gate.forward_ms) != 1800 or int(gate.forward_look) != 320 \
			or int(gate.strafe_ms) != 1200 or int(gate.strafe_look) != -320 \
			or int(gate.return_ms) != 900 or int(gate.return_look) != 160:
		return {"error": "motion gate metadata does not match this run"}
	return {"gate": gate}


func _poll_stop_request() -> Dictionary:
	if not FileAccess.file_exists(_stop_request_file):
		return {"present": false}
	var parsed := _read_json_object(_stop_request_file)
	if not String(parsed.get("error", "")).is_empty():
		return parsed
	var request: Dictionary = parsed.value
	var expected_keys := PackedStringArray([
		"pid", "requested_utc", "run_id", "schema", "topology",
	])
	if not _has_exact_keys(request, expected_keys):
		return {"error": "stop request property set drift"}
	if String(request.schema) != "opennova.parity-joiner-stop-request.v1" \
			or String(request.run_id) != _run_id \
			or String(request.topology) != _topology \
			or int(request.pid) != OS.get_process_id() \
			or not _is_utc_timestamp(String(request.requested_utc)):
		return {"error": "stop request metadata does not match this process"}
	return {"present": true, "request": request}


func _begin_shutdown(request: Dictionary, exit_code: int, emit_witness: bool) -> void:
	if _phase in [Phase.SHUTDOWN_WORLD, Phase.SHUTDOWN_GAME, Phase.DONE]:
		return
	_release_motion_keys()
	_shutdown_request = request.duplicate(true)
	_shutdown_emit_witness = emit_witness
	_requested_exit_code = exit_code
	if is_instance_valid(_game):
		_shutdown_load_operation = _game.begin_runtime_shutdown()
	elif is_instance_valid(_world) and _world.has_method("unload"):
		_world.call("unload")
	_world = null
	_phase = Phase.SHUTDOWN_WORLD


func _tick_shutdown_world() -> void:
	if _shutdown_load_operation != null \
			and not _shutdown_load_operation.is_settled():
		return
	_shutdown_load_operation = null
	if is_instance_valid(_game):
		_game.finish_runtime_shutdown()
		# This MainLoop owns the game outright. Free synchronously after load
		# settlement so no queued Node/GDExtension object can survive the later
		# ScriptServer/module deinitialization boundary.
		_game.free()
	_game = null
	_phase = Phase.SHUTDOWN_GAME


func _tick_shutdown_game() -> void:
	if _shutdown_emit_witness and not _write_shutdown_witness():
		_requested_exit_code = 7
	_phase = Phase.DONE
	quit(_requested_exit_code)


func _write_shutdown_witness() -> bool:
	var witness := {
		"schema": "opennova.parity-joiner-shutdown.v1",
		"run_id": _run_id,
		"topology": _topology,
		"process_id": OS.get_process_id(),
		"requested_utc": String(_shutdown_request.get("requested_utc", "")),
		"completed_utc": completion_timestamp_for_request(
				String(_shutdown_request.get("requested_utc", ""))),
		"clean": true,
	}
	if FileAccess.file_exists(_shutdown_witness_file):
		push_error("[parity-joiner] shutdown witness path already exists")
		return false
	if not _write_atomic_json_create(_shutdown_witness_file, witness):
		push_error("[parity-joiner] could not publish shutdown witness")
		return false
	print("[parity-joiner] cooperative shutdown complete")
	return true


func _release_motion_keys() -> void:
	for key in [KEY_W, KEY_A, KEY_D]:
		_key(key, false)
	_motion_key = KEY_NONE


func _key(key: Key, pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = key
	event.physical_keycode = key
	event.pressed = pressed
	Input.parse_input_event(event)


func _read_json_object(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {"error": "could not open JSON artifact: %s" % path}
	var raw := file.get_as_text()
	file.close()
	var parsed: Variant = JSON.parse_string(raw)
	if not parsed is Dictionary:
		return {"error": "JSON artifact is not an object: %s" % path}
	return {"value": parsed}


func _has_exact_keys(value: Dictionary, expected: PackedStringArray) -> bool:
	var actual := PackedStringArray()
	for key in value.keys():
		actual.append(String(key))
	actual.sort()
	var sorted_expected := expected.duplicate()
	sorted_expected.sort()
	return actual == sorted_expected


func _is_utc_timestamp(value: String) -> bool:
	var pattern := RegEx.new()
	if pattern.compile(
			"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:"
			+ "[0-9]{2}(?:\\.[0-9]{1,7})?Z$") != OK:
		return false
	return pattern.search(value) != null


## Public driver contract: the shutdown witness's completion stamp (100 ns
## UTC shape, clamped so a backward wall-clock step never precedes the
## request). parity_joiner_driver_contract_test pins it.
static func completion_timestamp_for_request(requested_utc: String) -> String:
	var unix_time := Time.get_unix_time_from_system()
	var whole_seconds := int(floor(unix_time))
	var fraction_100ns := int(floor(
			(unix_time - float(whole_seconds)) * 10_000_000.0))
	var parts := Time.get_datetime_dict_from_unix_time(whole_seconds)
	var completed_utc := "%04d-%02d-%02dT%02d:%02d:%02d.%07dZ" % [
		int(parts.year), int(parts.month), int(parts.day),
		int(parts.hour), int(parts.minute), int(parts.second), fraction_100ns,
	]
	# The runner emits the same fixed-width UTC form. If the wall clock steps
	# backward while the request is draining, equality is the conservative
	# lower bound: the ACK itself proves completion happened after receipt.
	return requested_utc if completed_utc < requested_utc else completed_utc


func _write_atomic_json_replace(path: String, value: Dictionary) -> bool:
	var temp := "%s.tmp.%d" % [path, OS.get_process_id()]
	if FileAccess.file_exists(temp):
		DirAccess.remove_absolute(temp)
	if not _write_json_file(temp, value):
		return false
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)
	if DirAccess.rename_absolute(temp, path) != OK:
		DirAccess.remove_absolute(temp)
		return false
	return true


func _write_atomic_json_create(path: String, value: Dictionary) -> bool:
	if FileAccess.file_exists(path):
		return false
	var temp := "%s.tmp.%d" % [path, OS.get_process_id()]
	if FileAccess.file_exists(temp):
		DirAccess.remove_absolute(temp)
	if not _write_json_file(temp, value):
		return false
	if FileAccess.file_exists(path) or DirAccess.rename_absolute(temp, path) != OK:
		DirAccess.remove_absolute(temp)
		return false
	return true


func _write_json_file(path: String, value: Dictionary) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return false
	file.store_string(JSON.stringify(value))
	file.flush()
	var write_error := file.get_error()
	file.close()
	return write_error == OK
