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

func test_mp_setup_carries_the_session_variables() -> void:
	if not _register_gametext_fixture():
		return
	var screen := _setup_screen(LoadingScreenInfo.make("00TRg.bms", true, "DEMOHOST",
			"Trainingsmission", 0x10010, "Welcome aboard"))
	assert_true(screen.has_session_overlay())
	var lines := screen.session_overlay_lines()
	assert_eq(lines[0], "DEMOHOST")
	assert_eq(lines[1], "Trainingsmission")
	assert_ne(lines[2], "", "LTGT_AAS resolves from the gametext table")
	assert_eq(lines[3], "Welcome aboard")
	Strings.register_table("gametext", null)


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
func _register_gametext_fixture() -> bool:
	var path := RetailData.fixture("rtxt/gametext.bin")
	if path.is_empty():
		pending(RetailData.fixture_pending_text("rtxt/gametext.bin"))
		return false
	var table := RtxtStringFile.new()
	assert_eq(table.load_from_byte_array(FileAccess.get_file_as_bytes(path)), OK,
		"the reference gametext.bin loads")
	Strings.register_table("gametext", table)
	return true


func _make_temp_dir(name: String) -> String:
	var dir := OS.get_cache_dir().path_join("opennova_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	_temp_dirs.append(dir)
	return dir


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
