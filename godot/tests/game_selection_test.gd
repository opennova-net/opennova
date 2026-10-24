extends GutTest

# Product selection is process-local. A stale saved game cannot select retail
# content or its updater on a later launch.

const STATE_CONFIG_PATH := "user://opennova.cfg"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


func before_each() -> void:
	LaunchFlags.set_args_override(PackedStringArray([]))
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func after_each() -> void:
	LaunchFlags.clear_args_override()
	if _had_state_config:
		var f := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if f != null:
			f.store_buffer(_saved_state_config)
			f.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func test_product_defaults_to_opennova_using_jo_compatibility() -> void:
	assert_eq(LaunchFlags.product(), "opennova")
	assert_eq(LaunchFlags.game(), "jo")


func test_explicit_retail_game_selects_its_compatibility_profile() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["/game", "DFX2"]))
	assert_eq(LaunchFlags.product(), "dfx2")
	assert_eq(LaunchFlags.game(), "dfx2")


func test_saved_game_is_ignored_by_the_next_launch() -> void:
	ConfigStore.write(STATE_CONFIG_PATH, "resources", "game", "dfx2")
	assert_eq(LaunchFlags.product(), "opennova")
	assert_eq(LaunchFlags.game(), "jo")


func test_format_callers_can_request_an_explicit_profile_fallback() -> void:
	assert_eq(LaunchFlags.game("jodemo"), "jodemo")
	assert_eq(LaunchFlags.game(""), "jo")
	assert_eq(LaunchFlags.game("JODEMO"), "jodemo")
