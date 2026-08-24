extends GutTest

# A shipped download works with zero setup: the editor defaults to the assets/ bundled
# beside its exe (the zip's game sources), and Export Game repacks the shipped game from
# them. The runtime half of the same lifecycle (opennova.exe default-mounting its own
# dir) is covered in game/main_game_lifecycle_test.gd.


func _fresh_dir(label: String) -> String:
	var dir := OS.get_cache_dir().path_join("shipped_%s_%d" % [label, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


func test_bundled_assets_dir_requires_an_assets_sibling() -> void:
	var base := _fresh_dir("base")
	assert_eq(EditorResourceLibrary.bundled_assets_dir(base), "",
			"no assets/ beside the exe -> no default (a dev run)")
	DirAccess.make_dir_recursive_absolute(base.path_join("assets"))
	assert_eq(EditorResourceLibrary.bundled_assets_dir(base), base.path_join("assets"),
			"a shipped layout's assets/ sibling is the default project")


func test_editor_defaults_to_bundled_assets_and_never_persists() -> void:
	var saved := OnedSettings.get_resource_dir()
	OnedSettings.set_resource_dir("")
	var base := _fresh_dir("exe")
	DirAccess.make_dir_recursive_absolute(base.path_join("assets"))

	var library := EditorResourceLibrary.new()
	library.bundled_probe_override = base
	var state: Dictionary = library.load_state()
	assert_eq(String(state["root_dir"]), base.path_join("assets"),
			"an unconfigured editor mounts the bundled sources")
	assert_eq(OnedSettings.get_resource_dir(), "",
			"the default is never written to the config")

	var configured := _fresh_dir("configured")
	OnedSettings.set_resource_dir(configured)
	assert_eq(String(library.load_state()["root_dir"]), configured,
			"an explicit configured dir wins over the bundle")
	OnedSettings.set_resource_dir(saved)


func test_settings_panel_export_game_packs_into_the_chosen_dir() -> void:
	var src := _fresh_dir("src")
	var f := FileAccess.open(src.path_join("items.def"), FileAccess.WRITE)
	f.store_string("defs")
	f.close()
	var library := EditorResourceLibrary.new()
	library.set_root_dir(src, false, false)

	var statuses: Array = []
	var panel := ShellSettingsPanel.new()
	panel.setup(
			library,
			func() -> EditorWorkspace: return null,
			func(_path: String, _persist: bool, _scan: bool) -> Error: return OK,
			func(text: String, _duration: float = 0.0) -> void: statuses.append(text),
			Callable(),
			func() -> String: return src,
			Callable(),
			func() -> Node: return null,
			func(_active: bool) -> void: pass)

	var game_dir := _fresh_dir("game")
	panel.export_game_to(game_dir)
	assert_true(FileAccess.file_exists(game_dir.path_join("localres.pff")),
			"the action packs through EditorGamePacker.export_game into the chosen dir")
	assert_true(statuses.size() > 0 and String(statuses[-1]).begins_with("Exported"),
			"and reports the result: %s" % str(statuses))
