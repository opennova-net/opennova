extends GutTest

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_live_pair.sbf"
const PAIR_SCRIPT := "user://music_live_pair.bin"
const PAIR_PROFILE := "user://music_live_pair.music_profile.json"


func before_each() -> void:
	_cleanup_temp_pair()


func after_each() -> void:
	_cleanup_temp_pair()


func test_live_starts_and_stops():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var start_btn: Button = lm.get_node("%StartButton")
	var stop_btn: Button = lm.get_node("%StopButton")
	# Polish B1 P0: pressing Start must actually flip the VM into RUNNING. Pre-fix
	# this would silently bail because GDScript's set_script call dispatched to
	# Object.set_script and never reached our wrapper. assert_true on vm_state to
	# guard against the regression coming back.
	start_btn.pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(lm._director.vm_state(), 1, "VM RUNNING after Start press (was 0 before B1 fix)")
	stop_btn.pressed.emit()
	await get_tree().create_timer(0.05).timeout
	assert_eq(lm._director.vm_state(), 0, "VM STOPPED after Stop press")


func test_compile_failure_flashes_label():
	# The raw drawer is gone; a failed compile (rare, since structured edits are gated
	# and roll back) surfaces on the always-visible transport label.
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var label: Label = lm.get_node("%StateLabel")
	doc.compile_finished.emit(false, [{"line": 3, "col": 1, "message": "boom"}])
	assert_true(label.text.contains("boom"), "transport label flashes the diagnostic on compile failure")


func test_var_inspector_infrastructure():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	# VarInspector should build a control row for each of the 17 int32 slots
	# (Var00..Var16). The old positional `*2+1` child-count assertion went
	# stale when Var16 was added; the registry exposes a stable count instead.
	var inspector: Control = lm.get_node("%VarInspector")
	assert_not_null(inspector, "VarInspector node exists")
	assert_eq(inspector.control_count(), 17, "17 var rows built (Var00..Var16)")
	assert_not_null(inspector.get_value_control(0), "Var00 has a control")
	assert_not_null(inspector.get_value_control(16), "Var16 has a control")


func test_typed_inspector_controls_for_gamescript():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var inspector: Control = lm.get_node("%VarInspector")
	# HealthPct (Var07) renders as a 0..100 slider, MissionActive (Var01) as a
	# checkbox; an unknown-role slot stays a plain int32 spinbox.
	assert_true(inspector.get_value_control(7) is HSlider, "HealthPct is a slider")
	assert_true(inspector.get_value_control(1) is CheckBox, "MissionActive is a checkbox")
	assert_true(inspector.get_value_control(3) is SpinBox, "raw slot is a spinbox")
	assert_eq(inspector.get_value_control(7).tooltip_text,
		"HealthPct (Var07). Slider range: 0..100. Adjusting writes Var07 in the VM.")
	assert_eq(inspector.get_value_control(1).tooltip_text,
		"MissionActive (Var01). Checkbox: off=0, on=1. Adjusting writes Var01 in the VM.")
	assert_eq(inspector.get_value_control(3).tooltip_text,
		"Var03. Raw int32 slot. Adjusting writes Var03 in the VM.")


# Polish B4: Pause and Resume buttons on the toolbar.
func test_pause_resume_buttons_present():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	assert_not_null(lm.get_node_or_null("%PauseButton"), "PauseButton present in toolbar")
	assert_not_null(lm.get_node_or_null("%ResumeButton"), "ResumeButton present in toolbar")


func test_transport_buttons_explain_disabled_states():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var start: Button = lm.get_node("%StartButton")
	var pause: Button = lm.get_node("%PauseButton")
	var resume: Button = lm.get_node("%ResumeButton")
	var stop: Button = lm.get_node("%StopButton")
	assert_true(start.disabled, "Start disabled until a project is loaded")
	assert_eq(start.tooltip_text, "Open or create a music project first.")
	assert_true(pause.disabled, "Pause disabled before playback starts")
	assert_eq(pause.tooltip_text, "Start playback first.")
	assert_true(resume.disabled, "Resume disabled before playback is paused")
	assert_eq(resume.tooltip_text, "Pause playback first.")
	assert_true(stop.disabled, "Stop disabled before playback starts")
	assert_eq(stop.tooltip_text, "Start playback first.")

	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	assert_false(start.disabled, "Start enables once a project is loaded")
	assert_eq(start.tooltip_text, "Compile and start the loaded music script.")
	assert_true(pause.disabled, "Pause still waits for playback")
	assert_eq(pause.tooltip_text, "Start playback first.")

	start.pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_true(start.disabled, "Start disabled while running")
	assert_eq(start.tooltip_text, "Already running.")
	assert_false(pause.disabled, "Pause enables while running")
	assert_eq(pause.tooltip_text, "Pause playback.")
	assert_true(resume.disabled, "Resume waits for paused state")
	assert_eq(resume.tooltip_text, "Pause playback first.")
	assert_false(stop.disabled, "Stop enables while running")
	assert_eq(stop.tooltip_text, "Stop playback.")

	pause.pressed.emit()
	await get_tree().process_frame
	assert_true(start.disabled, "Start disabled while paused")
	assert_eq(start.tooltip_text, "Resume or stop before starting again.")
	assert_true(pause.disabled, "Pause disabled while already paused")
	assert_eq(pause.tooltip_text, "Already paused.")
	assert_false(resume.disabled, "Resume enables while paused")
	assert_eq(resume.tooltip_text, "Resume playback.")
	assert_false(stop.disabled, "Stop stays enabled while paused")
	assert_eq(stop.tooltip_text, "Stop playback.")


