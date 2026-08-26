extends GutTest

# Recently used resource directories live in ONED's own config; the game runtime
# keeps separate state. Snapshot and restore ONED's config around each test, and use temp
# directories outside user:// so resource-root validation accepts them.

const STATE_CONFIG_PATH := OnedSettings.CONFIG_PATH
const TEST_ROOT := "opennova_recent_dirs_test"

var _saved_state_config := PackedByteArray()
var _had_state_config := false
var _base := ""


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()
	# Start from a clean config so leftover state never leaks between tests.
	if _had_state_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	_base = OS.get_cache_dir().path_join(TEST_ROOT).path_join("run_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_base)


func after_each() -> void:
	if _had_state_config:
		var f := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if f != null:
			f.store_buffer(_saved_state_config)
			f.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


# A fresh, real directory on disk under the test base (so is_valid_root passes).
func _make_dir(name: String) -> String:
	var path := _base.path_join(name)
	DirAccess.make_dir_recursive_absolute(path)
	return path


func test_apply_records_dir_at_front() -> void:
	var a := _make_dir("alpha")
	OnedSettings.set_resource_dir(a)
	var recent := OnedSettings.get_recent_dirs()
	assert_eq(recent.size(), 1, "A valid applied dir is recorded.")
	assert_eq(recent[0], a, "It sits at the front of the recents list.")


func test_reapply_dedupes_and_bumps_to_front() -> void:
	var a := _make_dir("alpha")
	var b := _make_dir("bravo")
	OnedSettings.set_resource_dir(a)
	OnedSettings.set_resource_dir(b)
	OnedSettings.set_resource_dir(a)
	var recent := OnedSettings.get_recent_dirs()
	assert_eq(Array(recent), [a, b], "Re-applying A moves it to the front with no duplicate.")


func test_list_is_capped_at_limit() -> void:
	var total := OnedSettings.RECENT_LIMIT + 2
	var dirs: Array[String] = []
	for i in total:
		var d := _make_dir("dir_%d" % i)
		dirs.append(d)
		OnedSettings.set_resource_dir(d)
	var recent := OnedSettings.get_recent_dirs()
	assert_eq(recent.size(), OnedSettings.RECENT_LIMIT, "Recents are capped at RECENT_LIMIT.")
	assert_eq(recent[0], dirs[total - 1], "Most recent is first.")
	assert_false(recent.has(dirs[0]), "The oldest entries fall off the end.")


func test_clearing_active_dir_leaves_recents() -> void:
	var a := _make_dir("alpha")
	OnedSettings.set_resource_dir(a)
	OnedSettings.set_resource_dir("")
	assert_eq(OnedSettings.get_resource_dir(), "", "Active dir is cleared.")
	assert_eq(Array(OnedSettings.get_recent_dirs()), [a], "Clearing the active dir does not touch recents.")


func test_invalid_paths_are_not_recorded() -> void:
	OnedSettings.set_resource_dir(_base.path_join("does_not_exist"))
	OnedSettings.set_resource_dir(OS.get_user_data_dir().path_join("leaked"))
	assert_eq(OnedSettings.get_recent_dirs().size(), 0, "Non-existent and user-data paths are rejected.")


func test_stale_dirs_drop_on_read() -> void:
	var a := _make_dir("alpha")
	var b := _make_dir("bravo")
	OnedSettings.set_resource_dir(a)
	OnedSettings.set_resource_dir(b)
	DirAccess.remove_absolute(a)
	var recent := OnedSettings.get_recent_dirs()
	assert_eq(Array(recent), [b], "A deleted directory is dropped from the read list.")


func test_trailing_slash_dedupes() -> void:
	var a := _make_dir("alpha")
	OnedSettings.set_resource_dir(a)
	OnedSettings.set_resource_dir(a + "/")
	assert_eq(OnedSettings.get_recent_dirs().size(), 1, "A trailing slash is the same directory.")


func test_clear_recent_dirs_empties_list() -> void:
	OnedSettings.set_resource_dir(_make_dir("alpha"))
	OnedSettings.set_resource_dir(_make_dir("bravo"))
	OnedSettings.clear_recent_dirs()
	assert_eq(OnedSettings.get_recent_dirs().size(), 0, "clear_recent_dirs forgets every entry.")


func test_add_recent_dir_records_without_changing_active() -> void:
	var a := _make_dir("alpha")
	OnedSettings.add_recent_dir(a)
	assert_eq(Array(OnedSettings.get_recent_dirs()), [a], "add_recent_dir records the directory.")
	assert_eq(OnedSettings.get_resource_dir(), "", "add_recent_dir does not change the active dir.")


func test_recording_preserves_other_sections() -> void:
	var config := ConfigFile.new()
	config.set_value("layout", "left_split_offset", 123)
	config.save(STATE_CONFIG_PATH)
	OnedSettings.set_resource_dir(_make_dir("alpha"))
	var reloaded := ConfigFile.new()
	assert_eq(reloaded.load(STATE_CONFIG_PATH), OK, "Config reloads.")
	assert_eq(int(reloaded.get_value("layout", "left_split_offset", -1)), 123, "Unrelated sections survive a recents write.")
