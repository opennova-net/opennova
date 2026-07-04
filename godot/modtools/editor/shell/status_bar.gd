class_name ShellStatusBar
extends RefCounted

## The shell's bottom status bar (tool/message, context, camera, fps), the
## transient status message shown in the tool cell, and the top-bar context
## header + OS window title it keeps in sync. Refreshed from the shell's
## per-frame poll; every text set is change-cached so the common frame does
## no work.

var _tool_label: Label
var _context_label: Label
var _camera_label: Label
var _fps_label: Label
var _context_workspace_label: Label
var _context_doc_label: Label
# func() -> EditorWorkspace: the active workspace.
var _active_workspace: Callable

var _message_text: String = ""
var _message_until: float = 0.0
var _message_severity: StringName = &"info"

# B10: per-severity defaults (duration <= 0 picks these) and the theme
# variation each severity renders with. Unknown severities read as info.
const _SEVERITY_DURATIONS := {
	&"info": 4.0,
	&"success": 3.5,
	&"warn": 6.0,
	&"error": 8.0,
}
const _SEVERITY_VARIATIONS := {
	&"info": &"Info",
	&"success": &"Success",
	&"warn": &"Warn",
	&"error": &"Error",
}
# Last applied context-header doc title + OS window title; each is recomputed
# only when its text actually changes.
var _context_ws_label_cache := ""
var _context_doc_title_cache := ""
var _window_title_cache := ""


func setup(
	tool_label: Label,
	context_label: Label,
	camera_label: Label,
	fps_label: Label,
	context_workspace_label: Label,
	context_doc_label: Label,
	active_workspace: Callable
) -> void:
	_tool_label = tool_label
	_context_label = context_label
	_camera_label = camera_label
	_fps_label = fps_label
	_context_workspace_label = context_workspace_label
	_context_doc_label = context_doc_label
	_active_workspace = active_workspace


func show_status_message(text: String, duration: float = 0.0, severity: StringName = &"info") -> void:
	# Mirror into the MCP log hub so connected agents see what the human sees
	# (static no-op while no agent server is running).
	McpLogHub.note_status(text)
	_message_text = text
	_message_severity = severity if _SEVERITY_VARIATIONS.has(severity) else &"info"
	var effective := duration
	if effective <= 0.0:
		effective = float(_SEVERITY_DURATIONS.get(_message_severity, 4.0))
	_message_until = Time.get_ticks_msec() / 1000.0 + effective


func refresh() -> void:
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null:
		_tool_label.text = ""
		_context_label.text = ""
		_camera_label.text = ""
		_fps_label.text = ""
		return

	var now := Time.get_ticks_msec() / 1000.0
	if _message_text != "" and now < _message_until:
		_tool_label.text = _message_text
		_tool_label.theme_type_variation = _SEVERITY_VARIATIONS.get(_message_severity, &"Info")
	else:
		_tool_label.text = workspace.get_status_tool()
		_tool_label.theme_type_variation = &""

	_context_label.text = workspace.get_status_context()
	# The capability hook: each workspace reports its own active camera
	# (mission's follows play mode).
	var status_camera: Camera3D = workspace.get_viewport_camera() if workspace.shows_camera_status() else null
	if status_camera != null:
		var pos: Vector3 = status_camera.global_position
		_camera_label.text = "%.0f, %.0f, %.0f" % [pos.x, pos.y, pos.z]
	else:
		_camera_label.text = ""
	_fps_label.text = "%d fps" % Engine.get_frames_per_second()
	_refresh_context_header(workspace)


# Top-bar context: the active workspace name (Heading) plus its document title
# (Muted), and the OS window title. The doc label is suppressed when the title
# is just the workspace name (nothing open) so the header never reads
# "Mission / Mission"; get_project_title() already folds in the dirty "*".
func _refresh_context_header(workspace: EditorWorkspace) -> void:
	var ws_label := "" if workspace == null else workspace.get_workspace_label()
	var title := "OpenNova Editor" if workspace == null else workspace.get_project_title()
	if ws_label == _context_ws_label_cache and title == _context_doc_title_cache:
		return
	_context_ws_label_cache = ws_label
	_context_doc_title_cache = title
	_context_workspace_label.text = ws_label
	var doc_text := "" if title == ws_label else title
	_context_doc_label.text = doc_text
	# Size the doc label to its text instead of a fixed column: short titles stop
	# wasting top-bar width, long ones clip at a cap so the 1024px shell still
	# fits every group.
	if doc_text.is_empty():
		_context_doc_label.custom_minimum_size.x = 0.0
	else:
		var font := _context_doc_label.get_theme_font(&"font")
		var font_size := _context_doc_label.get_theme_font_size(&"font_size")
		if font != null:
			var text_width := font.get_string_size(doc_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
			_context_doc_label.custom_minimum_size.x = clampf(text_width + 8.0, 0.0, 280.0)
	_refresh_window_title(ws_label, title)


# Mirror the context into the OS window title, cached so it only re-sets on
# change. Headless display servers no-op window_set_title, which is harmless.
func _refresh_window_title(ws_label: String, title: String) -> void:
	var window_title := "OpenNova Editor"
	if not ws_label.is_empty():
		var head := ws_label if title == ws_label else "%s - %s" % [ws_label, title]
		window_title = "%s - OpenNova Editor" % head
	if window_title == _window_title_cache:
		return
	_window_title_cache = window_title
	DisplayServer.window_set_title(window_title)
