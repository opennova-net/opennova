extends GutTest

# Recently used resource directories, recorded in the shared editor/runtime config
# by NovaResourceDirSettings. Mirrors terrain_editor_state_test.gd: snapshot and
# restore user://terrain_editor_state.cfg around each test, and use temp dirs under
# OS.get_cache_dir() (outside the user-data dir, so is_valid_root accepts them).

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
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
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


# A fresh, real directory on disk under the test base (so is_valid_root passes).
func _make_dir(name: String) -> String:
	var path := _base.path_join(name)
	DirAccess.make_dir_recursive_absolute(path)
	return path


func test_apply_records_dir_at_front() -> void:
	var a := _make_dir("alpha")
	NovaResourceDirSettings.set_resource_dir(a)
	var recent := NovaResourceDirSettings.get_recent_dirs()
	assert_eq(recent.size(), 1, "A valid applied dir is recorded.")
	assert_eq(recent[0], a, "It sits at the front of the recents list.")


func test_reapply_dedupes_and_bumps_to_front() -> void:
	var a := _make_dir("alpha")
	var b := _make_dir("bravo")
	NovaResourceDirSettings.set_resource_dir(a)
	NovaResourceDirSettings.set_resource_dir(b)
	NovaResourceDirSettings.set_resource_dir(a)
	var recent := NovaResourceDirSettings.get_recent_dirs()
	assert_eq(Array(recent), [a, b], "Re-applying A moves it to the front with no duplicate.")


func test_list_is_capped_at_limit() -> void:
	var total := NovaResourceDirSettings.RECENT_LIMIT + 2
	var dirs: Array[String] = []
	for i in total:
		var d := _make_dir("dir_%d" % i)
		dirs.append(d)
		NovaResourceDirSettings.set_resource_dir(d)
	var recent := NovaResourceDirSettings.get_recent_dirs()
	assert_eq(recent.size(), NovaResourceDirSettings.RECENT_LIMIT, "Recents are capped at RECENT_LIMIT.")
	assert_eq(recent[0], dirs[total - 1], "Most recent is first.")
	assert_false(recent.has(dirs[0]), "The oldest entries fall off the end.")


func test_clearing_active_dir_leaves_recents() -> void:
	var a := _make_dir("alpha")
	NovaResourceDirSettings.set_resource_dir(a)
	NovaResourceDirSettings.set_resource_dir("")
	assert_eq(NovaResourceDirSettings.get_resource_dir(), "", "Active dir is cleared.")
	assert_eq(Array(NovaResourceDirSettings.get_recent_dirs()), [a], "Clearing the active dir does not touch recents.")


func test_invalid_paths_are_not_recorded() -> void:
	NovaResourceDirSettings.set_resource_dir(_base.path_join("does_not_exist"))
	NovaResourceDirSettings.set_resource_dir(OS.get_user_data_dir().path_join("leaked"))
	assert_eq(NovaResourceDirSettings.get_recent_dirs().size(), 0, "Non-existent and user-data paths are rejected.")


func test_stale_dirs_drop_on_read() -> void:
	var a := _make_dir("alpha")
	var b := _make_dir("bravo")
	NovaResourceDirSettings.set_resource_dir(a)
	NovaResourceDirSettings.set_resource_dir(b)
	DirAccess.remove_absolute(a)
	var recent := NovaResourceDirSettings.get_recent_dirs()
	assert_eq(Array(recent), [b], "A deleted directory is dropped from the read list.")


func test_trailing_slash_dedupes() -> void:
	var a := _make_dir("alpha")
	NovaResourceDirSettings.set_resource_dir(a)
	NovaResourceDirSettings.set_resource_dir(a + "/")
	assert_eq(NovaResourceDirSettings.get_recent_dirs().size(), 1, "A trailing slash is the same directory.")


func test_clear_recent_dirs_empties_list() -> void:
	NovaResourceDirSettings.set_resource_dir(_make_dir("alpha"))
	NovaResourceDirSettings.set_resource_dir(_make_dir("bravo"))
	NovaResourceDirSettings.clear_recent_dirs()
	assert_eq(NovaResourceDirSettings.get_recent_dirs().size(), 0, "clear_recent_dirs forgets every entry.")


func test_add_recent_dir_records_without_changing_active() -> void:
	var a := _make_dir("alpha")
	NovaResourceDirSettings.add_recent_dir(a)
	assert_eq(Array(NovaResourceDirSettings.get_recent_dirs()), [a], "add_recent_dir records the directory.")
	assert_eq(NovaResourceDirSettings.get_resource_dir(), "", "add_recent_dir does not change the active dir.")


func test_recording_preserves_other_sections() -> void:
	var config := ConfigFile.new()
	config.set_value("layout", "left_split_offset", 123)
	config.save(STATE_CONFIG_PATH)
	NovaResourceDirSettings.set_resource_dir(_make_dir("alpha"))
	var reloaded := ConfigFile.new()
	assert_eq(reloaded.load(STATE_CONFIG_PATH), OK, "Config reloads.")
	assert_eq(int(reloaded.get_value("layout", "left_split_offset", -1)), 123, "Unrelated sections survive a recents write.")


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
