extends GutTest

const PROBE_PATH := "res://tests/foliage_spawn_capture_probe.gd"
const ProbeScript := preload(PROBE_PATH)


func test_spawn_capture_uses_real_player_and_public_foliage_api() -> void:
	var source := FileAccess.get_file_as_string(PROBE_PATH)
	assert_false(source.is_empty(), "Spawn capture probe source should be readable.")
	assert_true(source.contains('const DEFAULT_MISSION := "00TRe.bms"'))
	assert_true(source.contains("_mission_workspace.play_mission()"),
		"Probe should boot the real play-in-editor GameWorld.")
	assert_true(source.contains("_mission_workspace.play_controller()"),
		"Probe should bind the workspace's public live-play seam.")
	assert_true(source.contains("world.has_local_player()"),
		"Probe should fail unless the mission spawned a local player.")
	assert_true(source.contains("world.local_player_position()"),
		"Probe should record the real local-player anchor.")
	assert_true(source.contains("world.set_foliage_hidden(true)"),
		"Foliage-hidden A/B should use GameWorld's public API.")
	assert_true(source.contains('mission_name.get_file().get_basename()'),
		"Capture filenames should identify the mission under comparison.")
	assert_true(source.contains('_capture_stem + "_spawn_default.png"'),
		"Each mission should keep its own exact-spawn capture.")
	assert_true(source.contains("workstation.set_resource_root_dir(resource_dir, false)"),
		"The probe should mount its resource root transiently.")
	assert_false(source.contains("ResourceDirSettings.set_resource_dir(resource_dir)"),
		"The probe must not overwrite the user's persisted resource root.")
	for forbidden in [
		"_find_painted", "get_foliage_index_world", "camera.global_position =",
		"camera.global_transform =", "camera.look_at(", "Input.parse_input_event",
	]:
		assert_false(source.contains(forbidden),
			"Probe must not search painted cells, teleport the camera, or synthesize input: %s" % forbidden)


func test_spawn_capture_requires_runtime_foliage_for_00tre_only() -> void:
	var empty_stats := {
		"runtime_detail_intents": 0,
		"detail_high_instances": 0,
		"detail_low_instances": 0,
	}
	assert_false(ProbeScript.runtime_foliage_validation_error(
		"00TRe.bms", empty_stats, 0).is_empty(),
		"The foliage oracle must not PASS 00TRe when dispatch produced nothing.")
	assert_false(ProbeScript.runtime_foliage_validation_error(
		"00TRe.bms", {"runtime_detail_intents": 1}, 1).is_empty(),
		"Intent-only 00TRe output is not enough when the host has no detail instances.")
	assert_eq(ProbeScript.runtime_foliage_validation_error("00TRe.bms", {
		"runtime_detail_intents": 1,
		"detail_high_instances": 1,
		"detail_low_instances": 0,
	}, 1), "")
	assert_eq(ProbeScript.runtime_foliage_validation_error(
		"00TRa.bms", empty_stats, 0), "",
		"00TRa's exact spawn is an intentional zero-visible-foliage control.")


func test_spawn_capture_settles_visibility_changes_before_each_image() -> void:
	var source := FileAccess.get_file_as_string(PROBE_PATH)
	assert_false(source.is_empty(), "Spawn capture probe source should be readable.")
	assert_true(source.contains("const VISIBILITY_SETTLE_FRAMES := 3"),
		"The A/B probe should give renderer visibility changes time to reach the viewport.")
	assert_true(source.contains("world.set_foliage_hidden(false)\n\tawait _settle(VISIBILITY_SETTLE_FRAMES)"),
		"The visible capture must settle after enabling foliage.")
	assert_true(source.contains("world.set_foliage_hidden(true)\n\tawait _settle(VISIBILITY_SETTLE_FRAMES)"),
		"The hidden capture must settle after disabling foliage.")


func test_spawn_capture_uses_standalone_game_aspect_not_editor_dock_aspect() -> void:
	var source := FileAccess.get_file_as_string(PROBE_PATH)
	assert_false(source.is_empty(), "Spawn capture probe source should be readable.")
	assert_true(source.contains("const CAPTURE_VIEWPORT_SIZE := Vector2i(1600, 900)"),
		"Retail comparisons should use the standalone game's 16:9 viewport.")
	assert_true(source.contains("play_container.stretch = false"),
		"The editor dock must stop overriding the comparison viewport size.")
	assert_true(source.contains("play_viewport.size = CAPTURE_VIEWPORT_SIZE"),
		"The clean played world should render at the fixed comparison size.")
