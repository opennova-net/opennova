extends GutTest

# Runtime shell (main_game): the F9 "change game folder" hotkey may only summon the
# asset picker from the menu front-end and never while one is already open or while a
# mission is live. The native dialog can't be shown headless, so these gate the pure
# predicate that decides whether the hotkey acts (the hotkey just calls into it).

const MainGameScript := preload("res://game/main_game.gd")
const MainGameScene := preload("res://game/main_game.tscn")
const ShellPresentationSessionScript := preload(
		"res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
const FIXTURE_DIR := "res://../assets"
const POLICY_FILE := "policy.bin"
const POLICY_PLAIN := "persisted game profile reached the mounted root"
const SCR_KEY_DEFAULT := 0xabee_face

var _saved_config := PackedByteArray()
var _had_config := false
var _temp_dir := ""
var _shell: Node = null


class FakeGameHud:
	extends RefCounted
	var crosshair_style := -1
	var visible := true

	func set_crosshair_style(style: int) -> void:
		crosshair_style = style


class HudHiddenPresenterHarness:
	extends GameHudPresenter
	var begin_calls := 0
	var finish_calls := 0
	var capture_active := false

	func begin_hud_hidden_capture() -> Error:
		begin_calls += 1
		capture_active = true
		return OK

	func finish_hud_hidden_capture() -> void:
		finish_calls += 1
		capture_active = false

	func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
		var witness := HudHiddenCaptureWitness.new()
		if not capture_active:
			witness.error = "HUD-hidden capture is not active"
			return witness
		witness.hud_detail_level = 3
		witness.gameplay_hud_visible = false
		witness.player_view_effects_active = true
		witness.fps_counter_visible = false
		return witness


func before_each() -> void:
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
			if _had_config else PackedByteArray()
	Strings.clear()


func after_each() -> void:
	if is_instance_valid(_shell):
		var world = _shell.get_node_or_null("World")
		if world != null:
			world.unload()
			var world_root = world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_shell = _shell.get_node_or_null("MenuLayer/MenuShell")
		if menu_shell != null and menu_shell.get_resource_root() != null:
			var menu_root = menu_shell.get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
		await get_tree().process_frame
	NovaMusicService.stop_context()
	if not _temp_dir.is_empty():
		_remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	Strings.clear()


func _make() -> MainGame:
	# Not added to the tree on purpose: the predicate only reads _state/_picker, and
	# staying out of the tree keeps _ready/@onready (which need the full scene) from running.
	var game = MainGameScript.new()
	autofree(game)
	return game


func test_can_summon_in_menu_state() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	game._picker = null
	assert_true(game._can_summon_dir_picker(), "picker summonable from the menu front-end")


func test_cannot_summon_during_mission() -> void:
	var game := _make()
	game._picker = null
	game._state = MainGameScript.State.WORLD
	assert_false(game._can_summon_dir_picker(), "not summonable while a world is live")
	game._state = MainGameScript.State.PAUSED
	assert_false(game._can_summon_dir_picker(), "not summonable from the pause overlay")


func test_cannot_summon_while_picker_open() -> void:
	var game := _make()
	game._state = MainGameScript.State.MENU
	var picker := FileDialog.new()
	game._picker = picker
	assert_false(game._can_summon_dir_picker(), "no second picker while one is already open")
	picker.queue_free()


func test_loading_background_query_is_false_without_a_live_handoff() -> void:
	var game := _make()
	assert_false(game.has_loading_background(),
			"the public loading-art query is safe while the shell is idle")


func test_main_game_scene_mounts_the_gameplay_fps_counter() -> void:
	var game := MainGameScene.instantiate()
	autofree(game)
	var fps_label := game.get_node_or_null("HUD/FpsLabel") as Label
	assert_not_null(fps_label,
			"ordinary gameplay mounts the player-visible FPS counter")
	if fps_label != null:
		assert_true(fps_label.visible,
				"the FPS counter starts visible outside screenshot capture")


func test_world_only_capture_hides_layers_without_overwriting_descendant_state() -> void:
	# Session-direct (the file's ShellPresentationSession pattern): the
	# capture contract is the session's, and driving it with its own typed
	# args needs no reach into MainGame's private fields. The MainGame
	# delegate is covered by the busy/unconfigured tests below.
	var session := ShellPresentationSessionScript.new()
	var mount := Node.new()
	add_child_autofree(mount)
	var hud := CanvasLayer.new()
	var menu_layer := CanvasLayer.new()
	var viewmodel_layer := CanvasLayer.new()
	var nested_owner := Node.new()
	var nested_overlay := CanvasLayer.new()
	viewmodel_layer.name = "ViewmodelPass"
	nested_overlay.name = "DebugOverlay"
	mount.add_child(hud)
	mount.add_child(menu_layer)
	# The production shape: the rig parents ViewmodelPass to the CAMERA's
	# viewport (the build step in player_viewmodel_rig.gd), and the session's
	# lookup is that viewport — there is no name-search fallback.
	var camera := Camera3D.new()
	mount.add_child(camera)
	camera.get_viewport().add_child(viewmodel_layer)
	autofree(viewmodel_layer)

	var visible_hud := Control.new()
	var hidden_hud := Control.new()
	var visible_menu := Control.new()
	var visible_viewmodel := Control.new()
	hidden_hud.visible = false
	hud.add_child(visible_hud)
	hud.add_child(hidden_hud)
	hud.add_child(nested_owner)
	nested_owner.add_child(nested_overlay)
	menu_layer.add_child(visible_menu)
	viewmodel_layer.add_child(visible_viewmodel)

	assert_eq(session.begin_world_only_capture(hud, menu_layer, camera), OK)
	assert_false(hud.visible,
			"the HUD layer hides nested CanvasLayers such as DebugOverlay")
	assert_false(menu_layer.visible)
	assert_false(viewmodel_layer.visible)
	assert_false(nested_overlay.visible,
			"CanvasLayer visibility does not propagate to nested layers")
	assert_true(visible_hud.visible,
			"capture suppression must not rewrite descendant UI state")
	assert_false(hidden_hud.visible)
	assert_true(visible_menu.visible)
	assert_true(visible_viewmodel.visible)
	assert_eq(session.begin_world_only_capture(hud, menu_layer, camera),
			ERR_BUSY,
			"a nested capture cannot overwrite the saved visibility snapshot")

	# A real shell transition may update descendants while the async capture is
	# settling. Cleanup must reveal that new state, not replay a stale child copy.
	visible_menu.visible = false
	session.finish_world_only_capture()
	assert_true(hud.visible)
	assert_true(menu_layer.visible)
	assert_true(viewmodel_layer.visible)
	assert_true(nested_overlay.visible)
	assert_true(visible_hud.visible)
	assert_false(hidden_hud.visible, "an initially hidden child stays hidden")
	assert_false(visible_menu.visible,
			"a visibility change made during capture survives cleanup")
	assert_true(visible_viewmodel.visible)
	session.finish_world_only_capture() # idempotent cleanup


func test_world_only_capture_rejects_an_unconfigured_shell() -> void:
	var game := _make()
	assert_eq(game.mcp_begin_world_only_capture(), ERR_UNCONFIGURED)


func test_hud_hidden_capture_delegates_through_main_game_and_keeps_canvas_active() -> void:
	var game := _make()
	var hud := CanvasLayer.new()
	var fps_label := Label.new()
	fps_label.name = "FpsLabel"
	hud.add_child(fps_label)
	var presenter := HudHiddenPresenterHarness.new()
	game.add_child(hud)
	game.add_child(presenter)
	game.set("_hud", hud)
	game.set("_hud_presenter", presenter)

	assert_eq(game.begin_hud_hidden_capture(), OK)
	assert_eq(presenter.begin_calls, 1)
	var witness: HudHiddenCaptureWitness = game.hud_hidden_capture_witness()
	assert_eq(witness.hud_detail_level, 3)
	assert_false(witness.gameplay_hud_visible)
	assert_true(witness.player_view_effects_active)
	assert_false(witness.ads_active)
	assert_false(witness.big_map_active)
	assert_true(witness.hud_canvas_layer_active)
	assert_false(witness.fps_counter_visible)
	game.finish_hud_hidden_capture()
	assert_eq(presenter.finish_calls, 1)
	assert_true(fps_label.visible)

	var unconfigured := _make()
	assert_eq(unconfigured.begin_hud_hidden_capture(), ERR_UNCONFIGURED)
	assert_false(unconfigured.hud_hidden_capture_witness().is_valid())


func test_runtime_shutdown_restores_an_active_hud_hidden_capture() -> void:
	var game := _make()
	var hud := CanvasLayer.new()
	var fps_label := Label.new()
	fps_label.name = "FpsLabel"
	hud.add_child(fps_label)
	var presenter := HudHiddenPresenterHarness.new()
	game.add_child(hud)
	game.add_child(presenter)
	game.set("_hud", hud)
	game.set("_hud_presenter", presenter)

	assert_eq(game.begin_hud_hidden_capture(), OK)
	assert_false(fps_label.visible)
	game.begin_runtime_shutdown()
	assert_true(fps_label.visible,
			"runtime cancellation restores screenshot-only FPS suppression")
	assert_false(presenter.capture_active,
			"runtime cancellation also restores the gameplay HUD detail")


func test_shell_presentation_session_rejects_unconfigured_hud_hidden_capture() -> void:
	var session := ShellPresentationSessionScript.new()
	assert_eq(session.begin_hud_hidden_capture(null, null), ERR_UNCONFIGURED)
	assert_false(session.hud_hidden_capture_witness(null, null).is_valid())
	session.finish_hud_hidden_capture(null)


func test_shell_presentation_session_owns_hud_hidden_capture_boundary() -> void:
	var session := ShellPresentationSessionScript.new()
	var presenter := HudHiddenPresenterHarness.new()
	var hud := CanvasLayer.new()
	var fps_label := Label.new()
	fps_label.name = "FpsLabel"
	hud.add_child(fps_label)
	autofree(presenter)
	autofree(hud)

	assert_eq(session.begin_hud_hidden_capture(presenter, hud), OK)
	assert_false(fps_label.visible,
			"the FPS counter is hidden only while screenshot presentation is active")
	assert_eq(session.begin_hud_hidden_capture(presenter, hud), ERR_BUSY,
			"a nested screenshot cannot replace the visibility snapshot")
	var witness: HudHiddenCaptureWitness = \
			session.hud_hidden_capture_witness(presenter, hud)
	assert_true(witness.is_valid())
	assert_eq(witness.hud_detail_level, 3)
	assert_false(witness.gameplay_hud_visible)
	assert_true(witness.player_view_effects_active)
	assert_true(witness.hud_canvas_layer_active)

	session.finish_hud_hidden_capture(presenter)
	assert_true(fps_label.visible,
			"screenshot cleanup restores the exact ordinary FPS visibility")
	session.finish_hud_hidden_capture(presenter)
	assert_true(fps_label.visible, "screenshot cleanup is idempotent")
	assert_false(session.hud_hidden_capture_witness(presenter, hud).is_valid())

	# An already-hidden FPS label remains hidden after the capture transaction;
	# cleanup restores state rather than unconditionally showing telemetry.
	fps_label.visible = false
	assert_eq(session.begin_hud_hidden_capture(presenter, hud), OK)
	assert_false(fps_label.visible)
	session.finish_hud_hidden_capture(presenter)
	assert_false(fps_label.visible,
			"capture restores an initially hidden FPS counter exactly")


func test_shell_presentation_session_stages_and_reveals_world_atomically() -> void:
	var session := ShellPresentationSessionScript.new()
	var menu := MenuShell.new()
	var world := GameWorld.new()
	var hud := CanvasLayer.new()
	var hud_item := Control.new()
	var loaded_callback := func() -> void: pass
	var failed_callback := func(_reason: String) -> void: pass
	autofree(menu)
	autofree(world)
	autofree(hud)
	hud.add_child(hud_item)

	session.begin_world_load(
			menu, world, hud, loaded_callback, failed_callback)
	assert_false(menu.visible)
	assert_false(world.visible)
	assert_false(hud_item.visible)
	assert_true(world.world_loaded.is_connected(loaded_callback))
	assert_true(world.load_failed.is_connected(failed_callback))

	session.finish_world_load(WorldLoadCoordinator.new(), world, hud)
	assert_true(world.visible)
	assert_true(hud_item.visible)


func test_main_frame_probe_spans_are_default_off() -> void:
	var game := _make()
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	add_child_autofree(world)
	# The shell's camera member is typed FlyCamera (the scene's camera
	# class); a plain Camera3D would be rejected by the typed member set.
	var camera := FlyCamera.new()
	add_child_autofree(camera)
	world.set("_world_ready", true)
	game.set("_world", world)
	game.set("_camera", camera)
	game.set("_state", MainGameScript.State.WORLD)

	game.call("_process", 0.0)
	assert_true((game.get("_perf_probe_spans") as Dictionary).is_empty(),
			"ordinary main frames make no clock reads or span writes")

	game.call("set_perf_probe_enabled", true)
	game.call("_process", 0.0)
	var spans: Dictionary = game.get("_perf_probe_spans")
	assert_true(spans.has_all(["before", "world", "after", "hud"]),
			"an explicitly enabled probe captures each main-frame phase")

	game.call("set_perf_probe_enabled", false)
	assert_true((game.get("_perf_probe_spans") as Dictionary).is_empty(),
			"probe teardown cannot leave stale measurements behind")


func test_main_frame_stats_feeds_gate_on_the_board() -> void:
	# The F3 Stats capture rides the same frame-leg measurements as the probe
	# but lands on the FrameStatsBoard, and only while capture is active.
	var game := _make()
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	add_child_autofree(world)
	# The shell's camera member is typed FlyCamera (the scene's camera
	# class); a plain Camera3D would be rejected by the typed member set.
	var camera := FlyCamera.new()
	add_child_autofree(camera)
	world.set("_world_ready", true)
	game.set("_world", world)
	game.set("_camera", camera)
	game.set("_state", MainGameScript.State.WORLD)

	var board: FrameStatsBoard = game.get_frame_stats_board()
	assert_not_null(board, "the shell owns a frame-stats board from construction")
	game.call("_process", 0.0)
	assert_eq(board.drain().sample_frames[FrameStatsBoard.FRAME_WORLD], 0,
			"ordinary main frames feed nothing")

	board.set_capture_active(true)
	game.call("_process", 0.0)
	var counts := board.drain().sample_frames
	for slot in [FrameStatsBoard.FRAME_PLAYER_BEFORE, FrameStatsBoard.FRAME_WORLD,
			FrameStatsBoard.FRAME_PLAYER_AFTER, FrameStatsBoard.FRAME_HUD]:
		assert_eq(counts[slot], 1, "an active board captures each main-frame leg")
	assert_true((game.get("_perf_probe_spans") as Dictionary).is_empty(),
			"stats capture never writes the probe span dictionary")


func test_root_render_measurement_releases_on_capture_close_and_tree_exit() -> void:
	_shell = await _make_packed_shell("jodemo")
	if _shell == null:
		return
	var board: FrameStatsBoard = _shell.get_frame_stats_board()
	board.set_capture_active(true)
	await get_tree().process_frame
	assert_true(_shell.is_root_render_stats_measured(),
			"an active Stats capture measures the root viewport")

	board.set_capture_active(false)
	assert_false(_shell.is_root_render_stats_measured(),
			"the capture close edge releases measurement synchronously")

	board.set_capture_active(true)
	await get_tree().process_frame
	assert_true(_shell.is_root_render_stats_measured())
	var parent := _shell.get_parent()
	parent.remove_child(_shell)
	assert_false(_shell.is_root_render_stats_measured(),
			"leaving the tree releases RenderingServer measurement state")

	# Reattach for the suite's normal shell/resource cleanup. Keep capture
	# closed so the next process frame cannot re-arm measurement.
	parent.add_child(_shell)
	board.set_capture_active(false)


func test_runtime_root_honors_the_persisted_game_profile() -> void:
	_shell = await _make_packed_shell("jodemo")
	if _shell == null:
		return
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	assert_not_null(menu_shell.get_driver(),
			"the packed fixture boots the public menu shell")
	if menu_shell.get_driver() == null:
		return
	var root: ResourceRoot = menu_shell.get_resource_root()
	assert_not_null(root, "the live menu exposes its mounted runtime root")
	if root != null:
		assert_eq(root.read_file(POLICY_FILE).get_string_from_utf8(), POLICY_PLAIN,
				"the persisted /game profile selects the root's SCR policy")


func test_mission_text_effect_reaches_hud_objective() -> void:
	# Drained effects carry {kind, a..d, str} (Simulation::drain_effects); the
	# WAC text/ptext family lands as kind=="text" with the string in "str". The
	# old handler read nonexistent "text"/"message" keys, so mission text never
	# reached the HUD.
	# The surface lives on the shared GameHudPresenter (main_game passes through);
	# out-of-tree _make() never runs _ready, so drive the presenter directly.
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		{"kind": "dialog", "a": 3},
		{"kind": "text", "str": "Proceed to the beach"},
		{"kind": "text", "str": ""},
	])
	assert_eq(presenter.hud_objective_line(), "Proceed to the beach",
		"kind=='text' effect drives the HUD objective line; empty/other kinds ignored")


