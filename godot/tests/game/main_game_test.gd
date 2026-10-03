extends GutTest

# Runtime shell lifecycle, presentation, and explicit resource-root contracts.

const MainGameScript := preload("res://game/main_game.gd")
const MainGameScene := preload("res://game/main_game.tscn")
const ShellPresentationSessionScript := preload(
		"res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
static var FIXTURE_DIR := RuntimeFixture.directory()
const POLICY_FILE := "policy.bin"
const POLICY_PLAIN := "persisted game profile reached the mounted root"
const SCR_KEY_DEFAULT := 0xabee_face

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: Node = null


var _hud_fixture_dir := ""


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	# A shell booted here sees only the launch flags a case sets through the
	# override (the GUT process carries none; no sibling leftovers).
	LaunchFlags.set_args_override(PackedStringArray([]))
	Strings.clear()


func after_each() -> void:
	if not _hud_fixture_dir.is_empty():
		TestFs.remove_dir_recursive(_hud_fixture_dir)
		_hud_fixture_dir = ""
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()
	Strings.clear()


func _make() -> MainGame:
	# Not added to the tree: these public queries only read state, and
	# staying out of the tree keeps _ready/@onready (which need the full scene) from running.
	var game = MainGameScript.new()
	autofree(game)
	return game


func test_repeat_close_request_ends_a_pending_music_drain() -> void:
	# Out of the tree the quit's SceneTree exit is skipped, so the shell's own
	# drain latch is the observable. The first close stops the music context and
	# waits for the mixer to release the stopped playbacks; a second close while
	# that drain is pending must finish the quit instead of being swallowed.
	var game := _make()
	var bank := SbfBank.new()
	bank.load_from_path("res://../fixtures/sbf/synth_gamemus.sbf")
	var script := MusicScript.new()
	script.load_from_path("res://../fixtures/mus/synth_gamemus.bin")
	var dir: MusicDirector = MusicService.director()
	assert_not_null(dir, "the autoload owns the one director")
	if dir == null:
		return
	dir.bank = bank
	dir.load_mus_script(script)
	dir.start()
	await wait_until(dir.has_pending_playback, 2.0)
	assert_true(dir.has_pending_playback(), "the fixture reached real streaming playback")
	game.request_quit()
	assert_true(game.is_quit_drain_pending(),
			"the first close stops the context and waits for the mixer")
	game.request_quit()
	assert_false(game.is_quit_drain_pending(),
			"a repeat close while the drain is pending finishes the quit at once")
	await wait_until(func() -> bool: return not dir.has_pending_playback(), 2.0)
	await get_tree().process_frame
	assert_false(dir.has_pending_playback(), "the mixer released the stopped playback")
	assert_false(game.is_quit_drain_pending(),
			"the drained first request does not re-arm the latch")


func test_loading_background_query_is_false_without_a_live_handoff() -> void:
	var game := _make()
	assert_false(game.has_loading_background(),
			"the public loading-art query is safe while the shell is idle")


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
	var nested_owner := Node.new()
	var nested_overlay := CanvasLayer.new()
	nested_overlay.name = "NestedLayer"
	mount.add_child(hud)
	mount.add_child(menu_layer)
	# The FP gun draws inside the beauty pass: a world-only capture hides it
	# through the presenter's capture latch, not through a CanvasLayer.
	var presenter := LocalPlayerPresenter.new()
	presenter.name = "LocalPlayerPresenter"
	mount.add_child(presenter)

	var visible_hud := Control.new()
	var hidden_hud := Control.new()
	var visible_menu := Control.new()
	hidden_hud.visible = false
	hud.add_child(visible_hud)
	hud.add_child(hidden_hud)
	hud.add_child(nested_owner)
	nested_owner.add_child(nested_overlay)
	menu_layer.add_child(visible_menu)

	assert_eq(session.begin_world_only_capture(hud, menu_layer, presenter), OK)
	assert_false(hud.visible,
			"the HUD layer hides nested CanvasLayers")
	assert_false(menu_layer.visible)
	assert_true(presenter.viewmodel_rig().is_capture_hidden(),
			"the capture latches the FP gun hidden through the presenter")
	assert_false(nested_overlay.visible,
			"CanvasLayer visibility does not propagate to nested layers")
	assert_true(visible_hud.visible,
			"capture suppression must not rewrite descendant UI state")
	assert_false(hidden_hud.visible)
	assert_true(visible_menu.visible)
	assert_eq(session.begin_world_only_capture(hud, menu_layer, presenter),
			ERR_BUSY,
			"a nested capture cannot overwrite the saved visibility snapshot")

	# A real shell transition may update descendants while the async capture is
	# settling. Cleanup must reveal that new state, not replay a stale child copy.
	visible_menu.visible = false
	session.finish_world_only_capture()
	assert_true(hud.visible)
	assert_true(menu_layer.visible)
	assert_false(presenter.viewmodel_rig().is_capture_hidden(),
			"cleanup releases the FP gun's capture latch")
	assert_true(nested_overlay.visible)
	assert_true(visible_hud.visible)
	assert_false(hidden_hud.visible, "an initially hidden child stays hidden")
	assert_false(visible_menu.visible,
			"a visibility change made during capture survives cleanup")
	session.finish_world_only_capture() # idempotent cleanup


func test_world_only_capture_rejects_an_unconfigured_shell() -> void:
	var game := _make()
	assert_eq(game.mcp_begin_world_only_capture(), ERR_UNCONFIGURED)


func test_hud_hidden_capture_delegates_through_main_game_and_keeps_canvas_active() -> void:
	# The REAL shell in the minimal mission (WorldFixture.boot_shell): MainGame
	# delegates to its ShellPresentationSession over its own GameHudPresenter,
	# whose overlay the first in-world frame built. The delegation reads back
	# through the presenter's public level + witness seams, and the shell's HUD
	# CanvasLayer stays active throughout (the witness' canvas fact).
	_shell = await _booted_in_world()
	if _shell == null:
		return
	var presenter: GameHudPresenter = _shell.get_hud_presenter()
	var level_before := presenter.hud_detail_level()

	assert_eq(_shell.begin_hud_hidden_capture(), OK)
	assert_eq(presenter.get_game_hud().get_hud_detail_level(),
			HudOverlay.hud_detail_level_blank(),
			"the shell delegate decluttered the presenter's built overlay")
	# The overlay redraws at the blank level before the witness compiles its
	# gameplay draw families.
	await get_tree().process_frame
	var witness: HudHiddenCaptureWitness = _shell.hud_hidden_capture_witness()
	assert_true(witness.is_valid(), witness.error)
	assert_eq(witness.hud_detail_level, 3)
	assert_false(witness.gameplay_hud_visible)
	assert_true(witness.player_view_effects_active)
	assert_false(witness.ads_active)
	assert_false(witness.big_map_active)
	assert_true(witness.hud_canvas_layer_active)
	_shell.finish_hud_hidden_capture()
	assert_eq(presenter.hud_detail_level(), level_before,
			"finish restores the exact in-memory level through the presenter")
	assert_eq(presenter.get_game_hud().get_hud_detail_level(), level_before)
	assert_false(_shell.hud_hidden_capture_witness().is_valid(),
			"no witness outside the transaction")

	var unconfigured := _make()
	assert_eq(unconfigured.begin_hud_hidden_capture(), ERR_UNCONFIGURED)
	assert_false(unconfigured.hud_hidden_capture_witness().is_valid())


func test_runtime_shutdown_restores_an_active_hud_hidden_capture() -> void:
	_shell = await _booted_in_world()
	if _shell == null:
		return
	var presenter: GameHudPresenter = _shell.get_hud_presenter()
	var level_before := presenter.hud_detail_level()

	assert_eq(_shell.begin_hud_hidden_capture(), OK)
	assert_true(_shell.hud_hidden_capture_witness().is_valid())
	assert_eq(presenter.get_game_hud().get_hud_detail_level(),
			HudOverlay.hud_detail_level_blank())
	_shell.begin_runtime_shutdown()
	assert_false(presenter.hud_hidden_capture_witness().is_valid(),
			"runtime cancellation also restores the gameplay HUD detail")
	assert_eq(presenter.hud_detail_level(), level_before,
			"the in-memory level is the pre-capture one, not the blank capture level")
	assert_false(_shell.hud_hidden_capture_witness().is_valid())


func test_shell_presentation_session_rejects_unconfigured_hud_hidden_capture() -> void:
	var session := ShellPresentationSessionScript.new()
	assert_eq(session.begin_hud_hidden_capture(null, null), ERR_UNCONFIGURED)
	assert_false(session.hud_hidden_capture_witness(null, null).is_valid())
	session.finish_hud_hidden_capture(null)


func test_shell_presentation_session_owns_hud_hidden_capture_boundary() -> void:
	# The session over a REAL GameHudPresenter with its overlay built
	# (HudFixture: a loaded world over the staged HUD root) and a HUD layer:
	# the boundary reads back through the presenter's level + witness seams.
	var session := ShellPresentationSessionScript.new()
	_hud_fixture_dir = HudFixture.stage_root(true)
	var presenter := HudFixture.booted_presenter(self, _hud_fixture_dir)
	var hud := CanvasLayer.new()
	autofree(hud)
	var level_before := presenter.hud_detail_level()

	assert_eq(session.begin_hud_hidden_capture(presenter, hud), OK)
	assert_eq(presenter.get_game_hud().get_hud_detail_level(),
			HudOverlay.hud_detail_level_blank(),
			"the gameplay HUD detail is hidden only while screenshot presentation is active")
	assert_eq(session.begin_hud_hidden_capture(presenter, hud), ERR_BUSY,
			"a nested screenshot cannot replace the visibility snapshot")
	await get_tree().process_frame
	var witness: HudHiddenCaptureWitness = \
			session.hud_hidden_capture_witness(presenter, hud)
	assert_true(witness.is_valid(), witness.error)
	assert_eq(witness.hud_detail_level, 3)
	assert_false(witness.gameplay_hud_visible)
	assert_true(witness.player_view_effects_active)
	assert_true(witness.hud_canvas_layer_active)

	session.finish_hud_hidden_capture(presenter)
	assert_eq(presenter.hud_detail_level(), level_before,
			"screenshot cleanup restores the ordinary HUD detail")
	assert_eq(presenter.get_game_hud().get_hud_detail_level(), level_before)
	# Idempotent cleanup: a second finish is a no-op on the restored level (the
	# observable consequence of the session's one-shot finish).
	session.finish_hud_hidden_capture(presenter)
	assert_eq(presenter.hud_detail_level(), level_before, "screenshot cleanup is idempotent")
	assert_false(session.hud_hidden_capture_witness(presenter, hud).is_valid())


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


# One REAL main frame of the booted in-world shell: `process_frame` fires before
# the frame's _process callbacks, so a switch flipped now is seen by exactly
# one _process after the second await.
func _one_main_frame() -> void:
	await get_tree().process_frame
	await get_tree().process_frame


func test_main_frame_probe_spans_are_default_off() -> void:
	# The real shell in the minimal mission (WorldFixture.boot_shell): its
	# _process runs the main-frame legs every tree frame.
	_shell = await _booted_in_world()
	if _shell == null:
		return
	await _one_main_frame()
	assert_true(_shell.get_perf_probe_switches().spans.is_empty(),
			"ordinary main frames make no clock reads or span writes")

	_shell.set_perf_probe_enabled(true)
	await _one_main_frame()
	var spans: Dictionary = _shell.get_perf_probe_switches().spans
	assert_true(spans.has_all(["before", "world", "after", "hud"]),
			"an explicitly enabled probe captures each main-frame phase")

	_shell.set_perf_probe_enabled(false)
	assert_true(_shell.get_perf_probe_switches().spans.is_empty(),
			"probe teardown cannot leave stale measurements behind")


func test_main_frame_stats_feeds_gate_on_the_board() -> void:
	# The F3 Stats capture rides the same frame-leg measurements as the probe
	# but lands on the FrameStats, and only while capture is active.
	_shell = await _booted_in_world()
	if _shell == null:
		return
	var board: FrameStats = _shell.get_frame_stats()
	assert_not_null(board, "the shell owns a frame-stats board from construction")
	await _one_main_frame()
	assert_eq(board.drain().sample_frames[FrameStats.FRAME_WORLD], 0,
			"ordinary main frames feed nothing")

	board.set_capture_active(true)
	await _one_main_frame()
	var counts := board.drain().sample_frames
	# How many main frames ran since activation depends on the test's own
	# frame phase; the contract is that every leg is captured on each of them.
	var frames: int = counts[FrameStats.FRAME_WORLD]
	assert_gt(frames, 0, "an active board captures the main frames")
	for slot in [FrameStats.FRAME_PLAYER_BEFORE, FrameStats.FRAME_WORLD,
			FrameStats.FRAME_PLAYER_AFTER, FrameStats.FRAME_HUD]:
		assert_eq(counts[slot], frames, "an active board captures each main-frame leg")
	assert_true(_shell.get_perf_probe_switches().spans.is_empty(),
			"stats capture never writes the probe span dictionary")


func test_root_render_measurement_releases_on_capture_close_and_tree_exit() -> void:
	_shell = await _make_packed_shell("jodemo")
	if _shell == null:
		return
	var board: FrameStats = _shell.get_frame_stats()
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
		MissionEffect.make("dialog", 3),
		MissionEffect.make("text", 0, 0, 0, "Proceed to the beach"),
		MissionEffect.make("text"),
	])
	assert_eq(presenter.hud_objective_line(), "Proceed to the beach",
		"kind=='text' effect drives the HUD objective line; empty/other kinds ignored")