# Polish B5+B2: state label reads STOPPED at rest, and after a halt the
# directly-invoked _on_halted handler flips it to HALTED.
func test_state_label_after_halt():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var label: Label = lm.get_node("%StateLabel")
	assert_eq(label.text, "STOPPED", "label starts at STOPPED")
	lm._on_halted()
	assert_eq(label.text, "HALTED", "halted handler flips label to HALTED")


# Regression: when bind_document fires before a file is loaded (the common
# path: workspace mounts the empty document, then the user clicks Open), Live
# mode must still react when the document later loads a project.
func test_open_after_bind_enables_start():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var start_btn: Button = lm.get_node("%StartButton")
	assert_true(start_btn.disabled, "Start disabled while no project loaded")
	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	assert_false(start_btn.disabled, "Start enables after project loads (changed signal fix)")


# Regression: the var inspector shows raw VarXX labels until set_script_name is
# called with a known script. Opening a known script after mount must rebuild
# the labels (and is now checked via the registry helper, not child indices).
func test_var_labels_refresh_when_script_loads():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var inspector: Control = lm.get_node("%VarInspector")
	assert_eq(inspector.get_row_label_text(0), "Var00", "raw label before script loads")
	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	# jo_gamemus's chunk name is "gamescript" and Var01 has a known role.
	assert_eq(inspector.get_row_label_text(1), "MissionActive (Var01)",
		"Var01 picks up the friendly name once gamescript loads")


func test_custom_profile_labels_refresh_when_sidecar_exists():
	var doc = MusicEditorDocument.new()
	_copy_temp_pair()
	var profile_path := PAIR_PROFILE
	var f := FileAccess.open(profile_path, FileAccess.WRITE)
	assert_not_null(f, "profile sidecar writable")
	f.store_string(JSON.stringify({
		"script_name": "gamescript",
		"vars": {"3": {"label": "CustomMood", "kind": "slider", "min": 0, "max": 5}}
	}))
	f.close()
	doc.open_pair(PAIR_BANK)
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var inspector: Control = lm.get_node("%VarInspector")
	assert_eq(inspector.get_row_label_text(3), "CustomMood (Var03)")
	assert_true(inspector.get_value_control(3) is HSlider, "custom profile kind drives control")
	_remove_user_file(profile_path)


func test_start_compiles_dirty_script_before_running():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	doc.set_script_text(&"gamescript", "script customscript\nsection Begin\n{\n  done\n}\n")
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(doc.mus_script.get_default_script_name(), "customscript",
		"Live start compiles and applies dirty script text before loading VM")


func test_var_inspector_poll_refresh_catches_silent_vm_mutation():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var inspector: Control = lm.get_node("%VarInspector")
	lm._director.set_var(4, 77)
	inspector.refresh_from_director()
	assert_eq((inspector.get_value_control(4) as SpinBox).value, 77.0,
		"poll refresh mirrors vars even when no variable_changed callback fired")


# Regression: a typed var with a nominal min > 0 (menuscript "Entry", min 1)
# must still show the VM's true default (0), not clamp it up to the min and
# look "hardset". The spinbox uses allow_lesser/allow_greater so its range is
# a hint, never a clamp.
func test_entry_var_shows_true_value_not_clamped_min():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var insp: Control = lm.get_node("%VarInspector")
	insp.set_script_name("menuscript")
	await get_tree().process_frame
	var ctrl = insp.get_value_control(0)
	assert_true(ctrl is SpinBox, "Entry renders as a spinbox")
	assert_eq(ctrl.value, 0.0, "Entry shows the true VM value 0, not the clamped min 1")
	assert_true(ctrl.allow_lesser, "spinbox can represent values below the nominal min")


