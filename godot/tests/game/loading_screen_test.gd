extends GutTest

## The mission loading screen (LoadingScreen): the sidecar-image rule, the
## game-type text mapping, exact stage progress and fill arithmetic, and
## the SP-vs-MP text split — each against the witnessed original behavior
## [orig: Render_LoadingScreen @ 0x521d10, HUD_GetLoadingScreenTextByGameType
## @ 0x51f300, LoadingScreen_UpdateAndPresent @ 0x586be0, HUD_DrawProgressBar_0
## @ 0x5d4c40].

const LoadingScreen := preload("res://game/ui/loading_screen.gd")
const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir in _temp_dirs:
		TestFs.remove_dir_recursive(dir)
	_temp_dirs.clear()


# The sidecar name rule is the engine's (ctest hud_loading_screen).


# --- background resolution [orig: exists probe @ 0x521db5, fallback @ 0x521e20] ---

func test_background_prefers_mission_sidecar_then_falls_back() -> void:
	var dir := _make_temp_dir("loadscreen_bg")
	_touch(dir.path_join("00trg.pcx"))
	_touch(dir.path_join("loadscrn.pcx"))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var bg := LoadingBackground.resolve(root, "00TRg.bms")
	assert_eq(bg.name.to_lower(), "00trg.pcx", "sidecar wins when present")
	assert_true(bg.custom)
	bg = LoadingBackground.resolve(root, "OTHER.bms")
	assert_eq(bg.name, "loadscrn.pcx", "missing sidecar falls back")
	assert_false(bg.custom)


func test_background_decodes_sidecar_from_language_archive_without_loose_mode() -> void:
	var dir := _make_temp_dir("loadscreen_language_pff")
	WorldFixture.write_pff(self, dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": TestPcx.ramp_2x2(),
	}])
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir, "", false, "jo"), OK,
			"the retail archive table mounts with loose lookup disabled")
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, LoadingScreenInfo.for_mission("00TRg.bms"))
	assert_true(screen.has_background(),
			"setup decodes the mission sidecar found only in language.pff")


func test_background_setup_forces_loose_image_over_archive_in_packed_mode() -> void:
	var dir := _make_temp_dir("loadscreen_loose_first")
	TestFs.write_bytes(self, dir.path_join("00trg.pcx"), TestPcx.solid_2x2(Color.BLUE))
	WorldFixture.write_pff(self, dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": TestPcx.solid_2x2(Color.RED),
	}])
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir), OK,
		"packed-default mode would normally select the archived PCX")
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, LoadingScreenInfo.for_mission("00TRg.bms"))

	assert_true(screen.has_background())
	var texture := LoadingScreen.load_background_texture(root, "00trg.pcx")
	assert_not_null(texture)
	if texture != null:
		assert_true(texture.get_image().get_pixel(0, 0).is_equal_approx(Color.BLUE),
			"the loading-screen caller forwards the witnessed loose-first policy")


# --- game-type -> LoadingText key [orig: switch @ 0x51f30b-0x51f3a6] -----------

func test_gametype_keys_match_the_witnessed_switch() -> void:
	assert_eq(HudPos.loading_gametype_text_key(0), "LTGT_DM")
	assert_eq(HudPos.loading_gametype_text_key(0x10000), "LTGT_TDM")
	assert_eq(HudPos.loading_gametype_text_key(0x10020), "LTGT_COOP")
	assert_eq(HudPos.loading_gametype_text_key(0x30020), "LTGT_COOP",
		"the 0x20000 bit is masked out of the coop compare")
	assert_eq(HudPos.loading_gametype_text_key(0x00001), "LTGT_KOTH")
	assert_eq(HudPos.loading_gametype_text_key(0x10001), "LTGT_TKOTH")
	assert_eq(HudPos.loading_gametype_text_key(0x90002), "LTGT_SD")
	assert_eq(HudPos.loading_gametype_text_key(0x10002), "LTGT_AD")
	assert_eq(HudPos.loading_gametype_text_key(0x10004), "LTGT_CTF")
	assert_eq(HudPos.loading_gametype_text_key(0x10008), "LTGT_FB")
	assert_eq(HudPos.loading_gametype_text_key(0x10010), "LTGT_AAS")
	assert_eq(HudPos.loading_gametype_text_key(0x50010), "LTGT_CAC")