func test_console_debug_text_does_not_reach_hud_objective() -> void:
	# consol/pconsol ride the distinct debug_text channel. The game does not yet
	# present an on-screen debug console, so these effects remain intentionally
	# unrouted instead of replacing player-facing mission text.
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		{"kind": "text", "str": "Hold this position"},
		{"kind": "debug_text", "str": "trigger 17 entered"},
	])
	assert_eq(presenter.hud_objective_line(), "Hold this position",
		"debug_text stays off the player-facing HUD mission-text channel")


func test_lose_effect_sets_endround_banner_and_message() -> void:
	# The WAC Lose banner trio is shell presentation [orig: WacAction_Lose @0x4ed3f0 ->
	# GameMsg_AddChatLineAndRelay/SetBannerText/SetTeamBannerText]: the effect carries
	# the gametext KEY; the presenter resolves it against 'Misc' (the miss-format marker
	# stands in when no gametext table is registered) and keeps the banner line for
	# the MISSION FAILED screen.
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		{"kind": "lose", "a": 0, "str": "STRMISC_KILLEDGREEN"},
	])
	assert_string_contains(presenter.endround_banner_line(), "STRMISC_KILLEDGREEN",
			"the lose banner resolves (or marks) the Misc gametext key")
	assert_eq(presenter.pending_hud_message_count(), 1,
			"the lose banner also lands one chat-feed line [orig: Chat_AddMessageChannel1]")
	presenter.teardown()
	assert_eq(presenter.endround_banner_line(), "",
			"teardown clears the banner [orig: the round-start HUD reset @0x5b71b0]")


