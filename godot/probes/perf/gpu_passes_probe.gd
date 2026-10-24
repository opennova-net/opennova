extends GameProbe

## gpu_passes: the frame's GPU time per viewport and per pass. Every frame of
## the window it reads the RenderingDevice's captured timestamps (the frame the
## renderer last finished), splits them per viewport through the vp_begin_ /
## vp_end_ pair each measured viewport carries, and charges each interval to
## the label that opens it. With the engine's --gpu-profile flag on, the
## labels are the renderer's own passes (Render Depth Pre-Pass, Render Opaque
## Pass, Tonemap, Render CanvasItems, ...); without it only the viewport totals
## and the effects' own spans exist. The FrameStats capture is held for the
## run, so FrameFX and the slot captures also stamp their sub-spans
## (opennova_*). Leaves measure_render_time on for the viewports it measured.
## Needs a window: the question is the GPU's frame.

const VP_BEGIN := "vp_begin_"
const VP_END := "vp_end_"
const FRAME_OWNER := "(frame)"
const MIN_LOGGED_MS := 0.01
const SCAN_INTERVAL_MS := 1000
const MS_PER_SECOND := 1000.0


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var warmup_ms := int(float(ctx.args.get("warmup_seconds", 3.0)) * MS_PER_SECOND)
	var window_ms := int(float(ctx.args.get("window_seconds", 10.0)) * MS_PER_SECOND)
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return ProbeVerdict.failed("no RenderingDevice (the compatibility renderer has no timestamps)")

	var board := ctx.frame_stats()
	if board != null:
		var was_capturing := board.is_capture_active()
		ctx.defer_restore(func() -> void:
			if is_instance_valid(board):
				board.set_capture_active(was_capturing))
		board.set_capture_active(true)

	var names := {}
	_measure_viewports(ctx.tree, names)
	await _drain_for_ms(ctx, board, warmup_ms)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled during warm-up")

	var frames: Array = []
	var last_frame := -1
	var deadline := Time.get_ticks_msec() + window_ms
	var next_scan := Time.get_ticks_msec() + SCAN_INTERVAL_MS
	while Time.get_ticks_msec() < deadline and not ctx.cancelled:
		await ctx.tree.process_frame
		if board != null:
			board.drain()
		if Time.get_ticks_msec() >= next_scan:
			# A view made mid-window (the NVG raster, a scope target) joins.
			_measure_viewports(ctx.tree, names)
			next_scan = Time.get_ticks_msec() + SCAN_INTERVAL_MS
		var frame := rd.get_captured_timestamps_frame()
		if frame == last_frame:
			continue
		last_frame = frame
		var count := rd.get_captured_timestamps_count()
		var entries: Array = []
		for i in count:
			entries.append([rd.get_captured_timestamp_name(i), rd.get_captured_timestamp_gpu_time(i)])
		frames.append(entries)
	if frames.is_empty():
		return ProbeVerdict.failed("no captured timestamp frames in the window")

	var table := aggregate(frames, names)
	ctx.log("frames=%d gpu frame mean=%.3f ms (first to last timestamp)" % [
			table.frames, table.frame_ms])
	# Largest view first, passes of 0.01 ms or more: the runner's log keeps a
	# bounded number of lines.
	for row in table.rows_by_total():
		ctx.log("-- %s: %.3f ms (in %d frames)" % [row.name, row.total_ms, row.frames])
		for pair in row.passes:
			if float(pair[1]) >= MIN_LOGGED_MS:
				ctx.log("     %8.3f  %s" % [float(pair[1]), String(pair[0])])
	return ProbeVerdict.passed("%d frames, %.3f ms GPU per frame" % [
			table.frames, table.frame_ms], table.to_data())


## One viewport's (or the frame's own) share of the window.
class OwnerRow:
	extends RefCounted
	var name := ""
	## Mean ms per frame the viewport drew (vp_end_ - vp_begin_).
	var total_ms := 0.0
	## Frames that drew it.
	var frames := 0
	## [label, mean ms per sampled frame] pairs, largest first.
	var passes: Array = []


