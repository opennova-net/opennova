extends GameProbe

## joiner_net_trace: the joiner net readout (Simulation.debug_joiner_net_trace over engine
## runtime/inmatch/joiner_net_trace.h), one record per client net frame the joiner's
## runtime ran to its end. An instrument: it reads, it changes nothing a player or the
## wire sees.
##
## The engine keeps the last 2048 records (about 33 s) whatever runs, so the probe can
## read them after a window as well as during one. Each record:
## - the clocks: `frame_tick` (g_CurrentTick's stand-in), `client_tick` (the 0x61-seeded
##   network tick), `wall_ms` (the connection's monotonic milliseconds at the frame's end:
##   the ticks one render frame banked share it) and `pump_loop` (the retail loop the
##   frame ran in: network_frame is the game loop);
## - the send holdoff: `countdown_before` (ahead of the frame's step), `holdoff_ticks`
##   (the dictated period), `block_open`, and `uplink_sent` (the C2S 0x0C left);
## - `frames_applied`: the S2C 0x0A frames the replica has applied, a running count; a
##   step between two records is a 0x0A that frame's receive took;
## - the 62-frame quality fold when it ran (`quality_folded`): its inputs (`fold_fps`,
##   `fold_deploy_hold`, `fold_spawn_gate`, `fold_preround`), the terms it pushed
##   (`fold_bandwidth`, `fold_ping`; 0 when held), `window_quality`, `host_metric` and
##   the `level` it set; and a C2S 0x4C the frame queued (`report_queued`,
##   `report_level`).
##
## `from_seq` picks the first record; `watch_ms` > 0 keeps polling each frame for that
## long. The records go to joiner_net_trace.json in the run directory; the verdict
## carries the counts and, for the uplink phase, two histograms over the game loop's
## frames: each 0x0A receipt's `countdown_before`, and each uplink's distance in frames
## from the last 0x0A receipt ahead of it. The log carries each fold and each 0x4C.

const SIM_TIMEOUT_MS := 60_000

var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var from_seq := int(ctx.args.get("from_seq", 0))
	var watch_ms := int(ctx.args.get("watch_ms", 0))
	# No wait for the local player: that wait dismisses the loading presentation, which a
	# joiner's final wait reads as its ESC. The readout needs a runtime alone.
	var sim_deadline := Time.get_ticks_msec() + SIM_TIMEOUT_MS
	while ctx.sim() == null and Time.get_ticks_msec() < sim_deadline and not ctx.cancelled:
		await ctx.tree.process_frame
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
		ctx.progress({"records": records.size(), "next_seq": next_seq})
	var result := _summary(records)
	result["from_seq"] = from_seq
	result["next_seq"] = next_seq
	result["records"] = records
	var path := ctx.artifact_dir.path_join("joiner_net_trace.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "\t"))
		file.close()
		ctx.artifact("joiner_net_trace", path, "json")
	return ProbeVerdict.passed("%d frame record(s), %d uplink(s), %d 0x0A receipt(s)" % [
			records.size(), int(result.uplinks), int(result.receipts)], result)


## Appends the live readout's records from `from_seq` (as JSON) and returns the seq to
## read next. A first seq past `from_seq` means the ring overwrote the records between.
func _take(records: Array, from_seq: int) -> int:
	var sim := _ctx.sim()
	if sim == null:
		return from_seq
	var rows: Array[JoinerNetTraceRow] = sim.debug_joiner_net_trace(from_seq)
	if not rows.is_empty() and rows[0].seq > from_seq:
		_ctx.log("records %d..%d were overwritten before this read" % [from_seq,
				rows[0].seq - 1])
	for row in rows:
		var value: Dictionary = row.to_json_value()
		records.append(value)
		if bool(value.quality_folded):
			_ctx.log(("seq=%d tick=%d fold fps=%d holds=%s/%s/%s terms=%d/%d window=%d "
					+ "host=%d level=%d") % [int(value.seq), int(value.frame_tick), int(value.fold_fps),
					str(value.fold_deploy_hold), str(value.fold_spawn_gate),
					str(value.fold_preround), int(value.fold_bandwidth), int(value.fold_ping),
					int(value.window_quality), int(value.host_metric), int(value.level)])
		if bool(value.report_queued):
			_ctx.log("seq=%d tick=%d 0x4C level=%d" % [int(value.seq), int(value.frame_tick),
					int(value.report_level)])
	return sim.debug_joiner_net_trace_next_seq()


## The counts and the two uplink-phase histograms over the game loop's frames.
func _summary(records: Array) -> Dictionary:
	var uplinks := 0
	var receipts := 0
	var folds := 0
	var reports := 0
	var receipt_countdowns := {}
	var uplink_since_receipt := {}
	var last_applied := -1
	var last_receipt_tick := -1
	for value: Dictionary in records:
		var applied := int(value.frames_applied)
		var game_loop := String(value.pump_loop) == "network_frame"
		var receipt := last_applied >= 0 and applied > last_applied
		last_applied = applied
		if bool(value.quality_folded):
			folds += 1
		if bool(value.report_queued):
			reports += 1
		if receipt:
			receipts += 1
			if game_loop:
				var key := str(int(value.countdown_before))
				receipt_countdowns[key] = int(receipt_countdowns.get(key, 0)) + 1
				last_receipt_tick = int(value.frame_tick)
		if bool(value.uplink_sent):
			uplinks += 1
			if game_loop and last_receipt_tick >= 0:
				var since := str(int(value.frame_tick) - last_receipt_tick)
				uplink_since_receipt[since] = int(uplink_since_receipt.get(since, 0)) + 1
	return {
		"uplinks": uplinks,
		"receipts": receipts,
		"folds": folds,
		"reports": reports,
		"receipt_countdown_before": receipt_countdowns,
		"uplink_frames_since_receipt": uplink_since_receipt,
	}
