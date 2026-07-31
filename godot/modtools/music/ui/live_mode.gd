class_name MusicLiveMode
extends Control

const MusicVarInspectorClass = preload("res://modtools/music/ui/var_inspector.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")
const MusVarNames = preload("res://modtools/music/mus_var_names.gd")
const MusicAudioPreviewClass = preload("res://modtools/music/music_audio_preview.gd")
const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")
const MusicSectionProgramViewClass = preload("res://modtools/music/ui/section_program_view.gd")
const MusInputNames = preload("res://modtools/music/mus_input_names.gd")
const MusicNavClass = preload("res://modtools/music/ui/music_nav.gd")

# Most breadcrumb segments rendered before the older hops collapse into "…".
const MAX_CRUMBS := 5

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
const ADD_STATE_TOOLTIP := "Create a new empty state, then wire it up by dragging tracks onto it and drawing transitions."
const TRANSPORT_OPEN_PROJECT_TOOLTIP := "Open or create a music project first."
const TRANSPORT_START_TOOLTIP := "Compile and start the loaded music script."
const TRANSPORT_START_FIRST_TOOLTIP := "Start playback first."
const TRANSPORT_PAUSE_FIRST_TOOLTIP := "Pause playback first."
const TRANSPORT_PAUSE_TOOLTIP := "Pause playback."
const TRANSPORT_RESUME_TOOLTIP := "Resume playback."
const TRANSPORT_STOP_TOOLTIP := "Stop playback."
const TRANSPORT_ALREADY_RUNNING_TOOLTIP := "Already running."
const TRANSPORT_ALREADY_PAUSED_TOOLTIP := "Already paused."
const TRANSPORT_RESUME_OR_STOP_TOOLTIP := "Resume or stop before starting again."
const JUMP_OPEN_PROJECT_TOOLTIP := "Open or create a music project first."
const JUMP_START_FIRST_TOOLTIP := "Start playback before jumping to a section."
const JUMP_RUNNING_TOOLTIP := "Jump to any section while playback is running."
const JUMP_NO_SECTIONS_TOOLTIP := "No sections available to jump to."
const MAP_START_FIRST_TOOLTIP := "Start playback before jumping to a state"
const UNLINKED_STATE_TOOLTIP := "No other state transitions here yet. Add a transition from another state so playback can reach it."

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
var _preview: Node  # MusicAudioPreview, for track-chip previews on the map
# Auto-follow the live VM: when on, a section transition drills the program view to the
# entered state. A manual node click pins a state (turns this off); Start/Stop re-arms.
var _follow_live: bool = true
var _current_section: StringName = &""
# Consecutive re-entries of the SAME section. A self-loop section (e.g. the
# gamescript's `Missionnull { enter Missionnull }`) re-fires section_entered
# every VM tick; we count those as idle rather than logging each one.
var _idle_ticks: int = 0
var _last_state: int = VM_STOPPED
var _start_warning_until_ms: int = 0
# "<script>:<node count>" of the last map we fit to the viewport. _refresh_map
# rebuilds the GraphNodes on every section transition, but only an actual
# topology change (open/close a different script) should re-fit — so playback
# and the user's manual pan/zoom are never yanked around mid-run.
var _fit_signature: String = ""

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
@onready var _map: GraphEdit = %SectionMap
@onready var _states_list: ItemList = %StatesList
@onready var _canvas: Control = %Canvas
var _add_state_btn: Button
var _sidebar_add_state_btn: Button
# Level-2 drill-in: the state's block-stack program view swaps into the
# center canvas (the map keeps its GraphEdit; inside a state, programs are
# vertical lists).
var _program_view: Control
# Where the user is + how they got there (trail, back/forward). Every drill,
# back-to-map, breadcrumb click and sidebar click routes through this so the
# canvas can never disagree with the trail.
var _nav: RefCounted
var _breadcrumb: HBoxContainer
var _breadcrumb_label: Label
var _breadcrumb_rename_btn: Button
var _breadcrumb_delete_btn: Button
var _follow_btn: CheckButton
var _nav_back_btn: Button
var _nav_fwd_btn: Button
var _crumb_segments: HBoxContainer
var _map_header: Control
var _map_toolbar: Control
# Centered "create or open" prompt shown when no project is loaded, so the blank
# map isn't a dead end (the loudest from-scratch gap was that New gave you nothing
# and there was no visual way to make a project).
var _empty_state: Control
var _logic_section_name: String = ""