## The window's reduction (aggregate).
class Table:
	extends RefCounted
	var frames := 0
	## Mean first-to-last timestamp span per sampled frame.
	var frame_ms := 0.0
	## Owner name -> OwnerRow.
	var owners := {}

	func rows_by_total() -> Array:
		var rows: Array = owners.values()
		rows.sort_custom(func(a, b): return a.total_ms > b.total_ms)
		return rows

	## The verdict's data (the runner's transport edge).
	func to_data() -> Dictionary:
		var out := {}
		for name in owners:
			var row: OwnerRow = owners[name]
			out[name] = {"total_ms": row.total_ms, "frames": row.frames, "passes": row.passes}
		return {"frames": frames, "frame_ms": frame_ms, "owners": out}


## The per-viewport, per-label means over `frames` (each an ordered array of
## [name, gpu_ns] pairs). An interval belongs to the label that opens it and to
## the viewport whose vp_begin_ / vp_end_ pair encloses it (FRAME_OWNER
## outside every pair); `names` maps a viewport RID id (the vp_ suffix) to the
## name it is reported under. Means are per sampled frame; a viewport's
## total_ms and frames count only the frames that drew it.
static func aggregate(frames: Array, names: Dictionary) -> Table:
	var sums := {} # owner -> {label -> ns}
	var totals := {} # owner -> ns
	var drawn := {} # owner -> frames
	var frame_ns := 0
	for entries in frames:
		var n: int = entries.size()
		if n < 2:
			continue
		frame_ns += int(entries[n - 1][1]) - int(entries[0][1])
		var owners: Array[String] = [FRAME_OWNER]
		var begun := {}
		for i in n:
			var label := String(entries[i][0])
			var at := int(entries[i][1])
			if label.begins_with(VP_BEGIN):
				var owner := _owner_name(label.substr(VP_BEGIN.length()), names)
				owners.push_back(owner)
				begun[owner] = at
			elif label.begins_with(VP_END):
				var owner := _owner_name(label.substr(VP_END.length()), names)
				if begun.has(owner):
					totals[owner] = int(totals.get(owner, 0)) + at - int(begun[owner])
					drawn[owner] = int(drawn.get(owner, 0)) + 1
					begun.erase(owner)
				if owners.size() > 1 and owners.back() == owner:
					owners.pop_back()
			if i == n - 1:
				break
			var span := int(entries[i + 1][1]) - at
			var current: String = owners.back()
			var row: Dictionary = sums.get(current, {})
			row[label] = int(row.get(label, 0)) + span
			sums[current] = row
	var table := Table.new()
	table.frames = frames.size()
	table.frame_ms = float(frame_ns) / 1.0e6 / float(maxi(table.frames, 1))
	for owner in sums:
		var row := OwnerRow.new()
		row.name = owner
		var labels: Dictionary = sums[owner]
		for label in labels:
			row.passes.append([label, float(labels[label]) / 1.0e6 / float(table.frames)])
		row.passes.sort_custom(func(a, b): return float(a[1]) > float(b[1]))
		row.frames = int(drawn.get(owner, table.frames))
		row.total_ms = float(totals.get(owner, 0)) / 1.0e6 / float(maxi(row.frames, 1))
		table.owners[owner] = row
	return table


static func _owner_name(id_text: String, names: Dictionary) -> String:
	return String(names.get(id_text, "viewport " + id_text))


# Every Viewport of the tree (the root Window included) measured, its RID id
# named by its node path.
static func _measure_viewports(tree: SceneTree, names: Dictionary) -> void:
	for node in ProbePerfSetup.nodes_of_class(tree, "Viewport"):
		var viewport := node as Viewport
		var rid := viewport.get_viewport_rid()
		var key := str(rid.get_id())
		if names.has(key):
			continue
		names[key] = str(viewport.get_path())
		RenderingServer.viewport_set_measure_render_time(rid, true)


static func _drain_for_ms(ctx: ProbeContext, board: FrameStats, duration_ms: int) -> void:
	var deadline := Time.get_ticks_msec() + duration_ms
	while Time.get_ticks_msec() < deadline and not ctx.cancelled:
		await ctx.tree.process_frame
		if board != null:
			board.drain()
