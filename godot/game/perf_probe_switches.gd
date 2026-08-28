class_name PerfProbeSwitches
extends RefCounted

## The shell's frame-span probe switches (MainGame.get_perf_probe_switches):
## the perf probes turn the span clock on for a measurement window and skip
## the world tick or the HUD tick for their A/B legs. Opt-in by design: an
## ordinary frame reads no clock and writes no span. Typed record per ADR
## 0017, the public seam per ADR 0018.

## Clock reads and span writes happen only while true.
var enabled := false
## The last frame's shell spans (before / world / after / hud, microseconds):
## the probe's report edge, filled by the frame-phase sampler by key and
## formatted straight into the probe's text (dict_contract_allowlist).
var spans: Dictionary = {}
## The A/B legs: skip the world tick, skip the HUD tick (only while enabled).
var skip_world := false
var skip_hud := false


func set_enabled(value: bool) -> void:
	enabled = value
	spans.clear()
	if not value:
		skip_world = false
		skip_hud = false