# --- W4-6a decomposition: Live-mode operation sections ------------------
# Method bundles in the F5 controller-section shape (see
# modtools/mission/controller/controller_section.gd): ALL state stays on
# this mount (sections reach it through `_lm`); every moved method keeps a
# delegate below, so callers, .connect targets and tests never moved.
# Constructed in _init (not _ready) so any pre-ready call path -- e.g. a
# mount that binds the document before mounting -- already has them.
const LiveModeMapView := preload("res://modtools/music/ui/live_mode_map_view.gd")
const LiveModeAuthoringOps := preload("res://modtools/music/ui/live_mode_authoring_ops.gd")
const LiveModeNavOps := preload("res://modtools/music/ui/live_mode_nav_ops.gd")
const LiveModeTransportLog := preload("res://modtools/music/ui/live_mode_transport_log.gd")
var _map_view_ops  # LiveModeMapView (created in _init)
var _authoring_ops  # LiveModeAuthoringOps (created in _init)
var _nav_ops  # LiveModeNavOps (created in _init)
var _transport_ops  # LiveModeTransportLog (created in _init)


func _init() -> void:
	_map_view_ops = LiveModeMapView.new(self)
	_authoring_ops = LiveModeAuthoringOps.new(self)
	_nav_ops = LiveModeNavOps.new(self)
	_transport_ops = LiveModeTransportLog.new(self)


func bind_document(document: RefCounted) -> void:
	if _document == document:
		return
	# When the workspace re-binds (e.g. user closed and reopened a project),
	# unhook from the old document's changed signal so we don't keep getting
	# pings after it's gone.
	if _document != null and _document.changed.is_connected(_on_document_changed):
		_document.changed.disconnect(_on_document_changed)
	if _document != null and _document.has_signal("compile_finished") \
			and _document.compile_finished.is_connected(_on_compile_finished):
		_document.compile_finished.disconnect(_on_compile_finished)
	var had_document := _document != null
	_document = document
	# A different project's history is meaningless; forget it (initial bind has
	# nothing to forget, so skip the reset churn there).
	if had_document and _nav != null:
		_nav.reset()
	if _document != null:
		_document.changed.connect(_on_document_changed)
		# Surface compile failures on the always-visible transport label. Structured
		# edits are gated and rolled back, so a failure here is rare (mainly Save);
		# without this it would be silent.
		if _document.has_signal("compile_finished"):
			_document.compile_finished.connect(_on_compile_finished)
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
	_refresh_map()
	_refresh_button_state()
	_refresh_var_labels()
	_refresh_jump_options()
	_refresh_now_playing()
	# If the user is drilled into a section's program, an edit / undo / redo must
	# rebuild that graph too (document.changed only refreshes the map above).
	# Re-populate from the new AST; if the section vanished (deleted, or renamed out
	# from under the breadcrumb), scrub it from the trail -- the nav lands on the
	# previous surviving location and announces it.
	if _program_view != null and _program_view.visible and _logic_section_name != "":
		if not _populate_program_view(_logic_section_name):
			_nav.remove_section(_logic_section_name)
		else:
			_refresh_breadcrumb_action_state()
			_update_breadcrumb_status(_logic_section_name)


