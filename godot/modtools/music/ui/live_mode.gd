class_name MusicLiveMode
extends Control

const MusicVarInspectorClass = preload("res://modtools/music/ui/var_inspector.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")
const MusVarNames = preload("res://modtools/music/mus_var_names.gd")
const MusicAudioPreviewClass = preload("res://modtools/music/music_audio_preview.gd")
const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")
const MusicSectionLogicGraphClass = preload("res://modtools/music/ui/section_logic_graph.gd")
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
# Auto-follow the live VM: when on, a section transition drills the blueprint to the
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
# Level-2 drill-in: the section logic graph swaps into the center canvas (Stage 2).
var _logic_graph: GraphEdit
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
	# If the user is drilled into a section's blueprint, an edit / undo / redo must
	# rebuild that graph too (document.changed only refreshes the map above).
	# Re-populate from the new AST; if the section vanished (deleted, or renamed out
	# from under the breadcrumb), scrub it from the trail -- the nav lands on the
	# previous surviving location and announces it.
	if _logic_graph != null and _logic_graph.visible and _logic_section_name != "":
		if not _populate_logic_graph(_logic_section_name):
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
	# The drill-in blueprint graph is now the sole authoring surface; the right-dock
	# inspector and the raw-script drawer are both gone. All authoring intents
	# (add/replace/delete/reorder/add-play) route from the graph in _install_logic_graph.
	_install_add_state_button()
	_install_logic_graph()
	_install_empty_state()
	if _var_inspector.has_method("bind_director"):
		_var_inspector.call("bind_director", _director)
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
		if _logic_graph != null:
			_logic_graph.set_active_offset(-1)


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
	_follow_live = true
	if _volume_meter != null:
		_volume_meter.set_volume(0, 0)
	_director.start()
	_start_warning_until_ms = 0
	_apply_state_label(_director.vm_state())
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_map()


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
	_follow_live = true
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_map()
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
	_follow_live = true
	_refresh_button_state()
	_refresh_now_playing()
	_refresh_map()
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
	# Real transition: reset idle, log it, rebuild the map + now-playing, and -- while
	# auto-following and already drilled into a blueprint -- follow the VM into the
	# entered state's blueprint so the live highlight tracks where it actually is.
	_current_section = section_name
	_idle_ticks = 0
	_refresh_map()
	_refresh_now_playing()
	if _follow_live and _logic_graph != null and _logic_graph.visible:
		_drill_into(String(section_name), false)
	_log_typed(EvType.SECTION, "section -> %s" % section_name)


func _refresh_map() -> void:
	if _map == null:
		return
	_map.clear_connections()
	for c in _map.get_children():
		if c is GraphNode:
			c.free()
	_refresh_empty_state()
	if _document == null or not _document.script_loaded():
		_refresh_states_list()
		return
	var script_name: StringName = StringName(_document.mus_script.get_default_script_name())
	# Model-driven (opcode-level), not string-parsed: edges are correct, switch
	# fan-out is captured, and self-looping idle states are flagged.
	var model: Array = MusicSectionGraphClass.build(_document.mus_script, script_name)
	if model.is_empty():
		return
	var names: Array = _bank_names()
	# Per-section logic glyph summary (◇if ⋔switch ✎var ƒcall) so the map node
	# surfaces the logic that isn't a track chip -- the part that used to be
	# invisible until you opened the cramped right list. Keyed by section index.
	var logic_by_index: Dictionary = _build_logic_summaries(script_name)
	var incoming_by_index: Dictionary = _incoming_by_index(model)
	var node_by_index: Dictionary = {}
	for section in model:
		var idx: int = int(section.get("index", -1))
		var section_name := String(section.get("name", ""))
		var gn := GraphNode.new()
		# Floor the node width so play-chip names (e.g. "JOMEN602A") read instead of
		# clipping to "soun"; long names still ellipsize with a full-name tooltip.
		# Wider for the blueprint look + the logic badge row.
		gn.custom_minimum_size = Vector2(240, 0)
		gn.add_theme_font_size_override("title_font_size", 15)
		gn.name = "S_%d" % idx
		gn.set_meta("section", section_name)
		gn.gui_input.connect(_on_node_gui_input.bind(section_name))
		gn.title = section_name
		if bool(section.get("is_entry", false)):
			gn.title += "   ★ start"
			# Be honest about the fixed entry: the first-declared state is always the
			# start, and there is no set-start / reorder affordance yet.
			gn.tooltip_text = "Start state: playback begins here. The first state is always the start (choosing a different start state, or reordering states, isn't supported yet)."
		if bool(section.get("is_idle_loop", false)):
			gn.title += "   ↻ idle"
		if _section_is_unlinked_in_model(section, incoming_by_index):
			gn.title += "   unlinked"
			gn.tooltip_text = UNLINKED_STATE_TOOLTIP
		# Body: up to 3 track chips; the full list shows in the right inspector.
		var plays: Array = section.get("plays", [])
		var shown: int = mini(plays.size(), 3)
		for p in range(shown):
			var play: Dictionary = plays[p]
			var track: int = int(play.get("track", -1))
			var chip := MusicTrackChipClass.new()
			gn.add_child(chip)
			chip.setup(track, _track_name(names, track), bool(play.get("wait", false)), false)
			chip.preview_requested.connect(_preview_track)
		if plays.size() > shown:
			var more := Label.new()
			more.text = "  +%d more" % (plays.size() - shown)
			more.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
			gn.add_child(more)
		# Logic badge: the if/switch/var/call summary the chips can't show. Only
		# when the state actually runs logic; reads as "open me to see the blueprint".
		var badge_text: String = String(logic_by_index.get(idx, ""))
		if badge_text != "":
			var badge := Label.new()
			badge.text = badge_text
			badge.add_theme_color_override("font_color", Color(0.72, 0.74, 0.82))
			badge.tooltip_text = "Logic this state runs (◇ if · ⋔ switch · ✎ var · ƒ call). Double-click to open its blueprint."
			gn.add_child(badge)
		if gn.get_child_count() == 0:
			# Slot 0 needs a row; an idle / branch-only section plays nothing.
			var spacer := Label.new()
			spacer.text = " "
			gn.add_child(spacer)
		gn.add_child(_blueprint_button(section_name))
		# Output pin coloured by the dominant outgoing edge kind so a switch fan-out
		# (cyan) reads apart from a plain transition (blue) / branch (gold). Input pin
		# stays a muted neutral. GraphEdit tints each wire by its source pin colour.
		gn.set_slot(0, true, 0, Color(0.45, 0.50, 0.60), true, 0, _section_edge_color(section))
		_map.add_child(gn)
		node_by_index[idx] = gn
	_layout_map(model, node_by_index)
	for section in model:
		var from_idx: int = int(section.get("index", -1))
		for e in section.get("edges", []):
			var to_idx: int = int(e.get("to", -1))
			if to_idx == from_idx:
				continue  # self-loop is the ↻ idle badge, not a drawn connection
			if node_by_index.has(from_idx) and node_by_index.has(to_idx):
				_map.connect_node("S_%d" % from_idx, 0, "S_%d" % to_idx, 0)
	_highlight_active_node()
	# Fit the graph to the viewport, but only when the topology actually changed
	# (a different script / node count) -- not on the per-transition rebuilds that
	# keep the same sections, which would otherwise yank the view mid-playback.
	var sig: String = "%s:%d" % [String(script_name), node_by_index.size()]
	if sig != _fit_signature:
		_fit_signature = sig
		_fit_map_to_view()
	_refresh_states_list()