func test_unknown_gametype_yields_no_key() -> void:
	# The original leaves the line empty for an unlisted type (LABEL_29 with a
	# null lookup) — never a wrong label.
	assert_eq(HudPos.loading_gametype_text_key(-1), "")
	assert_eq(HudPos.loading_gametype_text_key(0xDEAD), "")


# --- exact stage progress ------------------------------------------------------


# --- bar fill arithmetic [orig: v8 @ 0x5d4c40] ----------------------------------

func test_bar_fill_span_matches_the_original_arithmetic() -> void:
	var x := 368
	var w := 286
	var empty := HudPos.loading_bar_fill_span(x, w, 0)
	assert_eq(empty.y, empty.x, "0%% -> empty fill")
	var full := HudPos.loading_bar_fill_span(x, w, 100)
	assert_eq(full.x, x + 3)
	assert_eq(full.y - full.x, w, "100%% fills exactly the inner width")
	var half := HudPos.loading_bar_fill_span(x, w, 50)
	assert_eq(half.y - half.x, 144, "50%% of the 286 bar = 50*(286+2)/100 = 144")


# --- setup: SP draws no session text, MP does [orig: gate @ 0x521ebe] -----------

func test_sp_setup_loads_image_only() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	assert_false(screen.has_session_overlay())
	assert_eq(screen.session_overlay_lines(), PackedStringArray(["", "", "", ""]))


func test_mp_setup_carries_the_session_variables() -> void:
	_register_gametext()
	var screen := _setup_screen(LoadingScreenInfo.make("00TRg.bms", true, "DEMOHOST",
			"Trainingsmission", 0x10010, "Welcome aboard"))
	assert_true(screen.has_session_overlay())
	var lines := screen.session_overlay_lines()
	assert_eq(lines[0], "DEMOHOST")
	assert_eq(lines[1], "Trainingsmission")
	assert_ne(lines[2], "", "LTGT_AAS resolves from the gametext table")
	assert_eq(lines[3], "Welcome aboard")
	Strings.register_table("gametext", null)


# A joiner's screen goes up before the host is known; the identification carries
# the S2C 0x60's CUSTOMTEXT, whose parse clears it first, so the update replaces
# the message even with an empty one (engine: npwire/session_vars.h).
func test_a_join_identification_replaces_the_server_message() -> void:
	var screen := _setup_screen(LoadingScreenInfo.make("00TRg.bms", true, "", "", 0, ""))
	assert_eq(screen.session_overlay_lines()[3], "", "no message before the host is known")
	screen.update_session_info(null, LoadingScreenInfo.make("", true, "DEMOHOST", "", -1,
			"Welcome aboard"))
	var lines := screen.session_overlay_lines()
	assert_eq(lines[0], "DEMOHOST")
	assert_eq(lines[3], "Welcome aboard", "the host's message under the band")
	screen.update_session_info(null, LoadingScreenInfo.make("", true, "", "", -1, ""))
	lines = screen.session_overlay_lines()
	assert_eq(lines[0], "DEMOHOST", "an empty name keeps the current one")
	assert_eq(lines[3], "", "a host with no message draws none")


# --- the session block composites into the background as the game blends it
# [orig: Render_LoadingScreen @ 0x521f1a / @ 0x521f86 -> CGameFont_RenderText
# @ 0x675e40 -> CGameFont_DrawBlendedGlyph @ 0x6744a0] (D-LOADSCR-2) ------------

func test_session_block_composites_into_the_background() -> void:
	var font := FntResource.new()
	assert_eq(font.load_from_bytes(FileAccess.get_file_as_bytes(FONT_FIXTURE)), OK)
	var background := Image.create_empty(800, 600, false, Image.FORMAT_RGB8)
	var quiet := HudPos.composite_loading_session_text(background, font, font, "HOST", "", "",
			"Message", "", 661)
	assert_not_null(quiet)
	if quiet == null:
		return
	assert_eq(quiet.get_size(), Vector2i(800, 600), "the background's own size")
	assert_eq(quiet.get_format(), Image.FORMAT_RGBA8)
	assert_true(_lit(quiet, Rect2i(21, 29, 180, 50)), "the title blends into the band")
	assert_false(_lit(quiet, Rect2i(0, 505, 800, 95)), "no server message, nothing under the band")
	var told := HudPos.composite_loading_session_text(background, font, font, "HOST", "", "",
			"Message", "HI", 661)
	assert_true(_lit(told, Rect2i(16, 522, 384, 78)), "the server message under the band")
	assert_eq(background.get_pixel(30, 40), Color.BLACK, "the background itself is untouched")
	assert_null(HudPos.composite_loading_session_text(null, font, font, "", "", "", "", "", 661),
			"no background, no composite")


