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

# Event-log entry types, used to colour rows and gate the per-type filters.
# section/sound are the events authors validate against and show by default;
# var/volume are high-frequency (the VM fires them on every write / GSV call)
# and are surfaced on the Variables tab + volume meter instead, so their log
# lines are off by default. echo/system always show (errors must never hide).
enum EvType { SECTION, SOUND, VAR, VOLUME, ECHO, SYSTEM }

const _EV_COLOR := {
	EvType.SECTION: Color(0.55, 0.8, 1.0),
	EvType.SOUND: Color(0.6, 0.9, 0.6),
	EvType.VAR: Color(0.85, 0.7, 1.0),
	EvType.VOLUME: Color(1.0, 0.85, 0.4),
	EvType.ECHO: Color(0.8, 0.8, 0.8),
	EvType.SYSTEM: Color(1.0, 0.6, 0.55),
}

var _document: RefCounted
var _director: NovaMusicDirector
var _current_section: StringName = &""
# Consecutive re-entries of the SAME section. A self-loop section (e.g. the
# gamescript's `Missionnull { enter Missionnull }`) re-fires section_entered
# every VM tick; we count those as idle rather than logging each one.
var _idle_ticks: int = 0
var _last_state: int = VM_STOPPED
var _start_warning_until_ms: int = 0

# De-spam state. The VM fires on_volume_changed on every GSV/GSDV call and
# on_var_changed on every pop_global write, including same-value writes, so we
# only log a line when the value actually changes (the meter / inspector still
# reflect every update). Reset on each Start.
var _last_logged_var: Dictionary = {}
var _vol_seen: bool = false
var _last_vol_l: int = 0
var _last_vol_r: int = 0

# Coalesce run-length for the events log: the last appended row's identity and
# how many identical events have folded into it. A distinct (type, text) breaks
# the run and starts a new row, so e.g. Multiplayerstart's 20 identical play
# lines collapse to one "play 0 (sound_0)  x20" row.
var _last_log_type: int = -1
var _last_log_text: String = ""
var _last_log_count: int = 0

@onready var _start_btn: Button = %StartButton
@onready var _pause_btn: Button = %PauseButton
@onready var _resume_btn: Button = %ResumeButton
@onready var _stop_btn: Button = %StopButton
@onready var _state_label: Label = %StateLabel
@onready var _now_playing: Label = %NowPlaying
@onready var _jump_option: OptionButton = %JumpSection
@onready var _volume_meter: Control = %VolumeMeter
@onready var _var_inspector: Control = %VarInspector
@onready var _events: ItemList = %Events
@onready var _clear_btn: Button = %ClearButton
@onready var _filter_section: CheckBox = %FilterSection
@onready var _filter_sound: CheckBox = %FilterSound
@onready var _filter_var: CheckBox = %FilterVar
@onready var _filter_volume: CheckBox = %FilterVolume
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
# (open/close/reorder/rename/etc). All Live-mode panels read off the document,
# so refresh them together. Without this, opening a project via the Open menu
# (vs. having it pre-loaded from saved state) leaves the Start button grayed
# out, var labels stuck on Var00, the jump dropdown empty, and the section
# graph empty: bind_document only fires once on mount, before the user has
# chosen a file.
func _on_document_changed() -> void:
	_refresh_graph()
	_refresh_button_state()
	_refresh_var_labels()
	_refresh_jump_options()
	_refresh_now_playing()


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
	if _jump_option != null:
		_jump_option.item_selected.connect(_on_jump_selected)
	if _var_inspector.has_method("bind_director"):
		_var_inspector.call("bind_director", _director)
	_apply_state_label(VM_STOPPED)
	_refresh_graph()
	_refresh_button_state()
	_refresh_var_labels()
	_refresh_jump_options()
	_refresh_now_playing()


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
		# Section-graph buttons + jump dropdown enable only while RUNNING, and
		# now-playing clears on stop/halt, so refresh them on every transition.
		_refresh_graph()
		_refresh_now_playing()
	if state == VM_RUNNING or state == VM_PAUSED:
		if _var_inspector != null and _var_inspector.has_method("refresh_from_director"):
			_var_inspector.call("refresh_from_director")


func _on_start() -> void:
	if _document == null or not _document.bank_loaded() or not _document.script_loaded():
		# Surface the failure on the state label for 2 seconds. push_warning
		# was invisible to the user; this lands in the same spot they're
		# already looking after pressing Start.
		_flash_start_warning("Load a project first")
		return
	if _document.has_method("prepare_script_for_run"):
		var errs: Array = _document.prepare_script_for_run()
		if errs.size() > 0:
			_flash_start_warning("Fix script errors first")
			return
	_director.bank = _document.bank
	_director.load_mus_script(_document.mus_script)
	_refresh_var_labels()
	# Fresh run: clear de-spam memory + section/idle state and reset the meter so
	# the first events log cleanly rather than being suppressed against a prior
	# run's values.
	_last_logged_var.clear()
	_vol_seen = false
	_current_section = &""
	_idle_ticks = 0
	if _volume_meter != null:
		_volume_meter.set_volume(0, 0)
	_director.start()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_graph()


