class_name MusicLiveMode
extends Control

const MusicVarInspectorClass = preload("res://modtools/music/ui/var_inspector.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")

# VM state values mirror libs/mus MusVMState.
const VM_STOPPED := 0
const VM_RUNNING := 1
const VM_PAUSED := 2
const VM_HALTED := 3
const VM_ERROR := 4

# Colours used by the state label. STOPPED leaves the theme default in place;
# the rest swap a theme override on/off so a dark editor theme still reads
# RUNNING as obviously green.
const COLOR_RUNNING := Color(0.4, 0.85, 0.4)
const COLOR_PAUSED := Color(1.0, 0.85, 0.3)
const COLOR_HALTED := Color(1.0, 0.85, 0.3)
const COLOR_ERROR := Color(1.0, 0.4, 0.4)

var _document: RefCounted
var _director: NovaMusicDirector
var _current_section: StringName = &""
var _last_state: int = VM_STOPPED
var _start_warning_until_ms: int = 0

@onready var _start_btn: Button = %StartButton
@onready var _pause_btn: Button = %PauseButton
@onready var _resume_btn: Button = %ResumeButton
@onready var _stop_btn: Button = %StopButton
@onready var _state_label: Label = %StateLabel
@onready var _var_inspector: Control = %VarInspector
@onready var _events: ItemList = %Events
@onready var _clear_btn: Button = %ClearButton
@onready var _graph: VBoxContainer = %SectionGraph


func bind_document(document: RefCounted) -> void:
	if _document == document:
		return
	# When the workspace re-binds (e.g. user closed and reopened a project),
	# unhook from the old document's changed signal so we don't keep getting
	# pings after it's gone.
	if _document != null and _document.changed.is_connected(_on_document_changed):
		_document.changed.disconnect(_on_document_changed)
	_document = document
	if _document != null:
		_document.changed.connect(_on_document_changed)
	if is_node_ready():
		_on_document_changed()


# Fired both on initial bind and any time the document re-emits `changed`
# (open/close/reorder/rename/etc). All three Live-mode panels read off the
# document, so refresh them together. Without this, opening a project via
# the Open menu (vs. having it pre-loaded from saved state) leaves the
# Start button grayed out, var labels stuck on Var00, and the section
# graph empty: bind_document only fires once on mount, before the user
# has chosen a file.
func _on_document_changed() -> void:
	_refresh_graph()
	_refresh_button_state()
	_refresh_var_labels()


func _ready() -> void:
	_director = NovaMusicDirector.new()
	_director.auto_start = false
	add_child(_director)
	_director.section_entered.connect(_on_section)
	_director.sound_triggered.connect(_on_sound)
	_director.echo.connect(_on_echo)
	_director.halted.connect(_on_halted)
	_director.vm_error.connect(_on_vm_error)
	_director.variable_changed.connect(_on_variable_changed)
	_director.volume_changed.connect(_on_volume_changed)
	_start_btn.pressed.connect(_on_start)
	_pause_btn.pressed.connect(_on_pause)
	_resume_btn.pressed.connect(_on_resume)
	_stop_btn.pressed.connect(_on_stop)
	_clear_btn.pressed.connect(_on_clear)
	if _var_inspector.has_method("bind_director"):
		_var_inspector.call("bind_director", _director)
	_apply_state_label(VM_STOPPED)
	_refresh_graph()
	_refresh_button_state()
	_refresh_var_labels()


# Polls the VM only while it's plausibly active, and only flips the label on
# transition. Steady-state runs avoid the theme-override churn that would
# otherwise fight the signal-driven updates.
func _process(_delta: float) -> void:
	if _director == null:
		return
	# Clear an expired Start-warning flash so the label snaps back to state.
	if _start_warning_until_ms != 0 and Time.get_ticks_msec() >= _start_warning_until_ms:
		_start_warning_until_ms = 0
		_apply_state_label(_last_state)
	var state: int = _director.vm_state()
	if state != _last_state:
		_apply_state_label(state)
		_refresh_button_state()


func _on_start() -> void:
	if _document == null or not _document.bank_loaded() or not _document.script_loaded():
		# Surface the failure on the state label for 2 seconds. push_warning
		# was invisible to the user; this lands in the same spot they're
		# already looking after pressing Start.
		_flash_start_warning("Load a project first")
		return
	_director.bank = _document.bank
	_director.load_mus_script(_document.mus_script)
	_refresh_var_labels()
	_director.start()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()


# Push the loaded script's name down to the var inspector so it can swap
# raw VarXX labels for known friendly names (menuscript / gamescript). When
# no script is loaded, falls back to the empty-string form which renders
# raw VarXX.
func _refresh_var_labels() -> void:
	if _var_inspector == null or not _var_inspector.has_method("set_script_name"):
		return
	var script_name: String = ""
	if _document != null and _document.script_loaded():
		script_name = String(_document.mus_script.get_default_script_name())
	_var_inspector.call("set_script_name", script_name)


func _on_pause() -> void:
	if _director == null:
		return
	_director.pause()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_log("paused")


func _on_resume() -> void:
	if _director == null:
		return
	_director.resume()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_log("resumed")


func _on_stop() -> void:
	if _director == null:
		return
	_director.stop()
	_apply_state_label(VM_STOPPED)
	_refresh_button_state()


# Public stop hook used by music_workspace.gd::activate_workflow when leaving
# Live mode so audio doesn't bleed into the next workspace tab. No-op when
# the VM is already stopped.
func stop_director() -> void:
	if _director == null:
		return
	if _director.vm_state() == VM_STOPPED:
		return
	_director.stop()
	_apply_state_label(VM_STOPPED)
	_refresh_button_state()


