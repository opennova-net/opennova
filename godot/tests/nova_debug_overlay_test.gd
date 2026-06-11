extends GutTest

# NovaDebugOverlay: the F3 mission debug overlay over a live MissionRuntime.
# Drives the REAL runtime (real NovaSimulation over a fake placed node, the
# mission_runtime_test bootstrap) to pin: the re-resolved runtime source
# (mission reloads recreate the runtime), the entities/sim/vars panes, the
# transport buttons, the writes gate, and the hidden-pauses-refresh contract.

const OverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const MainGameScript := preload("res://game/main_game.gd")


class FakeModel:
	extends Node3D
	func play_part_anim(_channel: int, _play_type: int, _time_s: float) -> void:
		pass
	func set_part_phase(_channel: int, _phase: int) -> void:
		pass


func _make_runtime() -> Node:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)  # KIND_ORGANIC
	var container := Node3D.new()
	add_child_autofree(container)
	var model := FakeModel.new()
	model.set_meta("entity_ref", { "kind": 3, "index": 0, "bms_id": 0, "group": -1 })
	container.add_child(model)
	var rt: Node = MissionRuntime.new()
	add_child_autofree(rt)
	assert_eq(int(rt.setup(md, container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS })), 1)
	return rt


func _make_overlay() -> CanvasLayer:
	var overlay: CanvasLayer = OverlayScript.new()
	add_child_autofree(overlay)
	return overlay


func test_attach_and_toggle_populates_entities() -> void:
	var rt := _make_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(rt)
	assert_false(overlay.visible, "the overlay starts hidden")
	overlay.toggle()
	assert_true(overlay.visible)
	assert_false(overlay._status_label.visible, "a live runtime hides the empty-state line")
	assert_true(overlay._tabs.visible)
	assert_eq(overlay._entity_list.item_count, 1, "the one promoted organic lists")
	assert_string_contains(overlay._entity_list.get_item_text(0), "ssn",
		"rows carry the runtime id")


func test_without_runtime_reports_no_mission() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true(overlay._status_label.visible, "no source - the overlay says so")
	assert_true(overlay._tabs.visible,
		"the tabs stay usable (the perf pane works from host-wide state, no sim needed)")
	assert_eq(overlay._entity_list.item_count, 0, "the sim-fed panes sit empty")

	overlay.set_runtime_source(func(): return null)
	overlay.refresh_now()
	assert_true(overlay._status_label.visible, "a null-returning source reads as no mission")


func test_runtime_source_survives_reloads() -> void:
	# Mission reloads free and recreate the MissionRuntime; the overlay must
	# re-resolve through its source every refresh, never hold the old node.
	var holder := {"rt": _make_runtime()}
	var overlay := _make_overlay()
	overlay.set_runtime_source(func(): return holder["rt"])
	overlay.toggle()
	assert_eq(overlay._entity_list.item_count, 1)

	(holder["rt"] as Node).free()
	holder["rt"] = null
	overlay.refresh_now()
	assert_true(overlay._status_label.visible, "a freed runtime degrades to the empty state")
	assert_eq(overlay._entity_list.item_count, 0, "...and clears the dead sim's rows")

	holder["rt"] = _make_runtime()
	overlay.refresh_now()
	assert_eq(overlay._entity_list.item_count, 1, "the recreated runtime picks right back up")


func test_transport_buttons_drive_the_runtime() -> void:
	var rt := _make_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(rt)
	overlay.toggle()

	var t0 := int(rt.get_sim().get_logic_tick())
	overlay._step_button.pressed.emit()
	assert_eq(int(rt.get_sim().get_logic_tick()), t0 + 1, "Step advances exactly one tick")
	assert_false(rt.is_playing(), "Step leaves the sim paused")

	overlay._play_button.pressed.emit()
	assert_true(rt.is_playing(), "Play starts the transport")
	overlay._pause_button.pressed.emit()
	assert_false(rt.is_playing(), "Pause stops it")

	overlay._stop_button.pressed.emit()
	assert_eq(int(rt.get_sim().get_logic_tick()), t0, "Stop rewinds to the play-start baseline")
	assert_string_contains(overlay._tick_label.text, "paused", "the tick line reads the transport")


