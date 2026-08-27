class_name ProbeFrameSampler
extends RefCounted

## The frame-time sampler the perf probes share: run process frames for
## `duration_ms`, hand `counter_row(sec_frames, sec_accum)` to `log` once per
## second of samples, and reduce to avg/p50/p95/max plus the eight worst
## frames. `cancelled` (a bool supplier) ends the window early.


static func measure(tree: SceneTree, duration_ms: int, counter_row: Callable, log: Callable,
		cancelled: Callable = Callable()) -> Dictionary:
	var samples: Array[float] = []
	var sec_accum := 0.0
	var sec_frames := 0
	var deadline := Time.get_ticks_msec() + duration_ms
	var last := Time.get_ticks_usec()
	while Time.get_ticks_msec() < deadline:
		if cancelled.is_valid() and bool(cancelled.call()):
			break
		await tree.process_frame
		var now := Time.get_ticks_usec()
		var ms := float(now - last) / 1000.0
		last = now
		samples.append(ms)
		sec_accum += ms
		sec_frames += 1
		if sec_accum >= 1000.0:
			log.call(counter_row.call(sec_frames, sec_accum))
			# Do not charge the probe's own diagnostic pulls to the next frame.
			last = Time.get_ticks_usec()
			sec_accum = 0.0
			sec_frames = 0
	log.call(counter_row.call(sec_frames, sec_accum))
	return reduce(samples)


## avg/p50/p95/max over frame-time samples plus the eight worst frames tagged
## with their offset into the window.
static func reduce(samples: Array[float]) -> Dictionary:
	var s := samples.duplicate()
	s.sort()
	var n := s.size()
	if n == 0:
		return {avg = 0.0, p50 = 0.0, p95 = 0.0, mx = 0.0, n = 0, worst = []}
	var sum := 0.0
	for v in s:
		sum += v
	var worst: Array[String] = []
	var tagged := []
	var t_ms := 0.0
	for v in samples:
		tagged.append([v, t_ms])
		t_ms += v
	tagged.sort_custom(func(a, b): return a[0] > b[0])
	for i in mini(8, tagged.size()):
		worst.append("%.1fms@t+%.2fs" % [tagged[i][0], tagged[i][1] / 1000.0])
	return {
		avg = sum / n, p50 = s[n >> 1], p95 = s[int(float(n) * 0.95)], mx = s[n - 1],
		n = n, worst = worst,
	}


## The one-line report the perf probes log per phase.
static func report_line(label: String, st: Dictionary) -> String:
	return "%s frames=%5d avg=%6.2fms p50=%6.2fms p95=%6.2fms max=%7.2fms worst=[%s]" % [
			label, st.n, st.avg, st.p50, st.p95, st.mx, ", ".join(st.worst)]
