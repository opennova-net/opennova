extends GameProbe

## body_trace: the body readout (Simulation.debug_body_trace over engine
## runtime/world/body_trace.h), per logic tick, of the registry handles it names. An
## instrument: it reads, it changes nothing a player or the wire sees.
##
## `watch_handles` (up to 8 registry handles) and, with `watch_local`, the local body's own
## registry handle (a joiner's L, not its wire handle) are watched while this run lasts, 8 at
## most; the run clears the watch when it ends. Each watched handle gives, once per entity
## pass:
## - `tick`: whether it exists (`present`), its Position (`pos`, 16.16 world), its velocity
##   triple (`vel`: a person's +0x98 / +0x9C / +0xA0, the last its slideDecay; a hull's motor
##   words), both Flags words (`flags`, `engine_flags`: 0x40 the seat bit, 2 the dead bit),
##   `health`, its seat (`mounted`, `mount_target`, `mount_seat`, `mount_bone`), +0x28
##   (`ground_target`) and its collision model instance (`collision_model`, -1 none); for a
##   row with a brain, its current and pending states (`brain_state`, `brain_pend`, -1 without
##   one) and the think countdown +0x2AC (`think_clock`).
## and, as they happen:
## - `ground_probe`: the movement resolver's settle-tail probe on it: the probe's start
##   (`pos`), the height it returned (`probe_z`), the entity it hit (`ground_target`, the word
##   that lands in +0x28) and that entity's `collision_model`.
## - `force`: the collision force applicator's kick on it (`hit_type`, `force_type`, the
##   attacker in `other`, the `source` point, `vel` after the kick).
## - `blast`: a blast's damage leg on it (the entry's kz type in `hit_type`, the attacker in
##   `other`, the entry's centre in `source`, `damage`, `health_before`, `health`; an item's
##   record is taken ahead of its drain, which the next `tick` shows).
## - `ai_event`: a timed AI event dispatched to its brain: the event's type word (`ai_event`)
##   and the brain's state before the dispatch (`brain_pend`) and after its transition
##   (`brain_state`).
##
## `trigger_handle` (-1 none) keeps only the window around that entity's first `tick` at
## Health <= 0: `pre_ticks` before it to `post_ticks` after, and ends the run once the window
## closes; without a trigger every record of the `watch_ms` run is kept. The records go to
## body_trace.json in the run directory.

const READY_TIMEOUT_MS := 60_000

