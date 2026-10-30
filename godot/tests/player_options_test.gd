extends GutTest

## Shared player options: one normalized state over the process's cfg block
## (GameCfgStore, retail's game.cfg words) feeds both menu surfaces and the
## device/runtime application seams; the block reaches game.cfg at its save
## points. The record's clamps, defaults and words are the engine's (ctest
## npruntime_player_options); these cases drive the binding and the device.

var _saved_option_volumes := PackedInt32Array()
var _run_dir := ""


func before_each() -> void:
	# The block's game.cfg in a working directory of this test's own.
	_run_dir = OS.get_cache_dir().path_join("opennova_player_options_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()
	_saved_option_volumes = AudioOut.option_volumes()


func after_each() -> void:
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)
	AudioOut.set_option_volumes(_saved_option_volumes[0], _saved_option_volumes[1], _saved_option_volumes[2])


# A game.cfg in the run directory (version 29 and `lines`), read by the next
# block: the start's read of a fresh process.
func _write_cfg(lines: Array[String]) -> void:
	var file := FileAccess.open(_run_dir.path_join(GameCfgStore.FILE_NAME), FileAccess.WRITE)
	file.store_string("version = 29\r\n" + "\r\n".join(lines) + "\r\n")
	file.close()
	GameCfgStore.reset()


func _saved_cfg() -> String:
	return FileAccess.get_file_as_string(_run_dir.path_join(GameCfgStore.FILE_NAME))


func test_defaults_and_current_snapshot_are_detached() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	assert_eq(state.sound_fx_volume, 192)
	assert_eq(state.dialog_volume, 192)
	assert_eq(state.music_volume, 192)
	assert_eq(state.crosshair_style, 0)
	assert_eq(state.crosshair_color, 0xFFFFFF, "the retail default crosshair colour is white")
	assert_true(state.crosshair_spread, "the retail default spread is on")
	assert_eq(state.aspect_mode, 0, "the defaults' memset placeholder, until the video test")

	state.sound_fx_volume = 12
	state.crosshair_style = 9
	assert_eq(options.current().sound_fx_volume, 192, "editing a snapshot cannot mutate the owner")
	assert_eq(options.current().crosshair_style, 0)


func test_tip_options_default_on_and_live_as_cfg_words() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	assert_true(state.keyboard_tips, "enable_keyboardtips defaults to 1")
	assert_true(state.gameplay_tips, "enable_gameplaytips defaults to 1")
	state.keyboard_tips = false
	options.update(state)
	var config := GameCfgStore.block()
	assert_eq(
		config.get_word("enable_keyboardtips"),
		0,
		"the keyboard-tips word is retail's integer in the block"
	)
	assert_eq(config.get_word("enable_gameplaytips"), 1)
	var reloaded := PlayerOptions.new().current()
	assert_false(reloaded.keyboard_tips)
	assert_true(reloaded.gameplay_tips)
	var copy := reloaded.copy()
	assert_false(copy.keyboard_tips, "the snapshot copy carries both words")
	assert_true(copy.gameplay_tips)


func test_update_clamps_into_the_block_and_saves_at_the_save_point() -> void:
	var options := PlayerOptions.new()
	var emitted: Array[PlayerOptionsState] = []
	options.changed.connect(func(state: PlayerOptionsState) -> void: emitted.append(state))

	var state := options.current()
	state.sound_fx_volume = -2
	state.dialog_volume = 93
	state.music_volume = 999
	state.crosshair_style = 99
	state.crosshair_color = 0x1FF8040
	state.crosshair_spread = false
	options.update(state)

	var current := options.current()
	assert_eq(current.sound_fx_volume, 0)
	assert_eq(current.dialog_volume, 93)
	assert_eq(current.music_volume, 255)
	assert_eq(current.crosshair_style, 24)
	assert_eq(current.crosshair_color, 0xFF8040, "the colour normalizes to its 24-bit RGB")
	assert_false(current.crosshair_spread)
	assert_eq(emitted.size(), 1, "one atomic update publishes one state")
	assert_eq(emitted[0].crosshair_style, 24)

	assert_false(
		FileAccess.file_exists(_run_dir.path_join(GameCfgStore.FILE_NAME)),
		"an edit reaches game.cfg only at a save point"
	)
	assert_eq(GameCfgStore.save(), OK)
	var text := _saved_cfg()
	assert_string_contains(text, "SFXLevel             = 0\r\n")
	assert_string_contains(text, "DialogueLevel        = 93\r\n")
	assert_string_contains(text, "music_volume         = 255\r\n")
	assert_string_contains(text, "xhair_appearance        = 24\r\n")
	assert_string_contains(text, "xhair_color             = %d\r\n" % 0xFF8040)
	assert_string_contains(text, "xhair_spread            = 0\r\n")


