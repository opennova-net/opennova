extends GutTest

const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var _test_root := ""


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = (
			FileAccess.get_file_as_bytes(STATE_CONFIG_PATH)
			if _had_state_config else PackedByteArray())
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	_test_root = OS.get_cache_dir().path_join(
			"opennova_perf_mount_guard_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_test_root)


func after_each() -> void:
	if _had_state_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_state_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	DirAccess.remove_absolute(_test_root)


func test_restore_reinstates_empty_mount_expansion_and_recents_exactly() -> void:
	var original := ConfigFile.new()
	original.set_value("resources", "resource_dir", "")
	original.set_value("resources", "expansion", "jox01")
	original.set_value("resources", "recent_dirs", PackedStringArray(["C:/old/a", "C:/old/b"]))
	original.set_value("layout", "probe_sentinel", 17)
	assert_eq(original.save(STATE_CONFIG_PATH), OK)
	var expected := FileAccess.get_file_as_bytes(STATE_CONFIG_PATH)

	var guard = MountGuard.new()
	assert_eq(guard.capture(), OK)
	ResourceDirSettings.set_resource_dir(_test_root)
	ResourceDirSettings.set_expansion("")
	assert_eq(guard.restore(), OK)
	assert_eq(FileAccess.get_file_as_bytes(STATE_CONFIG_PATH), expected,
			"restore puts back the exact pre-probe config, including empty mount and recents")


func test_guard_uses_the_resource_settings_config_path() -> void:
	assert_eq(MountGuard.STATE_CONFIG_PATH, ResourceDirSettings.CONFIG_PATH)


func test_restore_removes_config_created_from_an_unset_baseline() -> void:
	assert_false(FileAccess.file_exists(STATE_CONFIG_PATH))
	var guard = MountGuard.new()
	assert_eq(guard.capture(), OK)
	ResourceDirSettings.set_resource_dir(_test_root)
	ResourceDirSettings.set_expansion("temporary")
	assert_true(FileAccess.file_exists(STATE_CONFIG_PATH))
	assert_eq(guard.restore(), OK)
	assert_false(FileAccess.file_exists(STATE_CONFIG_PATH),
			"an originally absent config remains absent after the probe")