func _ready() -> void:
	_director = NovaMusicDirector.new()
	_director.auto_start = false
	add_child(_director)
	_preview = MusicAudioPreviewClass.new()
	_preview.name = "ChipPreview"
	add_child(_preview)
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
	if _map != null:
		_map.node_selected.connect(_on_map_node_selected)
	if _states_list != null:
		_states_list.item_selected.connect(_on_states_item_selected)
	_nav = MusicNavClass.new()
	_nav.location_changed.connect(_on_nav_location_changed)
	# The drill-in program view is the sole authoring surface; the right-dock
	# inspector and the raw-script drawer are both gone. All authoring intents
	# (add/replace/delete/reorder/add-play) route from the graph in _install_program_view.
	_install_add_state_button()
	_install_program_view()
	_install_empty_state()
	if _var_inspector.has_method("bind_director"):
		_var_inspector.call("bind_director", _director)
	# A variable rename must reach every surface that shows var names: the event
	# log resolves per-line, but the drilled program view and its pickers cache the
	# var list, so re-populate them.
	if _var_inspector != null and _var_inspector.has_signal("names_changed"):
		_var_inspector.connect("names_changed", _on_var_names_changed)
	_apply_state_label(VM_STOPPED)
	_refresh_map()
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
	# Reflect the (programmatically-set) follow state on the breadcrumb toggle without
	# re-emitting toggled -- many edit paths pin follow off; this keeps the UI honest.
	if _follow_btn != null and _follow_btn.button_pressed != _follow_live:
		_follow_btn.set_pressed_no_signal(_follow_live)
	var state: int = _director.vm_state()
	if state != _last_state:
		_apply_state_label(state)
		_refresh_button_state()
		# Section-graph buttons + jump dropdown enable only while RUNNING, and
		# now-playing clears on stop/halt, so refresh them on every transition.
		_refresh_map()
		_refresh_now_playing()
	if state == VM_RUNNING or state == VM_PAUSED:
		if _var_inspector != null and _var_inspector.has_method("refresh_from_director"):
			_var_inspector.call("refresh_from_director")
		_update_live_highlight(state)
	else:
		if _program_view != null:
			_program_view.set_active_offset(-1)


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


# --- Section jump ------------------------------------------------------

# Populate the jump dropdown with every section in the loaded script (the flat
# list, so win/lose stings that no variable branch reaches are still
# selectable). Disabled until the VM is RUNNING.
func _refresh_jump_options() -> void:
	if _jump_option == null:
		return
	_jump_option.clear()
	if _document == null or not _document.script_loaded():
		_update_jump_enabled()
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	var sections: PackedStringArray = _document.mus_script.get_section_names(script_name)
	for s in sections:
		_jump_option.add_item(String(s))
	_update_jump_enabled()


func _update_jump_enabled() -> void:
	if _jump_option == null:
		return
	var has_script: bool = _document != null and _document.script_loaded()
	if not has_script:
		_jump_option.disabled = true
		_jump_option.tooltip_text = JUMP_OPEN_PROJECT_TOOLTIP
		return
	if _jump_option.item_count == 0:
		_jump_option.disabled = true
		_jump_option.tooltip_text = JUMP_NO_SECTIONS_TOOLTIP
		return
	if _last_state != VM_RUNNING:
		_jump_option.disabled = true
		_jump_option.tooltip_text = JUMP_START_FIRST_TOOLTIP
		return
	_jump_option.disabled = false
	_jump_option.tooltip_text = JUMP_RUNNING_TOOLTIP


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


# --- W4-6a section delegates --------------------------------------------
# Every method that moved to a live_mode_*.gd section keeps a thin delegate
# here, so signal wiring, tests, the workspace's duck-typed stop_director
# hook, and cross-section calls all keep resolving on the mount.

# -> live_mode_map_view.gd


func _on_section(section_name: StringName) -> void:
	_map_view_ops._on_section(section_name)


func _refresh_map() -> void:
	_map_view_ops._refresh_map()


func _open_program_button(section_name: String) -> Button:
	return _map_view_ops._open_program_button(section_name)


func _incoming_by_index(model: Array) -> Dictionary:
	return _map_view_ops._incoming_by_index(model)


func _section_is_unlinked_in_model(section: Dictionary, incoming_by_index: Dictionary) -> bool:
	return _map_view_ops._section_is_unlinked_in_model(section, incoming_by_index)


func _build_logic_summaries(script_name: StringName) -> Dictionary:
	return _map_view_ops._build_logic_summaries(script_name)


func _section_logic_badge(stmts: Array) -> String:
	return _map_view_ops._section_logic_badge(stmts)


