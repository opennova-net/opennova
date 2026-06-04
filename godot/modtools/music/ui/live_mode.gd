class_name MusicLiveMode
extends Control

const MusicVarInspectorClass = preload("res://modtools/music/ui/var_inspector.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")
const MusVarNames = preload("res://modtools/music/mus_var_names.gd")
const MusicAudioPreviewClass = preload("res://modtools/music/music_audio_preview.gd")
const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")
const MusicSectionLogicGraphClass = preload("res://modtools/music/ui/section_logic_graph.gd")

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
@onready var _advanced_drawer: PanelContainer = %AdvancedDrawer
@onready var _advanced_toggle: Button = %AdvancedToggle
var _add_state_btn: Button
# Level-2 drill-in: the section logic graph swaps into the center canvas (Stage 2).
var _logic_graph: GraphEdit
var _breadcrumb: HBoxContainer
var _breadcrumb_label: Label
var _map_header: Control
var _map_toolbar: Control
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
	_document = document
	if _document != null:
		_document.changed.connect(_on_document_changed)
		# Surface compile failures on the always-visible transport label and pop
		# the Advanced drawer (where the detailed error list lives). Without this a
		# failed Compile&Run / Save was silent whenever the drawer was collapsed.
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
	# from under the breadcrumb), fall back to the map.
	if _logic_graph != null and _logic_graph.visible and _logic_section_name != "":
		if not _populate_logic_graph(_logic_section_name):
			_back_to_map()


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
	# The drill-in blueprint graph is now the sole per-statement authoring surface;
	# the old right-dock inspector is gone. All authoring intents (add/replace/delete/
	# reorder/add-play) route from the graph in _install_logic_graph below.
	if _advanced_toggle != null:
		_advanced_toggle.toggled.connect(_on_advanced_toggled)
	_install_add_state_button()
	_install_logic_graph()
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
			_map.remove_child(c)
			c.queue_free()
	if _document == null or not _document.script_loaded():
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
	var node_by_index: Dictionary = {}
	for section in model:
		var idx: int = int(section.get("index", -1))
		var gn := GraphNode.new()
		# Floor the node width so play-chip names (e.g. "JOMEN602A") read instead of
		# clipping to "soun"; long names still ellipsize with a full-name tooltip.
		# Wider for the blueprint look + the logic badge row.
		gn.custom_minimum_size = Vector2(240, 0)
		gn.add_theme_font_size_override("title_font_size", 15)
		gn.name = "S_%d" % idx
		gn.set_meta("section", String(section.get("name", "")))
		gn.gui_input.connect(_on_node_gui_input.bind(String(section.get("name", ""))))
		gn.title = String(section.get("name", ""))
		if bool(section.get("is_entry", false)):
			gn.title += "   ★ start"
		if bool(section.get("is_idle_loop", false)):
			gn.title += "   ↻ idle"
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


func _on_advanced_toggled(pressed: bool) -> void:
	if _advanced_drawer != null:
		_advanced_drawer.visible = pressed


# A failed compile (Compile / Compile&Run / Save) only populates the Script-mode
# error list inside the Advanced drawer. Flash the first diagnostic on the
# transport label and reveal the drawer so the failure is never silent.
func _on_compile_finished(success: bool, errors: Array) -> void:
	if success or errors.is_empty():
		return
	var first: Dictionary = errors[0] if errors[0] is Dictionary else {}
	var msg: String = String(first.get("message", "compile error"))
	var line: int = int(first.get("line", 0))
	if line > 0:
		msg = "line %d: %s" % [line, msg]
	_flash_start_warning(msg)
	_reveal_advanced_drawer()


func _reveal_advanced_drawer() -> void:
	if _advanced_drawer != null:
		_advanced_drawer.visible = true
	# Keep the toggle in sync without re-triggering _on_advanced_toggled.
	if _advanced_toggle != null and _advanced_toggle.has_method("set_pressed_no_signal"):
		_advanced_toggle.set_pressed_no_signal(true)