func test_vars_pane_filters_and_gates_writes() -> void:
	var rt := _make_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(rt)
	rt.get_sim().set_mission_variable(5, 42)
	overlay.toggle()

	assert_true(overlay._nonzero_check.button_pressed, "changed-only filter defaults on")
	assert_not_null(overlay._vars_rows.get_node_or_null("VarRow_V5"),
		"the one set value renders a row")
	assert_null(overlay._vars_rows.get_node_or_null("VarRow_V6"), "zeros stay filtered")
	assert_null(overlay._vars_rows.get_node_or_null("VarRow_V5/VarEdit_V5"),
		"rows are read-only while edits are off")

	overlay._writes_check.button_pressed = true
	overlay._writes_check.toggled.emit(true)
	var edit := overlay._vars_rows.get_node_or_null("VarRow_V5/VarEdit_V5") as LineEdit
	assert_not_null(edit, "the edits toggle turns V rows into fields")
	if edit == null:
		return
	edit.text = "99"
	edit.text_submitted.emit("99")
	assert_eq(rt.get_sim().get_mission_variable(5), 99, "a submitted edit writes the live mission")

	overlay._writes_check.button_pressed = false
	overlay._writes_check.toggled.emit(false)
	overlay._on_var_submitted("123", 5)
	assert_eq(rt.get_sim().get_mission_variable(5), 99,
		"with edits off a submit is ignored (defense in depth)")


func test_vars_rebuild_defers_while_an_edit_is_in_progress() -> void:
	# With the changed-only filter, a var flipping zero<->nonzero on a running
	# mission changes the visible SET - the rebuild must never destroy a
	# LineEdit mid-typing; it lands on the next refresh after the field blurs.
	var rt := _make_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(rt)
	rt.get_sim().set_mission_variable(5, 42)
	overlay.toggle()
	overlay._writes_check.button_pressed = true
	overlay._writes_check.toggled.emit(true)
	var edit := overlay._vars_rows.get_node_or_null("VarRow_V5/VarEdit_V5") as LineEdit
	assert_not_null(edit)
	if edit == null:
		return
	edit.grab_focus()
	edit.text = "12"  # in-flight typing

	rt.get_sim().set_mission_variable(7, 1)  # the visible set changes underneath
	overlay.refresh_now()
	assert_true(is_instance_valid(edit) and edit.has_focus(),
		"the rebuild defers while the edit is in progress")
	assert_eq(edit.text, "12", "...keeping the typed text")
	assert_null(overlay._vars_rows.get_node_or_null("VarRow_V7"),
		"the stale set survives one cycle by design")

	edit.release_focus()
	overlay.refresh_now()
	assert_not_null(overlay._vars_rows.get_node_or_null("VarRow_V7"),
		"the deferred rebuild lands after the blur")


func test_entity_detail_card_follows_selection() -> void:
	var rt := _make_runtime()
	var overlay := _make_overlay()
	overlay.set_runtime(rt)
	overlay.toggle()
	assert_string_contains(overlay._entity_detail.text, "Select a unit",
		"no selection shows the hint")
	overlay._entity_list.select(0)
	overlay._entity_list.item_selected.emit(0)
	# The bootstrap organic has no route, so it idles in state 0; the card
	# still names the state through ai_state_name ("?" for the unknowns).
	assert_string_contains(overlay._entity_detail.text, "state: ",
		"the card shows the AI state line")
	assert_string_contains(overlay._entity_detail.text, "health",
		"...and the health line")
	assert_string_contains(overlay._entity_detail.text, "on foot",
		"...and the organic's infantry trait")


func test_refresh_timer_pauses_while_hidden() -> void:
	var overlay := _make_overlay()
	assert_true(overlay._timer.paused, "hidden overlay never refreshes")
	overlay.toggle()
	assert_false(overlay._timer.paused, "showing resumes the low-Hz refresh")
	assert_almost_eq(overlay._timer.wait_time, 0.25, 0.001, "~4 Hz, never per-frame")
	overlay.toggle()
	assert_true(overlay._timer.paused)


# --- The game host's F3 seam --------------------------------------------------
# The real scene, with the resource dir cleared (before_all/after_all, so an
# aborted test can never leave the user's real setting blank) - _ready then
# stops at the headless picker guard instead of mounting a real game folder.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
var _saved_resource_dir := ""


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)


func test_main_game_f3_toggles_overlay_lazily() -> void:
	var game = add_child_autofree(preload("res://game/main_game.tscn").instantiate())
	await get_tree().process_frame

	assert_null(game._debug_overlay, "no overlay until the first F3")
	game._toggle_debug_overlay()
	assert_not_null(game._debug_overlay, "the first toggle builds it")
	assert_true(game._debug_overlay.visible)
	assert_true(game._debug_overlay._status_label.visible,
		"without a running mission it reports so instead of erroring")
	game._toggle_debug_overlay()
	assert_false(game._debug_overlay.visible, "the second toggle hides, never rebuilds")
