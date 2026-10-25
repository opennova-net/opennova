extends GameProbe

## load_timeline: the last mission load's timing (GameWorld.last_load_timeline(), the
## LoadTimeline the load's stages bracket in godot/src/world/game_world_load.cpp: a single
## player's or a host's load, a host's map change, a joiner's S2C 0x11 head or its S2C 0x1A
## load). An instrument: it reads, it changes nothing a player or the wire sees.
##
## The result: the timeline's `label`, `total_ms` (to now while the load runs) and `summary`
## (its top-level spans, up to 20, largest first); each span in order with its `name`,
## `depth`, `end_us` (0 while open) and `duration_us` (its start is end_us - duration_us);
## and the two clocks at the read, `ticks_usec` (the spans' clock) and `unix_ms`, so a span
## end lands on a capture's clock at unix_ms - (ticks_usec - end_us) / 1000. The first frames
## past the load (GameWorld.last_post_load_frames(): a span per frame, its legs nested, its
## session updates as an `updates_<n>` span) ride under `post_load_frames` with their `label`,
## `total_ms` and `spans` on the same clock. The record goes to load_timeline.json in the run
## directory; the log carries the summary line.

const SUMMARY_TOP_N := 20
const MS_PER_S := 1000.0


func run(ctx: ProbeContext) -> ProbeVerdict:
	var world := ctx.world()
	if world == null:
		return ProbeVerdict.failed("no game world to read")
	var timeline: LoadTimeline = world.last_load_timeline()
	if timeline == null:
		return ProbeVerdict.failed("no load has run: the world holds no timeline")
	# Both clocks back to back, ahead of the walk.
	var ticks_usec := Time.get_ticks_usec()
	var unix_ms := roundi(Time.get_unix_time_from_system() * MS_PER_S)
	var spans := _span_records(timeline)
	var summary := timeline.summary(SUMMARY_TOP_N)
	var result := {
		"label": timeline.label,
		"total_ms": timeline.total_ms(),
		"summary": summary,
		"ticks_usec": ticks_usec,
		"unix_ms": unix_ms,
		"spans": spans,
	}
	var post_load: LoadTimeline = world.last_post_load_frames()
	if post_load != null:
		result["post_load_frames"] = {
			"label": post_load.label,
			"total_ms": post_load.total_ms(),
			"spans": _span_records(post_load),
		}
	ctx.log(summary)
	var path := ctx.artifact_dir.path_join("load_timeline.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "\t"))
		file.close()
		ctx.artifact("load_timeline", path, "json")
	return ProbeVerdict.passed("%s (%d span(s))" % [summary, spans.size()], result)


func _span_records(timeline: LoadTimeline) -> Array:
	var spans: Array = []
	for span: LoadTimelineSpan in timeline.spans():
		spans.append({
			"name": span.name,
			"depth": span.depth,
			"end_us": span.end_us,
			"duration_us": span.duration_us(),
		})
	return spans