func test_fresh_profile_seeds_the_video_test_words_once() -> void:
	# Retail's first-launch video test writes display_16x9 from the primary
	# desktop's ratio -- 1 (the widescreen row) past 1.34, else 0 -- and the
	# graphics rungs, and saves the config, so the desktop is sampled once.
	# Headless has no desktop to vary: both branches go through the seed helper
	# with explicit sizes.
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1024, 768)), 0)
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1280, 1024)), 0)
	assert_eq(
		PlayerOptions.fresh_profile_aspect_mode(Vector2i(1366, 1024)),
		0,
		"1.334 stays under the 1.34 line"
	)
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1920, 1200)), 1)
	assert_eq(PlayerOptions.fresh_profile_aspect_mode(Vector2i(1920, 1080)), 1)
	var seeded := PlayerOptions.fresh_profile_aspect_mode(PlayerOptions.desktop_size())
	# No game.cfg: the mount's read finds the defaults, and the video test runs.
	assert_true(GameCfgStore.enter_main_menu(null))
	var state := PlayerOptions.new().current()
	assert_eq(state.aspect_mode, seeded, "a fresh profile carries the desktop's verdict")
	assert_eq(state.object_polydetail, GameWorld.object_detail_fresh_profile())
	assert_eq(state.texfilter_level, GameWorld.texfilter_level_fresh_profile())
	assert_eq(state.particle_density, GameWorld.particle_density_fresh_profile())
	assert_eq(state.texcompression_level, GameWorld.texcompression_level_fresh_profile())
	assert_eq(state.terrain_texdetail, GameWorld.terrain_texdetail_fresh_profile())
	var text := _saved_cfg()
	assert_string_contains(
		text,
		"display_16x9         = %d\r\n" % seeded,
		"the seed is saved at once, as the video test's config save does"
	)
	assert_string_contains(
		text, "object_polydetail    = %d\r\n" % GameWorld.object_detail_fresh_profile()
	)
	# Saved, the words are read back and the desktop never re-sampled: a
	# profile carrying the other row keeps it.
	_write_cfg(["display_16x9 = %d" % (1 - seeded), "object_polydetail = 0"])
	assert_true(GameCfgStore.enter_main_menu(null))
	assert_eq(
		PlayerOptions.new().current().aspect_mode, 1 - seeded, "a saved word wins over the desktop"
	)
	assert_eq(PlayerOptions.new().current().object_polydetail, 0)


# The video test's desktop is retail's GetSystemMetrics sample in its DPI-unaware process: on
# Windows the engine's read, which answers with no window (a headless run's display server has no
# screen); elsewhere the display server's primary screen stands in.
# [orig: Game_RunVideoTestDialog @0x53EC10 - GetSystemMetrics(0) called @0x53ECD8, (1) @0x53ECE2]
func test_the_video_tests_desktop_is_the_dpi_unaware_sample() -> void:
	var desktop := PlayerOptions.desktop_size()
	assert_eq(desktop, GameConfigBlock.primary_desktop_size(), "the binding's sample")
	if OS.get_name() == "Windows":
		assert_true(desktop.x > 0 and desktop.y > 0, "the system metrics answer with no window")
	else:
		assert_eq(
			desktop,
			DisplayServer.screen_get_size(DisplayServer.SCREEN_PRIMARY),
			"off Windows the display server's primary screen stands in"
		)


# The three volume options are the engine's sound option words (runtime/audio/sound_options.h), which the
# sounds scale their volume bytes by as the mixer opens them [orig: Game_ApplySessionSettingsToGlobals
# @ 0x551581..0x5515A2].
func test_update_hands_each_audio_option_to_the_engine() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	state.sound_fx_volume = 32
	state.dialog_volume = 96
	state.music_volume = 160
	options.update(state)
	assert_eq(AudioOut.option_volumes(), PackedInt32Array([32, 96, 160]),
			"the sound-effects, dialogue and music words")