func _blueprint_button(section_name: String) -> Button:
	var b := Button.new()
	b.name = "OpenBlueprintButton"
	b.text = "Blueprint"
	b.tooltip_text = "Open this state's editable blueprint."
	b.focus_mode = Control.FOCUS_NONE
	b.pressed.connect(func(): _drill_into(section_name))
	return b


func _incoming_by_index(model: Array) -> Dictionary:
	var incoming: Dictionary = {}
	for section in model:
		incoming[int(section.get("index", -1))] = 0
	for section in model:
		var from_idx: int = int(section.get("index", -1))
		for e in section.get("edges", []):
			var to_idx: int = int(e.get("to", -1))
			if to_idx == from_idx:
				continue
			incoming[to_idx] = int(incoming.get(to_idx, 0)) + 1
	return incoming


func _section_is_unlinked_in_model(section: Dictionary, incoming_by_index: Dictionary) -> bool:
	if bool(section.get("is_entry", false)):
		return false
	var idx: int = int(section.get("index", -1))
	return int(incoming_by_index.get(idx, 0)) == 0


# Build {section_index -> compact logic-glyph summary} from the program AST, so a
# map node can show the if/switch/var/call shape it runs (the logic that isn't a
# track chip). One AST parse per rebuild; cheap for the shipped scripts.
func _build_logic_summaries(script_name: StringName) -> Dictionary:
	var out: Dictionary = {}
	if _document == null or not _document.script_loaded():
		return out
	var ast: Array = _document.mus_script.get_program_ast(script_name)
	for sec in ast:
		out[int(sec.get("index", -1))] = _section_logic_badge(sec.get("statements", []))
	return out


# Compact glyph summary of the LOGIC a section runs (◇ if · ⋔ switch · ✎ var ·
# ƒ call), counts elided when zero, empty when the section is just plays/
# transitions. Recurses if/switch bodies so nested logic still surfaces.
func _section_logic_badge(stmts: Array) -> String:
	var c := {"if": 0, "switch": 0, "var": 0, "call": 0}
	_count_logic(stmts, c)
	var parts := PackedStringArray()
	if c["if"] > 0:
		parts.append("◇%d" % c["if"])
	if c["switch"] > 0:
		parts.append("⋔%d" % c["switch"])
	if c["var"] > 0:
		parts.append("✎%d" % c["var"])
	if c["call"] > 0:
		parts.append("ƒ%d" % c["call"])
	return "  ".join(parts)


func _count_logic(stmts: Array, c: Dictionary) -> void:
	for s in stmts:
		match String(s.get("kind", "")):
			"if":
				c["if"] += 1
				_count_logic(s.get("then", []), c)
				_count_logic(s.get("else", []), c)
			"switch":
				c["switch"] += 1
			"assign":
				c["var"] += 1
				if bool(s.get("has_call", false)):
					c["call"] += 1
			"incdec":
				c["var"] += 1
			"expr":
				if bool(s.get("has_call", false)):
					c["call"] += 1


# Output-pin colour for a section, by its dominant outgoing edge kind: a switch
# fan-out reads cyan, a conditional branch gold, a plain transition blue.
func _section_edge_color(section: Dictionary) -> Color:
	var has_switch := false
	var has_branch := false
	for e in section.get("edges", []):
		var k := int(e.get("kind", 0))
		if k == MusicSectionGraphClass.KIND_SWITCH:
			has_switch = true
		elif k == MusicSectionGraphClass.KIND_BRANCH:
			has_branch = true
	if has_switch:
		return MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_SWITCH)
	if has_branch:
		return MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_BRANCH)
	return MusicSectionGraphClass.edge_color(MusicSectionGraphClass.KIND_TRANSITION)


# Center + zoom the section map so the whole graph fills the GraphEdit instead of
# clustering in the top-left with empty canvas to the right. Deferred one frame so
# the GraphNodes have sized themselves from their chip content before we measure.
func _fit_map_to_view() -> void:
	if _map == null or not is_inside_tree() or get_tree() == null:
		return
	await get_tree().process_frame
	if _map == null or not is_instance_valid(_map):
		return
	var first: bool = true
	var min_x: float = 0.0
	var min_y: float = 0.0
	var max_x: float = 0.0
	var max_y: float = 0.0
	for c in _map.get_children():
		if not (c is GraphNode):
			continue
		var gn: GraphNode = c
		var p: Vector2 = gn.position_offset
		var s: Vector2 = gn.size
		if first:
			min_x = p.x; min_y = p.y; max_x = p.x + s.x; max_y = p.y + s.y
			first = false
		else:
			min_x = minf(min_x, p.x); min_y = minf(min_y, p.y)
			max_x = maxf(max_x, p.x + s.x); max_y = maxf(max_y, p.y + s.y)
	if first:
		return  # no nodes
	var content := Vector2(max_x - min_x, max_y - min_y)
	if content.x <= 0.0 or content.y <= 0.0:
		return
	var view := _map.size
	var margin := 80.0
	var zoom := minf((view.x - margin) / content.x, (view.y - margin) / content.y)
	zoom = clampf(zoom, 0.25, 1.0)
	_map.zoom = zoom
	# scroll_offset is in zoomed pixels: a node at position_offset shows at
	# position_offset*zoom - scroll_offset, so center the content's midpoint.
	var center := Vector2((min_x + max_x) * 0.5, (min_y + max_y) * 0.5)
	_map.scroll_offset = center * zoom - view * 0.5


