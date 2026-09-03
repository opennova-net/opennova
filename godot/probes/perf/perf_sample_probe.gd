extends GameProbe

## perf_sample: after a warm-up, sample the frame-time counters the shell
## publishes for a fixed window (the world tick's AUDIO leg for the D-SND-16
## cadence A/B, the runtime's PRESENT/SIM legs for the native-row-walk A/B)
## with vsync off, and report avg/p95/max per counter plus the frame wall
## time. Runs on a loaded mission; headless is fine for the tick counters.

const COUNTER_SOURCES := {
	# world tick legs (GameWorld.get_runtime_perf_counters)
	"world": ["world", "tick_us"],
	"foliage": ["world", "foliage_us"],
	"runtime": ["world", "runtime_us"],
	"audio": ["world", "audio_us"],
	# the mission-audio tick inside the audio leg
	"audio_tick": ["audio", "tick_us"],
	# the session frame (MissionPresentation.get_perf_counters)
	"sim": ["runtime", "sim_us"],
	"present": ["runtime", "present_us"],
	"effects": ["runtime", "effects_us"],
}
const DEFAULT_COUNTERS := ["audio", "audio_tick", "present", "sim", "world"]

var _ctx: ProbeContext
var _samples: Dictionary = {}
var _frame_us: Array[int] = []
var _last_frame_t := 0
var _sampling := false


func run(ctx: ProbeContext) -> ProbeVerdict:
	# The launch lands under the start-mission splash with the world held
	# un-ticked; leave it and wait for the local player before measuring.
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	_ctx = ctx
	var warm_ms := int(ctx.args.get("warm_ms", 6000))
	var sample_ms := int(ctx.args.get("sample_ms", 10000))
	var counters: Array = ctx.args.get("counters", DEFAULT_COUNTERS)
	for key in counters:
		if not COUNTER_SOURCES.has(String(key)):
			return ProbeVerdict.failed("unknown counter '%s'; known: %s" % [key, ", ".join(COUNTER_SOURCES.keys())])
		_samples[String(key)] = []
	if ctx.world() == null or ctx.runtime() == null:
		return ProbeVerdict.failed("no loaded world/runtime")
	ProbePerfSetup.uncap_frame_rate(ctx)
	var tree := ctx.tree
	tree.process_frame.connect(_on_frame)
	ctx.defer_restore(func() -> void:
		if tree.process_frame.is_connected(_on_frame):
			tree.process_frame.disconnect(_on_frame))

	ctx.log("warming %d ms, then sampling %d ms: %s" % [warm_ms, sample_ms, ", ".join(_samples.keys())])
	await ctx.wait_ms(warm_ms)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled during warm-up")
	_sampling = true
	_last_frame_t = 0
	await ctx.wait_ms(sample_ms)
	_sampling = false
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled during the sample window")

	var data := {
		"frames": _frame_us.size(),
		"window_ms": sample_ms,
		"counters": {},
	}
	for key in _samples:
		var values: Array = _samples[key]
		var stats := _stats(values)
		(data["counters"] as Dictionary)[key] = stats
		ctx.log("%-10s avg=%.1fus p95=%dus max=%dus" % [key, stats["avg_us"], stats["p95_us"], stats["max_us"]])
	var frame := _stats(_frame_us)
	data["frame"] = frame
	var fps := 1000000.0 / float(frame["avg_us"]) if float(frame["avg_us"]) > 0.0 else 0.0
	ctx.log("frame wall: avg=%.2fms (%.0f fps) over %d frames" % [float(frame["avg_us"]) / 1000.0, fps, _frame_us.size()])
	var audio := ctx.world().get_mission_audio()
	if audio != null:
		data["mission_audio_counters"] = audio.get_perf_counters().to_json_value()
	return ProbeVerdict.passed("%d frames sampled at %.0f fps" % [_frame_us.size(), fps], data)


func _on_frame() -> void:
	if not _sampling or _ctx == null:
		return
	var now := Time.get_ticks_usec()
	if _last_frame_t > 0:
		_frame_us.append(now - _last_frame_t)
	_last_frame_t = now
	var world := _ctx.world()
	var runtime := _ctx.runtime()
	if world == null or runtime == null:
		return
	var world_counters: Dictionary = world.get_runtime_perf_counters()
	var audio_counters: Dictionary = world_counters.get("audio", {})
	var runtime_counters: Dictionary = runtime.get_perf_counters()
	for key in _samples:
		var source: Array = COUNTER_SOURCES[key]
		var table: Dictionary
		match String(source[0]):
			"world":
				table = world_counters
			"audio":
				table = audio_counters
			_:
				table = runtime_counters
		(_samples[key] as Array).append(int(table.get(source[1], 0)))


static func _stats(values: Array) -> Dictionary:
	if values.is_empty():
		return {"avg_us": 0.0, "p95_us": 0, "max_us": 0, "samples": 0}
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0
	for v in sorted:
		total += int(v)
	return {
		"avg_us": float(total) / float(sorted.size()),
		"p95_us": int(sorted[mini(int(float(sorted.size() - 1) * 0.95), sorted.size() - 1)]),
		"max_us": int(sorted.back()),
		"samples": sorted.size(),
	}