func test_objective_and_relay_effects_post_chat_lines() -> void:
	# BMS ShowWin/LoseSubgoal's objective notification lands as two chat-feed
	# lines, the gametext header and the mission directive, and a one-character
	# directive is dropped (the engine's hud_game_text.h objective_directive).
	# The S2C 0x3F mission-text relay posts its key's resolved line; an
	# unresolved key posts nothing.
	var gametext := RtxtStringFile.new()
	var misc := gametext.add_section("Misc")
	gametext.add_entry("STRMISC_NEWOBJECTIVE", "New Objective", misc, Vector2i.ZERO)
	var mission := RtxtStringFile.new()
	var win := mission.add_section("WinConditions")
	mission.add_entry("STRWINDIRECTIVE021", "Take the bridge", win, Vector2i.ZERO)
	mission.add_entry("STRWINDIRECTIVE022", "x", win, Vector2i.ZERO)
	mission.add_entry("STRRELAY01", "Reinforcements inbound", win, Vector2i.ZERO)
	var old_gametext := Strings.get_table(Strings.TABLE_GAMETEXT)
	var old_mission := Strings.get_table(Strings.TABLE_MISSION)
	Strings.register_table(Strings.TABLE_GAMETEXT, gametext)
	Strings.register_table(Strings.TABLE_MISSION, mission)
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([MissionEffect.make("objective", 2, 1, 21)])
	assert_eq(presenter.pending_chat_line_count(), 2,
			"the header and the directive each post one line")
	presenter.apply_mission_effects([MissionEffect.make("objective", 3, 1, 22)])
	assert_eq(presenter.pending_chat_line_count(), 3,
			"a one-character directive is dropped; the header still posts")
	presenter.apply_mission_effects([
		MissionEffect.make("mission_text_chat", 0, 0, 0, "STRRELAY01"),
		MissionEffect.make("mission_text_chat", 0, 0, 0, "MISSING"),
	])
	assert_eq(presenter.pending_chat_line_count(), 4,
			"the relayed key resolves; an unresolved key posts nothing")
	Strings.register_table(Strings.TABLE_GAMETEXT, old_gametext)
	Strings.register_table(Strings.TABLE_MISSION, old_mission)