# Push the loaded script's name down to the var inspector so it can swap raw
# VarXX labels for known friendly names (menuscript / gamescript) and pick
# friendlier controls. When no script is loaded, falls back to the
# empty-string form which renders raw VarXX spinboxes.
func _refresh_var_labels() -> void:
	if _var_inspector == null or not _var_inspector.has_method("set_script_name"):
		return
	var script_name: String = ""
	var profile_path: String = ""
	if _document != null and _document.script_loaded():
		script_name = String(_document.mus_script.get_default_script_name())
		if _document.has_method("get_var_profile_path"):
			profile_path = _document.get_var_profile_path()
	if _var_inspector.has_method("set_profile_path"):
		_var_inspector.call("set_profile_path", profile_path)
	_var_inspector.call("set_script_name", script_name)


func start_from_script_mode() -> void:
	_on_start()


func _on_pause() -> void:
	if _director == null:
		return
	_director.pause()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_log_typed(EvType.SYSTEM, "paused")


func _on_resume() -> void:
	if _director == null:
		return
	_director.resume()
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_log_typed(EvType.SYSTEM, "resumed")


func _on_stop() -> void:
	if _director == null:
		return
	_director.stop()
	_apply_state_label(VM_STOPPED)
	_current_section = &""
	_idle_ticks = 0
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_graph()
	if _volume_meter != null:
		_volume_meter.set_volume(0, 0)


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
	_current_section = &""
	_idle_ticks = 0
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_graph()
	if _volume_meter != null:
		_volume_meter.set_volume(0, 0)


# --- Section jump ------------------------------------------------------

# Populate the jump dropdown with every section in the loaded script (the flat
# list, so win/lose stings that no variable branch reaches are still
# selectable). Disabled until the VM is RUNNING.
func _refresh_jump_options() -> void:
	if _jump_option == null:
		return
	_jump_option.clear()
	if _document == null or not _document.script_loaded():
		_jump_option.disabled = true
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	var sections: PackedStringArray = _document.mus_script.get_section_names(script_name)
	for s in sections:
		_jump_option.add_item(String(s))
	_update_jump_enabled()


func _update_jump_enabled() -> void:
	if _jump_option == null:
		return
	_jump_option.disabled = (_last_state != VM_RUNNING) or _jump_option.item_count == 0


func _on_jump_selected(index: int) -> void:
	if _director == null or _last_state != VM_RUNNING:
		return
	if index < 0 or index >= _jump_option.item_count:
		return
	var section := StringName(_jump_option.get_item_text(index))
	_director.jump_to_section(section)
	_log_typed(EvType.SYSTEM, "jump -> %s" % section)


func _on_graph_section_pressed(section_name: StringName) -> void:
	if _director == null or _last_state != VM_RUNNING:
		return
	_director.jump_to_section(section_name)
	_log_typed(EvType.SYSTEM, "jump -> %s" % section_name)


# --- Now playing -------------------------------------------------------

# Single source of truth for the now-playing label. Suffix precedence: a
# just-triggered sound (the most recent concrete event) wins, else the idle
# marker if we're self-looping in the current section, else nothing.
func _refresh_now_playing(sound_name: String = "") -> void:
	if _now_playing == null:
		return
	if _last_state != VM_RUNNING and _last_state != VM_PAUSED:
		_now_playing.text = "Now playing: —"
		return
	var sec: String = String(_current_section)
	if sec == "":
		_now_playing.text = "Now playing: —"
		return
	var suffix := ""
	if sound_name != "":
		suffix = "  (♪ %s)" % sound_name
	elif _idle_ticks > 0:
		suffix = "  (idle — looping, waiting for the game)"
	_now_playing.text = "Now playing: %s%s" % [sec, suffix]


# --- Director signal handlers ------------------------------------------

func _on_section(section_name: StringName) -> void:
	# A self-loop section (e.g. the gamescript's `Missionnull { enter Missionnull }`)
	# re-fires section_entered ~60/sec. libs/mus + the director stay byte-faithful;
	# we collapse the repeats here. Same section as last time = an idle tick: bump
	# the counter and refresh the now-playing suffix only. No new log row, no graph
	# rebuild (which would otherwise thrash 60/sec).
	if section_name == _current_section and _current_section != &"":
		_idle_ticks += 1
		if _idle_ticks == 1:
			_refresh_now_playing()
		return
	# Real transition: reset idle, log it, rebuild the graph + now-playing.
	_current_section = section_name
	_idle_ticks = 0
	_refresh_graph()
	_refresh_now_playing()
	_log_typed(EvType.SECTION, "section -> %s" % section_name)


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
	var running: bool = _last_state == VM_RUNNING
	for section_name in graph.keys():
		var row := HBoxContainer.new()
		# The section name is a flat button: clicking it jumps the running VM
		# there. Disabled when not RUNNING (jump_to_section no-ops pre-Start
		# anyway, but disabling makes the rule visible).
		var btn := Button.new()
		btn.flat = true
		btn.text = section_name
		btn.disabled = not running
		btn.tooltip_text = "Jump the running script to this section."
		if section_name == String(_current_section):
			btn.add_theme_color_override("font_color", Color(1.0, 0.83, 0.47))
		btn.pressed.connect(_on_graph_section_pressed.bind(StringName(section_name)))
		row.add_child(btn)
		for target in graph[section_name]:
			var arrow := Label.new()
			arrow.text = " -> %s" % target
			row.add_child(arrow)
		_graph.add_child(row)


