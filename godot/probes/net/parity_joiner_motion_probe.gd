extends GameProbe

## parity_joiner_motion: the joiner's walk/strafe/turn witness inside the
## runner's steady capture window. After a pre-roll, hold W 1.8 s turning
## 320 px, D 1.2 s turning -320 px and A 0.9 s turning 160 px through the
## real input path (the look in twenty equal samples per phase through
## Simulation.add_local_player_look), with an inter-phase gap between them;
## the witness is published as progress every heartbeat and the completed
## exercise (gate metadata echoed: run_id, topology, steady_started_utc)
## is the verdict's data. Needs an in-match joiner with a local player.

const HEARTBEAT_INTERVAL_MS := 500

var _ctx: ProbeContext
var _readiness_mode := ""
var _auto_deploy := false
var _sequence := 0
var _last_heartbeat := 0
var _held: Key = KEY_NONE


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_readiness_mode = String(ctx.args.get("readiness_mode", "in_match"))
	_auto_deploy = bool(ctx.args.get("auto_deploy", false))
	var run_id := String(ctx.args.get("run_id", ""))
	var topology := String(ctx.args.get("topology", ""))
	var steady_started_utc := String(ctx.args.get("steady_started_utc", ""))
	if not ParityJoinerWitness.is_utc_timestamp(steady_started_utc):
		return ProbeVerdict.failed("steady_started_utc is not a UTC timestamp: %s" % steady_started_utc)
	var state := ParityJoinerWitness.read_state(ctx)
	if state.is_empty() or not bool(state.local_player) or not bool(state.in_match):
		return ProbeVerdict.failed("the joiner is not in match with a local player",
				ParityJoinerWitness.compose(state if not state.is_empty() else ParityJoinerWitness.empty_state(),
						_readiness_mode, _auto_deploy, true, 0))
	ctx.defer_restore(func() -> void:
		for phase in ParityJoinerWitness.MOTION_PHASES:
			_key(phase.key, false))
	var created_utc := ParityJoinerWitness.utc_now()
	ctx.log("motion gate accepted (run %s, %s, steady from %s)" % [run_id, topology, steady_started_utc])

	if not await _wait(ParityJoinerWitness.MOTION_PRE_ROLL_MS):
		return _cancelled()
	var started_ticks := Time.get_ticks_msec()
	var first := true
	for phase in ParityJoinerWitness.MOTION_PHASES:
		if not first and not await _wait(ParityJoinerWitness.MOTION_INTER_PHASE_GAP_MS):
			return _cancelled()
		first = false
		var error := await _motion_phase(phase)
		if not error.is_empty():
			return ProbeVerdict.failed(error, _witness(true))
	var completed_ticks := Time.get_ticks_msec()
	ParityJoinerWitness.exercise = {
		"run_id": run_id,
		"topology": topology,
		"steady_started_utc": steady_started_utc,
		"created_utc": created_utc,
		"started_ticks_msec": started_ticks,
		"completed_ticks_msec": completed_ticks,
	}
	var witness := _witness(true)
	ctx.progress(witness)
	ctx.log("walk/strafe/turn witness complete (%d ms)" % (completed_ticks - started_ticks))
	return ProbeVerdict.passed("walk/strafe/turn exercise complete", witness)


## Hold the phase's key for its duration, feeding the look in equal samples.
func _motion_phase(phase: Dictionary) -> String:
	var key: Key = phase.key
	var duration_ms := int(phase.duration_ms)
	var total_look := int(phase.look)
	var samples := ParityJoinerWitness.LOOK_SAMPLES_PER_PHASE
	var look_per_sample := int(float(total_look) / float(samples))
	var final_remainder := total_look - look_per_sample * samples
	var started := Time.get_ticks_msec()
	var sample_index := 0
	_key(key, true)
	_held = key
	while not _ctx.cancelled:
		var elapsed := mini(duration_ms, Time.get_ticks_msec() - started)
		while sample_index < samples:
			var due_ms := float(duration_ms) * float(sample_index) / float(samples)
			if float(elapsed) < due_ms:
				break
			var look := look_per_sample
			if sample_index == samples - 1:
				look += final_remainder
			var sim := _ctx.sim()
			if sim == null:
				_release()
				return "simulation disappeared during the %s phase" % String(phase.name)
			sim.add_local_player_look(float(look), 0.0)
			sample_index += 1
		_heartbeat()
		if elapsed >= duration_ms:
			break
		await _ctx.tree.process_frame
	_release()
	if _ctx.cancelled:
		return "cancelled during the %s phase" % String(phase.name)
	if sample_index != samples:
		return "the %s phase emitted an incomplete look sample set (%d/%d)" % [
				String(phase.name), sample_index, samples]
	_ctx.log("%s phase done: %d ms, %d look samples" % [String(phase.name), duration_ms, samples])
	return ""


func _wait(duration_ms: int) -> bool:
	var until := Time.get_ticks_msec() + duration_ms
	while Time.get_ticks_msec() < until and not _ctx.cancelled:
		_heartbeat()
		await _ctx.tree.process_frame
	return not _ctx.cancelled


func _heartbeat() -> void:
	var now := Time.get_ticks_msec()
	if now - _last_heartbeat < HEARTBEAT_INTERVAL_MS:
		return
	_last_heartbeat = now
	_ctx.progress(_witness(true))


func _witness(exercise_motion: bool) -> Dictionary:
	_sequence += 1
	var state := ParityJoinerWitness.read_state(_ctx)
	return ParityJoinerWitness.compose(
			state if not state.is_empty() else ParityJoinerWitness.empty_state(),
			_readiness_mode, _auto_deploy, exercise_motion, _sequence)


func _cancelled() -> ProbeVerdict:
	return ProbeVerdict.failed("cancelled before the exercise completed", _witness(true))


func _release() -> void:
	if _held != KEY_NONE:
		_key(_held, false)
		_held = KEY_NONE


func _key(key: Key, pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = key
	event.physical_keycode = key
	event.pressed = pressed
	Input.parse_input_event(event)