func test_mission_end_screen_lose_form_and_exit() -> void:
	# The MISSION FAILED form composes the failed line + the WAC Lose banner and
	# exits over exit_requested [orig: the Cinematic_EpilogUpdate mode-2 leg; ESC ->
	# g_mission_exit_reason=1].
	var screen := MissionEndScreen.new()
	add_child_autofree(screen)
	watch_signals(screen)
	screen.setup({"ended": true, "winner_team": 2}, "You shot a friendly unit!", null)
	assert_true(_screen_has_label_containing(screen, "You shot a friendly unit!"),
			"the lose form shows the stored banner line")
	screen.request_exit()
	assert_signal_emit_count(screen, "exit_requested", 1)
	screen.request_exit()
	assert_signal_emit_count(screen, "exit_requested", 1,
			"the exit is one-shot (the shell tears the world down once)")


func test_mission_end_screen_win_form_counts() -> void:
	# The win form's count lines follow the witnessed sums [orig:
	# epilog_cinematic_state_machine_update @0x576240 case 4 — TEAMUNITS =
	# by-player + by-others, FRIENDLYUNITS likewise].
	var screen := MissionEndScreen.new()
	add_child_autofree(screen)
	screen.setup({
		"ended": true, "winner_team": 1,
		"enemy_kills": 3, "enemy_kills_by_others": 2,
		"bluekills": 1, "team_kills_by_others": 1,
		"greenkills": 0, "friendly_kills_by_others": 0,
	}, "", null)
	assert_true(_screen_has_label_containing(screen, "5"), "enemy units = 3 + 2")
	assert_true(_screen_has_label_containing(screen, "2"), "team units = 1 + 1")


