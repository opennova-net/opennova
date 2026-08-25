extends GutTest

const SCENE := preload("res://modtools/oned_main.tscn")
const CONFIG_PATH := OnedSettings.CONFIG_PATH

var _saved_config := PackedByteArray()
var _had_config := false


class StopFailureSession:
	extends GameRunSession

	var stop_calls := 0

	func is_running() -> bool:
		return true

	func stop() -> bool:
		stop_calls += 1
		status_changed.emit("Could not stop the running process.", &"error")
		return false

	func shutdown() -> bool:
		return true


func before_each() -> void:
	_had_config = FileAccess.file_exists(CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(CONFIG_PATH) if _had_config else PackedByteArray()
	if _had_config:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))


func after_each() -> void:
	if _had_config:
		var file := FileAccess.open(CONFIG_PATH, FileAccess.WRITE)
		file.store_buffer(_saved_config)
		file.close()
	elif FileAccess.file_exists(CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))


func _app() -> OnedApp:
	var app := SCENE.instantiate() as OnedApp
	add_child_autofree(app)
	return app


func test_oned_is_one_compact_settings_and_run_surface() -> void:
	var app := _app()
	assert_eq(app.get_window().title, "ONED")
	assert_not_null(app.get_run_session())
	assert_not_null(app.get_node("%ResourceDirEdit"))
	assert_not_null(app.get_node("%GameCodeEdit"))
	assert_not_null(app.get_node("%ExpansionEdit"))
	assert_not_null(app.get_node("%RetailDirEdit"))
	assert_not_null(app.get_node("%RunOpenNovaButton"))
	assert_not_null(app.get_node("%RunRetailButton"))
	assert_not_null(app.get_node("%StopButton"))
	assert_not_null(app.get_node("%StatusLabel"))
	assert_string_contains(
			(app.get_node("%ResourceDirEdit") as LineEdit).placeholder_text,
			"packed",
			"one directory picker accepts loose sources or a packed game")

	var source := FileAccess.get_file_as_string("res://modtools/oned_main.tscn")
	for removed in [
		"Workspace", "MissionEditor", "TerrainEditor", "ObjectEditor",
		"EnvironmentEditor", "McpService", "Export Game", "F6",
	]:
		assert_false(source.contains(removed), "ONED has no %s surface" % removed)


func test_visible_actions_expose_only_f5_f7_and_f8() -> void:
	var app := _app()
	var open_button := app.get_node("%RunOpenNovaButton") as Button
	var retail_button := app.get_node("%RunRetailButton") as Button
	var stop_button := app.get_node("%StopButton") as Button
	assert_string_contains(open_button.text, "F5")
	assert_string_contains(retail_button.text, "F7")
	assert_string_contains(stop_button.text, "F8")
	assert_true(stop_button.disabled, "nothing is managed at boot")


func test_stop_failure_keeps_the_actionable_session_error() -> void:
	var app := _app()
	var session := StopFailureSession.new()
	app.attach_run_session(session)

	app.stop_game()

	assert_eq(session.stop_calls, 1)
	assert_eq(
			(app.get_node("%StatusLabel") as Label).text,
			"Could not stop the running process.")


func test_resource_and_profile_fields_persist_without_a_project_file() -> void:
	var root := ProjectSettings.globalize_path(
			"res://../.godot-target-fixtures/oned_app_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	var app := _app()
	var resource := app.get_node("%ResourceDirEdit") as LineEdit
	var game := app.get_node("%GameCodeEdit") as LineEdit
	var expansion := app.get_node("%ExpansionEdit") as LineEdit

	resource.text = root
	assert_true(ResourceDirSettings.is_valid_root(root), "the explicit test directory is valid")
	assert_true(app.should_persist_resource_dir(), "the typed directory replaces the fallback")
	resource.focus_exited.emit()
	game.text = "JODEMO"
	game.focus_exited.emit()
	expansion.text = " jox01 "
	expansion.focus_exited.emit()

	assert_eq(OnedSettings.get_resource_dir(), root)
	assert_eq(OnedSettings.get_game(), "jodemo")
	assert_eq(OnedSettings.get_expansion(), "jox01")
	assert_true(OnedSettings.get_recent_dirs().has(root))
	assert_false(FileAccess.file_exists(root.path_join("oned.proj")))
	DirAccess.remove_absolute(root)


func test_implicit_bundled_assets_default_is_never_persisted() -> void:
	var root := ProjectSettings.globalize_path(
			"res://../.godot-target-fixtures/oned_bundled_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	var app := _app()
	app.set_implicit_resource_dir(root)

	app.commit_settings()

	assert_eq(OnedSettings.get_resource_dir(), "",
			"displaying the packaged fallback does not turn it into an explicit setting")
	assert_false(OnedSettings.get_recent_dirs().has(root),
			"the implicit fallback does not enter recents")
	DirAccess.remove_absolute(root)


func test_temporarily_unavailable_explicit_path_is_not_erased_on_close() -> void:
	var unavailable := "C:/temporarily-offline-opennova-assets"
	ConfigStore.write(
			OnedSettings.CONFIG_PATH,
			OnedSettings.SECTION,
			OnedSettings.DIR_KEY,
			unavailable)
	var app := _app()
	var resource := app.get_node("%ResourceDirEdit") as LineEdit
	var open_button := app.get_node("%RunOpenNovaButton") as Button
	var retail_button := app.get_node("%RunRetailButton") as Button

	assert_eq(resource.text, unavailable,
			"an unavailable explicit directory stays visible instead of selecting bundled assets")
	assert_eq(OnedSettings.get_resource_dir(), unavailable)
	assert_true(open_button.disabled)
	assert_true(retail_button.disabled)

	app.commit_settings()

	assert_eq(String(ConfigStore.read(
			OnedSettings.CONFIG_PATH,
			OnedSettings.SECTION,
			OnedSettings.DIR_KEY,
			"")), unavailable,
			"an unavailable saved directory remains available for a future session")


func test_temporarily_unavailable_retail_path_remains_visible_and_saved() -> void:
	var unavailable := "C:/temporarily-offline-joint-operations"
	OnedSettings.set_retail_dir(unavailable)
	var app := _app()
	var retail := app.get_node("%RetailDirEdit") as LineEdit

	assert_eq(retail.text, unavailable,
			"an unavailable install stays visible so it can recover when remounted")
	app.commit_settings()
	assert_eq(OnedSettings.get_retail_dir(), unavailable)


func test_invalid_resource_directory_disables_both_run_targets() -> void:
	var app := _app()
	var resource := app.get_node("%ResourceDirEdit") as LineEdit
	resource.text = "C:/definitely-missing-oned-assets"
	resource.text_changed.emit(resource.text)
	var open_button := app.get_node("%RunOpenNovaButton") as Button
	var retail_button := app.get_node("%RunRetailButton") as Button
	assert_true(open_button.disabled)
	assert_true(retail_button.disabled)
	assert_string_contains(open_button.tooltip_text, "resource directory")
	assert_string_contains(retail_button.tooltip_text, "resource directory")