# Deterministic BFS layered layout from the entry section, left to right, so the
# map doesn't reshuffle on every rebuild (GraphEdit.arrange_nodes is
# non-deterministic). Within each layer, nodes are ordered by the average row of
# their already-placed parents (barycenter) so a switch's targets sit beside the
# switch instead of sprawling, which cuts edge crossings. Sections no edge
# reaches (win/lose stings) trail in a final column.
func _layout_map(model: Array, node_by_index: Dictionary) -> void:
	var entry_idx: int = -1
	var adj: Dictionary = {}
	for section in model:
		var idx: int = int(section.get("index", -1))
		var outs: Array = []
		for e in section.get("edges", []):
			var t: int = int(e.get("to", -1))
			if t != idx:
				outs.append(t)
		adj[idx] = outs
		if bool(section.get("is_entry", false)):
			entry_idx = idx
	var layer: Dictionary = {}
	var queue: Array = []
	if entry_idx >= 0:
		layer[entry_idx] = 0
		queue.append(entry_idx)
	while not queue.is_empty():
		var n: int = queue.pop_front()
		for t in adj.get(n, []):
			if not layer.has(t):
				layer[t] = int(layer[n]) + 1
				queue.append(t)
	var max_layer: int = 0
	for v in layer.values():
		max_layer = maxi(max_layer, int(v))
	for section in model:
		var idx2: int = int(section.get("index", -1))
		if not layer.has(idx2):
			layer[idx2] = max_layer + 1
	# Reverse adjacency: who points at each node (self-loops already excluded).
	var incoming: Dictionary = {}
	for src in adj.keys():
		for dst in adj[src]:
			if not incoming.has(dst):
				incoming[dst] = []
			(incoming[dst] as Array).append(src)
	# Group nodes by layer, node-index order as the stable tiebreak.
	var members: Dictionary = {}
	var indices: Array = node_by_index.keys()
	indices.sort()
	var top_layer: int = 0
	for idx3 in indices:
		var l3: int = int(layer.get(idx3, 0))
		if not members.has(l3):
			members[l3] = []
		(members[l3] as Array).append(idx3)
		top_layer = maxi(top_layer, l3)
	# Place layer by layer; order each layer by the barycenter of its already-
	# placed parents. Parentless nodes (entry, unreached stings) keep index order.
	var assigned_row: Dictionary = {}
	for l4 in range(top_layer + 1):
		if not members.has(l4):
			continue
		var group: Array = (members[l4] as Array).duplicate()
		var bary: Dictionary = {}
		for idx4 in group:
			var total: float = 0.0
			var cnt: int = 0
			for src2 in incoming.get(idx4, []):
				if assigned_row.has(src2):
					total += float(assigned_row[src2])
					cnt += 1
			bary[idx4] = (total / cnt) if cnt > 0 else 1e9
		group.sort_custom(func(a, b):
			if bary[a] == bary[b]:
				return int(a) < int(b)
			return bary[a] < bary[b])
		# assigned_row stays the integer rank (drives the NEXT layer's barycenter
		# ordering); the visual y is centered on a shared mid-axis so a 1-node
		# layer lands at y=0 and a fan-out layer spreads symmetrically above and
		# below it. The linear chain then threads the middle of each fan instead
		# of pinning to the top with the fan hanging one-sidedly below.
		var row: int = 0
		var span: float = float(group.size() - 1) * 0.5
		for idx5 in group:
			assigned_row[idx5] = row
			var y: float = (float(row) - span) * 180.0
			(node_by_index[idx5] as GraphNode).position_offset = Vector2(l4 * 320, y)
			row += 1


# Tint the running VM's current section; clear the rest. Applied on each rebuild
# (step 3 turns this into an in-place glow that does not rebuild the map).
func _highlight_active_node() -> void:
	if _map == null:
		return
	for gn in _map.get_children():
		if gn is GraphNode:
			var active: bool = _last_state == VM_RUNNING \
				and String(gn.get_meta("section", "")) == String(_current_section)
			gn.modulate = Color(0.6, 1.0, 0.6) if active else Color(1, 1, 1)


# Resolve bank entry names so chips read "combat1" not "sound_3". Empty when no
# bank is loaded alongside the script.
func _bank_names() -> Array:
	if _document == null or not _document.bank_loaded():
		return []
	var out: Array = []
	for e in _document.bank.get_entries():
		out.append(String(e.get("name", "")))
	return out


func _track_name(names: Array, track: int) -> String:
	if track >= 0 and track < names.size() and String(names[track]) != "":
		return String(names[track])
	return "sound_%d" % track


# Preview a bank track through our own MusicAudioPreview (a chip's ▶ button).
func _preview_track(track: int) -> void:
	if _preview == null or _document == null or not _document.bank_loaded():
		return
	var stream: NovaSbfAudioStream = _document.bank.get_stream_at(track)
	if stream != null:
		_preview.play_stream(stream)


# A map node was clicked. While RUNNING this jumps the VM there. Double-click drills
# into the blueprint; right-click opens the state menu. No-ops the jump while stopped.
func _on_map_node_selected(node: Node) -> void:
	if node == null:
		return
	var sec: String = String(node.get_meta("section", ""))
	if sec == "":
		return
	# Clicking the section that is currently playing keeps live auto-follow on;
	# clicking a different one pins away from it (Start/Stop, or clicking the live
	# state again, re-arms follow so a transition re-drills the blueprint).
	_follow_live = (_last_state == VM_RUNNING and sec == String(_current_section))
	# While running, also jump the VM there.
	if _last_state == VM_RUNNING:
		_on_graph_section_pressed(StringName(sec))
	else:
		_flash_start_warning(MAP_START_FIRST_TOOLTIP)


# A failed compile (Save, or the rare structured edit that doesn't round-trip)
# flashes the first diagnostic on the always-visible transport label. Structured
# edits are gated and roll back on failure, so this is a belt-and-suspenders surface.
func _on_compile_finished(success: bool, errors: Array) -> void:
	if success or errors.is_empty():
		return
	var first: Dictionary = errors[0] if errors[0] is Dictionary else {}
	var msg: String = String(first.get("message", "compile error"))
	var line: int = int(first.get("line", 0))
	if line > 0:
		msg = "line %d: %s" % [line, msg]
	_flash_start_warning(msg)