func _screen_has_label_containing(node: Node, text: String) -> bool:
	if node is Label and (node as Label).text.contains(text):
		return true
	for child in node.get_children():
		if _screen_has_label_containing(child, text):
			return true
	return false


func test_crosshair_option_updates_an_existing_hud() -> void:
	# The Options signal reaches the built HUD through the shared presenter's public
	# set_crosshair_style (main_game delegates its _on_crosshair_style_changed there).
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	var hud := FakeGameHud.new()
	presenter._game_hud = hud
	presenter.set_crosshair_style(13)
	assert_eq(hud.crosshair_style, 13, "A paused game's HUD adopts the menu selection immediately.")


func test_hud_loads_text_for_the_mission_that_actually_started() -> void:
	var root := ResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../assets")
	assert_eq(root.set_root_dir(fixture_dir), OK)

	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	# The environment child makes the code-built world mission-loadable: the
	# typed placement path stamps _env.light_state onto every placed batch.
	var env := MissionEnvironment.new()
	env.name = "MissionEnvironment"
	world.add_child(env)
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(root)
	assert_eq(world.mission_file, "", "normal SP/host/join starts do not populate the exported boot option")
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_eq(world.mission_file, "", "the exported boot option remains separate after a normal load")
	var runtime = world.get_runtime()
	assert_not_null(runtime)
	assert_eq(runtime.get_mission_file(), "mnml.bms",
			"the shared F3 runtime receives the mission that actually loaded")

	# The mission string table selection lives on the shared HUD presenter now (the
	# exists-only mission-bin fallback rides its world wiring).
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.setup(world, null, null)
	presenter._load_hud_text_tables(root)

	assert_not_null(Strings.get_table("mission"),
		"mnml.bin exists and must be selected from the successfully loaded BMS; medmssn.bin is absent")


