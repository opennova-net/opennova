extends GutTest

## The mission loading screen (LoadingScreen): the sidecar-image rule, the
## game-type text mapping, exact stage progress and fill arithmetic, and
## the SP-vs-MP text split — each against the witnessed original behavior
## [orig: Render_LoadingScreen @ 0x521d10, HUD_GetLoadingScreenTextByGameType
## @ 0x51f300, LoadingScreen_UpdateAndPresent @ 0x586be0, HUD_DrawProgressBar_0
## @ 0x5d4c40].

const LoadingScreen := preload("res://game/ui/loading_screen.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir in _temp_dirs:
		TestFs.remove_dir_recursive(dir)
	_temp_dirs.clear()


# --- sidecar image name [orig: 0x521d66/0x521dab] -----------------------------

func test_sidecar_name_replaces_bms_extension() -> void:
	assert_eq(HudPos.loading_sidecar_image_name("00TRg.bms"), "00TRg.pcx")
	assert_eq(HudPos.loading_sidecar_image_name("TDH_I5A.BMS"), "TDH_I5A.pcx")


func test_sidecar_name_appends_when_no_extension() -> void:
	# Path_ReplaceOrAppendExtension appends when there is nothing to replace.
	assert_eq(HudPos.loading_sidecar_image_name("dvxi5"), "dvxi5.pcx")


func test_sidecar_name_uses_the_file_part_only() -> void:
	assert_eq(HudPos.loading_sidecar_image_name("maps/ASH_I5A.bms"), "ASH_I5A.pcx")


# --- background resolution [orig: exists probe @ 0x521db5, fallback @ 0x521e20] ---

func test_background_prefers_mission_sidecar_then_falls_back() -> void:
	var dir := _make_temp_dir("loadscreen_bg")
	_touch(dir.path_join("00trg.pcx"))
	_touch(dir.path_join("loadscrn.pcx"))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var bg := LoadingScreen.resolve_background(root, "00TRg.bms")
	assert_eq(bg.name.to_lower(), "00trg.pcx", "sidecar wins when present")
	assert_true(bg.custom)
	bg = LoadingScreen.resolve_background(root, "OTHER.bms")
	assert_eq(bg.name, "loadscrn.pcx", "missing sidecar falls back")
	assert_false(bg.custom)


func test_background_decodes_sidecar_from_language_archive_without_loose_mode() -> void:
	var dir := _make_temp_dir("loadscreen_language_pff")
	WorldFixture.write_pff(self, dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": _test_pcx_bytes(),
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
	TestFs.write_bytes(self, dir.path_join("00trg.pcx"), _solid_test_pcx(Color.BLUE))
	WorldFixture.write_pff(self, dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": _solid_test_pcx(Color.RED),
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
	_write_test_pcx(dir.path_join("00trg.pcx"))
	_write_test_pcx(dir.path_join("loadscrn.pcx"))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, info)
	assert_true(screen.has_background(), "the test pcx decodes into a texture")
	return screen


# The shipped gametext.bin (the LTGT_* game-type labels live in its table) comes
# from the reference fixture set (docs/asset-gated-tests.md); false = pending.
func _make_temp_dir(name: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	_temp_dirs.append(dir)
	return dir


func _touch(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_8(0)
	f.close()


# A minimal valid 8-bit palettized PCX (2x2) so ResourceRoot.load_texture
# has something real to decode.
func _write_test_pcx(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(_test_pcx_bytes())
	f.close()


func _test_pcx_bytes() -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(128)
	bytes[0] = 0x0A  # manufacturer
	bytes[1] = 5     # version
	bytes[2] = 1     # RLE
	bytes[3] = 8     # bits per pixel
	# xmin/ymin = 0, xmax/ymax = 1 (little-endian u16 pairs at 4..11)
	bytes[8] = 1
	bytes[10] = 1
	bytes[65] = 1    # planes
	bytes[66] = 2    # bytes per line
	for p in [0, 1, 2, 3]:  # 4 literal pixels (values < 0xC0 pass through RLE)
		bytes.append(p)
	bytes.append(0x0C)  # palette marker
	for i in range(256):
		bytes.append(i)  # r
		bytes.append(i)  # g
		bytes.append(i)  # b
	return bytes


func _solid_test_pcx(color: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(128)
	bytes[0] = 0x0A
	bytes[1] = 5
	bytes[2] = 1
	bytes[3] = 8
	bytes[8] = 1
	bytes[10] = 1
	bytes[65] = 1
	bytes[66] = 2
	for _pixel in range(4):
		bytes.append(1)
	bytes.append(0x0C)
	for index in range(256):
		if index == 1:
			bytes.append(int(color.r * 255.0))
			bytes.append(int(color.g * 255.0))
			bytes.append(int(color.b * 255.0))
		else:
			bytes.append(0)
			bytes.append(0)
			bytes.append(0)
	return bytes