func _on_sound(idx: int, sound_name: StringName, wait: bool) -> void:
	_log_typed(EvType.SOUND, "play %d (%s)%s" % [idx, sound_name, " wait" if wait else ""])
	_refresh_now_playing(String(sound_name))


func _on_echo(arg: int) -> void:
	_log_typed(EvType.ECHO, "echo %d" % arg)


# Emitted by the VM when a section runs to its terminal `done`. Treat as a
# clean end-of-track and surface it visually so the user sees Stop wasn't
# required.
func _on_halted() -> void:
	_apply_state_label(VM_HALTED)
	_refresh_now_playing()
	_log_typed(EvType.SYSTEM, "halted")


# vm_error means mus_vm_load_script or runtime hit a hard fault. The director
# already flipped its internal state to ERROR; we mirror it.
func _on_vm_error(message: String) -> void:
	_apply_state_label(VM_ERROR)
	if message.is_empty():
		_log_typed(EvType.SYSTEM, "error")
	else:
		_log_typed(EvType.SYSTEM, "error: %s" % message)


func _on_variable_changed(var_index: int, value: int) -> void:
	# The Variables tab mirrors every write live; only log a line when the
	# value actually changes (and the var filter is on), so per-frame writes
	# don't drown the log.
	if _last_logged_var.get(var_index, null) == value:
		return
	_last_logged_var[var_index] = value
	_log_typed(EvType.VAR, "var Var%02d = %d" % [var_index, value])


func _on_volume_changed(left: int, right: int) -> void:
	# GSV/GSDV emit 16.16 fixed-point ints (witnessed: Jointops.exe!Intrinsic_GSV @
	# 0x6720E0). The meter always shows the latest value; the log line only
	# fires on an actual change (and when the volume filter is on) so a
	# per-tick volume loop doesn't flood the list.
	if _volume_meter != null:
		_volume_meter.set_volume(left, right)
	if _vol_seen and left == _last_vol_l and right == _last_vol_r:
		return
	_vol_seen = true
	_last_vol_l = left
	_last_vol_r = right
	_log_typed(EvType.VOLUME, "volume L=%.2f R=%.2f" % [left / 65536.0, right / 65536.0])


func _on_clear() -> void:
	if _events == null:
		return
	_events.clear()
	_last_log_type = -1
	_last_log_text = ""
	_last_log_count = 0


# --- Events log --------------------------------------------------------

# True if the given event type should be appended right now. section/sound/var/
# volume are gated by their filter checkboxes; echo/system always show so
# errors and halts can't be hidden. Filters apply to FUTURE entries only,
# which preserves the incremental-append design (no full rebuild).
func _is_type_shown(type: int) -> bool:
	match type:
		EvType.SECTION:
			return _filter_section == null or _filter_section.button_pressed
		EvType.SOUND:
			return _filter_sound == null or _filter_sound.button_pressed
		EvType.VAR:
			return _filter_var == null or _filter_var.button_pressed
		EvType.VOLUME:
			return _filter_volume == null or _filter_volume.button_pressed
		_:
			return true


# Append a typed, colour-coded line, drop the oldest entry past the 50-row cap,
# and scroll the tail into view. ensure_current_is_visible only nudges the
# scroll when an item is current, so we set the new tail current first.
func _log_typed(type: int, text: String) -> void:
	if _events == null:
		return
	if not _is_type_shown(type):
		return
	# Coalesce a run of identical (type, text) events into the existing tail row
	# as "<text>  xN" rather than appending duplicates. A distinct line breaks
	# the run (so the cap test's distinct "flood N" lines never fold).
	if _events.item_count > 0 and type == _last_log_type and text == _last_log_text:
		_last_log_count += 1
		var tail := _events.item_count - 1
		_events.set_item_text(tail, "[%s] %s  x%d" % [_timestamp(), text, _last_log_count])
		if _EV_COLOR.has(type):
			_events.set_item_custom_fg_color(tail, _EV_COLOR[type])
		_events.select(tail)
		_events.ensure_current_is_visible()
		return
	_events.add_item("[%s] %s" % [_timestamp(), text])
	if _EV_COLOR.has(type):
		_events.set_item_custom_fg_color(_events.item_count - 1, _EV_COLOR[type])
	if _events.item_count > 50:
		_events.remove_item(0)
	_last_log_type = type
	_last_log_text = text
	_last_log_count = 1
	if _events.item_count > 0:
		_events.select(_events.item_count - 1)
		_events.ensure_current_is_visible()


# Back-compat shim: untyped log lines are system events. Kept so callers/tests
# that use _log() keep working.
func _log(text: String) -> void:
	_log_typed(EvType.SYSTEM, text)


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
# Also keeps the jump dropdown's RUNNING-only rule in sync.
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
	_update_jump_enabled()
