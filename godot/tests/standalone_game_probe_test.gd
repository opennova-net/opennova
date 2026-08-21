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

# Self-contained scene probes: operator-run visual scenes with their own
# roots (no standalone-game harness). Inventoried for discoverability; the
# lighter contract below pins readable + compiles + no dead PIE API.
const SELF_CONTAINED_PROBES: Array[String] = [
	"res://tests/effects_visual_probe.gd",
]

const REMOVED_PIE_PROBES: Array[String] = [
	"res://tests/destruction_probe.gd",
	"res://tests/destruction_probe.tscn",
	"res://tests/sun_shadow_catcher_probe.gd",
	"res://tests/sun_shadow_catcher_probe.tscn",
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


func test_self_contained_probes_compile_without_removed_pie_api() -> void:
	for path in SELF_CONTAINED_PROBES:
		var source := FileAccess.get_file_as_string(path)
		assert_false(source.is_empty(), "%s should remain readable." % path)
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
	assert_true(source.contains("game.dismiss_start_mission_splash()"),
		"Rendered probes must leave the player-paced splash through the shell seam.")
	assert_true(source.contains("while game.is_world_loading():"),
			"The shared harness must not return a briefing frame as gameplay.")
	assert_true(source.contains("local_player_profile: Dictionary = {}"),
			"Rendered comparison probes may stage an exact production spawn profile.")
	assert_true(source.contains("game.set_local_player_profile(local_player_profile)"),
			"The optional profile must use MainGame's public pre-spawn seam.")


func test_obsolete_pie_only_probes_are_removed() -> void:
	for path in REMOVED_PIE_PROBES:
		assert_false(ResourceLoader.exists(path), "%s should be removed." % path)


func test_editor_screenshot_driver_has_no_runtime_capture_leg() -> void:
	var source := FileAccess.get_file_as_string(SCREENSHOT_CAPTURE_PATH)
	assert_false(source.is_empty(), "Editor screenshot driver should remain readable.")
	assert_false(source.contains("mission_" + "play"),
		"Editor screenshots must not recreate an in-process game runtime.")