func test_present_tracks_exact_stage_progress() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	screen.set_progress(50)
	screen.present()
	assert_eq(screen.displayed_progress(), 50,
		"the bar displays the exact real stage checkpoint")
	screen.present()
	assert_eq(screen.displayed_progress(), 50, "an immediate re-present holds")
	screen.present(true)
	assert_eq(screen.displayed_progress(), 50,
		"a forced window-pump redraw cannot invent progress")


func test_canvas_layer_screen_tracks_viewport_resize() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var layer := CanvasLayer.new()
	viewport.add_child(layer)
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	layer.add_child(screen)
	await get_tree().process_frame
	assert_eq(screen.presented_size(), Vector2i(320, 180),
		"a CanvasLayer loading screen initially fills its viewport")
	assert_true((screen.size * screen.scale).is_equal_approx(Vector2(320, 180)))

	viewport.size = Vector2i(640, 360)
	await get_tree().process_frame
	assert_eq(screen.presented_size(), Vector2i(640, 360),
		"the loading image refits when fullscreen changes the viewport")
	assert_true((screen.size * screen.scale).is_equal_approx(Vector2(640, 360)),
		"the submitted loading image is transformed across the full new surface")


# --- a present draws the screen alone [orig: LoadingScreen_UpdateAndPresent
# @0x586be0 - Clear @0x586c5a, the screen @0x586c64, the bar @0x586cd0, Present
# @0x586d53] --------------------------------------------------------------------

func test_a_present_holds_every_offscreen_target_but_the_screens_viewport_chain() -> void:
	# The debug view's chain: an outer target holding the screen's viewport (the
	# GameViewport), the loading layer and the screen inside it.
	var outer := _target(self, SubViewport.UPDATE_ALWAYS)
	var screen_viewport := _target(outer, SubViewport.UPDATE_ALWAYS)
	var layer := CanvasLayer.new()
	screen_viewport.add_child(layer)
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	layer.add_child(screen)
	# The offscreen targets: a cube face whose capture is pending, a mirror drawing
	# every frame, a sibling drawing when visible, and one already off.
	var face := _target(screen_viewport, SubViewport.UPDATE_ONCE)
	var mirror := _target(screen_viewport, SubViewport.UPDATE_ALWAYS)
	var sibling := _target(self, SubViewport.UPDATE_WHEN_VISIBLE)
	var idle := _target(screen_viewport, SubViewport.UPDATE_DISABLED)
	assert_eq(screen.get_viewport(), screen_viewport, "the screen draws in its own viewport")

	var held := LoadingScreen.hold_offscreen_targets(self, screen.get_viewport())
	assert_eq(held.size(), 3, "the face, the mirror and the sibling are held; the idle one is off already")
	for target: SubViewport in [face, mirror, sibling, idle]:
		assert_eq(_server_mode(target), RenderingServer.VIEWPORT_UPDATE_DISABLED,
				"%s draws nothing in the present" % target.name)
	assert_eq(_server_mode(screen_viewport), RenderingServer.VIEWPORT_UPDATE_ALWAYS,
			"the screen's own viewport draws the screen")
	assert_eq(_server_mode(outer), RenderingServer.VIEWPORT_UPDATE_ALWAYS,
			"a viewport containing the screen's draws it")

	LoadingScreen.release_offscreen_targets(held)
	assert_eq(_server_mode(face), RenderingServer.VIEWPORT_UPDATE_ONCE,
			"a pending capture still draws with the next world frame")
	assert_eq(_server_mode(mirror), RenderingServer.VIEWPORT_UPDATE_ALWAYS)
	assert_eq(_server_mode(sibling), RenderingServer.VIEWPORT_UPDATE_WHEN_VISIBLE)
	assert_eq(_server_mode(idle), RenderingServer.VIEWPORT_UPDATE_DISABLED)


