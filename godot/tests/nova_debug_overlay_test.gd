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








func test_transport_signal_stays_quiet_without_a_runtime() -> void:
	var overlay := _make_overlay()
	overlay.toggle()
	watch_signals(overlay)
	overlay._play_button.pressed.emit()
	overlay._stop_button.pressed.emit()
	assert_signal_not_emitted(overlay, "transport_used",
		"a press with nothing to act on announces nothing")












func test_view_tab_skeleton_toggle_emits() -> void:
	# The View tab's "Show skeletons" checkbox is a pure view toggle: it needs no
	# runtime and only emits intent for the host to act on (build/free the 3D view).
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay._tabs.get_node_or_null("View"), "a View tab exists")
	assert_not_null(overlay._skeleton_check, "the skeleton checkbox is reachable as a member")
	assert_eq(overlay._skeleton_check.name, "ViewSkeletons")
	assert_false(overlay._skeleton_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	overlay._skeleton_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "skeleton_debug_toggled", [true])
	overlay._skeleton_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "skeleton_debug_toggled", [false])


func test_view_tab_collision_toggle_emits() -> void:
	# The View tab's "Show collision" checkbox: same host-neutral, runtime-free
	# contract as the skeleton toggle -- it only emits intent; the host builds/frees
	# the collision debug view.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay._collision_check, "the collision checkbox is reachable as a member")
	assert_eq(overlay._collision_check.name, "ViewCollision")
	assert_false(overlay._collision_check.button_pressed, "it defaults off")

	watch_signals(overlay)
	overlay._collision_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [true])
	overlay._collision_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [false])


func test_view_tab_hide_foliage_toggle_emits() -> void:
	# The View tab's "Hide foliage" checkbox: same host-neutral, runtime-free contract as
	# the skeleton toggle -- it only emits intent for the host to act on.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_not_null(overlay._foliage_check, "the foliage checkbox is reachable as a member")
	assert_eq(overlay._foliage_check.name, "ViewHideFoliage")
	assert_false(overlay._foliage_check.button_pressed, "it defaults off (foliage shown)")

	watch_signals(overlay)
	overlay._foliage_check.toggled.emit(true)
	assert_signal_emitted_with_parameters(overlay, "foliage_hidden_toggled", [true])
	overlay._foliage_check.toggled.emit(false)
	assert_signal_emitted_with_parameters(overlay, "foliage_hidden_toggled", [false])



const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
var _saved_resource_dir := ""


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)


