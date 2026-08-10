extends GutTest

## The menu backdrop device leg (MenuVideoUnderlay): the witnessed
## expansion-first source pick, the converted-sibling requirement, and the
## STARTUP/strips draw gate (policy pinned engine-side by the menu_video
## ctest; witness: docs/mnu/menu-re.md "The menu backdrop (Bink underlay)").

var _root_dir := ""


func before_each() -> void:
	_root_dir = "user://underlay_test_%d" % randi()
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(_root_dir))


func after_each() -> void:
	_remove_tree(ProjectSettings.globalize_path(_root_dir))


func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for name in dir.get_files():
		DirAccess.remove_absolute(path.path_join(name))
	for name in dir.get_directories():
		_remove_tree(path.path_join(name))
	DirAccess.remove_absolute(path)


func _touch(rel: String) -> void:
	var absolute := ProjectSettings.globalize_path(_root_dir.path_join(rel))
	DirAccess.make_dir_recursive_absolute(absolute.get_base_dir())
	var f := FileAccess.open(absolute, FileAccess.WRITE)
	f.store_string("stub")
	f.close()


func _make_underlay() -> MenuVideoUnderlay:
	var underlay: MenuVideoUnderlay = add_child_autofree(MenuVideoUnderlay.new())
	return underlay


func test_expansion_movie_wins_and_needs_its_own_sibling() -> void:
	_touch("main.bik")
	_touch("main.ogv")
	_touch("expansion/revx02/main.bik")
	_touch("expansion/revx02/main.ogv")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "revx02")
	assert_eq(underlay.get_slot_source(0), "expansion/revx02/main.ogv",
			"the expansion's movie is the witnessed pick")
	assert_eq(underlay.get_active_slot_count(), 1)
	assert_eq(underlay.get_unconverted_count(), 0)


func test_selected_movie_without_sibling_counts_unconverted() -> void:
	# The expansion bik is the witnessed pick; playing the root's converted
	# copy would show the WRONG movie, so the slot stays empty instead.
	_touch("main.bik")
	_touch("main.ogv")
	_touch("expansion/revx02/main.bik")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "revx02")
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_unconverted_count(), 1)


func test_missing_movies_skip_silently() -> void:
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "")
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_unconverted_count(), 0)


func test_screen_gate_follows_the_startup_rule() -> void:
	var underlay := _make_underlay()
	underlay.set_screen("STARTUP")
	assert_true(underlay.is_startup_layout(), "STARTUP is the front page")
	underlay.set_screen("SINGLE_PLAYER")
	assert_false(underlay.is_startup_layout(), "sub-screens take the strips")


func test_stop_clears_slots() -> void:
	_touch("header.bik")
	_touch("header.ogv")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "")
	assert_eq(underlay.get_active_slot_count(), 1)
	underlay.stop()
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_slot_source(1), "")
