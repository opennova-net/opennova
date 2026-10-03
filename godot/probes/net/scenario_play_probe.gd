extends GameProbe

## scenario_play: plays one shared network-parity scenario -- the same JSON
## the retail onHook driver consumes -- through the scripted input device
## (ScriptedInput, attached at the device seam ControlsModel.set_scripted_input),
## so the binding scan, PlayerActions' edges and gates, the simulation and
## the wire run exactly as they do for a physical keyboard and mouse.
##
## The scenario: {"setup": {"pose": {"position_bms": [x, y, z], "yaw": deg,
## "pitch": deg}}, "steps": [{"tick": t, then one of "down": code | "up":
## code | "press": code | "look_px": [dx, dy] | "end": true}]}, codes being
## retail action codes. The setup pose lands first: a host or single player
## takes teleport_local_player, a joiner the ApplyPose write on its own L
## (Simulation.debug_apply_local_pose), which its next C2S 0x0C uplink carries.
## Then the script arms, the next device sample latches its start logic tick,
## and the verdict reports {start_logic_tick, end_logic_tick, steps: [{index,
## applied_logic_tick[, release_logic_tick]}], setup}. `wait` false returns as
## soon as the start latched and leaves the script playing; a later run with
## neither `scenario` nor `path` collects that script's record. Role-agnostic.

const LOCAL_PLAYER_TIMEOUT_MS := 60_000
## A start that no device sample latches means the world is not ticking.
const START_LATCH_TIMEOUT_MS := 10_000
const HEARTBEAT_INTERVAL_MS := 500

var _ctx: ProbeContext
var _last_heartbeat := 0


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var scenario: Dictionary = ctx.args.get("scenario", {})
	var path := String(ctx.args.get("path", ""))
	var wait := bool(ctx.args.get("wait", true))
	var controls := ControlsBindings.model()
	if scenario.is_empty() and path.is_empty():
		return await _collect(controls, wait)
	if not scenario.is_empty() and not path.is_empty():
		return ProbeVerdict.failed("pass `scenario` or `path`, not both")
	if not path.is_empty():
		var read := _read_scenario(path)
		if read.has("error"):
			return ProbeVerdict.failed(String(read.error))
		scenario = read.scenario
	var steps: Variant = scenario.get("steps")
	if not (steps is Array):
		return ProbeVerdict.failed("the scenario carries no `steps` array")
	var driver := ScriptedInput.new()
	if driver.load_steps(steps) != OK:
		return ProbeVerdict.failed("the scenario's steps are refused: %s" % driver.get_error())

	if not await ctx.wait_for_local_player(LOCAL_PLAYER_TIMEOUT_MS):
		return ProbeVerdict.failed("no local player to drive")
	var setup := _apply_setup(scenario.get("setup"))
	if setup.has("error"):
		return ProbeVerdict.failed(String(setup.error), {"setup": setup})

	var previous := controls.get_scripted_input()
	if previous != null and not previous.is_done():
		previous.cancel()
		ctx.log("cancelled the scenario still playing from an earlier run")
	controls.set_scripted_input(driver)
	if wait:
		ctx.defer_restore(func() -> void:
			driver.cancel()
			if controls.get_scripted_input() == driver:
				controls.set_scripted_input(null))
	driver.start()
	ctx.log("scenario armed: %d steps, setup %s" % [(steps as Array).size(), JSON.stringify(setup)])

	var latch_deadline := Time.get_ticks_msec() + START_LATCH_TIMEOUT_MS
	while driver.get_state() == ScriptedInput.STATE_ARMED:
		if ctx.cancelled:
			driver.cancel()
			return ProbeVerdict.failed("cancelled before the start latched", _record(driver, setup))
		if Time.get_ticks_msec() >= latch_deadline:
			driver.cancel()
			return ProbeVerdict.failed("no device sample latched the start: the world is not ticking",
					_record(driver, setup))
		await ctx.tree.process_frame
	ctx.log("start latched at logic tick %d" % driver.get_start_logic_tick())
	if not wait:
		return ProbeVerdict.passed("scenario playing from logic tick %d" % driver.get_start_logic_tick(),
				_record(driver, setup))
	return await _finish(driver, setup)


## Hold until the attached script ends, then report it (collect mode).
func _collect(controls: ControlsModel, wait: bool) -> ProbeVerdict:
	var driver := controls.get_scripted_input()
	if driver == null:
		return ProbeVerdict.failed("no scenario is attached to collect")
	if not wait or driver.is_done():
		if driver.is_done():
			controls.set_scripted_input(null)
		return _verdict(driver, {})
	_ctx.defer_restore(func() -> void:
		if driver.is_done() and controls.get_scripted_input() == driver:
			controls.set_scripted_input(null))
	return await _finish(driver, {})