# Double-clicking a state drills into its logic-graph blueprint. Single clicks fall
# through to GraphEdit's node_selected (-> _on_map_node_selected). Right-click opens
# the state context menu (rename / delete / open) -- the state-level operations the
# removed right inspector used to own. The raw script stays reachable via the
# transport's "Advanced script" toggle.
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


# Reveal the raw-script drawer at a section. Fired by the blueprint's open_raw intent
# (editing a construct the visual form can't represent losslessly) and the transport's
# "Advanced script" toggle. The blueprint is the language; this is the last resort.
func _on_inspector_advanced(section_name: StringName) -> void:
	if _advanced_toggle != null:
		_advanced_toggle.set_pressed_no_signal(true)
	if _advanced_drawer != null:
		_advanced_drawer.visible = true
	var script_node := _script_panel()
	if script_node != null and script_node.has_method("scroll_to_section"):
		script_node.scroll_to_section(section_name)


func _script_panel() -> Node:
	return find_child("Script", true, false)


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

# Mount a "＋ Add State" button just above the section map. The loudest missing
# affordance was that there was no visual way to add a section.
func _install_add_state_button() -> void:
	if _map == null:
		return
	var col := _map.get_parent()
	if col == null:
		return
	var toolbar := HBoxContainer.new()
	toolbar.name = "MapToolbar"
	_add_state_btn = Button.new()
	_add_state_btn.text = "＋ Add State"
	_add_state_btn.tooltip_text = "Create a new empty state, then wire it up by dragging tracks onto it and drawing transitions."
	_add_state_btn.pressed.connect(_on_add_state)
	toolbar.add_child(_add_state_btn)
	col.add_child(toolbar)
	col.move_child(toolbar, _map.get_index())
	_map_toolbar = toolbar
	_map_header = col.get_node_or_null("MapHeader")


func _on_add_state() -> void:
	if _document == null or not _document.script_loaded():
		_flash_start_warning("Open a project first")
		return
	if not (_document.has_method("can_edit_plays") and _document.can_edit_plays()):
		_flash_start_warning("Script must compile to add a state")
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

# Mount the section logic graph + a Back breadcrumb into the center column,
# hidden until the user drills into a state. The map and the logic graph share
# the canvas; only one is visible at a time.
func _install_logic_graph() -> void:
	if _map == null:
		return
	var col := _map.get_parent()
	if col == null:
		return
	_breadcrumb = HBoxContainer.new()
	_breadcrumb.name = "LogicBreadcrumb"
	var back := Button.new()
	back.text = "◀ Map"
	back.tooltip_text = "Back to the state map."
	back.focus_mode = Control.FOCUS_NONE
	back.pressed.connect(_back_to_map)
	_breadcrumb.add_child(back)
	_breadcrumb_label = Label.new()
	_breadcrumb_label.add_theme_color_override("font_color", Color(0.85, 0.92, 1.0))
	_breadcrumb_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_breadcrumb.add_child(_breadcrumb_label)
	# Rename / delete THIS state -- the state-level operations the right inspector
	# used to own, now on the blueprint's own breadcrumb. They act on the drilled-in
	# section and reuse the document's rename_section / delete_section.
	var ren := Button.new()
	ren.text = "✎ Rename"
	ren.tooltip_text = "Rename this state."
	ren.focus_mode = Control.FOCUS_NONE
	ren.pressed.connect(_on_breadcrumb_rename)
	_breadcrumb.add_child(ren)
	var del := Button.new()
	del.text = "✕ Delete"
	del.tooltip_text = "Delete this state (only if nothing else points at it)."
	del.focus_mode = Control.FOCUS_NONE
	del.pressed.connect(_on_breadcrumb_delete)
	_breadcrumb.add_child(del)
	_breadcrumb.visible = false
	col.add_child(_breadcrumb)
	col.move_child(_breadcrumb, 0)

	_logic_graph = MusicSectionLogicGraphClass.new()
	_logic_graph.name = "LogicGraph"
	_logic_graph.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_logic_graph.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_logic_graph.visible = false
	_logic_graph.open_section_requested.connect(func(n): _drill_into(String(n)))
	# Stage 3 authoring intents reuse the inspector's document handlers verbatim --
	# the graph and the inspector emit the same add/replace/delete/reorder/add-play
	# signals, so one set of parity-gated, undoable write-path routes serves both.
	_logic_graph.add_statement_requested.connect(_on_inspector_add_statement)
	_logic_graph.replace_statement_requested.connect(_on_inspector_replace_statement)
	_logic_graph.delete_statement_requested.connect(_on_inspector_delete_statement)
	_logic_graph.reorder_statement_requested.connect(_on_inspector_reorder_statement)
	_logic_graph.add_play_requested.connect(_on_inspector_add_play)
	_logic_graph.open_raw_requested.connect(_on_inspector_advanced)
	_logic_graph.author_failed.connect(func(msg: String): _flash_start_warning(msg))
	col.add_child(_logic_graph)


