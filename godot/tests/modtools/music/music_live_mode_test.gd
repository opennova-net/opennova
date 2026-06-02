extends GutTest

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const LiveModeScene = preload("res://modtools/music/ui/live_mode.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_live_pair.sbf"
const PAIR_SCRIPT := "user://music_live_pair.bin"


func after_each() -> void:
	_remove_user_file(PAIR_BANK)
	_remove_user_file(PAIR_SCRIPT)


func test_live_starts_and_stops():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var start_btn: Button = lm.get_node("VBox/Toolbar/StartButton")
	var stop_btn: Button = lm.get_node("VBox/Toolbar/StopButton")
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


func test_var_spinbox_infrastructure():
	var doc = MusicEditorDocument.new()
	doc.open_pair(_copy_temp_pair())
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	# VarInspector should be present with 16 rows (32 children: label + spinbox per row).
	var inspector: Control = lm.get_node("VBox/VarInspector")
	assert_not_null(inspector, "VarInspector node exists")
	var grid = inspector.get_node_or_null("Scroll/Grid")
	assert_not_null(grid, "Grid inside VarInspector exists")
	# 16 vars x 2 children each = 32
	assert_eq(grid.get_child_count(), 32, "16 label+spinbox pairs built")
	pass_test("var spinbox infrastructure intact")


# Polish B4: Pause and Resume buttons on the toolbar.
func test_pause_resume_buttons_present():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	assert_not_null(lm.get_node_or_null("VBox/Toolbar/PauseButton"),
		"PauseButton present in toolbar")
	assert_not_null(lm.get_node_or_null("VBox/Toolbar/ResumeButton"),
		"ResumeButton present in toolbar")


# Polish B5+B2: state label reads STOPPED at rest, and after a halt the
# directly-invoked _on_halted handler flips it to HALTED. We invoke the
# handler rather than wait for a script to actually run-to-end, so the test
# stays fast and deterministic.
func test_state_label_after_halt():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var label: Label = lm.get_node("VBox/StateLabel")
	assert_eq(label.text, "STOPPED", "label starts at STOPPED")
	lm._on_halted()
	assert_eq(label.text, "HALTED", "halted handler flips label to HALTED")


# Regression: when bind_document fires before a file is loaded (the
# common path: workspace mounts the empty document, then the user clicks
# Open), Live mode must still react when the document later loads a
# project. Pre-fix, bind_document did a one-shot refresh and never
# listened to `changed`, leaving Start disabled and the section graph
# empty even after a successful open.
func test_open_after_bind_enables_start():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var start_btn: Button = lm.get_node("VBox/Toolbar/StartButton")
	assert_true(start_btn.disabled, "Start disabled while no project loaded")
	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	assert_false(start_btn.disabled, "Start enables after project loads (changed signal fix)")


# Regression: same root cause as above. The var inspector shows raw
# VarXX labels until set_script_name is called with a known script.
# That used to only happen on the initial bind, so opening a known
# script after mount left labels stuck on Var00..Var15.
func test_var_labels_refresh_when_script_loads():
	var doc = MusicEditorDocument.new()
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	lm.bind_document(doc)
	await get_tree().process_frame
	var grid = lm.get_node("VBox/VarInspector/Scroll/Grid")
	# At rest with no script, label slot 0 shows the raw form.
	var label0_before := (grid.get_child(0) as Label).text
	assert_eq(label0_before, "Var00", "raw label before script loads")
	doc.open_pair(_copy_temp_pair())
	await get_tree().process_frame
	# jo_gamemus's chunk name is "gamescript" and Var01 has a known role.
	# Labels are rebuilt from scratch; index 1*2 = 2 is the Var01 label.
	var label1_after := (grid.get_child(2) as Label).text
	assert_eq(label1_after, "MissionActive (Var01)",
		"Var01 picks up the friendly name once gamescript loads")


# Polish B3: events log uses incremental update (add_item + select +
# ensure_current_is_visible) and caps at 50 rows.
func test_events_log_appends_and_caps():
	var lm: Control = LiveModeScene.instantiate()
	add_child_autofree(lm)
	await get_tree().process_frame
	var events: ItemList = lm.get_node("VBox/Events")
	assert_eq(events.item_count, 0, "events list starts empty")
	lm._log("test entry 1")
	lm._log("test entry 2")
	assert_eq(events.item_count, 2, "two entries appended")
	for i in range(60):
		lm._log("flood %d" % i)
	assert_eq(events.item_count, 50, "events list capped at 50")
	# Latest entry visible (selected).
	assert_eq(events.get_selected_items().size(), 1, "tail row selected for auto-scroll")


func _copy_temp_pair() -> String:
	_copy_fixture(BANK_FIXTURE, PAIR_BANK)
	_copy_fixture(SCRIPT_FIXTURE, PAIR_SCRIPT)
	return PAIR_BANK


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
