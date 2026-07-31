extends GutTest

## The mission loading screen (NovaLoadingScreen): the sidecar-image rule, the
## game-type text mapping, the progress-bar smoothing and fill arithmetic, and
## the SP-vs-MP text split — each against the witnessed original behavior
## [orig: render_loading_screen @ 0x521d10, HUD_GetLoadingScreenTextByGameType
## @ 0x51f300, LoadingScreen_UpdateAndPresent @ 0x586be0, draw_progress_bar_0
## @ 0x5d4c40].

const LoadingScreen := preload("res://engine/ui/nova_loading_screen.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir in _temp_dirs:
		_remove_dir_recursive(dir)
	_temp_dirs.clear()


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


func test_background_decodes_sidecar_from_language_archive_without_loose_mode() -> void:
	var dir := _make_temp_dir("loadscreen_language_pff")
	_write_pff(dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": _test_pcx_bytes(),
	}])
	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(dir, "", false, "jo"), OK,
			"the retail archive table mounts with loose lookup disabled")
	var screen: NovaLoadingScreen = autofree(NovaLoadingScreen.new())
	screen.setup(root, {"mission_file": "00TRg.bms"})
	assert_true(screen.has_background(),
			"setup decodes the mission sidecar found only in language.pff")


func test_background_setup_forces_loose_image_over_archive_in_packed_mode() -> void:
	var dir := _make_temp_dir("loadscreen_loose_first")
	_write_bytes(dir.path_join("00trg.pcx"), _solid_test_pcx(Color.BLUE))
	_write_pff(dir.path_join("language.pff"), [{
		"name": "00trg.pcx",
		"bytes": _solid_test_pcx(Color.RED),
	}])
	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(dir), OK,
		"packed-default mode would normally select the archived PCX")
	var screen: NovaLoadingScreen = autofree(NovaLoadingScreen.new())
	screen.setup(root, {"mission_file": "00TRg.bms"})

	assert_true(screen.has_background())
	var texture := LoadingScreen.load_background_texture(root, "00trg.pcx")
	assert_not_null(texture)
	if texture != null:
		assert_true(texture.get_image().get_pixel(0, 0).is_equal_approx(Color.BLUE),
			"the loading-screen caller forwards the witnessed loose-first policy")


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

func test_displayed_value_catches_up_to_reported() -> void:
	# Our present() runs at the coarse progress-emit cadence, not the original's
	# high-frequency pump, so the displayed value must catch up to reported in
	# one draw or the bar never leaves ~10 (D-LOADSCR-1). A big jump lands ON
	# reported, not one step past a stale value.
	assert_eq(LoadingScreen.step_displayed(6, 26), 26, "a reported jump catches the bar up")
	assert_eq(LoadingScreen.step_displayed(50, 100), 100, "a jump to 100 fills the bar")


func test_displayed_value_leads_reported_by_at_most_ten() -> void:
	# Once caught up, the bar creeps +1 ahead per draw (the witnessed liveness
	# lead for a grinding stage that pulses one reported value), capped at +10.
	assert_eq(LoadingScreen.step_displayed(0, 0), 1, "creep ahead of a stalled 0")
	assert_eq(LoadingScreen.step_displayed(26, 26), 27, "creep one point ahead")
	assert_eq(LoadingScreen.step_displayed(9, 0), 10)
	assert_eq(LoadingScreen.step_displayed(10, 0), 10, "cap at reported + 10")
	assert_eq(LoadingScreen.step_displayed(36, 26), 36, "cap the lead at reported + 10")


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
	assert_false(screen.has_session_overlay())
	assert_eq(screen.session_overlay_lines(), PackedStringArray(["", "", "", ""]))


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
	assert_true(screen.has_session_overlay())
	var lines := screen.session_overlay_lines()
	assert_eq(lines[0], "DEMOHOST")
	assert_eq(lines[1], "Trainingsmission")
	assert_ne(lines[2], "", "LTGT_AAS resolves from the gametext table")
	assert_eq(lines[3], "Welcome aboard")
	NovaStrings.register_table("gametext", null)


func test_present_tracks_reported_progress_then_leads() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	screen.set_progress(50)
	# Unthrottled while the displayed value trails the reported one; the first
	# present catches the bar up to reported (not one step past a stale 0), so
	# the bar reflects real progress at our coarse present() cadence.
	screen.present()
	assert_eq(screen.displayed_progress(), 50,
		"the bar catches up to the reported value in one present")
	# Once caught up, an immediate re-present is throttled (no 100 ms elapsed,
	# reported unchanged, not trailing) — the bar holds, not double-steps.
	screen.present()
	assert_eq(screen.displayed_progress(), 50, "an immediate re-present is throttled")
	# The witnessed liveness lead (+1 past reported while a stage grinds) advances
	# on a due draw; force one to exercise it without the 100 ms wait.
	screen.present(true)
	assert_eq(screen.displayed_progress(), 51, "a due draw leads reported by one")


func test_background_availability_is_publicly_observable() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	assert_true(screen.has_background(),
		"tests and owners can observe whether setup found loading art")


func test_prepare_for_blocking_load_waits_for_a_completed_frame() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	add_child(screen)
	var preparable := screen.has_method("prepare_for_blocking_load")
	assert_true(preparable,
		"a mounted loading screen exposes the frame-registration handoff")
	if not preparable:
		return
	var frame_before := Engine.get_process_frames()
	var prepared: bool = bool(await screen.call("prepare_for_blocking_load"))
	assert_true(prepared)
	assert_gte(Engine.get_process_frames(), frame_before + 2,
		"one ordinary frame must complete before the blocking load begins")
	assert_gt(screen.displayed_progress(), 0,
		"preparation submits a non-empty progress bar with the registered frame")


func test_prepare_for_blocking_load_rejects_an_unmounted_screen() -> void:
	var screen := _setup_screen({"mission_file": "00TRg.bms"})
	var preparable := screen.has_method("prepare_for_blocking_load")
	assert_true(preparable)
	if not preparable:
		return
	assert_false(bool(await screen.call("prepare_for_blocking_load")),
		"there is no frame to present before the Control enters the SceneTree")


# --- helpers -------------------------------------------------------------------

func _setup_screen(info: Dictionary) -> NovaLoadingScreen:
	var dir := _make_temp_dir("loadscreen_setup")
	_write_test_pcx(dir.path_join("00trg.pcx"))
	_write_test_pcx(dir.path_join("loadscrn.pcx"))
	var root := NovaResourceRoot.new()
	root.set_root_dir(dir)
	var screen: NovaLoadingScreen = autofree(NovaLoadingScreen.new())
	screen.setup(root, info)
	assert_true(screen.has_background(), "the test pcx decodes into a texture")
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
	_temp_dirs.append(dir)
	return dir


func _touch(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_8(0)
	f.close()


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


# A minimal valid 8-bit palettized PCX (2x2) so NovaResourceRoot.load_texture
# has something real to decode.
func _write_test_pcx(path: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(_test_pcx_bytes())
	f.close()


func _write_bytes(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "the loading-art fixture is writable")
	if file != null:
		file.store_buffer(bytes)
		file.close()


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


# PFF3: 20-byte header, 36-byte entries with 16-byte names, then payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "the packed loading-art fixture is writable")
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
