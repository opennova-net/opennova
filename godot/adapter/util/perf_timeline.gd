class_name PerfTimeline
extends RefCounted

## Named nested wall-clock spans for one operation (a mission load). An owner
## creates a timeline with begin(), brackets stages with span()/end_span(), and
## finish()es it — which prints one structured line and retains the timeline in
## a small static ring so later tooling (the debug overlay's perf pane) can
## render recent history without re-running the operation. Nothing global runs
## between operations: a timeline only costs while its operation does.
##
## The static *_on helpers no-op on a null timeline, so instrumented engine
## paths (terrain open, the object placer) accept an optional timeline without
## burdening callers that do not measure.

const RING_SIZE := 8

static var _ring: Array = []

var label := ""
var _spans: Array = []  # { name: String, depth: int, start_us: int, end_us: int }
var _open: Array = []   # stack of indices into _spans
var _start_us := 0
var _end_us := 0


static func begin(operation_label: String) -> PerfTimeline:
	var timeline := PerfTimeline.new()
	timeline.label = operation_label
	timeline._start_us = Time.get_ticks_usec()
	return timeline


static func span_on(timeline: PerfTimeline, name: String) -> void:
	if timeline != null:
		timeline.span(name)


static func end_on(timeline: PerfTimeline) -> void:
	if timeline != null:
		timeline.end_span()


## Most-recent-first copy of the retained timelines.
static func history() -> Array:
	var out := _ring.duplicate()
	out.reverse()
	return out


static func latest() -> PerfTimeline:
	return _ring.back() if not _ring.is_empty() else null


func span(name: String) -> void:
	_open.push_back(_spans.size())
	_spans.append({ "name": name, "depth": _open.size() - 1, "start_us": Time.get_ticks_usec(), "end_us": 0 })


## Ends the innermost open span (no-op when none is open).
func end_span() -> void:
	if _open.is_empty():
		return
	var idx: int = _open.pop_back()
	_spans[idx]["end_us"] = Time.get_ticks_usec()


## Close any spans an early return left open plus the timeline itself, retain it
## in the ring, print the structured line, and return the one-line summary so
## callers can surface it (status bar, log).
func finish() -> String:
	while not _open.is_empty():
		end_span()
	_end_us = Time.get_ticks_usec()
	_ring.append(self)
	while _ring.size() > RING_SIZE:
		_ring.pop_front()
	var line := summary()
	print_verbose("PerfTimeline: ", line)
	return line


func total_ms() -> float:
	var end := _end_us if _end_us > 0 else Time.get_ticks_usec()
	return float(end - _start_us) / 1000.0


## Milliseconds of the first completed span named `name` (0.0 when absent).
func span_ms(name: String) -> float:
	for s in _spans:
		if String(s["name"]) == name and int(s["end_us"]) > 0:
			return float(int(s["end_us"]) - int(s["start_us"])) / 1000.0
	return 0.0


func span_names() -> PackedStringArray:
	var names := PackedStringArray()
	for s in _spans:
		names.append(String(s["name"]))
	return names


## Deep copy of the span records ({ name, depth, start_us, end_us }) for tooling.
func spans() -> Array:
	return _spans.duplicate(true)


## "label: 2.4s — terrain 1.8s, objects 520ms, parse 40ms".
func summary(top_n := 4) -> String:
	return "%s: %s" % [label, brief(top_n)]


## The label-less timing line ("2.4s — terrain 1.8s, objects 520ms"): total plus
## the largest top-level spans (descending, up to `top_n`). For embedding in a
## owner's own status message.
func brief(top_n := 4) -> String:
	var tops: Array = []
	for s in _spans:
		if int(s["depth"]) == 0 and int(s["end_us"]) > 0:
			tops.append(s)
	tops.sort_custom(func(a, b):
		return int(a["end_us"]) - int(a["start_us"]) > int(b["end_us"]) - int(b["start_us"]))
	var parts := PackedStringArray()
	for i in range(mini(top_n, tops.size())):
		var s: Dictionary = tops[i]
		parts.append("%s %s" % [s["name"], format_ms(float(int(s["end_us"]) - int(s["start_us"])) / 1000.0)])
	var head := format_ms(total_ms())
	return head if parts.is_empty() else "%s — %s" % [head, ", ".join(parts)]


## The one duration rendering every perf surface shares (the summary line and
## the F3 Perf tree agree by construction).
static func format_ms(ms: float) -> String:
	return ("%.1fs" % (ms / 1000.0)) if ms >= 1000.0 else ("%dms" % int(roundf(ms)))