# Double-clicking a state drills into its logic-graph blueprint. Single clicks fall
# through to GraphEdit's node_selected (-> _on_map_node_selected). Right-click opens
# the state context menu (rename / delete / open).
func _on_node_gui_input(event: InputEvent, section_name: String) -> void:
	if event is InputEventMouseButton and event.double_click \
			and event.button_index == MOUSE_BUTTON_LEFT:
		_drill_into(section_name)
	elif event is InputEventMouseButton and event.pressed \
			and event.button_index == MOUSE_BUTTON_RIGHT:
		_show_state_context_menu(section_name, event.global_position)


# Drag-to-add-play from the Tracks dock onto a blueprint node. The document gates
# this on can_edit_plays() and recompiles; document.changed rebuilds the map and
# re-populates the drilled-in blueprint, so there's nothing else to refresh here.
func _on_inspector_add_play(section_name: StringName, track: int) -> void:
	if _document == null or not _document.has_method("insert_play"):
		return
	if _document.insert_play(section_name, track):
		_follow_live = false


# Variable picker list for the authoring popups: Var00..Var16 (all 17 int32
# slots in MUS_GLOBALS_BYTES = 68/4, matching the Game Dials panel) with
# friendly, per-script names where known (the same map the Variables tab + event
# log use). Var16 is the user global; offering it here keeps the assignment /
# expression picker in step with the dials, which already show it.
func _build_var_list() -> Array:
	var out: Array = []
	var sname := ""
	if _document != null and _document.script_loaded():
		sname = String(_document.mus_script.get_default_script_name())
	for i in range(17):
		var label := MusVarNames.label_for(sname, i) if sname != "" else "Var%02d" % i
		out.append({"token": "Var%02d" % i, "label": label})
	return out


# --- Phase 2 authoring intent handlers (route to the document, keep pinned) ---

func _on_inspector_add_statement(section_index: int, lines: PackedStringArray) -> void:
	if _document == null or not _document.has_method("insert_statement"):
		return
	_follow_live = false
	if not _document.insert_statement(section_index, lines):
		_flash_start_warning("Couldn't add that here")


func _on_inspector_replace_statement(section_index: int, ordinal: int, lines: PackedStringArray) -> void:
	if _document == null or not _document.has_method("replace_statement"):
		return
	_follow_live = false
	if not _document.replace_statement(section_index, ordinal, lines):
		_flash_start_warning("Couldn't apply that edit")


func _on_inspector_delete_statement(section_index: int, ordinal: int) -> void:
	if _document == null or not _document.has_method("delete_statement"):
		return
	_follow_live = false
	if not _document.delete_statement(section_index, ordinal):
		_flash_start_warning("Couldn't delete that")


func _on_inspector_reorder_statement(section_index: int, ordinal: int, direction: int) -> void:
	if _document == null or not _document.has_method("reorder_statement"):
		return
	_follow_live = false
	if not _document.reorder_statement(section_index, ordinal, direction):
		_flash_start_warning("Can't move it further")


# Light the drilled-in blueprint statement the VM pc is on, but only while the graph
# shows the section that is actually running -- the pc is a global bytecode offset, so
# another section's nodes would mis-bracket it.
func _update_live_highlight(state: int) -> void:
	if _logic_graph != null and _logic_graph.visible:
		if state == VM_RUNNING and _logic_section_name == String(_current_section):
			_logic_graph.set_active_offset(_director.current_pc())
		else:
			_logic_graph.set_active_offset(-1)


# --- Add State (visual-first authoring slice) --------------------------

# Mount a "＋ Add State" button just above the canvas. The loudest missing
# affordance was that there was no visual way to add a section.
func _install_add_state_button() -> void:
	var col := get_node_or_null("%CenterCol")
	if col == null or _canvas == null:
		return
	var toolbar := HBoxContainer.new()
	toolbar.name = "MapToolbar"
	_add_state_btn = Button.new()
	_add_state_btn.text = "＋ Add State"
	_add_state_btn.tooltip_text = ADD_STATE_TOOLTIP
	_add_state_btn.pressed.connect(_on_add_state)
	toolbar.add_child(_add_state_btn)
	col.add_child(toolbar)
	col.move_child(toolbar, _canvas.get_index())
	_map_toolbar = toolbar
	_map_header = col.get_node_or_null("MapHeader")


# Centered welcome shown when nothing is loaded: a primary "create from scratch"
# button + a hint to Open. Without it, New produced a blank canvas with a disabled
# "＋ Add State" and the message "Open a project first" -- and no way to create one.
# Mounted into the map's column; _refresh_empty_state toggles it vs the map chrome.
func _install_empty_state() -> void:
	var col := get_node_or_null("%CenterCol")
	if col == null:
		return
	var center := CenterContainer.new()
	center.name = "EmptyState"
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.visible = false
	var box := VBoxContainer.new()
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 12)
	center.add_child(box)
	var title := Label.new()
	title.text = "No music project open"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 18)
	box.add_child(title)
	var new_btn := Button.new()
	new_btn.text = "＋ New music program"
	new_btn.tooltip_text = "Create a music program from scratch: a start state plus an empty sound bank. Import tracks in the Tracks dock, then wire up the states."
	new_btn.focus_mode = Control.FOCUS_NONE
	new_btn.pressed.connect(_on_new_project_pressed)
	box.add_child(new_btn)
	var hint := Label.new()
	hint.text = "or use Open to load an existing .sbf / .bin"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	box.add_child(hint)
	col.add_child(center)
	_empty_state = center


# Show the welcome prompt iff no script is loaded (hiding the map chrome behind
# it); otherwise hide it and -- when on the map, not drilled into a blueprint --
# restore the map chrome (the blueprint owns chrome visibility on its own).
func _refresh_empty_state() -> void:
	var empty: bool = _document == null or not _document.script_loaded()
	if _empty_state != null:
		_empty_state.visible = empty
	if empty:
		if _canvas != null:
			_canvas.visible = false
		if _map_toolbar != null:
			_map_toolbar.visible = false
		if _map_header != null:
			_map_header.visible = false
		if _breadcrumb != null:
			_breadcrumb.visible = false
	else:
		if _canvas != null:
			_canvas.visible = true
		if _breadcrumb != null:
			_breadcrumb.visible = true
		if _logic_section_name == "":
			_set_map_chrome_visible(true)