func _on_section(section_name: StringName) -> void:
	_current_section = section_name
	_refresh_graph()
	_log("section -> %s" % section_name)


func _refresh_graph() -> void:
	for c in _graph.get_children():
		c.queue_free()
	if _document == null or not _document.script_loaded():
		var hint := Label.new()
		hint.text = "Open a project to view section transitions."
		hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		_graph.add_child(hint)
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	var graph := MusicSectionGraphClass.analyze(_document.mus_script, script_name)
	if graph.is_empty():
		var empty_hint := Label.new()
		empty_hint.text = "(this script has no sections)"
		empty_hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		_graph.add_child(empty_hint)
		return
	for section_name in graph.keys():
		var row := HBoxContainer.new()
		var lbl := Label.new()
		lbl.text = section_name
		if section_name == String(_current_section):
			lbl.add_theme_color_override("font_color", Color(1.0, 0.83, 0.47))
		row.add_child(lbl)
		for target in graph[section_name]:
			var arrow := Label.new()
			arrow.text = " -> %s" % target
			row.add_child(arrow)
		_graph.add_child(row)


func _on_sound(idx: int, sound_name: StringName, wait: bool) -> void:
	_log("play %d (%s)%s" % [idx, sound_name, " wait" if wait else ""])


func _on_echo(arg: int) -> void:
	_log("echo %d" % arg)


# Emitted by the VM when a section runs to its terminal `done`. Treat as a
# clean end-of-track and surface it visually so the user sees Stop wasn't
# required.
func _on_halted() -> void:
	_apply_state_label(VM_HALTED)
	_log("halted")


# vm_error means mus_vm_load_script or runtime hit a hard fault. The director
# already flipped its internal state to ERROR; we mirror it.
func _on_vm_error(message: String) -> void:
	_apply_state_label(VM_ERROR)
	if message.is_empty():
		_log("error")
	else:
		_log("error: %s" % message)


func _on_variable_changed(var_index: int, value: int) -> void:
	_log("var Var%02d = %d" % [var_index, value])


func _on_volume_changed(left: int, right: int) -> void:
	# GSV/GSDV emit 16.16 fixed-point ints (witnessed: dfvas!Intrinsic_GSV @
	# 0x5578B0). Render as decimal so logs read as `volume L=200.00 R=200.00`
	# instead of the raw `L=13107200`.
	_log("volume L=%.2f R=%.2f" % [left / 65536.0, right / 65536.0])


func _on_clear() -> void:
	if _events == null:
		return
	_events.clear()


# Append the new line, drop the oldest entry if we're past the 50-row cap, and
# scroll the tail into view. ensure_current_is_visible only nudges the scroll
# when an item is current, so we set the new tail current first. The previous
# implementation cleared + re-added every entry every frame, with no item
# selected, leaving the scroll pinned to the head of the list.
func _log(text: String) -> void:
	if _events == null:
		return
	_events.add_item("[%s] %s" % [_timestamp(), text])
	if _events.item_count > 50:
		_events.remove_item(0)
	if _events.item_count > 0:
		_events.select(_events.item_count - 1)
		_events.ensure_current_is_visible()


func _timestamp() -> String:
	var t := Time.get_time_dict_from_system()
	return "%02d:%02d:%02d" % [t["hour"], t["minute"], t["second"]]


# Translate a MusVMState enum value into a label + colour. _process polling
# and direct signal handlers share this so transitions look consistent. While
# a Start-warning flash is active the label is owned by the warning instead.
func _apply_state_label(state: int) -> void:
	_last_state = state
	if _state_label == null:
		return
	if _start_warning_until_ms != 0 and Time.get_ticks_msec() < _start_warning_until_ms:
		return
	match state:
		VM_RUNNING:
			_state_label.text = "RUNNING"
			_state_label.add_theme_color_override("font_color", COLOR_RUNNING)
		VM_PAUSED:
			_state_label.text = "PAUSED"
			_state_label.add_theme_color_override("font_color", COLOR_PAUSED)
		VM_HALTED:
			_state_label.text = "HALTED"
			_state_label.add_theme_color_override("font_color", COLOR_HALTED)
		VM_ERROR:
			_state_label.text = "ERROR"
			_state_label.add_theme_color_override("font_color", COLOR_ERROR)
		_:
			_state_label.text = "STOPPED"
			_state_label.remove_theme_color_override("font_color")


# Show a 2-second red message owning the state label, e.g. "Load a project
# first" after Start was pressed without a project. _process clears it once
# the deadline passes and snaps the label back to vm_state().
func _flash_start_warning(message: String) -> void:
	if _state_label == null:
		return
	_state_label.text = message
	_state_label.add_theme_color_override("font_color", COLOR_ERROR)
	_start_warning_until_ms = Time.get_ticks_msec() + 2000


# Toolbar button enable rules:
# - Start: enabled iff a bank+script are both loaded AND VM is not running/paused
# - Pause: enabled iff RUNNING
# - Resume: enabled iff PAUSED
# - Stop: enabled iff RUNNING or PAUSED
func _refresh_button_state() -> void:
	var has_project: bool = _document != null and _document.bank_loaded() and _document.script_loaded()
	var state: int = _last_state
	if _start_btn != null:
		_start_btn.disabled = not has_project or state == VM_RUNNING or state == VM_PAUSED
	if _pause_btn != null:
		_pause_btn.disabled = state != VM_RUNNING
	if _resume_btn != null:
		_resume_btn.disabled = state != VM_PAUSED
	if _stop_btn != null:
		_stop_btn.disabled = state != VM_RUNNING and state != VM_PAUSED
