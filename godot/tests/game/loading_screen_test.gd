extends GutTest

## The mission loading screen (NovaLoadingScreen): the sidecar-image rule, the
## game-type text mapping, the progress-bar smoothing and fill arithmetic, and
## the SP-vs-MP text split — each against the witnessed original behavior
## [orig: render_loading_screen @ 0x521d10, HUD_GetLoadingScreenTextByGameType
## @ 0x51f300, LoadingScreen_UpdateAndPresent @ 0x586be0, draw_progress_bar_0
## @ 0x5d4c40].

const LoadingScreen := preload("res://engine/ui/nova_loading_screen.gd")


# --- sidecar image name [orig: 0x521d66/0x521dab] -----------------------------

func test_sidecar_name_replaces_bms_extension() -> void:
	assert_eq(LoadingScreen.sidecar_image_name("00TRg.bms"), "00TRg.pcx")
	assert_eq(LoadingScreen.sidecar_image_name("TDH_I5A.BMS"), "TDH_I5A.pcx")


func test_sidecar_name_appends_when_no_extension() -> void:
	# Path_ReplaceOrAppendExtension appends when there is nothing to replace.
	assert_eq(LoadingScreen.sidecar_image_name("dvxi5"), "dvxi5.pcx")


func test_sidecar_name_uses_the_file_part_only() -> void:
	assert_eq(LoadingScreen.sidecar_image_name("maps/ASH_I5A.bms"), "ASH_I5A.pcx")


# --- background resolution [orig: exists probe @ 0x521db5, fallback @ 0x521e20] ---

func test_background_prefers_mission_sidecar_then_falls_back() -> void:
	var dir := _make_temp_dir("loadscreen_bg")
	_touch(dir.path_join("00trg.pcx"))
	_touch(dir.path_join("loadscrn.pcx"))
	var root := NovaResourceRoot.new()
	root.set_root_dir(dir)
	var bg := LoadingScreen.resolve_background(root, "00TRg.bms")
	assert_eq(String(bg["name"]).to_lower(), "00trg.pcx", "sidecar wins when present")
	assert_true(bool(bg["custom"]))
	bg = LoadingScreen.resolve_background(root, "OTHER.bms")
	assert_eq(String(bg["name"]), "loadscrn.pcx", "missing sidecar falls back")
	assert_false(bool(bg["custom"]))


# --- game-type -> LoadingText key [orig: switch @ 0x51f30b-0x51f3a6] -----------

func test_gametype_keys_match_the_witnessed_switch() -> void:
	assert_eq(LoadingScreen.gametype_text_key(0), "LTGT_DM")
	assert_eq(LoadingScreen.gametype_text_key(0x10000), "LTGT_TDM")
	assert_eq(LoadingScreen.gametype_text_key(0x10020), "LTGT_COOP")
	assert_eq(LoadingScreen.gametype_text_key(0x30020), "LTGT_COOP",
		"the 0x20000 bit is masked out of the coop compare")
	assert_eq(LoadingScreen.gametype_text_key(0x00001), "LTGT_KOTH")
	assert_eq(LoadingScreen.gametype_text_key(0x10001), "LTGT_TKOTH")
	assert_eq(LoadingScreen.gametype_text_key(0x90002), "LTGT_SD")
	assert_eq(LoadingScreen.gametype_text_key(0x10002), "LTGT_AD")
	assert_eq(LoadingScreen.gametype_text_key(0x10004), "LTGT_CTF")
	assert_eq(LoadingScreen.gametype_text_key(0x10008), "LTGT_FB")
	assert_eq(LoadingScreen.gametype_text_key(0x10010), "LTGT_AAS")
	assert_eq(LoadingScreen.gametype_text_key(0x50010), "LTGT_CAC")


func test_unknown_gametype_yields_no_key() -> void:
	# The original leaves the line empty for an unlisted type (LABEL_29 with a
	# null lookup) — never a wrong label.
	assert_eq(LoadingScreen.gametype_text_key(-1), "")
	assert_eq(LoadingScreen.gametype_text_key(0xDEAD), "")