var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var handles: Array = ctx.args.get("watch_handles", [])
	var watch_ms := int(ctx.args.get("watch_ms", 0))
	var trigger := int(ctx.args.get("trigger_handle", -1))
	var pre_ticks := int(ctx.args.get("pre_ticks", 0))
	var post_ticks := int(ctx.args.get("post_ticks", 0))
	if not await _wait_for_mission(READY_TIMEOUT_MS):
		return ProbeVerdict.failed("no running mission to read")
	var watch := PackedInt32Array()
	for handle in handles:
		watch.append(int(handle))
	if bool(ctx.args.get("watch_local", false)):
		var own := int(ctx.sim().debug_local_body_handle())
		if own < 0:
			return ProbeVerdict.failed("watch_local: this process holds no local body")
		watch.append(own)
	if watch.is_empty():
		return ProbeVerdict.failed("nothing to watch: name watch_handles or watch_local")
	if watch.size() > 8:
		return ProbeVerdict.failed("%d handles named; the readout watches 8" % watch.size())
	var names := PackedStringArray()
	for handle in watch:
		names.append("0x%04x" % handle)
	ctx.sim().debug_set_body_trace_watch(watch)
	ctx.defer_restore(func() -> void:
		var live := ctx.sim()
		if live != null:
			live.debug_set_body_trace_watch(PackedInt32Array()))
	ctx.log("watching %s" % ", ".join(names))
	var records: Array = []
	var next_seq := ctx.sim().debug_body_trace_next_seq()
	var trigger_tick := -1
	var deadline := Time.get_ticks_msec() + watch_ms
	while Time.get_ticks_msec() < deadline and not ctx.cancelled:
		await ctx.tree.process_frame
		var sim := ctx.sim()
		if sim == null:
			break
		var first_new := records.size()
		next_seq = _take(records, next_seq)
		if trigger >= 0 and trigger_tick < 0:
			trigger_tick = _first_death_tick(records, first_new, trigger)
			if trigger_tick >= 0:
				ctx.log("0x%04x reached health 0 at tick %d" % [trigger, trigger_tick])
			else:
				records = _since_tick(records, sim.get_logic_tick() - pre_ticks)
		if trigger_tick >= 0 and sim.get_logic_tick() > trigger_tick + post_ticks:
			break
		ctx.progress({"records": records.size(), "next_seq": next_seq,
				"trigger_tick": trigger_tick})
	if trigger_tick >= 0:
		records = _window(records, trigger_tick - pre_ticks, trigger_tick + post_ticks)
	var live_sim := ctx.sim()
	var result := {
		"watch_handles": Array(watch),
		"trigger_handle": trigger,
		"trigger_tick": trigger_tick,
		"pre_ticks": pre_ticks,
		"post_ticks": post_ticks,
		"next_seq": next_seq,
		"logic_tick": live_sim.get_logic_tick() if live_sim != null else -1,
		"records": records,
	}
	var path := ctx.artifact_dir.path_join("body_trace.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "\t"))
		file.close()
		ctx.artifact("body_trace", path, "json")
	if trigger >= 0 and trigger_tick < 0:
		return ProbeVerdict.failed("0x%04x never reached health 0 (%d record(s) kept)" % [trigger,
				records.size()], result)
	return ProbeVerdict.passed("%d body record(s)" % records.size(), result)


## Waits for a simulation whose logic clock runs (a host's or a joiner's), whether or not
## it holds a local player.
func _wait_for_mission(timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while not _ctx.cancelled and Time.get_ticks_msec() < deadline:
		var sim := _ctx.sim()
		if sim != null and sim.get_logic_tick() > 0:
			return true
		await _ctx.tree.process_frame
	return false


## Appends the live readout's records from `from_seq` (as JSON), logging each force and blast
## record (the per-tick and ground-probe records stay in the artifact), and returns the seq to
## read next. A first seq past `from_seq` means the ring overwrote the records between.
func _take(records: Array, from_seq: int) -> int:
	var sim := _ctx.sim()
	if sim == null:
		return from_seq
	var rows: Array[BodyTraceRow] = sim.debug_body_trace(from_seq)
	if not rows.is_empty() and rows[0].seq > from_seq:
		_ctx.log("records %d..%d were overwritten before this read" % [from_seq,
				rows[0].seq - 1])
	for row in rows:
		var value: Dictionary = row.to_json_value()
		records.append(value)
		if String(value.event) == "force" or String(value.event) == "blast":
			_ctx.log(_line(value))
	return sim.debug_body_trace_next_seq()


## The tick of the trigger's first `tick` record at Health <= 0 among records[from..], -1 none.
func _first_death_tick(records: Array, from: int, trigger: int) -> int:
	for index in range(from, records.size()):
		var row: Dictionary = records[index]
		if String(row.event) == "tick" and int(row.handle) == trigger and bool(row.present) \
				and int(row.health) <= 0:
			return int(row.logic_tick)
	return -1


func _since_tick(records: Array, first_tick: int) -> Array:
	return records.filter(func(row: Dictionary) -> bool: return int(row.logic_tick) >= first_tick)


func _window(records: Array, first_tick: int, last_tick: int) -> Array:
	return records.filter(func(row: Dictionary) -> bool:
		return int(row.logic_tick) >= first_tick and int(row.logic_tick) <= last_tick)


func _line(row: Dictionary) -> String:
	var line := "seq=%d tick=%d %s 0x%04x" % [int(row.seq), int(row.logic_tick),
			String(row.event), int(row.handle)]
	match String(row.event):
		"force":
			line += " hit_type=%d force_type=%d by=0x%04x vel=%s" % [int(row.hit_type),
					int(row.force_type), int(row.other), str(row.vel)]
		"blast":
			line += " kz=%d by=0x%04x damage=%d health %d->%d" % [int(row.hit_type),
					int(row.other), int(row.damage), int(row.health_before), int(row.health)]
		"ai_event":
			line += " type=%d state %d->%d" % [int(row.ai_event), int(row.brain_pend),
					int(row.brain_state)]
	return line