func test_console_debug_text_does_not_reach_hud_objective() -> void:
	# consol/pconsol/consol# ride the debug_text kind into the system message
	# ring (the Triggered Text ring) without replacing the objective line.
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		MissionEffect.make("text", 0, 0, 0, "Hold this position"),
		MissionEffect.make("debug_text", 0, 0, 0, "trigger 17 entered"),
		MissionEffect.make("debug_text"),
	])
	assert_eq(presenter.hud_objective_line(), "Hold this position",
		"debug_text stays off the objective line")
	assert_eq(presenter.pending_hud_message_count(), 2,
		"the text line and the non-empty debug_text line queue for the rings")
	assert_eq(presenter.pending_chat_line_count(), 1,
		"the text line rides the CHAT ring, the debug_text line the SYSTEM ring")


func test_script_chat_lines_ride_the_chat_ring() -> void:
	# WAC text/ptext/text# and the lose line post into the CHAT ring
	# (Chat_AddMessageChannel1); the BMS triggered text and the console lines
	# post into the SYSTEM ring (Chat_AddMessageChannel2).
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		MissionEffect.make("text", 0, 0, 0, "Proceed to the beach"),
		MissionEffect.make("text", 7),
		MissionEffect.make("debug_text", 0, 0, 0, "Current Objective: Weapons Cache"),
		MissionEffect.make("lose", 0, 0, 0, "STRMISC_KILLEDGREEN"),
	])
	assert_eq(presenter.pending_hud_message_count(), 4,
		"every line queues until the HUD mounts")
	assert_eq(presenter.pending_chat_line_count(), 2,
		"the WAC text line and the lose line are CHAT ring lines")


