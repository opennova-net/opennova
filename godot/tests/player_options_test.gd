extends GutTest

## Shared player options: one normalized persisted state feeds both menu
## surfaces and the device/runtime application seams.

const BUS_NAMES := [&"SFX", &"Ambient", &"Voice", &"Music"]

var _saved_config := PackedByteArray()
var _had_config := false
var _saved_bus_volumes: Dictionary = {}


func before_each() -> void:
	_had_config = FileAccess.file_exists(PlayerOptions.CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(PlayerOptions.CONFIG_PATH) \
			if _had_config else PackedByteArray()
	if _had_config:
		DirAccess.remove_absolute(
				ProjectSettings.globalize_path(PlayerOptions.CONFIG_PATH))
	_saved_bus_volumes.clear()
	for bus_name: StringName in BUS_NAMES:
		var bus := AudioServer.get_bus_index(bus_name)
		if bus >= 0:
			_saved_bus_volumes[bus_name] = AudioServer.get_bus_volume_db(bus)


func after_each() -> void:
	if _had_config:
		var file := FileAccess.open(PlayerOptions.CONFIG_PATH, FileAccess.WRITE)
		assert_not_null(file)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(PlayerOptions.CONFIG_PATH):
		DirAccess.remove_absolute(
				ProjectSettings.globalize_path(PlayerOptions.CONFIG_PATH))
	for bus_name: StringName in _saved_bus_volumes:
		var bus := AudioServer.get_bus_index(bus_name)
		if bus >= 0:
			AudioServer.set_bus_volume_db(bus, float(_saved_bus_volumes[bus_name]))


func test_defaults_and_current_snapshot_are_detached() -> void:
	var options := PlayerOptions.new()
	var state := options.current()
	assert_eq(state.sound_fx_volume, 192)
	assert_eq(state.dialog_volume, 192)
	assert_eq(state.music_volume, 192)
	assert_eq(state.mouse_sensitivity, 128)
	assert_false(state.invert_mouse)
	assert_eq(state.crosshair_style, 0)
	assert_eq(state.crosshair_color, 0xFFFFFF,
			"the retail default crosshair colour is white")
	assert_true(state.crosshair_spread, "the retail default spread is on")

	state.sound_fx_volume = 12
	state.crosshair_style = 9
	assert_eq(options.current().sound_fx_volume, 192,
			"editing a snapshot cannot mutate the owner")
	assert_eq(options.current().crosshair_style, 0)


func test_update_clamps_persists_together_and_preserves_other_sections() -> void:
	ConfigStore.write(PlayerOptions.CONFIG_PATH, "resources", "resource_dir",
			"C:/Games/Joint Operations")
	var options := PlayerOptions.new()
	var emitted: Array[PlayerOptions.State] = []
	options.changed.connect(func(state: PlayerOptions.State) -> void:
		emitted.append(state)
	)

	var state := options.current()
	state.sound_fx_volume = -2
	state.dialog_volume = 93
	state.music_volume = 999
	state.mouse_sensitivity = 0
	state.invert_mouse = true
	state.crosshair_style = 99
	state.crosshair_color = 0x1FF8040
	state.crosshair_spread = false
	options.update(state)

	var current := options.current()
	assert_eq(current.sound_fx_volume, 0)
	assert_eq(current.dialog_volume, 93)
	assert_eq(current.music_volume, 255)
	assert_eq(current.mouse_sensitivity, 4)
	assert_true(current.invert_mouse)
	assert_eq(current.crosshair_style, 24)
	assert_eq(current.crosshair_color, 0xFF8040,
			"the colour normalizes to its 24-bit RGB")
	assert_false(current.crosshair_spread)
	assert_eq(emitted.size(), 1, "one atomic update publishes one state")
	assert_eq(emitted[0].crosshair_style, 24)

	var config := ConfigFile.new()
	assert_eq(config.load(PlayerOptions.CONFIG_PATH), OK)
	assert_eq(int(config.get_value("audio", "sound_fx_volume", -1)), 0)
	assert_eq(int(config.get_value("audio", "dialog_volume", -1)), 93)
	assert_eq(int(config.get_value("audio", "music_volume", -1)), 255)
	assert_eq(int(config.get_value("controls", "mouse_sensitivity", -1)), 4)
	assert_true(bool(config.get_value("controls", "invert_mouse", false)))
	assert_eq(int(config.get_value("player", "crosshair_style", -1)), 24)
	assert_eq(int(config.get_value("player", "crosshair_color", -1)), 0xFF8040)
	assert_false(bool(config.get_value("player", "crosshair_spread", true)))
	assert_eq(String(config.get_value("resources", "resource_dir", "")),
			"C:/Games/Joint Operations",
			"the options transaction preserves unrelated runtime settings")


func test_load_normalizes_corrupt_persisted_values() -> void:
	var config := ConfigFile.new()
	config.set_value("audio", "sound_fx_volume", 260)
	config.set_value("audio", "dialog_volume", -1)
	config.set_value("audio", "music_volume", 71)
	config.set_value("controls", "mouse_sensitivity", 900)
	config.set_value("controls", "invert_mouse", true)
	config.set_value("player", "crosshair_style", -7)
	config.set_value("player", "crosshair_color", 0x7F123456)
	assert_eq(config.save(PlayerOptions.CONFIG_PATH), OK)

	var state := PlayerOptions.new().current()
	assert_eq(state.sound_fx_volume, 255)
	assert_eq(state.dialog_volume, 0)
	assert_eq(state.music_volume, 71)
	assert_eq(state.mouse_sensitivity, 511)
	assert_true(state.invert_mouse)
	assert_eq(state.crosshair_style, 0)
	assert_eq(state.crosshair_color, 0x123456,
			"an out-of-range colour masks down to its RGB")


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


func test_apply_configures_a_new_simulation_mouse_state() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var options := PlayerOptions.new()
	var state := options.current()
	state.mouse_sensitivity = 511
	state.invert_mouse = true
	options.update(state)
	options.apply(sim)
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	assert_gt(sim.get_local_player_pitch_deg(), 8.0,
			"the persisted max sensitivity and inverted Y reach the new sim")


func _assert_bus_volume(bus_name: StringName, volume: int) -> void:
	var bus := AudioServer.get_bus_index(bus_name)
	assert_gte(bus, 0, "%s bus exists" % bus_name)
	if bus >= 0:
		assert_almost_eq(AudioServer.get_bus_volume_db(bus),
				float(SoundSelector.volume_db_from_255(volume)), 0.001,
				"%s receives the 0..255 option through the shared conversion" \
						% bus_name)
