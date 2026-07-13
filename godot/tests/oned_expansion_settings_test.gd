extends GutTest

const SettingsPanel := preload("res://modtools/editor/shell/settings_panel.gd")
const OnedSettings := preload("res://modtools/editor/oned_settings.gd")
const CONFIG_PATH := "user://oned_settings.cfg"
const TEST_ROOT := "opennova_oned_expansion_settings_test"

var _saved_config: Variant = null
var _base := ""


func before_each() -> void:
	_saved_config = FileAccess.get_file_as_bytes(CONFIG_PATH) if FileAccess.file_exists(CONFIG_PATH) else null
	if FileAccess.file_exists(CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	_base = OS.get_cache_dir().path_join(TEST_ROOT).path_join("run_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_base)


func after_each() -> void:
	if _saved_config is PackedByteArray:
		var file := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func test_loose_expansions_populate_and_restore_oned_selection() -> void:
	var root := _make_root("authoring", PackedStringArray(["jox02", "jox01"]))
	OnedSettings.set_expansion("jox02")
	var fixture := _make_panel(root)
	var row: HBoxContainer = fixture.row
	var option: OptionButton = fixture.option

	assert_true(row.visible)
	assert_eq(option.item_count, 3)
	assert_eq(option.get_item_metadata(0), "", "Base game is always the first choice.")
	assert_eq(option.get_item_metadata(1), "jox01")
	assert_eq(option.get_item_metadata(2), "jox02")
	assert_eq(option.get_item_metadata(option.selected), "jox02")


func test_expansion_choice_persists_and_base_clears_it() -> void:
	var fixture := _make_panel(_make_root("authoring", PackedStringArray(["jox01"])))
	var option: OptionButton = fixture.option

	option.select(1)
	option.item_selected.emit(1)
	assert_eq(OnedSettings.get_expansion(), "jox01")
	option.select(0)
	option.item_selected.emit(0)
	assert_eq(OnedSettings.get_expansion(), "")


func test_root_change_clears_a_stale_expansion_and_hides_empty_row() -> void:
	OnedSettings.set_expansion("missing")
	var fixture := _make_panel(_make_root("base_only", PackedStringArray()))
	assert_false((fixture.row as HBoxContainer).visible)
	assert_eq(OnedSettings.get_expansion(), "")
	assert_eq((fixture.option as OptionButton).get_item_metadata(0), "")


func _make_panel(root: String) -> Dictionary:
	var library := EditorResourceLibrary.new()
	autofree(library)
	assert_eq(library.set_root_dir(root, true, false).err, OK)
	var panel = SettingsPanel.new()
	autofree(panel)
	var row := HBoxContainer.new()
	add_child_autofree(row)
	var option := OptionButton.new()
	row.add_child(option)
	panel.setup(
		library,
		func(): return null,
		func(_path: String, _persist: bool, _scan: bool): return OK,
		func(_text: String, _duration: float): pass,
		func(_title: String, _on_pick: Callable, _current_dir: String): pass,
		func() -> String: return root,
		func(_dir: String): pass,
		func(): return null,
		func(_active: bool): pass
	)
	panel.bind_nodes(
		null, null, null, null, null, row, option, null,
		null, null, null, null, null, null
	)
	panel.wire()
	return {"panel": panel, "row": row, "option": option}


func _make_root(name: String, expansions: PackedStringArray) -> String:
	var root := _base.path_join(name)
	DirAccess.make_dir_recursive_absolute(root)
	for expansion in expansions:
		DirAccess.make_dir_recursive_absolute(root.path_join("expansion").path_join(expansion))
	return root


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
