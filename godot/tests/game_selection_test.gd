extends GutTest

# Game selection that drives SCR decode keying: the persisted game code
# (NovaResourceDirSettings) and the /game launch-flag fallback (NovaLaunchFlags).
# Snapshots user://terrain_editor_state.cfg around each test like recent_resource_dirs_test.gd.

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func after_each() -> void:
	if _had_state_config:
		var f := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if f != null:
			f.store_buffer(_saved_state_config)
			f.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func test_game_defaults_to_jo_when_unset() -> void:
	assert_eq(NovaResourceDirSettings.get_game(), "jo", "Absent game setting defaults to JO.")


func test_game_round_trip_and_normalizes() -> void:
	NovaResourceDirSettings.set_game("JODemo")
	assert_eq(NovaResourceDirSettings.get_game(), "jodemo", "Game code persists, lowercased.")


func test_blank_game_reads_as_jo() -> void:
	NovaResourceDirSettings.set_game("   ")
	assert_eq(NovaResourceDirSettings.get_game(), "jo", "A blank stored code reads back as JO.")


func test_set_game_preserves_other_sections() -> void:
	var config := ConfigFile.new()
	config.set_value("layout", "left_split_offset", 123)
	config.save(STATE_CONFIG_PATH)
	NovaResourceDirSettings.set_game("jodemo")
	var reloaded := ConfigFile.new()
	assert_eq(reloaded.load(STATE_CONFIG_PATH), OK, "Config reloads.")
	assert_eq(int(reloaded.get_value("layout", "left_split_offset", -1)), 123, "Unrelated sections survive a game write.")


# No /game flag is present in the test harness args, so game() returns the (normalized)
# fallback. This covers the fallback + default path that wires the persisted setting in.
func test_launch_flag_falls_back_to_setting_then_jo() -> void:
	assert_eq(NovaLaunchFlags.game("jodemo"), "jodemo", "With no flag, the fallback (persisted setting) wins.")
	assert_eq(NovaLaunchFlags.game(""), "jo", "An empty fallback resolves to JO.")
	assert_eq(NovaLaunchFlags.game(), "jo", "The default fallback is JO.")
	assert_eq(NovaLaunchFlags.game("JODEMO"), "jodemo", "The fallback is lowercased.")


func test_crosshair_style_defaults_round_trips_and_clamps() -> void:
	assert_eq(NovaResourceDirSettings.get_crosshair_style(), 0, "Absent style uses cross01.tga.")
	NovaResourceDirSettings.set_crosshair_style(17)
	assert_eq(NovaResourceDirSettings.get_crosshair_style(), 17, "Crosshair style persists.")
	NovaResourceDirSettings.set_crosshair_style(99)
	assert_eq(NovaResourceDirSettings.get_crosshair_style(), 24, "Style is capped at cross25.tga.")
	NovaResourceDirSettings.set_crosshair_style(-4)
	assert_eq(NovaResourceDirSettings.get_crosshair_style(), 0, "Negative styles clamp to cross01.tga.")


func test_set_crosshair_style_preserves_other_sections() -> void:
	var config := ConfigFile.new()
	config.set_value("layout", "left_split_offset", 123)
	config.save(STATE_CONFIG_PATH)
	NovaResourceDirSettings.set_crosshair_style(8)
	var reloaded := ConfigFile.new()
	assert_eq(reloaded.load(STATE_CONFIG_PATH), OK, "Config reloads.")
	assert_eq(int(reloaded.get_value("layout", "left_split_offset", -1)), 123,
		"Unrelated sections survive a crosshair write.")