func test_lose_effect_sets_endround_banner_and_message() -> void:
	# The WAC Lose banner trio is shell presentation [orig: WacAction_Lose @0x4ed3f0 ->
	# GameMsg_AddChatLineAndRelay/SetBannerText/SetTeamBannerText]: the effect carries
	# the gametext KEY; the presenter resolves it against 'Misc' (the miss-format marker
	# stands in when no gametext table is registered) and keeps the banner line for
	# the MISSION FAILED screen.
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.apply_mission_effects([
		MissionEffect.make("lose", 0, 0, 0, "STRMISC_KILLEDGREEN"),
	])
	assert_string_contains(presenter.endround_banner_line(), "STRMISC_KILLEDGREEN",
			"the lose banner resolves (or marks) the Misc gametext key")
	assert_eq(presenter.pending_hud_message_count(), 1,
			"the lose banner also lands one chat-feed line [orig: Chat_AddMessageChannel1]")
	assert_eq(presenter.pending_chat_line_count(), 1,
			"that line rides the CHAT ring")
	presenter.teardown()
	assert_eq(presenter.endround_banner_line(), "",
			"teardown clears the banner [orig: the round-start HUD reset @0x5b71b0]")


func test_crosshair_option_caches_before_hud_and_updates_an_existing_hud() -> void:
	var presenter := GameHudPresenter.new()
	autofree(presenter)
	presenter.set_crosshair_style(13)
	assert_eq(presenter.crosshair_style(), 13,
			"a pre-HUD choice is cached for the lazy build")

	# A REAL overlay over the staged HUD root (HudFixture): the presenter above
	# stays HUD-less, this one carries the built HudOverlay.
	_hud_fixture_dir = HudFixture.stage_root(true)
	var live := HudFixture.booted_presenter(self, _hud_fixture_dir)
	live.set_crosshair_style(17)
	assert_eq(live.get_game_hud().get_crosshair_style(), 17,
			"a paused game's existing HUD adopts the menu selection immediately")
	presenter.set_crosshair_style(99)
	assert_eq(presenter.crosshair_style(), HudOverlay.MAX_CROSSHAIR_STYLE,
			"the presenter keeps its lazy-build cache in the native art range")

func test_hud_loads_text_for_the_mission_that_actually_started() -> void:
	var root := ResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path(RuntimeFixture.directory())
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
	preload("res://game/world/hud_text_tables.gd").register(root, world)

	assert_not_null(Strings.get_table("mission"),
		"mnml.bin exists and must be selected from the successfully loaded BMS; medmssn.bin is absent")


# The packed lifecycle shell (WorldFixture.boot_shell) in the minimal mission,
# started through the real front end; null when the boot or the load failed.
func _booted_in_world() -> MainGame:
	var shell: MainGame = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if shell == null:
		return null
	_shell = shell
	var loaded: bool = await WorldFixture.start_shell_mission(self, shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return null
	# One shell frame ticks the HUD presenter, which builds the lazy overlay
	# the HUD-hidden capture declutters.
	await get_tree().process_frame
	assert_not_null(shell.get_hud_presenter().get_game_hud(),
			"the first in-world frame built the presenter's overlay")
	return shell


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
	WorldFixture.write_pff(self, _temp_dir.path_join("resource.pff"), entries)

	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", _temp_dir]))
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
