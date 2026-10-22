extends GameProbe

## use_press_trace: the USE readout (Simulation.debug_use_press_trace over engine
## runtime/world/use_press_trace.h), what each USE-ITEM press met on its way to the
## wire. An instrument: it reads, it changes nothing a player or the wire sees.
##
## The engine keeps the last 64 records whatever runs, so the probe can read them
## after a scenario_play run (no second probe runs beside one):
## - `press`: each held frame's case 0xB1. The arm the press took (mount_chain,
##   armory, armory_refused, vehicle_loadout, dropped); the local body's Flags
##   masked to 0x800 / 0x400000 / 0x200 in our two words (`flags`, `engine_flags`);
##   `mounted`; and the active slot's `slot_current` / `slot_next` action.
## - `toggle_gate`: the release's mount toggle gate, its answer (`allowed`) and the
##   slot's actions as the gate read them.
## - `toggle_scan`: a joiner's seat scan and request. find_mount_toggle_candidate's
##   answer (`candidate`) and pick (`vehicle`, `seat_index`, `bone`), and the
##   `outcome` (attach_queued, attach_refused, no_candidate, detach_queued, ...).
##   `attach_sent_before` is the runtime's framed-0x26 count when the request queued.
##   A readout whose `attach_requests_sent` exceeds it shows the 0x26 left.
##
## `from_seq` picks the first record; `watch_ms` > 0 keeps polling each frame for that
## long and logs each record as it lands. The records also go to use_press_trace.json
## in the run directory.

const LOCAL_PLAYER_TIMEOUT_MS := 60_000

var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var from_seq := int(ctx.args.get("from_seq", 0))
	var watch_ms := int(ctx.args.get("watch_ms", 0))
	if not await ctx.wait_for_local_player(LOCAL_PLAYER_TIMEOUT_MS):
		return ProbeVerdict.failed("no local player: the session is not running")
	if ctx.sim() == null:
		return ProbeVerdict.failed("no simulation to read")
	var records: Array = []
	var next_seq := _take(records, from_seq)
	var deadline := Time.get_ticks_msec() + watch_ms
	while watch_ms > 0 and Time.get_ticks_msec() < deadline and not ctx.cancelled:
		await ctx.tree.process_frame
		if ctx.sim() == null:
			break
		next_seq = _take(records, next_seq)
		ctx.progress({"records": records.size(), "next_seq": next_seq,
				"attach_requests_sent": _attach_requests_sent()})
	var sim := ctx.sim()
	var result := {
		"from_seq": from_seq,
		"next_seq": next_seq,
		"attach_requests_sent": _attach_requests_sent(),
		"logic_tick": sim.get_logic_tick() if sim != null else -1,
		"records": records,
	}
	var path := ctx.artifact_dir.path_join("use_press_trace.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "\t"))
		file.close()
		ctx.artifact("use_press_trace", path, "json")
	return ProbeVerdict.passed("%d USE record(s), %d 0x26 framed" % [records.size(),
			int(result.attach_requests_sent)], result)


## Appends the live readout's records from `from_seq` (as JSON), logging each, and
## returns the seq to read next. A first seq past `from_seq` means the ring
## overwrote the records between.
func _take(records: Array, from_seq: int) -> int:
	var sim := _ctx.sim()
	if sim == null:
		return from_seq
	var rows: Array[UsePressRow] = sim.debug_use_press_trace(from_seq)
	if not rows.is_empty() and rows[0].seq > from_seq:
		_ctx.log("records %d..%d were overwritten before this read" % [from_seq,
				rows[0].seq - 1])
	for row in rows:
		var value: Dictionary = row.to_json_value()
		records.append(value)
		_ctx.log(_line(value))
	return sim.debug_use_press_next_seq()


func _attach_requests_sent() -> int:
	var sim := _ctx.sim()
	return sim.debug_attach_requests_sent() if sim != null else 0


func _line(row: Dictionary) -> String:
	var line := "seq=%d tick=%d %s flags=0x%x engine_flags=0x%x mounted=%s" % [
			int(row.seq), int(row.logic_tick), String(row.event), int(row.flags),
			int(row.engine_flags), str(row.mounted)]
	if bool(row.has_slot):
		line += " slot=%d/%d" % [int(row.slot_current), int(row.slot_next)]
	match String(row.event):
		"press":
			line += " arm=%s" % String(row.arm)
		"toggle_gate":
			line += " allowed=%s" % str(row.allowed)
		"toggle_scan":
			line += " candidate=%s outcome=%s" % [str(row.candidate), String(row.outcome)]
			if bool(row.candidate):
				line += " vehicle=0x%04x seat=%d bone=%d" % [int(row.vehicle),
						int(row.seat_index), int(row.bone)]
			if String(row.outcome) == "attach_queued":
				line += " attach_sent_before=%d" % int(row.attach_sent_before)
	return line
