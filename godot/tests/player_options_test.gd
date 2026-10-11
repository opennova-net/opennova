extends GutTest

## Shared player options: one normalized state over the process's cfg block
## (GameCfgStore, retail's game.cfg words) feeds both menu surfaces and the
## device/runtime application seams; the block reaches game.cfg at its save
## points.

const BUS_NAMES := [&"SFX", &"Ambient", &"Voice", &"Music"]

var _saved_bus_volumes: Dictionary = {}
var _run_dir := ""


func before_each() -> void:
	# The block's game.cfg in a working directory of this test's own.
	_run_dir = OS.get_cache_dir().path_join("opennova_player_options_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_run_dir)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))
	GameCfgStore.reset()
	_saved_bus_volumes.clear()
	for bus_name: StringName in BUS_NAMES:
		var bus := AudioServer.get_bus_index(bus_name)
		if bus >= 0:
			_saved_bus_volumes[bus_name] = AudioServer.get_bus_volume_db(bus)


func after_each() -> void:
	GameCfgStore.reset()
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)
	for bus_name: StringName in _saved_bus_volumes:
		var bus := AudioServer.get_bus_index(bus_name)
		if bus >= 0:
			AudioServer.set_bus_volume_db(bus, float(_saved_bus_volumes[bus_name]))


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
	var emitted: Array[PlayerOptions.State] = []
	options.changed.connect(func(state: PlayerOptions.State) -> void: emitted.append(state))

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


func test_load_normalizes_corrupt_saved_words() -> void:
	_write_cfg(
		[
			"SFXLevel = 260",
			"DialogueLevel = -1",
			"music_volume = 71",
			"xhair_appearance = -7",
			"xhair_color = %d" % 0x7F123456
		]
	)
	var state := PlayerOptions.new().current()
	assert_eq(state.sound_fx_volume, 255)
	assert_eq(state.dialog_volume, 0)
	assert_eq(state.music_volume, 71)
	assert_eq(state.crosshair_style, 0)
	assert_eq(state.crosshair_color, 0x123456, "an out-of-range colour masks down to its RGB")


func test_native_aspect_mode_lives_as_the_cfg_word() -> void:
	# Any value outside 0..3 is retail's "the surface's own ratio": a cfg-only
	# choice with no authored spin row, kept verbatim rather than clamped.
	var options := PlayerOptions.new()
	var state := options.current()
	state.aspect_mode = -1
	options.update(state)
	assert_eq(PlayerOptions.new().current().aspect_mode, -1)
	assert_eq(GameCfgStore.save(), OK)
	assert_string_contains(_saved_cfg(), "display_16x9         = -1\r\n")


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
	assert_true(GameCfgStore.read_for_mount(null))
	var state := PlayerOptions.new().current()
	assert_eq(state.aspect_mode, seeded, "a fresh profile carries the desktop's verdict")
	assert_eq(state.object_polydetail, GameWorld.object_detail_fresh_profile())
	assert_eq(state.texfilter_level, GameWorld.texfilter_level_fresh_profile())
	assert_eq(state.particle_density, GameWorld.particle_density_fresh_profile())
	assert_eq(state.texcompression_level, GameWorld.texcompression_level_fresh_profile())
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
	assert_true(GameCfgStore.read_for_mount(null))
	assert_eq(
		PlayerOptions.new().current().aspect_mode, 1 - seeded, "a saved word wins over the desktop"
	)
	assert_eq(PlayerOptions.new().current().object_polydetail, 0)


func test_object_detail_lives_and_clamps_as_the_cfg_word() -> void:
	# game.cfg's object_polydetail: the video test's capable-device rung 2 on a
	# fresh profile; the load clamps a saved word into 0..3 (the engine's
	# renderer/object_lod.h through GameWorld).
	assert_eq(GameWorld.object_detail_fresh_profile(), 2)
	assert_eq(GameWorld.clamp_object_detail(-4), 0)
	assert_eq(GameWorld.clamp_object_detail(7), 3)
	var options := PlayerOptions.new()
	var state := options.current()
	state.object_polydetail = 3
	options.update(state)
	assert_eq(PlayerOptions.new().current().object_polydetail, 3)
	assert_eq(state.copy().object_polydetail, 3, "the snapshot copy carries the word")
	_write_cfg(["object_polydetail = 12"])
	assert_eq(
		PlayerOptions.new().current().object_polydetail,
		3,
		"a saved word past the top rung loads as 3"
	)
	_write_cfg(["object_polydetail = 0"])
	assert_eq(PlayerOptions.new().current().object_polydetail, 0)