func _on_new_project_pressed() -> void:
	if _document == null or not _document.has_method("new_project"):
		return
	# Emits `changed` -> _on_document_changed -> _refresh_map, which hides this
	# prompt and renders the new start state; then open that start state's
	# blueprint so first-time authors land on the Add-statement hint immediately.
	var rc: int = _document.new_project()
	if rc != OK:
		_flash_start_warning("Could not create music project")
		return
	_follow_live = false
	_drill_into("Begin")


# Why structured authoring is currently off, or "" when available. Prefers the
# document's specific reason (esp. the multi-chunk read-only case) over a generic
# "must compile" so the user understands a greyed-out edit instead of guessing.
func _authoring_blocked_reason() -> String:
	if _document == null or not _document.script_loaded():
		return "Open a project first"
	if _document.has_method("authoring_blocked_reason"):
		return String(_document.authoring_blocked_reason())
	if not (_document.has_method("can_author") and _document.can_author()):
		return "Script must compile first"
	return ""


func _on_add_state() -> void:
	if _document == null or not _document.script_loaded():
		_flash_start_warning("Open a project first")
		return
	var reason: String = _authoring_blocked_reason()
	if reason != "":
		_flash_start_warning(reason)
		return
	if not _document.has_method("add_section"):
		return
	var new_name := _unique_state_name()
	if not _document.add_section(StringName(new_name)):
		_flash_start_warning("Could not add state")
		return
	_log_typed(EvType.SYSTEM, "added state %s" % new_name)
	# add_section emitted `changed` -> the map already rebuilt; drill straight into
	# the new state's blueprint so the user can start authoring it.
	_follow_live = false
	_drill_into(new_name)


# Lowest free "State_N" so a fresh state never collides with an existing section.
func _unique_state_name() -> String:
	var existing: Dictionary = {}
	if _document != null and _document.script_loaded():
		var sname := StringName(_document.mus_script.get_default_script_name())
		for s in _document.mus_script.get_section_names(sname):
			existing[String(s)] = true
	var n := 1
	while existing.has("State_%d" % n):
		n += 1
	return "State_%d" % n


# --- Level-2 logic graph (drill-in blueprint) --------------------------

# Mount the navigation bar (back/forward + breadcrumb trail + state actions)
# and the section logic graph. The map and the logic graph share the canvas
# stack; only one is visible at a time. The nav bar stays up whenever a script
# is loaded -- on the map it just reads "Map" -- so back/forward always work.
func _install_logic_graph() -> void:
	var col := get_node_or_null("%CenterCol")
	var stack := get_node_or_null("%CanvasStack")
	if col == null or stack == null:
		return
	_breadcrumb = HBoxContainer.new()
	_breadcrumb.name = "LogicBreadcrumb"
	_nav_back_btn = Button.new()
	_nav_back_btn.text = "◀"
	_nav_back_btn.tooltip_text = "Back (Alt+Left)."
	_nav_back_btn.focus_mode = Control.FOCUS_NONE
	_nav_back_btn.disabled = true
	_nav_back_btn.pressed.connect(func(): _nav.go_back())
	_breadcrumb.add_child(_nav_back_btn)
	_nav_fwd_btn = Button.new()
	_nav_fwd_btn.text = "▶"
	_nav_fwd_btn.tooltip_text = "Forward (Alt+Right)."
	_nav_fwd_btn.focus_mode = Control.FOCUS_NONE
	_nav_fwd_btn.disabled = true
	_nav_fwd_btn.pressed.connect(func(): _nav.go_forward())
	_breadcrumb.add_child(_nav_fwd_btn)
	_crumb_segments = HBoxContainer.new()
	_crumb_segments.name = "CrumbTrail"
	_crumb_segments.add_theme_constant_override("separation", 2)
	_breadcrumb.add_child(_crumb_segments)
	# Status badges for the open state (loops to itself / unlinked), not a title:
	# the trail's last segment already names where you are.
	_breadcrumb_label = Label.new()
	_breadcrumb_label.add_theme_color_override("font_color", Color(0.7, 0.74, 0.82))
	_breadcrumb_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_breadcrumb.add_child(_breadcrumb_label)
	# Rename / delete THIS state -- the state-level operations the right inspector
	# used to own, now on the blueprint's own breadcrumb. They act on the drilled-in
	# section and reuse the document's rename_section / delete_section.
	_breadcrumb_rename_btn = Button.new()
	_breadcrumb_rename_btn.text = "✎ Rename"
	_breadcrumb_rename_btn.tooltip_text = "Rename this state."
	_breadcrumb_rename_btn.focus_mode = Control.FOCUS_NONE
	_breadcrumb_rename_btn.pressed.connect(_on_breadcrumb_rename)
	_breadcrumb.add_child(_breadcrumb_rename_btn)
	_breadcrumb_delete_btn = Button.new()
	_breadcrumb_delete_btn.text = "✕ Delete"
	_breadcrumb_delete_btn.tooltip_text = "Delete this state (only if nothing else points at it)."
	_breadcrumb_delete_btn.focus_mode = Control.FOCUS_NONE
	_breadcrumb_delete_btn.pressed.connect(_on_breadcrumb_delete)
	_breadcrumb.add_child(_breadcrumb_delete_btn)
	# Follow-live toggle: when on, a VM transition re-drills the blueprint into the
	# entered state. A manual node click pins (turns this off); flipping it back on
	# resumes following -- the explicit, visible control the auto-pin behaviour lacked.
	_follow_btn = CheckButton.new()
	_follow_btn.text = "Follow live"
	_follow_btn.tooltip_text = "Follow the VM into each state it enters. Clicking a state pins the view (turns this off); re-enable to resume following."
	_follow_btn.focus_mode = Control.FOCUS_NONE
	_follow_btn.button_pressed = _follow_live
	_follow_btn.toggled.connect(_on_follow_toggled)
	_breadcrumb.add_child(_follow_btn)
	_breadcrumb.visible = false
	col.add_child(_breadcrumb)
	col.move_child(_breadcrumb, 0)
	_rebuild_breadcrumb()

	_logic_graph = MusicSectionLogicGraphClass.new()
	_logic_graph.name = "LogicGraph"
	_logic_graph.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_logic_graph.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_logic_graph.visible = false
	_logic_graph.open_section_requested.connect(func(n): _drill_into(String(n)))
	# The blueprint's authoring intents route to the document's parity-gated, undoable
	# write path (these handlers keep their _on_inspector_* names from when the inspector
	# shared them; the inspector is gone, the graph is the sole emitter now).
	_logic_graph.add_statement_requested.connect(_on_inspector_add_statement)
	_logic_graph.replace_statement_requested.connect(_on_inspector_replace_statement)
	_logic_graph.delete_statement_requested.connect(_on_inspector_delete_statement)
	_logic_graph.reorder_statement_requested.connect(_on_inspector_reorder_statement)
	_logic_graph.add_play_requested.connect(_on_inspector_add_play)
	_logic_graph.author_failed.connect(func(msg: String): _flash_start_warning(msg))
	stack.add_child(_logic_graph)


