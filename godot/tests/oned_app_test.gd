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


# The surface is the engine's ImGui window; its seam (OnedUi) is the
# application interface these tests drive — the same edges the mouse produces.
func test_oned_is_one_compact_settings_and_run_surface() -> void:
	var app := _app()
	assert_eq(app.get_window().title, "ONED")
	assert_not_null(app.get_run_session())
	var ui := app.get_ui()
	assert_not_null(ui, "the surface seam is part of the scene")
	assert_false(ui.is_available(),
			"headless: the ImGui surface never attaches, the seam still works")
	assert_false(ui.is_running(), "nothing is managed at boot")
	assert_false(ui.get_opennova_block().is_empty(),
			"no valid game data yet: Run OpenNova explains itself instead of running")
	assert_false(ui.get_retail_block().is_empty(),
			"no valid game data yet: Stage & Run Retail explains itself")
	assert_string_contains(ui.get_status_text(), "run OpenNova or retail")

	var source := FileAccess.get_file_as_string("res://modtools/oned_main.tscn")
	for removed in [
		"Workspace", "MissionEditor", "TerrainEditor", "ObjectEditor",
		"EnvironmentEditor", "McpService", "Export Game", "F6", "LineEdit", "Button",
	]:
		assert_false(source.contains(removed), "ONED has no %s surface" % removed)
	assert_true(source.contains('type="OnedUi"'), "the engine draws the surface")


func test_surface_requests_reach_the_run_session() -> void:
	var app := _app()
	var ui := app.get_ui()
	ui.push_request(OnedUi.STOP)
	app.drain_ui_requests()
	assert_eq(ui.get_status_text(), "Nothing is running.",
			"Stop with nothing managed reports instead of failing")
	assert_eq(ui.get_status_kind(), &"info")
	assert_false(ui.has_requests(), "the drain consumed the queue")


func test_stop_failure_keeps_the_actionable_session_error() -> void:
	var app := _app()
	var session := StopFailureSession.new()
	app.attach_run_session(session)

	app.stop_game()

	assert_eq(session.stop_calls, 1)
	assert_eq(app.get_ui().get_status_text(), "Could not stop the running process.")
	assert_eq(app.get_ui().get_status_kind(), &"error")
	assert_true(app.get_ui().is_running(), "the surface keeps Stop enabled while the child lives")


func test_resource_and_profile_fields_persist_without_a_project_file() -> void:
	var root := ProjectSettings.globalize_path(
			"res://../.godot-target-fixtures/oned_app_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	var app := _app()
	var ui := app.get_ui()

	ui.set_resource_dir(root)
	ui.push_request(OnedUi.EDIT_RESOURCE_DIR)
	app.drain_ui_requests()
	assert_true(ResourceDirSettings.is_valid_root(root), "the explicit test directory is valid")
	assert_true(app.should_persist_resource_dir(), "the typed directory replaces the fallback")
	ui.push_request(OnedUi.APPLY_RESOURCE_DIR)
	ui.set_game_code("JODEMO")
	ui.push_request(OnedUi.COMMIT_PROFILE)
	ui.set_expansion(" jox01 ")
	ui.push_request(OnedUi.COMMIT_PROFILE)
	app.drain_ui_requests()

	assert_eq(OnedSettings.get_resource_dir(), root)
	assert_eq(OnedSettings.get_game(), "jodemo")
	assert_eq(OnedSettings.get_expansion(), "jox01")
	assert_eq(ui.get_game_code(), "jodemo", "the committed profile reads back normalized")
	assert_eq(ui.get_expansion(), "jox01")
	assert_true(OnedSettings.get_recent_dirs().has(root))
	assert_true(ui.get_recent_dirs().has(root), "the surface shows the new recent")
	assert_false(FileAccess.file_exists(root.path_join("oned.proj")))
	DirAccess.remove_absolute(root)


func test_implicit_bundled_assets_default_is_never_persisted() -> void:
	var root := ProjectSettings.globalize_path(
			"res://../.godot-target-fixtures/oned_bundled_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	var app := _app()
	app.set_implicit_resource_dir(root)
	assert_eq(app.get_ui().get_resource_dir(), root, "the fallback is displayed")

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
	var ui := app.get_ui()

	assert_eq(ui.get_resource_dir(), unavailable,
			"an unavailable explicit directory stays visible instead of selecting bundled assets")
	assert_eq(OnedSettings.get_resource_dir(), unavailable)
	assert_false(ui.get_opennova_block().is_empty())
	assert_false(ui.get_retail_block().is_empty())

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

	assert_eq(app.get_ui().get_retail_dir(), unavailable,
			"an unavailable install stays visible so it can recover when remounted")
	app.commit_settings()
	assert_eq(OnedSettings.get_retail_dir(), unavailable)


func test_invalid_resource_directory_disables_both_run_targets() -> void:
	var app := _app()
	var ui := app.get_ui()
	ui.set_resource_dir("C:/definitely-missing-oned-assets")
	ui.push_request(OnedUi.EDIT_RESOURCE_DIR)
	app.drain_ui_requests()
	assert_string_contains(ui.get_opennova_block(), "resource directory")
	assert_string_contains(ui.get_retail_block(), "resource directory")


func test_recent_selection_and_clear_flow_through_the_surface() -> void:
	var root := ProjectSettings.globalize_path(
			"res://../.godot-target-fixtures/oned_recent_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	OnedSettings.add_recent_dir(root)
	var app := _app()
	var ui := app.get_ui()
	assert_true(ui.get_recent_dirs().has(root), "recents are seeded from the settings")

	var index := ui.get_recent_dirs().find(root)
	ui.push_request(OnedUi.SELECT_RECENT, index)
	app.drain_ui_requests()
	assert_eq(ui.get_resource_dir(), root, "picking a recent fills the field")
	assert_eq(OnedSettings.get_resource_dir(), root, "...and applies it")

	ui.push_request(OnedUi.CLEAR_RECENTS)
	app.drain_ui_requests()
	assert_true(ui.get_recent_dirs().is_empty(), "clearing empties the surface's list")
	assert_true(OnedSettings.get_recent_dirs().is_empty())
	DirAccess.remove_absolute(root)