func _finish(driver: ScriptedInput, setup: Dictionary) -> ProbeVerdict:
	while not driver.is_done():
		if _ctx.cancelled:
			driver.cancel()
			return ProbeVerdict.failed("cancelled while the scenario played", _record(driver, setup))
		_heartbeat(driver)
		await _ctx.tree.process_frame
	return _verdict(driver, setup)


func _verdict(driver: ScriptedInput, setup: Dictionary) -> ProbeVerdict:
	var record := _record(driver, setup)
	match driver.get_state():
		ScriptedInput.STATE_FINISHED:
			var span := driver.get_end_logic_tick() - driver.get_start_logic_tick()
			_ctx.log("scenario finished at logic tick %d" % driver.get_end_logic_tick())
			return ProbeVerdict.passed("scenario played: %d steps over %d logic ticks" % [
					(record.steps as Array).size(), span], record)
		ScriptedInput.STATE_CANCELLED:
			return ProbeVerdict.failed("the scenario stopped: %s" % driver.get_cancel_reason(), record)
	return ProbeVerdict.passed("scenario %s" % driver.get_state_name(), record)


## The setup pose on this role's write. {} applies nothing.
func _apply_setup(setup_value: Variant) -> Dictionary:
	if setup_value == null:
		return {}
	if not (setup_value is Dictionary):
		return {"error": "`setup` must be an object"}
	var pose_value: Variant = (setup_value as Dictionary).get("pose")
	if pose_value == null:
		return {}
	if not (pose_value is Dictionary):
		return {"error": "`setup.pose` must be an object"}
	var pose: Dictionary = pose_value
	var position := _position(pose.get("position_bms"))
	if position.is_empty():
		return {"error": "`setup.pose.position_bms` must be [x, y, z] numbers"}
	var yaw: Variant = pose.get("yaw", 0.0)
	var pitch: Variant = pose.get("pitch", 0.0)
	if not (yaw is float or yaw is int) or not (pitch is float or pitch is int):
		return {"error": "`setup.pose` yaw and pitch must be numbers"}
	var sim := _ctx.sim()
	if sim == null:
		return {"error": "no simulation to pose"}
	var out := {
		"position_bms": position,
		"yaw": float(yaw),
		"pitch": float(pitch),
		"logic_tick": sim.get_logic_tick(),
	}
	var write_error: Error
	if sim.is_joiner():
		out.write = "apply_local_pose"
		write_error = sim.debug_apply_local_pose(position[0], position[1], position[2],
				float(yaw), float(pitch))
	else:
		out.write = "teleport_local_player"
		write_error = sim.debug_teleport_local_player(
				Vector3(position[0], position[1], position[2]), float(yaw), float(pitch))
	out.role = _role(sim)
	if write_error != OK:
		out.error = "the %s write refused the setup pose (error %d)" % [out.write, write_error]
	return out


func _record(driver: ScriptedInput, setup: Dictionary) -> Dictionary:
	var applied := driver.get_applied_logic_ticks()
	var released := driver.get_release_logic_ticks()
	var steps: Array = []
	for index in applied.size():
		var row := {"index": index, "applied_logic_tick": applied[index]}
		if released[index] >= 0:
			row.release_logic_tick = released[index]
		steps.append(row)
	var sim := _ctx.sim()
	return {
		"state": driver.get_state_name(),
		"role": _role(sim) if sim != null else "none",
		"start_logic_tick": driver.get_start_logic_tick(),
		"end_logic_tick": driver.get_end_logic_tick(),
		"steps": steps,
		"setup": setup,
		"cancel_reason": driver.get_cancel_reason(),
	}


func _read_scenario(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {"error": "no scenario file at %s" % path}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (parsed is Dictionary):
		return {"error": "%s is not a JSON object" % path}
	return {"scenario": parsed}


## [x, y, z] (or the debug snapshot's {x, y, z}) as three floats; [] otherwise.
func _position(value: Variant) -> Array:
	var axes: Array = []
	if value is Array:
		axes = value
	elif value is Dictionary:
		axes = [value.get("x"), value.get("y"), value.get("z")]
	if axes.size() != 3:
		return []
	for axis in axes:
		if not (axis is float or axis is int):
			return []
	return [float(axes[0]), float(axes[1]), float(axes[2])]


func _role(sim: Simulation) -> String:
	if sim.is_joiner():
		return "joiner"
	return "host" if sim.is_host_listening() else "local"


func _heartbeat(driver: ScriptedInput) -> void:
	var now := Time.get_ticks_msec()
	if now - _last_heartbeat < HEARTBEAT_INTERVAL_MS:
		return
	_last_heartbeat = now
	var sim := _ctx.sim()
	var applied := 0
	for tick in driver.get_applied_logic_ticks():
		if tick >= 0:
			applied += 1
	_ctx.progress({
		"state": driver.get_state_name(),
		"logic_tick": sim.get_logic_tick() if sim != null else -1,
		"start_logic_tick": driver.get_start_logic_tick(),
		"applied_steps": applied,
		"held_codes": Array(driver.get_held_codes()),
	})
