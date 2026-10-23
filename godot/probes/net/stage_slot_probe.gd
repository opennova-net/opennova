extends GameProbe

## stage_slot: the parity runner's slot staging on an OpenNova host (the E5
## gameplay scenarios' `staging` entries): each entry {"slot", "position_bms"}
## writes the three Position words of the body that player slot holds, the
## authority-side twin of the onHook bridge's apply_pose_slot on a retail host
## (engine: runtime/inmatch/slot_staging.h, Simulation.debug_stage_slot).
## Position only: any other field is refused (a staged heading never holds).
## The verdict carries a result per entry and passes only if every entry
## staged; a joiner or a single player has no slot table to stage.
##
## With `stage_tick` above 0 (the runner's staging_tick, R-L4-s32-1 and
## R-LX-s26-1) every entry stages on the first frame whose logic tick, counted
## from the mission start as retail's g_CurrentTick is, has reached
## `stage_tick`, at most `stage_tolerance` ticks past it; a host already past
## that fails (the tick never comes back). A camp AI's think key is the tick
## plus 36 times its DcbId (the runner's Invoke-ScenarioStaging cites
## retail's), so the tick a staging lands on sets which AI perceives, turns on
## and hits it first. The record's staging_tick gives the ticks past the value
## and the wait.

const LOCAL_PLAYER_TIMEOUT_MS := 60_000
## The runner's own wait for a retail host's tick (Wait-RetailHostTick), inside
## the probe's 60 s run.
const TICK_WAIT_MS := 50_000



func run(ctx: ProbeContext) -> ProbeVerdict:
	var staging_value: Variant = ctx.args.get("staging", [])
	if not (staging_value is Array) or (staging_value as Array).is_empty():
		return ProbeVerdict.failed("`staging` must be a non-empty array of {slot, position_bms}")
	if not await ctx.wait_for_local_player(LOCAL_PLAYER_TIMEOUT_MS):
		return ProbeVerdict.failed("no local player: the session is not running")
	var sim := ctx.sim()
	if sim == null:
		return ProbeVerdict.failed("no simulation to stage on")
	var staging_tick := {}
	var value := int(ctx.args.get("stage_tick", 0))
	if value > 0:
		var tolerance := int(ctx.args.get("stage_tolerance", 0))
		var started := Time.get_ticks_msec()
		var past := await _wait_tick(ctx, sim, value)
		if past < 0:
			return ProbeVerdict.failed("the logic tick did not reach %d within %d ms"
					% [value, TICK_WAIT_MS])
		if past > tolerance:
			return ProbeVerdict.failed(
					"the logic tick was %d past %d (tolerance %d) when the staging was due"
					% [past, value, tolerance], {"logic_tick": sim.get_logic_tick()})
		staging_tick = {
			"value": value,
			"tolerance": tolerance,
			"past": past,
			"waited_ms": Time.get_ticks_msec() - started,
		}
	var results: Array = []
	var all_staged := true
	for index in (staging_value as Array).size():
		var entry_value: Variant = (staging_value as Array)[index]
		var result := _stage(sim, entry_value)
		result.index = index
		if String(result.result) != "staged":
			all_staged = false
		results.append(result)
	var record := {"entries": results, "logic_tick": sim.get_logic_tick()}
	if not staging_tick.is_empty():
		record.staging_tick = staging_tick
	if not all_staged:
		return ProbeVerdict.failed("not every staging entry was written", record)
	return ProbeVerdict.passed("%d slot(s) staged" % results.size(), record)


## Await frames until the logic tick reaches `value`; the ticks past it (a host
## already there answers at once), or -1 on timeout or cancellation.
func _wait_tick(ctx: ProbeContext, sim: Simulation, value: int) -> int:
	var deadline := Time.get_ticks_msec() + TICK_WAIT_MS
	while sim.get_logic_tick() < value:
		if Time.get_ticks_msec() >= deadline or ctx.cancelled:
			return -1
		await ctx.wait_frames(1)
	return sim.get_logic_tick() - value


## One entry: {slot, position_bms} only; the result names the refusal.
func _stage(sim: Simulation, entry_value: Variant) -> Dictionary:
	if not (entry_value is Dictionary):
		return {"result": "an entry must be an object"}
	var entry: Dictionary = entry_value
	for key in entry.keys():
		if String(key) != "slot" and String(key) != "position_bms":
			return {"result": "field `%s` is refused: a staging entry takes a position only" % key}
	var slot_value: Variant = entry.get("slot")
	if not (slot_value is int or slot_value is float):
		return {"result": "`slot` must be a number"}
	var position := _position(entry.get("position_bms"))
	if position.is_empty():
		return {"result": "`position_bms` must be [x, y, z] numbers", "slot": int(slot_value)}
	var code: Error = sim.debug_stage_slot(int(slot_value), position[0], position[1], position[2])
	return {
		"slot": int(slot_value),
		"position_bms": position,
		"result": _result_name(code),
	}


## The binding's Error as the entry's result.
func _result_name(code: Error) -> String:
	match code:
		OK:
			return "staged"
		ERR_UNAVAILABLE:
			return "no host session"
		ERR_DOES_NOT_EXIST:
			return "the slot holds no player"
		ERR_UNCONFIGURED:
			return "the slot's player holds no body"
		ERR_INVALID_PARAMETER:
			return "the position does not fit the fixed-point words"
	return "error %d" % code


## [x, y, z] (or {x, y, z}) as three floats; [] otherwise.
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