func _make_packed_shell(game_code: String):
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_root_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	var entries: Array = []
	var fixture_path := ProjectSettings.globalize_path(FIXTURE_DIR)
	for filename in DirAccess.get_files_at(fixture_path):
		var bytes := FileAccess.get_file_as_bytes(fixture_path.path_join(filename))
		assert_false(bytes.is_empty(), "%s is available in the committed fixture" % filename)
		entries.append({"name": filename, "bytes": bytes})
	entries.append({
		"name": POLICY_FILE,
		"bytes": _scr_wrap_default_key(POLICY_PLAIN.to_utf8_buffer(), 1),
	})
	_write_pff(_temp_dir.path_join("resource.pff"), entries)

	ResourceDirSettings.set_resource_dir(_temp_dir)
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game(game_code)
	var shell = MainGameScene.instantiate()
	assert_not_null(shell)
	if shell == null:
		return null
	add_child(shell)
	await get_tree().process_frame
	return shell


func _u32(value: int) -> int:
	return value & 0xffff_ffff


func _rol32(value: int, shift: int) -> int:
	value = _u32(value)
	return _u32((value << shift) | (value >> (32 - shift)))


func _xor_scr_keystream(bytes: PackedByteArray, key: int) -> PackedByteArray:
	var out := bytes.duplicate()
	for index in range(out.size()):
		key = _u32(_rol32(_u32(key + _rol32(key, 11)), 4) ^ 1)
		out[index] = int(out[index]) ^ (key & 0xff)
	return out


func _scr_wrap_default_key(plain: PackedByteArray, version: int) -> PackedByteArray:
	var payload := _xor_scr_keystream(plain, SCR_KEY_DEFAULT)
	var out := PackedByteArray()
	out.resize(4 + payload.size())
	out[0] = 83 # S
	out[1] = 67 # C
	out[2] = 82 # R
	out[3] = version
	for index in range(payload.size()):
		out[4 + index] = payload[payload.size() - 1 - index]
	return out


# PFF3: 20-byte header, 36-byte entries with 16-byte names, then payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "the packed runtime fixture is writable")
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes: PackedByteArray = entry.bytes
		var name_bytes := String(entry.name).to_utf8_buffer()
		assert_true(name_bytes.size() <= 16, "%s fits the PFF name field" % entry.name)
		file.store_32(0)
		file.store_32(next_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry.bytes)
	file.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