# Polish B3: events log uses incremental update and caps at 50 rows. _log()
# is the system-event shim and is always shown.
func test_events_log_appends_and_caps():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	assert_eq(events.item_count, 0, "events list starts empty")
	lm._log("test entry 1")
	lm._log("test entry 2")
	assert_eq(events.item_count, 2, "two entries appended")
	for i in range(60):
		lm._log("flood %d" % i)
	assert_eq(events.item_count, 50, "events list capped at 50")
	assert_eq(events.get_selected_items().size(), 1, "tail row selected for auto-scroll")


# --- Section jump ------------------------------------------------------

func test_jump_dropdown_populates_after_open():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var jump: OptionButton = lm.get_node("%JumpSection")
	assert_eq(jump.item_count, 0, "no sections before a script loads")
	assert_true(jump.disabled, "jump disabled before a script loads")
	assert_eq(jump.tooltip_text, "Open or create a music project first.")
	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	var script_name := StringName(doc.mus_script.get_default_script_name())
	var expected: int = doc.mus_script.get_section_names(script_name).size()
	assert_eq(jump.item_count, expected, "dropdown lists every section (incl. unreachable stings)")
	assert_true(jump.disabled, "still disabled until RUNNING")
	assert_eq(jump.tooltip_text, "Start playback before jumping to a section.")


func test_jump_enabled_only_while_running():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var jump: OptionButton = lm.get_node("%JumpSection")
	assert_true(jump.disabled, "disabled while stopped")
	assert_eq(jump.tooltip_text, "Start playback before jumping to a section.")
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	assert_eq(lm._director.vm_state(), 1, "RUNNING after Start")
	assert_false(jump.disabled, "enabled while RUNNING")
	assert_eq(jump.tooltip_text, "Jump to any section while playback is running.")
	lm.get_node("%StopButton").pressed.emit()
	await get_tree().create_timer(0.05).timeout
	assert_true(jump.disabled, "disabled again after Stop")
	assert_eq(jump.tooltip_text, "Start playback before jumping to a section.")


func test_jump_to_section_drives_director():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	lm.get_node("%StartButton").pressed.emit()
	await get_tree().create_timer(0.1).timeout
	var script_name := StringName(doc.mus_script.get_default_script_name())
	var names: PackedStringArray = doc.mus_script.get_section_names(script_name)
	# Prefer a win/lose sting (the whole point: unreachable via the script's
	# own branches), else any section other than the current one.
	var target_idx := -1
	for i in range(names.size()):
		if names[i] in ["Win000", "Missionwin", "Lose000", "Missionlose"]:
			target_idx = i
			break
	if target_idx == -1:
		for i in range(names.size()):
			if names[i] != String(lm._current_section):
				target_idx = i
				break
	assert_gt(target_idx, -1, "found a section to jump to")
	# Jump fires section_entered synchronously, so _current_section updates
	# before _on_jump_selected returns (assert without ticking the VM again).
	lm._on_jump_selected(target_idx)
	assert_eq(String(lm._current_section), String(names[target_idx]),
		"jump drives the VM's current section")
	assert_string_contains(lm.get_node("%NowPlaying").text, String(names[target_idx]))


func test_section_map_builds_graph_nodes():
	# Direction B: the section view is now a GraphEdit state map (one GraphNode
	# per section), not a VBox of button rows. Clicking a node jumps the running
	# VM there; while stopped the jump is a no-op (the old buttons were disabled).
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var map = lm.get_node("%SectionMap")
	assert_true(map is GraphEdit, "section view is a GraphEdit map")
	var nodes := []
	for c in map.get_children():
		if c is GraphNode:
			nodes.append(c)
	assert_gt(nodes.size(), 0, "map built a GraphNode per section")
	assert_ne(String(nodes[0].title), "", "node carries its section name as the title")
	# Stopped: clicking a node must not jump (jump is RUNNING-gated, like the old
	# disabled buttons).
	var before := String(lm._current_section)
	lm._on_map_node_selected(nodes[0])
	assert_eq(String(lm._current_section), before, "node click is a no-op while stopped")


# --- Now playing / volume meter ----------------------------------------

func test_now_playing_readout():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var np: Label = lm.get_node("%NowPlaying")
	assert_eq(np.text, "Now playing: —", "dash at rest")
	lm._last_state = lm.VM_RUNNING
	lm._on_section(&"Intro")
	assert_string_contains(np.text, "Intro")


func test_volume_meter_reflects_signal():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	# 200 << 16 = 13107200 -> 200.0 after the 16.16 decode.
	lm._director.volume_changed.emit(13107200, 13107200)
	var meter: Control = lm.get_node("%VolumeMeter")
	var bar_l: ProgressBar = meter.get_node("VBox/RowL/BarL")
	assert_almost_eq(bar_l.value, 200.0, 0.5, "meter reflects the volume signal")


# --- Events log filtering + colour -------------------------------------