# --- State-level operations (rename / delete), re-homed from the right inspector ---

func _on_breadcrumb_rename() -> void:
	if _logic_section_name != "":
		_open_rename_dialog(_logic_section_name)


func _on_breadcrumb_delete() -> void:
	if _logic_section_name != "":
		_open_delete_section_dialog(_logic_section_name)


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
	edit.text = section_name
	box.add_child(edit)
	add_child(dlg)
	dlg.confirmed.connect(func():
		_do_rename_section(section_name, edit.text.strip_edges())
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()
	edit.select_all()
	edit.grab_focus()


func _do_rename_section(old_name: String, new_name: String) -> void:
	if new_name == "" or new_name == old_name:
		return
	if _document == null or not _document.has_method("rename_section"):
		return
	_follow_live = false
	# If we're drilled into this state, re-point the breadcrumb to the new name
	# BEFORE the rename so the post-change refresh re-populates the blueprint under
	# it (instead of failing to find the old name and bouncing back to the map).
	var was_drilled := _logic_section_name == old_name
	if was_drilled:
		_logic_section_name = new_name
		if _breadcrumb_label != null:
			_breadcrumb_label.text = "  ▸  %s   (blueprint)" % new_name
	if _document.rename_section(StringName(old_name), StringName(new_name)):
		_log_typed(EvType.SYSTEM, "renamed %s -> %s" % [old_name, new_name])
	else:
		if was_drilled:
			_logic_section_name = old_name
			if _breadcrumb_label != null:
				_breadcrumb_label.text = "  ▸  %s   (blueprint)" % old_name
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
		_log_typed(EvType.SYSTEM, "deleted state %s" % section_name)
	else:
		_flash_start_warning("Can't delete: state is still referenced")


# Drill into a state: swap the center canvas from the map to that section's logic
# graph (statements as exec-flow nodes) with a Back breadcrumb. A manual drill
# (double-click, "open ▸", Add State) pins (stops live auto-follow); a live-follow
# drill from _on_section passes pin=false so the VM keeps re-drilling on transitions.
func _drill_into(section_name: String, pin: bool = true) -> void:
	if _logic_graph == null or _document == null or not _document.script_loaded():
		return
	if not _populate_logic_graph(section_name):
		return
	if pin:
		_follow_live = false
	_logic_section_name = section_name
	_logic_graph.visible = true
	_set_map_chrome_visible(false)
	_breadcrumb.visible = true
	_breadcrumb_label.text = "  ▸  %s   (blueprint)" % section_name


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
			_logic_graph.configure_authoring(section_names, _build_var_list(), _document.mus_script, _bank_names(), editable)
			_logic_graph.show_section(sec, _bank_names())
			return true
	return false


func _back_to_map() -> void:
	if _logic_graph != null:
		_logic_graph.visible = false
	if _breadcrumb != null:
		_breadcrumb.visible = false
	_logic_section_name = ""
	_set_map_chrome_visible(true)


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
		_start_btn.disabled = not has_project or state == VM_RUNNING or state == VM_PAUSED
	if _pause_btn != null:
		_pause_btn.disabled = state != VM_RUNNING
	if _resume_btn != null:
		_resume_btn.disabled = state != VM_PAUSED
	if _stop_btn != null:
		_stop_btn.disabled = state != VM_RUNNING and state != VM_PAUSED
	_update_jump_enabled()
