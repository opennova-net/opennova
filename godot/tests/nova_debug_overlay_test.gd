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


# Duck-typed GameWorld stand-in for the overlay's world source: only the perf
# counter seam the frame-budget/foliage panes read.
class FakeWorld:
	extends RefCounted
	func get_runtime_perf_counters() -> Dictionary:
		return {
			"tick_us": 1234,
			"foliage_us": 200,
			"runtime_us": 15500,
			"audio_us": 45,
			"runtime": {
				"present_us": 700,
				"sim": { "sim_tick_us": 900, "net_tick_us": 0, "present_snapshot_us": 55 },
			},
			"foliage": { "far_cells_visible": 12, "total_instances": 3456 },
			"audio": {},
		}


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
	# 2 entities: the mission's organic + the auto-spawned local player (setup
	# spawns one for playable missions, the game default).
	assert_eq(int(rt.setup(md, container, { "tick_mode": NovaSimulation.TICK_EVERY_PROCESS })), 2)
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


func test_view_tab_collision_toggle_emits() -> void:
	# The View tab's "Show collision" checkbox: the same host-neutral, runtime-free
	# contract as the skeleton toggle -- it only emits intent; the host that owns the
	# world builds/frees the collision debug view.
	var overlay := _make_overlay()
	overlay.toggle()
	assert_false(overlay.is_collision_debug_on(), "the collision toggle seam resolves")

	assert_false(overlay.is_collision_debug_on(), "it defaults off")

	watch_signals(overlay)
	overlay.set_collision_debug(true)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [true])
	overlay.set_collision_debug(false)
	assert_signal_emitted_with_parameters(overlay, "collision_debug_toggled", [false])


func test_new_tabs_build_headless_without_a_sim_or_world() -> void:
	# Foliage + Player tabs exist and every pane survives a refresh with no
	# runtime, no world, no mission (the menu-only F3 case).
	var overlay := _make_overlay()
	overlay.toggle()
	assert_true(overlay.tab_names().has("Foliage"), "a Foliage tab exists")
	assert_true(overlay.tab_names().has("Player"), "a Player tab exists")
	assert_eq(overlay.player_panel_text(), "No mission running.",
		"the player pane reports the no-mission state")
	assert_true(overlay.is_foliage_status_visible(), "the foliage pane explains itself without a world")
	assert_true(overlay.perf_pane().frame_status.visible,
		"the frame budget explains itself without a world")
	assert_eq(overlay.perf_pane().frame_label_text("tick_us"), "—",
		"frame rows dash out without counters")


func test_frame_budget_and_foliage_render_from_world_source() -> void:
	# The optional world source feeds the Perf tab's frame budget and the Foliage
	# tab from GameWorld.get_runtime_perf_counters() (nested runtime/sim/foliage).
	var overlay := _make_overlay()
	var world := FakeWorld.new()
	overlay.set_world_source(func(): return world)
	overlay.toggle()

	assert_false(overlay.perf_pane().frame_status.visible, "counters present - no status line")
	assert_eq(overlay.perf_pane().frame_label_text("tick_us"), "1.23 ms",
		"top-level µs render (ms above 1000)")
	assert_eq(overlay.perf_pane().frame_label_text("sim_tick_us"), "900 µs",
		"the nested runtime.sim numbers render")
	assert_eq(overlay.perf_pane().frame_label_text("present_us"), "700 µs",
		"the nested runtime numbers render")

	assert_false(overlay.is_foliage_status_visible(), "foliage stats present - no status line")
	assert_eq(overlay.foliage_stat_text("far_cells_visible"), "12")
	assert_eq(overlay.foliage_stat_text("total_instances"), "3456")
	assert_eq(overlay.foliage_stat_text("model_uploads"), "—",
		"stats the world did not report stay dashed")

	# Dropping the world returns every pane to its empty state.
	overlay.set_world_source(func(): return null)
	overlay.refresh_now()
	assert_true(overlay.perf_pane().frame_status.visible)
	assert_eq(overlay.perf_pane().frame_label_text("tick_us"), "—")


func test_player_tab_reports_the_live_player_and_net_state() -> void:
	# The runtime bootstrap auto-spawns the local player (playable default), so
	# the Player tab reads every live sim seam end to end: pose, health, weapon,
	# the collision-maintained zone flags, and the SP network one-liner.
	var overlay := _make_overlay()
	var rt := _make_runtime()
	overlay.set_runtime(rt)
	overlay.toggle()
	var text: String = overlay.player_panel_text()
	assert_string_contains(text, "position:")
	assert_string_contains(text, "health: 100 / 100")
	assert_string_contains(text, "weapon: none")
	assert_string_contains(text, "indoors: no")
	assert_string_contains(text, "in armory zone: no")
	assert_string_contains(text, "network: single player")


func test_collision_debug_binding_shape_on_a_live_sim() -> void:
	# get_collision_debug() answers headless with the stable empty shape: no
	# collision models registered (nothing placed) and no player resolve yet.
	var rt := _make_runtime()
	var sim = rt.get_sim()
	assert_true(sim.has_method("get_collision_debug"), "the sim binds get_collision_debug")
	var debug: Dictionary = sim.get_collision_debug()
	assert_true(debug.has("instances"))
	assert_true(debug.has("player"))
	assert_eq((debug["instances"] as Array).size(), 0, "no collision models registered")
	assert_false(bool((debug["player"] as Dictionary).get("valid", true)),
		"no resolver pass has run - the player capture stays invalid")



const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
var _saved_resource_dir := ""


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)
