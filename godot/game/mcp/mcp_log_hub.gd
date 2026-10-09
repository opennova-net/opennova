class_name McpLogHub
extends RefCounted

## The MCP server's log surface: a ring buffer of structured entries that the
## get_logs tool pages with cursors. Three feeds:
##
##  - In-process notes: server request log, script ctx.log output, probes.
##  - Engine entries (source "engine"): the native io::log ring
##    (base/io/log_ring.h) drained through DevTools.engine_log_after —
##    every level the engine emits, warn+ also reaches the Godot log via
##    the chained push_warning forwarder.
##  - Godot lines (source "godot") tailed from Godot's rotated log file
##    (user://logs/godot.log, on by default on desktop). GDScript cannot
##    hook push_error in-process, so the file tail is how agents see script
##    errors, Godot warnings, and prints. Rotation happens at startup only,
##    so byte cursors stay valid for the whole session. When file logging is
##    off, that capture degrades to an explicit "unavailable" — never
##    silently.

const RING_CAP := 2000

static var instance: McpLogHub = null

var _entries: Array = []
var _next_seq := 1
var _engine_cursor := 0
var _engine_drain: Callable = Callable()
var _godot_log_path := ""
var _godot_pos := 0
var _godot_fragment := ""


func _init() -> void:
	var setting := String(ProjectSettings.get_setting("debug/file_logging/log_path", "user://logs/godot.log"))
	var global := resolve_godot_log_path(OS.get_cmdline_args(),
			ProjectSettings.globalize_path(setting))
	if FileAccess.file_exists(global):
		_godot_log_path = global
		_godot_pos = _file_length(global)


## The Godot log to tail: Godot's own `--log-file <path>` when the launch
## carried one (the scripted launchers always pass it), else `fallback` (the
## project's rotated user://logs file).
static func resolve_godot_log_path(args: PackedStringArray, fallback: String) -> String:
	for i in range(args.size()):
		var arg := String(args[i])
		if arg == "--log-file" and i + 1 < args.size():
			var path := String(args[i + 1]).strip_edges()
			return ProjectSettings.globalize_path(path) if not path.is_empty() else fallback
		if arg.begins_with("--log-file="):
			var inline := arg.get_slice("=", 1).strip_edges()
			return ProjectSettings.globalize_path(inline) if not inline.is_empty() else fallback
	return fallback


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


## Pull new engine io::log ring entries into the ring (source "engine").
## Called by the get_logs tool before paging. A sequence gap (the native
## 512-entry ring wrapped past unread entries) surfaces as one warn note.
func ingest_engine() -> void:
	var page: Dictionary = _engine_drain.call(_engine_cursor) \
			if _engine_drain.is_valid() else DevTools.engine_log_after(_engine_cursor)
	var seqs: PackedInt64Array = page.get("sequences", PackedInt64Array())
	if seqs.is_empty():
		return
	var levels: PackedStringArray = page.get("levels", PackedStringArray())
	var texts: PackedStringArray = page.get("texts", PackedStringArray())
	var lost := int(seqs[0]) - _engine_cursor - 1
	if lost > 0:
		note("engine", "warn",
				"engine log ring wrapped: %d unread entries lost" % lost)
	for i in range(seqs.size()):
		note("engine", levels[i], texts[i])
	_engine_cursor = int(seqs[seqs.size() - 1])


func godot_log_available() -> bool:
	return not _godot_log_path.is_empty()


## Byte position bookmark in the Godot log, for godot_log_delta() around an
## operation (bracketing a compile/execute to catch its script errors).
func godot_log_mark() -> int:
	return _file_length(_godot_log_path) if godot_log_available() else -1


## Godot log lines appended since `from_pos`, merged into structured
## { level, text } blocks (continuation lines fold into their parent entry).
## Stateless — does not move the ingest cursor.
func godot_log_delta(from_pos: int) -> Array:
	if not godot_log_available() or from_pos < 0:
		return []
	var raw := _read_from(from_pos)
	return _merge_lines(raw["lines"])


## Pull new Godot log lines into the ring (source "godot"). Called by the
## get_logs tool before paging.
func ingest_godot_log() -> void:
	if not godot_log_available():
		return
	var raw := _read_from(_godot_pos, _godot_fragment)
	_godot_pos = raw["end_pos"]
	_godot_fragment = raw["fragment"]
	for block in _merge_lines(raw["lines"]):
		note("godot", block["level"], block["text"])


## Page entries after `cursor` (a seq). cursor <= 0 returns the tail of the
## last `limit` entries. `sources` filters ("server", "script", "status",
## "probe", "engine", "godot"); empty means all. `dropped` counts entries the
## ring overwrote before they were read.
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
		"godot_log": "tailing" if godot_log_available() else "unavailable",
	}


## One page for an MCP session (McpServer.session): from `cursor` when the
## call names one (>= 0), else from where the session's last page ended; the
## session's cursor then moves to this page's end. An empty session (none by
## that id) pages from the start and keeps nothing.
func session_page(session: Dictionary, cursor: int, limit: int,
		sources := PackedStringArray()) -> Dictionary:
	if cursor < 0:
		cursor = int(session.get("log_cursor", 0)) \
				if not session.is_empty() else 0
	var page := get_entries(cursor, limit, sources)
	if not session.is_empty():
		session["log_cursor"] = page["next_cursor"]
	return page


## Test seam: replace the native ring drain (DevTools.engine_log_after) with
## a canned column supplier.
func set_engine_drain(drain: Callable) -> void:
	_engine_drain = drain
	_engine_cursor = 0


## Test seam (and escape hatch): point the Godot tail at a specific file.
func set_godot_log_path(path: String) -> void:
	_godot_log_path = path
	_godot_pos = 0
	_godot_fragment = ""


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
	var file := FileAccess.open(_godot_log_path, FileAccess.READ)
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