func test_a_present_holds_a_target_by_its_server_mode_not_its_node_property() -> void:
	# A drawn UPDATE_ONCE target keeps the node property while the server's copy is
	# off: the present has nothing of it to hold.
	var face := _target(self, SubViewport.UPDATE_ONCE)
	RenderingServer.viewport_set_update_mode(face.get_viewport_rid(),
			RenderingServer.VIEWPORT_UPDATE_DISABLED)
	assert_eq(face.render_target_update_mode, SubViewport.UPDATE_ONCE)
	var held := LoadingScreen.hold_offscreen_targets(self, get_viewport())
	assert_eq(held.size(), 0, "a drawn capture is not held")
	LoadingScreen.release_offscreen_targets(held)
	assert_eq(_server_mode(face), RenderingServer.VIEWPORT_UPDATE_DISABLED,
			"the release leaves a target it did not hold as it was")


func test_background_availability_is_publicly_observable() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	assert_true(screen.has_background(),
		"tests and owners can observe whether setup found loading art")


func test_prepare_for_blocking_load_waits_for_a_completed_frame() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	add_child(screen)
	var frame_before := Engine.get_process_frames()
	var operation := WorldLoadOperation.new()
	var prepared: bool = bool(await screen.prepare_for_blocking_load(operation))
	assert_true(prepared)
	assert_gte(Engine.get_process_frames(), frame_before + 2,
		"one ordinary frame must complete before the blocking load begins")
	assert_eq(screen.displayed_progress(), 0,
		"preparation cannot invent progress before the first real stage")


func test_prepare_for_blocking_load_rejects_an_unmounted_screen() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	var operation := WorldLoadOperation.new()
	assert_false(bool(await screen.prepare_for_blocking_load(operation)),
		"there is no frame to present before the Control enters the SceneTree")


func test_prepare_for_blocking_load_rejects_a_cancelled_operation() -> void:
	var screen := _setup_screen(LoadingScreenInfo.for_mission("00TRg.bms"))
	add_child(screen)
	var operation := WorldLoadOperation.new()
	assert_true(operation.cancel())
	assert_false(bool(await screen.prepare_for_blocking_load(operation)),
			"a cancelled load never enters the two-frame presentation barrier")


# --- helpers -------------------------------------------------------------------

func _setup_screen(info: LoadingScreenInfo) -> LoadingScreen:
	var dir := _make_temp_dir("loadscreen_setup")
	TestFs.write_bytes(self, dir.path_join("00trg.pcx"), TestPcx.ramp_2x2())
	TestFs.write_bytes(self, dir.path_join("loadscrn.pcx"), TestPcx.ramp_2x2())
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, info)
	assert_true(screen.has_background(), "the test pcx decodes into a texture")
	return screen


func _target(parent: Node, mode: SubViewport.UpdateMode) -> SubViewport:
	var target := SubViewport.new()
	target.size = Vector2i(16, 16)
	target.render_target_update_mode = mode
	if parent == self:
		add_child_autofree(target)
	else:
		parent.add_child(target)
	return target


func _server_mode(target: SubViewport) -> int:
	return RenderingServer.viewport_get_update_mode(target.get_viewport_rid())


# The menu-contract gametext.bin fixture, whose table carries the
# LTGT_* game-type labels the loading screen's session line reads.
func _register_gametext() -> void:
	var table := RtxtStringFile.new()
	assert_eq(table.load_from_byte_array(FileAccess.get_file_as_bytes(
			"res://../fixtures/game/gametext.bin")), OK, "fixture gametext.bin loads")
	Strings.register_table("gametext", table)


# Whether any pixel of `rect` carries red (the white text over a black background).
func _lit(image: Image, rect: Rect2i) -> bool:
	for y in range(rect.position.y, rect.end.y):
		for x in range(rect.position.x, rect.end.x):
			if image.get_pixel(x, y).r > 0.0:
				return true
	return false


func _make_temp_dir(name: String) -> String:
	var dir := TestFs.cache_dir(self, name)
	_temp_dirs.append(dir)
	return dir


func _touch(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_8(0)
	f.close()