# --- State-level operations (rename / delete), re-homed from the right inspector ---

func _on_breadcrumb_rename() -> void:
	var reason := _authoring_blocked_reason()
	if reason != "":
		_flash_start_warning(reason)
		return
	if _logic_section_name != "":
		_open_rename_dialog(_logic_section_name)


func _on_breadcrumb_delete() -> void:
	var reason := _authoring_blocked_reason()
	if reason != "":
		_flash_start_warning(reason)
		return
	if _logic_section_name != "":
		_open_delete_section_dialog(_logic_section_name)


func _on_follow_toggled(pressed: bool) -> void:
	_follow_live = pressed
	# Re-enabling while the VM is running snaps the blueprint to the live state now,
	# rather than waiting for the next transition.
	if pressed and _last_state == VM_RUNNING and String(_current_section) != "":
		_drill_into(String(_current_section), false)


# Right-click a state on the map: open its blueprint, rename it, or delete it.
# Rename + delete need an editable (single-chunk, compiling) script.
func _show_state_context_menu(section_name: String, global_pos: Vector2) -> void:
	var pop := PopupMenu.new()
	pop.add_item("Open blueprint", 0)
	pop.add_item("Rename state…", 1)
	pop.add_item("Delete state", 2)
	var can: bool = _document != null and _document.has_method("can_author") and _document.can_author()
	pop.set_item_disabled(1, not can)
	pop.set_item_disabled(2, not can)
	if not can:
		# Explain the greyed-out items (esp. the multi-chunk read-only case).
		var reason := _authoring_blocked_reason()
		pop.set_item_tooltip(1, reason)
		pop.set_item_tooltip(2, reason)
	add_child(pop)
	pop.id_pressed.connect(_on_state_menu_id.bind(section_name))
	pop.popup_hide.connect(pop.queue_free)
	pop.position = Vector2i(global_pos)
	pop.reset_size()
	pop.popup()


func _on_state_menu_id(id: int, section_name: String) -> void:
	match id:
		0: _drill_into(section_name)
		1: _open_rename_dialog(section_name)
		2: _open_delete_section_dialog(section_name)


