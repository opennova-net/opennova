extends GutTest

const PROBE_PATHS: Array[String] = [
	"res://tests/bend_capture_probe.gd",
	"res://tests/body_holds_probe.gd",
	"res://tests/body_reload_probe.gd",
	"res://tests/foliage_spawn_capture_probe.gd",
	"res://tests/fp_clean_probe.gd",
	"res://tests/fp_impact_probe.gd",
	"res://tests/lean_tp_probe.gd",
	"res://tests/oned_keys_probe.gd",
	"res://tests/pose_replay_probe.gd",
	"res://tests/render_align_probe.gd",
	"res://tests/terrain_seam_probe.gd",
	"res://tests/weapon_round_probe.gd",
]

const REMOVED_PIE_PROBES: Array[String] = [
	"res://tests/destruction_probe.gd",
	"res://tests/destruction_probe.tscn",
	"res://tests/perf_fire_probe.gd",
	"res://tests/perf_fire_probe.tscn",
]
const SCREENSHOT_CAPTURE_PATH := "res://modtools/tools/screenshot_capture.gd"


func test_rendered_probes_compile_without_editor_play_dependencies() -> void:
	for path in PROBE_PATHS:
		var source := FileAccess.get_file_as_string(path)
		assert_false(source.is_empty(), "%s should remain readable." % path)
		assert_true(source.contains("StandaloneProbe.boot("),
			"%s should boot the standalone game harness." % path)
		for stale_api in [
			"play_" + "mission(", "stop_" + "play_" + "mission(",
			"play_" + "controller(", "get_active_" + "runtime(",
		]:
			assert_false(source.contains(stale_api),
				"%s must not call removed PIE API %s." % [path, stale_api])
		var script := load(path) as Script
		assert_not_null(script, "%s should compile." % path)


func test_shared_probe_can_parse_an_exact_saved_bms_against_a_packed_runtime_root() -> void:
	var source := FileAccess.get_file_as_string(
		"res://tests/standalone_game_probe.gd")
	assert_true(source.contains("saved_mission_path: String = \"\""))
	assert_true(source.contains('Callable(world, "load_mission_data")'),
		"The two-root parity seam should still load through standalone GameWorld.")
	assert_false(source.contains("play_" + "controller"),
		"The saved-file seam must not recreate editor-owned runtime state.")


func test_obsolete_pie_only_probes_are_removed() -> void:
	for path in REMOVED_PIE_PROBES:
		assert_false(ResourceLoader.exists(path), "%s should be removed." % path)


func test_editor_screenshot_driver_has_no_runtime_capture_leg() -> void:
	var source := FileAccess.get_file_as_string(SCREENSHOT_CAPTURE_PATH)
	assert_false(source.is_empty(), "Editor screenshot driver should remain readable.")
	assert_false(source.contains("mission_" + "play"),
		"Editor screenshots must not recreate an in-process game runtime.")
