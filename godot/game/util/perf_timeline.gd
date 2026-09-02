class_name PerfTimeline
extends RefCounted

## Named nested wall-clock spans for one operation (a mission load). An owner
## creates a timeline with begin(), brackets stages with span()/end_span(), and
## finish()es it, which prints one structured line. The owner keeps the
## timeline it cares about (GameWorld.last_load_timeline()); nothing global
## runs between operations: a timeline only costs while its operation does.

var label := ""
var _spans: Array[Span] = []
var _open: Array[int] = []   # stack of indices into _spans
var _start_us := 0
var _end_us := 0


## One named wall-clock span: its nesting depth and its start / end ticks
## (end_us 0 while open).
class Span:
	extends RefCounted
	var name: String
	var depth: int
	var start_us: int
	var end_us: int = 0

	func _init(p_name: String, p_depth: int, p_start_us: int) -> void:
		name = p_name
		depth = p_depth
		start_us = p_start_us

	func duration_us() -> int:
		return end_us - start_us


static func begin(operation_label: String) -> PerfTimeline:
	var timeline := PerfTimeline.new()
	timeline.label = operation_label
	timeline._start_us = Time.get_ticks_usec()
	return timeline


func span(name: String) -> void:
	_open.push_back(_spans.size())
	_spans.append(Span.new(name, _open.size() - 1, Time.get_ticks_usec()))


## Ends the innermost open span (no-op when none is open).
func end_span() -> void:
	if _open.is_empty():
		return
	var idx: int = _open.pop_back()
	_spans[idx].end_us = Time.get_ticks_usec()


## Close any spans an early return left open plus the timeline itself, print
## the structured line, and return the one-line summary so callers can surface
## it (status bar, log).
func finish() -> String:
	while not _open.is_empty():
		end_span()
	_end_us = Time.get_ticks_usec()
	var line := summary()
	print_verbose("PerfTimeline: ", line)
	return line


func total_ms() -> float:
	var end := _end_us if _end_us > 0 else Time.get_ticks_usec()
	return float(end - _start_us) / 1000.0


## Milliseconds of the first completed span named `name` (0.0 when absent).
func span_ms(name: String) -> float:
	for s in _spans:
		if s.name == name and s.end_us > 0:
			return float(s.duration_us()) / 1000.0
	return 0.0


func span_names() -> PackedStringArray:
	var names := PackedStringArray()
	for s in _spans:
		names.append(s.name)
	return names


## The recorded spans, in order, for tooling.
func spans() -> Array[Span]:
	return _spans.duplicate()


## "label: 2.4s — terrain 1.8s, objects 520ms, parse 40ms".
func summary(top_n := 4) -> String:
	return "%s: %s" % [label, brief(top_n)]


## The label-less timing line ("2.4s — terrain 1.8s, objects 520ms"): total plus
## the largest top-level spans (descending, up to `top_n`). For embedding in a
## owner's own status message.
func brief(top_n := 4) -> String:
	var tops: Array[Span] = []
	for s in _spans:
		if s.depth == 0 and s.end_us > 0:
			tops.append(s)
	tops.sort_custom(func(a: Span, b: Span) -> bool:
		return a.duration_us() > b.duration_us())
	var parts := PackedStringArray()
	for i in range(mini(top_n, tops.size())):
		var s := tops[i]
		parts.append("%s %s" % [s.name, format_ms(float(s.duration_us()) / 1000.0)])
	var head := format_ms(total_ms())
	return head if parts.is_empty() else "%s — %s" % [head, ", ".join(parts)]


## The one duration rendering every perf surface shares (the summary line and
## the load status agree by construction).
static func format_ms(ms: float) -> String:
	return ("%.1fs" % (ms / 1000.0)) if ms >= 1000.0 else ("%dms" % int(roundf(ms)))