func _count_logic(stmts: Array, c: Dictionary) -> void:
	_map_view_ops._count_logic(stmts, c)


func _section_edge_color(section: Dictionary) -> Color:
	return _map_view_ops._section_edge_color(section)


func _fit_map_to_view() -> void:
	_map_view_ops._fit_map_to_view()


func _layout_map(model: Array, node_by_index: Dictionary) -> void:
	_map_view_ops._layout_map(model, node_by_index)


func _highlight_active_node() -> void:
	_map_view_ops._highlight_active_node()


func _bank_names() -> Array:
	return _map_view_ops._bank_names()


func _track_name(names: Array, track: int) -> String:
	return _map_view_ops._track_name(names, track)


func _preview_track(track: int) -> void:
	_map_view_ops._preview_track(track)


func _on_map_node_selected(node: Node) -> void:
	_map_view_ops._on_map_node_selected(node)


func _on_compile_finished(success: bool, errors: Array) -> void:
	_map_view_ops._on_compile_finished(success, errors)


func _on_node_gui_input(event: InputEvent, section_name: String) -> void:
	_map_view_ops._on_node_gui_input(event, section_name)


func _on_inspector_add_play(section_name: StringName, track: int) -> void:
	_map_view_ops._on_inspector_add_play(section_name, track)


func _build_var_list() -> Array:
	return _map_view_ops._build_var_list()


func _on_var_names_changed() -> void:
	_map_view_ops._on_var_names_changed()

# -> live_mode_authoring_ops.gd


func _on_inspector_add_statement(section_index: int, lines: PackedStringArray) -> void:
	_authoring_ops._on_inspector_add_statement(section_index, lines)


func _on_inspector_replace_statement(section_index: int, ordinal: int, lines: PackedStringArray) -> void:
	_authoring_ops._on_inspector_replace_statement(section_index, ordinal, lines)


func _on_inspector_delete_statement(section_index: int, ordinal: int) -> void:
	_authoring_ops._on_inspector_delete_statement(section_index, ordinal)


func _on_inspector_reorder_statement(section_index: int, ordinal: int, direction: int) -> void:
	_authoring_ops._on_inspector_reorder_statement(section_index, ordinal, direction)


func _on_program_insert_at(section_index: int, before_ordinal: int, lines: PackedStringArray) -> void:
	_authoring_ops._on_program_insert_at(section_index, before_ordinal, lines)


func _on_program_move(section_index: int, ordinal: int, before_ordinal: int) -> void:
	_authoring_ops._on_program_move(section_index, ordinal, before_ordinal)


func _on_program_run_count(section_index: int, start_ordinal: int, old_count: int, new_count: int) -> void:
	_authoring_ops._on_program_run_count(section_index, start_ordinal, old_count, new_count)


func _on_input_renamed(section_name: String, input_index: int, label: String) -> void:
	_authoring_ops._on_input_renamed(section_name, input_index, label)


func _update_live_highlight(state: int) -> void:
	_authoring_ops._update_live_highlight(state)


func _install_add_state_button() -> void:
	_authoring_ops._install_add_state_button()


func _install_empty_state() -> void:
	_authoring_ops._install_empty_state()


func _refresh_empty_state() -> void:
	_authoring_ops._refresh_empty_state()


func _on_new_project_pressed() -> void:
	_authoring_ops._on_new_project_pressed()


func _authoring_blocked_reason() -> String:
	return _authoring_ops._authoring_blocked_reason()


func _on_add_state() -> void:
	_authoring_ops._on_add_state()


func _unique_state_name() -> String:
	return _authoring_ops._unique_state_name()


func _install_program_view() -> void:
	_authoring_ops._install_program_view()


func _on_breadcrumb_rename() -> void:
	_authoring_ops._on_breadcrumb_rename()


func _on_breadcrumb_delete() -> void:
	_authoring_ops._on_breadcrumb_delete()


func _on_follow_toggled(pressed: bool) -> void:
	_authoring_ops._on_follow_toggled(pressed)


