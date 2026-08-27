class_name ProbeRun
extends RefCounted

## One probe run's record: identity, state, the log ring op=status pages
## through by cursor, progress, verdict, artifacts and the error text.

const LINE_CAP := 4000

const STATE_RUNNING := "running"
const STATE_PASSED := "passed"
const STATE_FAILED := "failed"
const STATE_CANCELLED := "cancelled"
const STATE_ERROR := "error"

var run_id := ""
var name := ""
var state := STATE_RUNNING
var started_at := ""
var started_ticks_ms := 0
var finished_ticks_ms := 0
var artifact_dir := ""
var verdict: ProbeVerdict = null
var artifacts: Array[Dictionary] = []
var progress: Dictionary = {}
var error := ""
var cancel_requested := false

var _lines: Array[Dictionary] = []
var _next_seq := 1


func add_line(text: String) -> void:
	_lines.append({
		"seq": _next_seq,
		"t_ms": Time.get_ticks_msec() - started_ticks_ms,
		"text": text,
	})
	_next_seq += 1
	if _lines.size() > LINE_CAP:
		_lines = _lines.slice(_lines.size() - LINE_CAP)


## The newest line's seq (0 before any line).
func last_seq() -> int:
	return _next_seq - 1


func is_running() -> bool:
	return state == STATE_RUNNING


func elapsed_ms() -> int:
	var end := finished_ticks_ms if finished_ticks_ms > 0 else Time.get_ticks_msec()
	return end - started_ticks_ms


## The game_probe op=status wire shape: lines with seq > cursor.
func to_status(cursor: int) -> Dictionary:
	var page: Array = []
	for line in _lines:
		if int(line["seq"]) > cursor:
			page.append(line)
	return {
		"run_id": run_id,
		"name": name,
		"state": state,
		"started_at": started_at,
		"elapsed_ms": elapsed_ms(),
		"lines": page,
		"next_cursor": maxi(cursor, last_seq()),
		"progress": McpJson.sanitize(progress),
		"verdict": verdict.to_json_value() if verdict != null else null,
		"artifacts": artifacts,
		"artifact_dir": artifact_dir,
		"cancel_requested": cancel_requested,
		"error": error,
	}
