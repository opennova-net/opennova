extends GutTest

const PROBE_PATH := "res://tests/foliage_spawn_capture_probe.gd"
const ProbeScript := preload(PROBE_PATH)


func test_spawn_capture_uses_real_player_and_public_foliage_api() -> void:
	var source := FileAccess.get_file_as_string(PROBE_PATH)
	assert_false(source.is_empty(), "Spawn capture probe source should be readable.")
	assert_true(source.contains('const DEFAULT_MISSION := "00TRe.bms"'))
	assert_true(source.contains("StandaloneProbe.boot("),
		"Probe should boot the real standalone game shell.")
	assert_true(source.contains("var world: GameWorld = _world"),
		"Probe should bind the standalone shell's GameWorld.")
	assert_true(source.contains("world.get_sim().has_local_player()"),
		"Probe should fail unless the mission spawned a local player.")
	assert_true(source.contains("world.get_sim().get_local_player_position()"),
		"Probe should record the real local-player anchor.")
	assert_true(source.contains("world.set_foliage_hidden(true)"),
		"Foliage-hidden A/B should use GameWorld's public API.")
	assert_true(source.contains('mission_name.get_file().get_basename()'),
		"Capture filenames should identify the mission under comparison.")
	assert_true(source.contains('_capture_stem + "_spawn_default.png"'),
		"Each mission should keep its own exact-spawn capture.")
	assert_false(source.contains("ResourceDirSettings.set_resource_dir("),
		"The probe must not overwrite the user's persisted resource root.")
	assert_false(source.contains("_mission_workspace"),
		"The capture must not recreate the removed embedded PIE path.")
	for forbidden in [
		"_find_painted", "get_foliage_index_world", "camera.global_position =",
		"camera.global_transform =", "camera.look_at(", "Input.parse_input_event",
	]:
		assert_false(source.contains(forbidden),
			"Probe must not search painted cells, teleport the camera, or synthesize input: %s" % forbidden)


func test_spawn_capture_requires_requested_runtime_expansion_and_archive_winners() -> void:
	assert_eq(ProbeScript.runtime_mount_validation_error(
		"revx02", "revx02", true), "")
	assert_ne(ProbeScript.runtime_mount_validation_error(
		"revx02", "", true), "",
		"A silent expansion-to-base fallback must make the capture fail.")
	assert_ne(ProbeScript.runtime_mount_validation_error(
		"revx02", "revx02", false), "",
		"A loose/editor root must not masquerade as the packed comparison mount.")

	var winning_entries := [
		{
			"logical_name": "00TRa.bms",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/localres.pff",
		},
		{
			"logical_name": "00TRa.trn",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/expansion/revx02/RevX02.pff",
		},
		{
			"logical_name": "00TRa.env",
			"source_type": "pff",
			"archive_path": "C:/Game/JO/expansion/revx02/RevX02.pff",
		},
	]
	assert_eq(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "")
	winning_entries[0]["archive_path"] = ""
	assert_ne(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "",
		"The compared mission itself must name its packed winning archive.")
	winning_entries[0]["archive_path"] = "C:/Game/JO/localres.pff"
	winning_entries[2]["source_type"] = "loose"
	assert_ne(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries), "",
		"Every reported comparison input must name its packed winning source.")
	winning_entries[2]["source_type"] = "pff"
	winning_entries[0] = {
		"logical_name": "00TRa.bms",
		"source_type": "loose",
		"source_path": "C:/Authoring/00TRa.bms",
		"archive_path": "",
	}
	assert_eq(ProbeScript.runtime_source_validation_error(
		"revx02", "00TRa.bms", winning_entries, true), "",
		"A saved loose mission may drive a standalone packed-runtime capture.")

	var source := FileAccess.get_file_as_string(PROBE_PATH)
	assert_true(source.contains('OS.get_environment("NOVA_MISSION_RESOURCE_DIR")'),
		"The loose authoring root must be configured separately.")
	assert_true(source.contains('OS.get_environment("NOVA_RUNTIME_RESOURCE_DIR")'),
		"The packed runtime root must be configured separately.")
	assert_true(source.contains('OS.get_environment("NOVA_EXPANSION")'),
		"The probe must receive an explicit expansion request.")
	assert_true(source.contains(
		"self, runtime_resource_dir, mission_name, requested_expansion, mission_path"),
		"The standalone shell must mount packed dependencies and parse the exact saved BMS.")
	assert_true(source.contains("list_file_entries()"),
		"The probe must report the VFS's winning source entries.")


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
		"Intent-only 00TRe output is not enough when the reimpl has no detail instances.")
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
	assert_true(source.contains("get_window().size = CAPTURE_VIEWPORT_SIZE"),
		"The native game window should render at the fixed comparison size.")
	assert_false(source.contains("PlayViewportContainer"),
		"The capture must not depend on an editor dock viewport.")