func _open_rename_dialog(section_name: String) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Rename state"
	dlg.min_size = Vector2i(340, 120)
	var box := VBoxContainer.new()
	dlg.add_child(box)
	var lbl := Label.new()
	lbl.text = "New name for '%s':" % section_name
	box.add_child(lbl)
	var edit := LineEdit.new()
	edit.name = "StateNameEdit"
	edit.text = section_name
	box.add_child(edit)
	var hint := Label.new()
	hint.name = "StateNameValidationHint"
	hint.add_theme_color_override("font_color", COLOR_ERROR)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(hint)
	add_child(dlg)
	var ok := dlg.get_ok_button()
	ok.text = "Rename"
	var refresh := func(_text):
		var reason := _section_name_validation_reason(edit.text, section_name)
		hint.text = reason
		hint.tooltip_text = reason
		ok.disabled = reason != ""
		ok.tooltip_text = reason if reason != "" else "Rename state."
	edit.text_changed.connect(refresh)
	dlg.confirmed.connect(func():
		_do_rename_section(section_name, edit.text.strip_edges())
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()
	refresh.call(edit.text)
	edit.select_all()
	edit.grab_focus()


func _section_name_validation_reason(candidate: String, current_name: String = "") -> String:
	var name := candidate.strip_edges()
	if _document != null and _document.has_method("validate_section_name"):
		return String(_document.validate_section_name(StringName(name), StringName(current_name)))
	if current_name != "" and name == current_name:
		return "Type a different state name."
	if name == "":
		return "Enter a state name."
	return ""


func _do_rename_section(old_name: String, new_name: String) -> void:
	if new_name == "" or new_name == old_name:
		return
	if _document == null or not _document.has_method("rename_section"):
		return
	_follow_live = false
	# If we're drilled into this state, re-point the shown-section name BEFORE the
	# rename so the post-change refresh re-populates the blueprint under it
	# (instead of failing to find the old name and bouncing back to the map).
	var was_drilled := _logic_section_name == old_name
	if was_drilled:
		_logic_section_name = new_name
	if _document.rename_section(StringName(old_name), StringName(new_name)):
		# Rewrite the trail only after the document accepted the rename, so a
		# rejected rename can't corrupt history entries of an unrelated state
		# that already carries the requested name.
		_nav.rename_section(old_name, new_name)
		_rebuild_breadcrumb()
		_refresh_states_list()
		_log_typed(EvType.SYSTEM, "renamed %s -> %s" % [old_name, new_name])
	else:
		if was_drilled:
			_logic_section_name = old_name
		_flash_start_warning("Rename rejected (name taken/invalid)")


func _open_delete_section_dialog(section_name: String) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Delete state"
	dlg.dialog_text = "Delete state '%s'?\nStates that other states point at can't be deleted until those links are retargeted." % section_name
	add_child(dlg)
	dlg.confirmed.connect(func():
		_do_delete_section(section_name)
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()


func _do_delete_section(section_name: String) -> void:
	if _document == null or not _document.has_method("delete_section"):
		return
	_follow_live = false
	# If we're deleting the drilled-in state, drop back to the map first so the
	# post-change refresh doesn't try to re-populate a now-gone section.
	if _logic_section_name == section_name:
		_back_to_map()
	if _document.delete_section(StringName(section_name)):
		# Scrub the dead state from the trail so back/forward can't revisit it.
		_nav.remove_section(section_name)
		_rebuild_breadcrumb()
		_log_typed(EvType.SYSTEM, "deleted state %s" % section_name)
	else:
		_flash_start_warning("Can't delete: state is still referenced")


# Drill into a state: navigate there; the nav announces the move and
# _on_nav_location_changed swaps the canvas. A manual drill (double-click,
# "open ▸", Add State, sidebar click) pins (stops live auto-follow) and pushes a
# trail hop; a live-follow drill from _on_section passes pin=false, which both
# keeps following and REPLACES the current trail entry so a transitioning VM
# doesn't flood the history with every state it enters.
func _drill_into(section_name: String, pin: bool = true) -> void:
	if _logic_graph == null or _document == null or not _document.script_loaded():
		return
	if pin:
		_follow_live = false
	_nav.navigate_to(MusicNavClass.section_entry(section_name), pin)


# The single place the canvas reacts to navigation. Map: hide the blueprint and
# restore the map chrome. Section: rebuild its blueprint and swap it in; a stale
# trail entry (the state vanished under the history) is scrubbed, which re-lands
# on the previous surviving location.
func _on_nav_location_changed(entry: Dictionary) -> void:
	if String(entry.get("kind", "")) == "map":
		if _logic_graph != null:
			_logic_graph.visible = false
		_logic_section_name = ""
		_set_map_chrome_visible(true)
		if _breadcrumb_label != null:
			_breadcrumb_label.text = ""
	else:
		var section_name := String(entry.get("name", ""))
		if not _populate_logic_graph(section_name):
			_nav.remove_section(section_name)
			return
		_logic_section_name = section_name
		if _canvas != null:
			_canvas.visible = true
		_logic_graph.visible = true
		_set_map_chrome_visible(false)
		_update_breadcrumb_status(section_name)
		_refresh_breadcrumb_action_state()
	if _breadcrumb != null and _document != null and _document.script_loaded():
		_breadcrumb.visible = true
	_rebuild_breadcrumb()
	_refresh_states_selection()


# Surface the open state's quirks next to the trail (the map shows the same
# badges on its nodes; without this the drilled-in view gave no hint why a
# state loops in place or can't be reached).
func _update_breadcrumb_status(section_name: String) -> void:
	if _breadcrumb_label == null:
		return
	var parts := PackedStringArray()
	if _section_is_idle_loop(section_name):
		parts.append("↻ loops to itself")
	elif _section_is_unlinked(section_name):
		parts.append("unlinked — nothing points here yet")
	_breadcrumb_label.text = "   " + " · ".join(parts) if parts.size() > 0 else ""


# Rebuild the breadcrumb trail row: back/forward enablement, one clickable
# segment per hop (older hops collapse into "…"), and the state-action buttons
# (rename/delete/follow) only while a state is open.
func _rebuild_breadcrumb() -> void:
	if _crumb_segments == null or _nav == null:
		return
	for c in _crumb_segments.get_children():
		_crumb_segments.remove_child(c)
		c.queue_free()
	var t: Array = _nav.trail()
	var start := 0
	if t.size() > MAX_CRUMBS:
		start = t.size() - MAX_CRUMBS
		var ell := Label.new()
		ell.text = "…"
		ell.tooltip_text = "%d earlier steps (use ◀ to walk back through them)" % start
		ell.add_theme_color_override("font_color", Color(0.55, 0.58, 0.65))
		_crumb_segments.add_child(ell)
	for i in range(start, t.size()):
		if _crumb_segments.get_child_count() > 0:
			var sep := Label.new()
			sep.text = "▸"
			sep.add_theme_color_override("font_color", Color(0.5, 0.53, 0.6))
			_crumb_segments.add_child(sep)
		var e: Dictionary = t[i]
		var seg := Button.new()
		seg.text = "Map" if String(e.get("kind", "")) == "map" else String(e.get("name", ""))
		seg.flat = true
		seg.focus_mode = Control.FOCUS_NONE
		if i == t.size() - 1:
			seg.disabled = true
			seg.tooltip_text = "You are here."
			seg.add_theme_color_override("font_disabled_color", Color(0.9, 0.94, 1.0))
		else:
			seg.tooltip_text = "Go back to %s." % seg.text
			var idx := i
			seg.pressed.connect(func(): _nav.jump_to(idx))
		_crumb_segments.add_child(seg)
	if _nav_back_btn != null:
		_nav_back_btn.disabled = not _nav.can_go_back()
	if _nav_fwd_btn != null:
		_nav_fwd_btn.disabled = not _nav.can_go_forward()
	var on_section: bool = not _nav.is_on_map()
	if _breadcrumb_rename_btn != null:
		_breadcrumb_rename_btn.visible = on_section
	if _breadcrumb_delete_btn != null:
		_breadcrumb_delete_btn.visible = on_section
	if _follow_btn != null:
		_follow_btn.visible = on_section


# Browser-style navigation keys/buttons, active while the workspace is on
# screen: Alt+Left / Alt+Right and the mouse back/forward thumb buttons.
func _unhandled_input(event: InputEvent) -> void:
	if _nav == null or not is_visible_in_tree():
		return
	if event is InputEventKey and event.pressed and event.alt_pressed:
		if event.keycode == KEY_LEFT:
			_nav.go_back()
			get_viewport().set_input_as_handled()
		elif event.keycode == KEY_RIGHT:
			_nav.go_forward()
			get_viewport().set_input_as_handled()
	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_XBUTTON1:
			_nav.go_back()
			get_viewport().set_input_as_handled()
		elif event.button_index == MOUSE_BUTTON_XBUTTON2:
			_nav.go_forward()
			get_viewport().set_input_as_handled()


# --- States sidebar ------------------------------------------------------

# Rebuild the always-visible state list (left of the canvas): the map row, then
# one row per state with its badges and the live ▶ marker. Selection mirrors
# the nav location so the user always knows where they are, however deep the
# trail goes.
func _refresh_states_list() -> void:
	if _states_list == null:
		return
	_states_list.clear()
	if _document == null or not _document.script_loaded():
		return
	_states_list.add_item("⌂ Map")
	_states_list.set_item_metadata(0, "")
	_states_list.set_item_tooltip(0, "The whole-program state map.")
	var sn := StringName(_document.mus_script.get_default_script_name())
	var model: Array = MusicSectionGraphClass.build(_document.mus_script, sn)
	var incoming := _incoming_by_index(model)
	for sec in model:
		var section_name := String(sec.get("name", ""))
		var label := section_name
		if bool(sec.get("is_entry", false)):
			label += "  ★"
		if bool(sec.get("is_idle_loop", false)):
			label += "  ↻"
		if _last_state == VM_RUNNING and section_name == String(_current_section):
			label = "▶ " + label
		var idx := _states_list.add_item(label)
		_states_list.set_item_metadata(idx, section_name)
		if _section_is_unlinked_in_model(sec, incoming):
			_states_list.set_item_custom_fg_color(idx, Color(0.66, 0.62, 0.52))
			_states_list.set_item_tooltip(idx, UNLINKED_STATE_TOOLTIP)
	_refresh_states_selection()


# Highlight the sidebar row for the current location without emitting
# item_selected (select() is signal-less).
func _refresh_states_selection() -> void:
	if _states_list == null or _nav == null:
		return
	var target: String = _nav.current_section()  # "" = the map row
	for i in range(_states_list.item_count):
		if String(_states_list.get_item_metadata(i)) == target:
			_states_list.select(i)
			return
	_states_list.deselect_all()


func _on_states_item_selected(index: int) -> void:
	if _states_list == null:
		return
	var section_name := String(_states_list.get_item_metadata(index))
	if section_name == "":
		_back_to_map()
	else:
		_drill_into(section_name)


func _refresh_breadcrumb_action_state() -> void:
	var reason := _authoring_blocked_reason()
	var blocked := reason != ""
	if _breadcrumb_rename_btn != null:
		_breadcrumb_rename_btn.disabled = blocked
		_breadcrumb_rename_btn.tooltip_text = reason if blocked else "Rename this state."
	if _breadcrumb_delete_btn != null:
		_breadcrumb_delete_btn.disabled = blocked
		_breadcrumb_delete_btn.tooltip_text = reason if blocked else "Delete this state (only if nothing else points at it)."


# Whether a section is a self-loop "idle" state, per the opcode-level section model
# (the AST dict doesn't carry it). Used to badge the drill-in breadcrumb.
func _section_is_idle_loop(section_name: String) -> bool:
	if _document == null or not _document.script_loaded():
		return false
	var ms = _document.mus_script
	if not ms.has_method("get_section_model"):
		return false
	var sn := StringName(ms.get_default_script_name())
	for sec in ms.get_section_model(sn):
		if String(sec.get("name", "")) == section_name:
			return bool(sec.get("is_idle_loop", false))
	return false


func _section_is_unlinked(section_name: String) -> bool:
	if _document == null or not _document.script_loaded():
		return false
	var ms = _document.mus_script
	if not ms.has_method("get_section_model"):
		return false
	var sn := StringName(ms.get_default_script_name())
	var model: Array = MusicSectionGraphClass.build(ms, sn)
	var incoming_by_index := _incoming_by_index(model)
	for sec in model:
		if String(sec.get("name", "")) == section_name:
			return _section_is_unlinked_in_model(sec, incoming_by_index)
	return false


# Build (or rebuild) the logic graph for `section_name` from the current AST,
# configuring its authoring context FIRST so the per-node tools + ＋Add palette
# match the script's current editability (can_author). Returns false if the
# section no longer exists (e.g. an edit/undo renamed or removed it).
func _populate_logic_graph(section_name: String) -> bool:
	if _logic_graph == null or _document == null or not _document.script_loaded():
		return false
	var sn := StringName(_document.mus_script.get_default_script_name())
	var ast: Array = _document.mus_script.get_program_ast(sn)
	for sec in ast:
		if String(sec.get("name", "")) == section_name:
			var editable: bool = _document.has_method("can_author") and _document.can_author()
			var section_names: PackedStringArray = _document.mus_script.get_section_names(sn)
			_logic_graph.configure_authoring(section_names, _build_var_list(), _document.mus_script, _bank_names(), editable, _authoring_blocked_reason())
			_logic_graph.show_section(sec, _bank_names())
			return true
	return false


# Navigate back to the map (a recorded hop, so forward can return).
func _back_to_map() -> void:
	if _nav != null:
		_nav.navigate_to(MusicNavClass.map_entry())


func _set_map_chrome_visible(v: bool) -> void:
	if _map != null:
		_map.visible = v
	if _map_header != null:
		_map_header.visible = v
	if _map_toolbar != null:
		_map_toolbar.visible = v


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
	# Friendly per-script name when known ("MissionActive (Var01)"), else raw
	# "Var01". Same mus_var_names map the Variables tab uses, so the log and the
	# inspector agree instead of the log showing opaque indices.
	var label := "Var%02d" % var_index
	if _document != null and _document.script_loaded():
		label = MusVarNames.label_for(String(_document.mus_script.get_default_script_name()), var_index)
	_log_typed(EvType.VAR, "%s = %d" % [label, value])


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
		var start_enabled := has_project and state != VM_RUNNING and state != VM_PAUSED
		var start_tip := TRANSPORT_START_TOOLTIP
		if not has_project:
			start_tip = TRANSPORT_OPEN_PROJECT_TOOLTIP
		elif state == VM_RUNNING:
			start_tip = TRANSPORT_ALREADY_RUNNING_TOOLTIP
		elif state == VM_PAUSED:
			start_tip = TRANSPORT_RESUME_OR_STOP_TOOLTIP
		_set_button_state(_start_btn, start_enabled, start_tip)
	if _pause_btn != null:
		_set_button_state(_pause_btn, state == VM_RUNNING,
			TRANSPORT_PAUSE_TOOLTIP if state == VM_RUNNING else TRANSPORT_ALREADY_PAUSED_TOOLTIP if state == VM_PAUSED else TRANSPORT_START_FIRST_TOOLTIP)
	if _resume_btn != null:
		_set_button_state(_resume_btn, state == VM_PAUSED,
			TRANSPORT_RESUME_TOOLTIP if state == VM_PAUSED else TRANSPORT_PAUSE_FIRST_TOOLTIP)
	if _stop_btn != null:
		var can_stop := state == VM_RUNNING or state == VM_PAUSED
		_set_button_state(_stop_btn, can_stop,
			TRANSPORT_STOP_TOOLTIP if can_stop else TRANSPORT_START_FIRST_TOOLTIP)
	_refresh_add_state_button_state()
	_update_jump_enabled()


func _set_button_state(button: Button, enabled: bool, tooltip: String) -> void:
	button.disabled = not enabled
	button.tooltip_text = tooltip


func _refresh_add_state_button_state() -> void:
	if _add_state_btn == null:
		return
	var reason := _authoring_blocked_reason()
	var blocked := reason != ""
	_add_state_btn.disabled = blocked
	_add_state_btn.tooltip_text = reason if blocked else ADD_STATE_TOOLTIP