func test_texture_filter_lives_and_clamps_as_the_cfg_word() -> void:
	# game.cfg's texfilter_level: the video test's 1 (trilinear) on a fresh
	# profile; the load clamps a saved word into 0..3 (the engine's
	# renderer/texture_filter.h through GameWorld).
	assert_eq(GameWorld.texfilter_level_fresh_profile(), 1)
	assert_eq(GameWorld.clamp_texfilter_level(-4), 0)
	assert_eq(GameWorld.clamp_texfilter_level(7), 3)
	var options := PlayerOptions.new()
	var state := options.current()
	state.texfilter_level = 3
	options.update(state)
	assert_eq(PlayerOptions.new().current().texfilter_level, 3)
	assert_eq(state.copy().texfilter_level, 3, "the snapshot copy carries the word")
	_write_cfg(["texfilter_level = 12"])
	assert_eq(
		PlayerOptions.new().current().texfilter_level,
		3,
		"a saved word past the top rung loads as 3"
	)
	_write_cfg(["texfilter_level = 0"])
	assert_eq(PlayerOptions.new().current().texfilter_level, 0)


func test_particle_density_lives_and_clamps_as_the_cfg_word() -> void:
	# game.cfg's particle_density: the video test's 2 (a CPU past its 2980 MHz
	# mark) on a fresh profile; the load clamps a saved word into 0..2 (the
	# engine's renderer/particle_density.h).
	assert_eq(GameWorld.particle_density_fresh_profile(), 2)
	assert_eq(GameWorld.clamp_particle_density(-4), 0)
	assert_eq(GameWorld.clamp_particle_density(7), 2)
	var options := PlayerOptions.new()
	var state := options.current()
	state.particle_density = 0
	options.update(state)
	assert_eq(PlayerOptions.new().current().particle_density, 0)
	assert_eq(state.copy().particle_density, 0, "the snapshot copy carries the word")
	_write_cfg(["particle_density = 9"])
	assert_eq(
		PlayerOptions.new().current().particle_density,
		2,
		"a saved word past the top rung loads as 2"
	)


func test_texture_compression_lives_as_the_unclamped_cfg_word() -> void:
	# game.cfg's texcompression_level: the video test's 1 (a shader-model-2
	# card) on a fresh profile; the load never clamps it (the engine's
	# renderer/texture_compression.h).
	assert_eq(GameWorld.texcompression_level_fresh_profile(), 1)
	var options := PlayerOptions.new()
	var state := options.current()
	state.texcompression_level = 2
	options.update(state)
	assert_eq(PlayerOptions.new().current().texcompression_level, 2)
	assert_eq(state.copy().texcompression_level, 2, "the snapshot copy carries the word")
	_write_cfg(["texcompression_level = 7"])
	assert_eq(
		PlayerOptions.new().current().texcompression_level,
		7,
		"the load keeps a word past the rows as written"
	)


func test_update_maps_each_audio_option_to_its_runtime_buses() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	state.sound_fx_volume = 32
	state.dialog_volume = 96
	state.music_volume = 160
	options.update(state)

	_assert_bus_volume(&"SFX", 32)
	_assert_bus_volume(&"Ambient", 32)
	_assert_bus_volume(&"Voice", 96)
	_assert_bus_volume(&"Music", 160)


func _assert_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	assert_gte(bus, 0, "%s bus exists" % bus_name)
	if bus >= 0:
		assert_almost_eq(
			AudioServer.get_bus_volume_db(bus),
			float(SoundSelector.volume_db_from_255(volume)),
			0.001,
			"%s receives the 0..255 option through the shared conversion" % bus_name
		)
