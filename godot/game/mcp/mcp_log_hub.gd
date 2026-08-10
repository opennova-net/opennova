class_name McpLogHub
extends RefCounted

## The MCP server's log surface: a ring buffer of structured entries that the
## get_logs tool pages with cursors. Two feeds:
##
##  - In-process notes: server request log, script ctx.log output, and editor
##    status-bar messages (EditorWorkstation.show_status_message mirrors here
##    via the static note_status, a no-op when no server is running).
##  - Engine lines tailed from Godot's rotated log file (user://logs/godot.log,
##    on by default on desktop). GDScript cannot hook push_error in-process,
##    so the file tail is how agents see engine errors, script errors, and
##    prints. Rotation happens at startup only, so byte cursors stay valid for
##    the whole session. When file logging is off, engine capture degrades to
##    an explicit "unavailable" — never silently.

const RING_CAP := 2000

static var instance: McpLogHub = null

var _entries: Array = []
var _next_seq := 1
var _engine_log_path := ""
var _engine_pos := 0
var _engine_fragment := ""


func _init() -> void:
	var setting := String(ProjectSettings.get_setting("debug/file_logging/log_path", "user://logs/godot.log"))
	var global := ProjectSettings.globalize_path(setting)
	if FileAccess.file_exists(global):
		_engine_log_path = global
		_engine_pos = _file_length(global)


## Mirror seam for EditorWorkstation.show_status_message; safe to call when no
## MCP service is running.
static func note_status(text: String) -> void:
	if instance != null:
		instance.note("status", "info", text)


func note_server(text: String) -> void:
	note("server", "info", text)


func note(source: String, level: String, text: String) -> void:
	_entries.append({
		"seq": _next_seq,
		"t_ms": Time.get_ticks_msec(),
		"source": source,
		"level": level,
		"text": text,
	})
	_next_seq += 1
	while _entries.size() > RING_CAP:
		_entries.pop_front()


func engine_available() -> bool:
	return not _engine_log_path.is_empty()


## Byte position bookmark in the engine log, for engine_delta() around an
## operation (the script runner brackets compiles/executes with this).
func engine_mark() -> int:
	return _file_length(_engine_log_path) if engine_available() else -1


## Engine log lines appended since `from_pos`, merged into structured
## { level, text } blocks (continuation lines fold into their parent entry).
## Stateless — does not move the ingest cursor.
func engine_delta(from_pos: int) -> Array:
	if not engine_available() or from_pos < 0:
		return []
	var raw := _read_from(from_pos)
	return _merge_lines(raw["lines"])


## Pull new engine log lines into the ring (source "engine"). Called by the
## get_logs tool before paging.
func ingest_engine() -> void:
	if not engine_available():
		return
	var raw := _read_from(_engine_pos, _engine_fragment)
	_engine_pos = raw["end_pos"]
	_engine_fragment = raw["fragment"]
	for block in _merge_lines(raw["lines"]):
		note("engine", block["level"], block["text"])


## Page entries after `cursor` (a seq). cursor <= 0 returns the tail of the
## last `limit` entries. `sources` filters ("server", "script", "status",
## "engine"); empty means all. `dropped` counts entries the ring overwrote
## before they were read.
func get_entries(cursor := 0, limit := 200, sources := PackedStringArray()) -> Dictionary:
	limit = clampi(limit, 1, RING_CAP)
	var first_seq := int(_entries[0]["seq"]) if not _entries.is_empty() else _next_seq
	var dropped := maxi(first_seq - 1 - cursor, 0) if cursor > 0 else 0
	var matched: Array = []
	for entry: Dictionary in _entries:
		if cursor > 0 and int(entry["seq"]) <= cursor:
			continue
		if not sources.is_empty() and not sources.has(String(entry["source"])):
			continue
		matched.append(entry)
	var start := maxi(matched.size() - limit, 0) if cursor <= 0 else 0
	var page := matched.slice(start, start + limit)
	var next_cursor := int(page.back()["seq"]) if not page.is_empty() else maxi(cursor, _next_seq - 1 if cursor <= 0 else cursor)
	return {
		"entries": page,
		"next_cursor": next_cursor,
		"dropped": dropped,
		"engine_log": "tailing" if engine_available() else "unavailable",
	}


## The newest assigned seq — hand this to clients as a "you are here" cursor.
func latest_cursor() -> int:
	return _next_seq - 1


## Test seam (and escape hatch): point the engine tail at a specific file.
func set_engine_log_path(path: String) -> void:
	_engine_log_path = path
	_engine_pos = 0
	_engine_fragment = ""


static func _file_length(path: String) -> int:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return 0
	var length := file.get_length()
	file.close()
	return length


# Read complete lines from `from_pos` to EOF. A trailing fragment without a
# newline is returned separately (mid-write line) so callers never ingest a
# half-written entry; `carry` is prepended to the first line read.
func _read_from(from_pos: int, carry := "") -> Dictionary:
	var out := { "lines": PackedStringArray(), "end_pos": from_pos, "fragment": carry }
	var file := FileAccess.open(_engine_log_path, FileAccess.READ)
	if file == null:
		return out
	var length := file.get_length()
	if from_pos >= length:
		file.close()
		return out
	file.seek(from_pos)
	var text := carry + file.get_buffer(length - from_pos).get_string_from_utf8()
	file.close()
	out["end_pos"] = length
	var lines := text.split("\n")
	if not text.ends_with("\n"):
		out["fragment"] = lines[lines.size() - 1]
		lines.remove_at(lines.size() - 1)
	else:
		out["fragment"] = ""
		if not lines.is_empty() and lines[lines.size() - 1].is_empty():
			lines.remove_at(lines.size() - 1)
	out["lines"] = lines
	return out


# Fold raw log lines into { level, text } blocks: a line starting with
# whitespace (at: frames, backtraces) continues the previous block.
static func _merge_lines(lines: PackedStringArray) -> Array:
	var blocks: Array = []
	for line in lines:
		var trimmed := line.strip_edges(false, true)
		if trimmed.is_empty():
			continue
		var continuation := line.begins_with(" ") or line.begins_with("\t")
		if continuation and not blocks.is_empty():
			blocks.back()["text"] += "\n" + trimmed.strip_edges()
			continue
		blocks.append({ "level": _classify(trimmed), "text": trimmed })
	return blocks


static func _classify(line: String) -> String:
	if line.begins_with("USER ERROR:") or line.begins_with("SCRIPT ERROR:") or line.begins_with("ERROR:"):
		return "error"
	if line.begins_with("USER WARNING:") or line.begins_with("WARNING:"):
		return "warn"
	return "info"