func _show_state_context_menu(section_name: String, global_pos: Vector2) -> void:
	_authoring_ops._show_state_context_menu(section_name, global_pos)


func _on_state_menu_id(id: int, section_name: String) -> void:
	_authoring_ops._on_state_menu_id(id, section_name)


func _open_rename_dialog(section_name: String) -> void:
	_authoring_ops._open_rename_dialog(section_name)


func _section_name_validation_reason(candidate: String, current_name: String = "") -> String:
	return _authoring_ops._section_name_validation_reason(candidate, current_name)


func _do_rename_section(old_name: String, new_name: String) -> void:
	_authoring_ops._do_rename_section(old_name, new_name)


func _open_delete_section_dialog(section_name: String) -> void:
	_authoring_ops._open_delete_section_dialog(section_name)


func _do_delete_section(section_name: String) -> void:
	_authoring_ops._do_delete_section(section_name)

# -> live_mode_nav_ops.gd


func _drill_into(section_name: String, pin: bool = true) -> void:
	_nav_ops._drill_into(section_name, pin)


func _on_nav_location_changed(entry: Dictionary) -> void:
	_nav_ops._on_nav_location_changed(entry)


func _update_breadcrumb_status(section_name: String) -> void:
	_nav_ops._update_breadcrumb_status(section_name)


func _rebuild_breadcrumb() -> void:
	_nav_ops._rebuild_breadcrumb()


func _unhandled_input(event: InputEvent) -> void:
	_nav_ops._unhandled_input(event)


func _refresh_states_list() -> void:
	_nav_ops._refresh_states_list()


func _refresh_states_selection() -> void:
	_nav_ops._refresh_states_selection()


func _on_states_item_selected(index: int) -> void:
	_nav_ops._on_states_item_selected(index)


func _refresh_breadcrumb_action_state() -> void:
	_nav_ops._refresh_breadcrumb_action_state()


func _section_is_idle_loop(section_name: String) -> bool:
	return _nav_ops._section_is_idle_loop(section_name)


func _section_is_unlinked(section_name: String) -> bool:
	return _nav_ops._section_is_unlinked(section_name)


func _populate_program_view(section_name: String) -> bool:
	return _nav_ops._populate_program_view(section_name)


func _back_to_map() -> void:
	_nav_ops._back_to_map()


func _set_map_chrome_visible(v: bool) -> void:
	_nav_ops._set_map_chrome_visible(v)

# -> live_mode_transport_log.gd


func _on_start() -> void:
	_transport_ops._on_start()


func _on_pause() -> void:
	_transport_ops._on_pause()


func _on_resume() -> void:
	_transport_ops._on_resume()


func _on_stop() -> void:
	_transport_ops._on_stop()


func stop_director() -> void:
	_transport_ops.stop_director()


func _on_sound(idx: int, sound_name: StringName, wait: bool) -> void:
	_transport_ops._on_sound(idx, sound_name, wait)


func _on_echo(arg: int) -> void:
	_transport_ops._on_echo(arg)


func _on_halted() -> void:
	_transport_ops._on_halted()


func _on_vm_error(message: String) -> void:
	_transport_ops._on_vm_error(message)


func _on_variable_changed(var_index: int, value: int) -> void:
	_transport_ops._on_variable_changed(var_index, value)


func _on_volume_changed(left: int, right: int) -> void:
	_transport_ops._on_volume_changed(left, right)


func _on_clear() -> void:
	_transport_ops._on_clear()


func _log_typed(type: int, text: String) -> void:
	_transport_ops._log_typed(type, text)


func _log(text: String) -> void:
	_transport_ops._log(text)


func _timestamp() -> String:
	return _transport_ops._timestamp()


func _flash_start_warning(message: String) -> void:
	_transport_ops._flash_start_warning(message)


func _refresh_button_state() -> void:
	_transport_ops._refresh_button_state()


func _set_button_state(button: Button, enabled: bool, tooltip: String) -> void:
	_transport_ops._set_button_state(button, enabled, tooltip)


func _refresh_add_state_button_state() -> void:
	_transport_ops._refresh_add_state_button_state()