func test_event_filter_hides_type():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	# var filter is off by default (the Variables tab mirrors values).
	assert_false(lm.get_node("%FilterVar").button_pressed, "var filter off by default")
	lm._on_variable_changed(3, 7)
	assert_eq(events.item_count, 0, "var line hidden while filter off")
	lm.get_node("%FilterVar").button_pressed = true
	lm._on_variable_changed(3, 9)
	assert_eq(events.item_count, 1, "var line shown once filter on (and value changed)")


func test_event_color_coding():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	lm._on_section(&"Begin")  # SECTION events are shown by default
	assert_eq(events.item_count, 1, "section event logged")
	assert_eq(events.get_item_custom_fg_color(0), lm._EV_COLOR[lm.EvType.SECTION],
		"section row is colour-coded")


# --- Run legibility: idle de-dup + log coalescing -----------------------

func test_section_self_loop_collapses_to_one_row():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._last_state = lm.VM_RUNNING
	var events: ItemList = lm.get_node("%Events")
	# First entry is a real transition; the next 30 are idle self-loop ticks.
	lm._on_section(&"Missionnull")
	for i in range(30):
		lm._on_section(&"Missionnull")
	assert_eq(events.item_count, 1, "self-loop section logged exactly once")
	assert_eq(lm._idle_ticks, 30, "idle ticks counted without flooding")
	var np: String = lm.get_node("%NowPlaying").text
	assert_string_contains(np, "Missionnull")
	assert_string_contains(np, "idle")


func test_real_section_change_clears_idle():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._last_state = lm.VM_RUNNING
	var events: ItemList = lm.get_node("%Events")
	lm._on_section(&"A")
	for i in range(5):
		lm._on_section(&"A")
	lm._on_section(&"B")
	assert_eq(events.item_count, 2, "only the two real transitions logged (A, B)")
	assert_eq(lm._idle_ticks, 0, "idle reset on a real transition")
	var np: String = lm.get_node("%NowPlaying").text
	assert_string_contains(np, "B")
	assert_false(np.contains("idle"), "no idle marker right after a real change")


func test_consecutive_identical_sounds_coalesce():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	# Multiplayerstart plays sound_0 twenty times; collapse to one counted row.
	for i in range(20):
		lm._on_sound(0, &"sound_0", false)
	assert_eq(events.item_count, 1, "20 identical plays coalesce to one row")
	assert_true(events.get_item_text(0).ends_with("x20"), "row shows the x20 count")
	assert_eq(events.get_item_custom_fg_color(0), lm._EV_COLOR[lm.EvType.SOUND],
		"coalesced row keeps the sound colour")


func test_log_typed_coalesces_identical_then_breaks():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	lm._log("same")
	lm._log("same")
	lm._log("same")
	assert_eq(events.item_count, 1, "identical lines coalesce")
	assert_true(events.get_item_text(0).ends_with("x3"), "counted x3")
	lm._log("different")
	assert_eq(events.item_count, 2, "a distinct line starts a new row")
	lm._log("different")
	assert_eq(events.item_count, 2, "the new line then coalesces")
	assert_true(events.get_item_text(1).ends_with("x2"), "second run counted x2")


func test_distinct_lines_do_not_coalesce():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	for i in range(5):
		lm._log("line %d" % i)
	assert_eq(events.item_count, 5, "distinct lines never fold together")


func test_idle_cleared_on_stop():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	lm._last_state = lm.VM_RUNNING
	lm._on_section(&"Loop")
	lm._on_section(&"Loop")
	lm._on_section(&"Loop")
	assert_gt(lm._idle_ticks, 0, "idling before stop")
	lm._on_stop()
	assert_eq(lm._idle_ticks, 0, "idle reset on stop")
	assert_eq(lm._current_section, &"", "current section cleared on stop")
	assert_eq(lm.get_node("%NowPlaying").text, "Now playing: —", "now-playing blanked on stop")


func test_clear_resets_coalesce_run():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("%Events")
	lm._log("x")
	lm._log("x")
	assert_eq(events.item_count, 1, "coalesced before clear")
	lm.get_node("%ClearButton").pressed.emit()
	assert_eq(events.item_count, 0, "list cleared")
	lm._log("x")
	assert_eq(events.item_count, 1, "fresh row after clear")
	assert_false(events.get_item_text(0).contains("  x"), "no leftover x-count on the fresh row")


func _copy_temp_pair() -> String:
	_copy_fixture(BANK_FIXTURE, PAIR_BANK)
	_copy_fixture(SCRIPT_FIXTURE, PAIR_SCRIPT)
	return PAIR_BANK


func _cleanup_temp_pair() -> void:
	_remove_user_file(PAIR_BANK)
	_remove_user_file(PAIR_SCRIPT)
	_remove_user_file(PAIR_PROFILE)


func _copy_fixture(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "Fixture should be readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	assert_not_null(dst, "Fixture destination should be writable: %s" % dst_path)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _remove_user_file(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
