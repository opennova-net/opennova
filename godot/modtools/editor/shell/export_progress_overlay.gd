class_name ShellExportProgress
extends RefCounted

## The modal export-progress overlay (backdrop + panel + bar + counts), driven
## by the ACTIVE workspace's is_busy/export-progress capability hooks from the
## shell's per-frame poll, with fade tweens on state flips.

var _shell: Control
var _backdrop: ColorRect
var _panel: PanelContainer
var _title_label: Label
var _message_label: Label
var _bar: ProgressBar
var _counts_label: Label
# func() -> EditorWorkspace: the active workspace.
var _active_workspace: Callable
# func(text, duration): the shell's status toast.
var _show_status: Callable
# func(): busy modulation refresh after an export completes.
var _refresh_shell_state: Callable

var _active: bool = false
var _tween: Tween


func setup(
	shell: Control,
	backdrop: ColorRect,
	panel: PanelContainer,
	title_label: Label,
	message_label: Label,
	bar: ProgressBar,
	counts_label: Label,
	active_workspace: Callable,
	show_status: Callable,
	refresh_shell_state: Callable
) -> void:
	_shell = shell
	_backdrop = backdrop
	_panel = panel
	_title_label = title_label
	_message_label = message_label
	_bar = bar
	_counts_label = counts_label
	_active_workspace = active_workspace
	_show_status = show_status
	_refresh_shell_state = refresh_shell_state


func on_export_started(_dir_path: String) -> void:
	# The workspace hook, not a hard-coded "Exporting terrain..." — any
	# exporting workspace announces itself (the same title the progress
	# overlay shows).
	var workspace: EditorWorkspace = _active_workspace.call()
	_show_status.call(workspace.get_export_progress_title() if workspace != null else "Exporting...", 30.0)
	_set_active(true)
	sync()


func on_export_completed(err: Error, message: String) -> void:
	_set_active(false)
	if not message.is_empty():
		_show_status.call(message, 6.0)
	else:
		_show_status.call("Export failed (error %d)" % err, 6.0)
	_refresh_shell_state.call()


func sync() -> void:
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null:
		_set_active(false)
		return

	var export_running: bool = workspace.is_busy()
	if export_running != _active:
		_set_active(export_running)
	if not export_running:
		return

	var phase: String = workspace.get_export_progress_phase()
	var message: String = workspace.get_export_progress_message()
	var current: int = workspace.get_export_progress_current()
	var total: int = workspace.get_export_progress_total()
	var ratio: float = clampf(workspace.get_export_progress_ratio(), 0.0, 1.0)

	_title_label.text = workspace.get_export_progress_title()
	if not phase.is_empty():
		_message_label.text = phase.capitalize() + ": " + message
	else:
		_message_label.text = message
	_bar.value = ratio * 100.0
	if total > 0:
		_counts_label.text = "%d / %d" % [current, total]
	else:
		_counts_label.text = ""


func _set_active(active: bool) -> void:
	if _active == active and _backdrop.visible == active:
		return
	_active = active
	if _tween:
		_tween.kill()
	_tween = _shell.create_tween()
	if active:
		_backdrop.visible = true
		_panel.visible = true
		_backdrop.modulate = Color(1.0, 1.0, 1.0, 0.0)
		_panel.modulate = Color(1.0, 1.0, 1.0, 0.0)
		_tween.tween_property(_backdrop, "modulate", Color(1.0, 1.0, 1.0, 1.0), 0.16).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_OUT)
		_tween.parallel().tween_property(_panel, "modulate", Color(1.0, 1.0, 1.0, 1.0), 0.16).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_OUT)
	else:
		_tween.tween_property(_backdrop, "modulate", Color(1.0, 1.0, 1.0, 0.0), 0.14).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_IN)
		_tween.parallel().tween_property(_panel, "modulate", Color(1.0, 1.0, 1.0, 0.0), 0.14).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_IN)
		_tween.finished.connect(func() -> void:
			if not _active:
				_backdrop.visible = false
				_panel.visible = false
		, CONNECT_ONE_SHOT)