# --- bar smoothing [orig: 0x586c3f] --------------------------------------------

func test_displayed_value_creeps_one_per_draw() -> void:
	assert_eq(LoadingScreen.step_displayed(0, 0), 1)
	assert_eq(LoadingScreen.step_displayed(50, 100), 51)


func test_displayed_value_leads_reported_by_at_most_ten() -> void:
	assert_eq(LoadingScreen.step_displayed(9, 0), 10)
	assert_eq(LoadingScreen.step_displayed(10, 0), 10, "cap at reported + 10")


func test_displayed_value_caps_at_hundred() -> void:
	assert_eq(LoadingScreen.step_displayed(99, 100), 100)
	assert_eq(LoadingScreen.step_displayed(100, 100), 100)


# --- bar fill arithmetic [orig: v8 @ 0x5d4c40] ----------------------------------

func test_bar_fill_span_matches_the_original_arithmetic() -> void:
	var x := 368
	var w := 286
	var empty := LoadingScreen.bar_fill_span(x, w, 0)
	assert_eq(empty.y, empty.x, "0%% -> empty fill")
	var full := LoadingScreen.bar_fill_span(x, w, 100)
	assert_eq(full.x, x + 3)
	assert_eq(full.y - full.x, w, "100%% fills exactly the inner width")
	var half := LoadingScreen.bar_fill_span(x, w, 50)
	assert_eq(half.y - half.x, 144, "50%% of the 286 bar = 50*(286+2)/100 = 144")


# --- setup: SP draws no session text, MP does [orig: gate @ 0x521ebe] -----------

func test_sp_setup_loads_image_only() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	assert_false(screen._in_session)
	assert_eq(screen._title, "")
	assert_eq(screen._mission_name, "")


func test_mp_setup_carries_the_session_variables() -> void:
	_register_gametext_fixture()
	var screen := _setup_screen({
		"mission_file": "00TRg.bms",
		"in_session": true,
		"server_name": "DEMOHOST",
		"mission_name": "Trainingsmission",
		"game_type": 0x10010,
		"custom_text": "Welcome aboard",
	})
	assert_true(screen._in_session)
	assert_eq(screen._title, "DEMOHOST")
	assert_eq(screen._mission_name, "Trainingsmission")
	assert_eq(screen._custom_text, "Welcome aboard")
	assert_ne(screen._game_type_text, "", "LTGT_AAS resolves from the gametext table")
	NovaStrings.register_table("gametext", null)


func test_present_creeps_toward_reported_progress() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	screen.set_progress(50)
	screen.present()
	screen.present()
	screen.present()
	assert_eq(screen.displayed_progress(), 3,
		"unthrottled while the displayed value trails the reported one")


# --- helpers -------------------------------------------------------------------

func _setup_screen(info: Dictionary) -> NovaLoadingScreen:
	var dir := _make_temp_dir("loadscreen_setup")
	_write_test_pcx(dir.path_join("00trg.pcx"))
	_write_test_pcx(dir.path_join("loadscrn.pcx"))
	var root := NovaResourceRoot.new()
	root.set_root_dir(dir)
	var screen: NovaLoadingScreen = autofree(NovaLoadingScreen.new())
	screen.setup(root, info)
	assert_not_null(screen._texture, "the test pcx decodes into a texture")
	return screen


func _register_gametext_fixture() -> void:
	var table := RtxtStringFile.new()
	assert_eq(table.load_from_byte_array(
		FileAccess.get_file_as_bytes("res://../fixtures/rtxt/gametext.bin")), OK,
		"gametext.bin fixture loads")
	NovaStrings.register_table("gametext", table)


func _make_temp_dir(name: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


func _touch(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_8(0)
	f.close()


# A minimal valid 8-bit palettized PCX (2x2) so NovaResourceRoot.load_texture
# has something real to decode.
func _write_test_pcx(path: String) -> void:
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
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(bytes)
	f.close()
