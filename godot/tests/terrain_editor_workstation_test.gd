extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
const TerrainEditorScene = preload("res://modtools/terrain/terrain_editor.tscn")
const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const TerrainWorkspaceScript = preload("res://modtools/editor/terrain_workspace.gd")
const TerrainEditorAssetDockScene = preload("res://modtools/terrain/ui/editor_asset_dock.tscn")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const EnvironmentInspectorScript = preload("res://modtools/environment/environment_inspector.gd")
const QuadrantBoardScript = preload("res://modtools/terrain/ui/widgets/quadrant_board.gd")
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const FIXTURE_CACHE_DIR := "opennova_test"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()


func after_each() -> void:
	# Persistence tests write user://terrain_editor_state.cfg; restore it so they
	# never leak a temp resource directory into the real editor's saved state.
	if _had_state_config:
		var f := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if f != null:
			f.store_buffer(_saved_state_config)
			f.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func after_all() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join(FIXTURE_CACHE_DIR))


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


func _workspace_action_texts(host: Node) -> Array:
	var texts := []
	for child in host.get_children():
		if child is Button:
			texts.append((child as Button).text)
	return texts


func _find_button_by_text(root: Node, text: String) -> Button:
	if root is Button and (root as Button).text == text:
		return root as Button
	for child in root.get_children():
		var found := _find_button_by_text(child, text)
		if found != null:
			return found
	return null


func _has_label_text(root: Node, text: String) -> bool:
	if root is Label and (root as Label).text == text:
		return true
	for child in root.get_children():
		if _has_label_text(child, text):
			return true
	return false


func _find_node_by_name(root: Node, node_name: String) -> Node:
	if root.name == node_name:
		return root
	for child in root.get_children():
		var found := _find_node_by_name(child, node_name)
		if found != null:
			return found
	return null


func _find_node_by_type(root: Node, type_name: String) -> Node:
	if root == null:
		return null
	if root.is_class(type_name):
		return root
	for child in root.get_children():
		var found := _find_node_by_type(child, type_name)
		if found != null:
			return found
	return null


func _make_resource_fixture(name: String) -> String:
	# Build fixtures under the OS cache dir (not user://): a resource library
	# never lives inside the app user-data dir, so this keeps fixtures out of the
	# editor's real state and compatible with _is_valid_resource_root().
	var root := OS.get_cache_dir().path_join(FIXTURE_CACHE_DIR).path_join("%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(root.path_join("missions"))
	DirAccess.make_dir_recursive_absolute(root.path_join("terrains"))
	DirAccess.make_dir_recursive_absolute(root.path_join("env"))
	DirAccess.make_dir_recursive_absolute(root.path_join("models"))
	DirAccess.make_dir_recursive_absolute(root.path_join("objects"))
	_write_fixture_file(root.path_join("missions/alpha.bms"), "bms")
	_write_fixture_file(root.path_join("terrains/alpha.trn"), "trn")
	_write_fixture_file(root.path_join("env/alpha.env"), "env")
	_write_fixture_file(root.path_join("models/alpha.glb"), "glb")
	_write_fixture_file(root.path_join("objects/alpha.3dp"), "3dp")
	_write_fixture_file(root.path_join("objects/alpha.3di"), "3di")
	_write_fixture_file(root.path_join("objects/alpha.ase"), "ase")
	return root


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func test_workstation_starts_with_object_domain_workspace() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var workspace_rail: HBoxContainer = workstation.get_node("%WorkspaceRail")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "Terrain should remain the default workspace.")
	assert_eq(workspace_rail.get_child_count(), 3, "The shell should expose Terrain, Object, and Mission workspaces.")
	assert_eq((workspace_rail.get_child(0) as Button).text, "Terrain", "Terrain should be the first workspace.")
	assert_eq((workspace_rail.get_child(1) as Button).text, "Object", "Object should replace the old standalone OED workflow.")
	assert_eq((workspace_rail.get_child(2) as Button).text, "Mission", "Mission should have a reserved workspace.")


func test_mission_placeholder_shows_no_document_actions() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	var actions_host: VBoxContainer = workstation.get_node("%WorkspaceActionsHost")
	var inspector_host: Control = workstation.get_node("%InspectorHost")
	assert_eq(workstation.get_node("%ProjectLabel").text, "Mission", "Placeholder workspaces should own the shell title while active.")
	assert_false(workstation.get_node("%AssetDock").visible, "Terrain properties should hide outside the terrain workspace.")
	assert_null(workstation.get_node_or_null("%FileMenu"), "Global File menu should be removed.")
	assert_null(workstation.get_node_or_null("%SaveButton"), "Global Save button should be removed.")
	assert_null(workstation.get_node_or_null("%ExportButton"), "Global Export button should be removed.")
	assert_null(workstation.get_node_or_null("WorkstationLayout/TopBar"), "The global top bar should be removed.")
	assert_null(workstation.get_node_or_null("%UndoButton"), "Undo should not have a toolbar button; it stays on universal shortcuts.")
	assert_null(workstation.get_node_or_null("%RedoButton"), "Redo should not have a toolbar button; it stays on universal shortcuts.")
	assert_false(actions_host.visible, "Mission should not expose workspace document actions.")
	assert_eq(actions_host.get_child_count(), 0, "Mission should not build document action buttons.")
	assert_true(_has_label_text(inspector_host, "Coming soon"), "Mission should show a compact coming-soon placeholder.")


func test_resource_index_lists_object_resources_without_glb_models() -> void:
	var root := _make_resource_fixture("resource_index_godot")
	var index := NovaResourceIndex.new()

	assert_eq(index.scan(root, true), OK, "Resource index should scan a filesystem directory.")
	assert_eq(index.get_resource_files("mission").size(), 1, "BMS files should be indexed as mission resources.")
	assert_eq(index.get_resource_files("terrain").size(), 1, "TRN files should be indexed as terrain resources.")
	assert_eq(index.get_resource_files("environment").size(), 1, "ENV files should be indexed as environment resources.")
	assert_eq(index.get_resource_files("object_project").size(), 1, "3DP files should be indexed as object workspaces.")
	assert_eq(index.get_resource_files("object_model").size(), 1, "3DI files should be indexed as object model resources.")
	assert_eq(index.get_resource_files("object_scene").size(), 1, "ASE files should be indexed as importable object scenes.")
	assert_eq(index.get_resource_files("glb").size(), 0, "GLB files should no longer be indexed.")
	assert_eq(index.get_resource_files("all").size(), 6, "All openable resources should exclude GLB.")
	assert_eq(String((index.get_resource_files("object_model")[0] as Dictionary).get("relative_path", "")), "objects/alpha.3di", "Object model entries should keep root-relative paths.")


func test_settings_viewport_popup_edits_resource_directory() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("settings_resource_dir")
	workstation._resource_recursive = true
	assert_eq(workstation._set_resource_root_dir("", false, false), OK, "Test should start with no configured resource directory.")

	var settings_button: Button = workstation.get_node("%SettingsToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_eq(settings_button.get_parent(), environment_button.get_parent(), "Settings should live in the viewport button rail.")
	assert_true(environment_button.get_index() < settings_button.get_index(), "Settings should sit beside Camera and Environment.")

	settings_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%SettingsPopup")
	var edit: LineEdit = workstation.get_node("%SettingsResourceDirEdit")
	var recursive: CheckBox = workstation.get_node("%SettingsRecursiveToggle")
	assert_true(popup.visible, "Settings button should open the viewport settings popup.")
	assert_true(settings_button.button_pressed, "Settings button should stay pressed while open.")
	assert_true(recursive.button_pressed, "Recursive scanning should default on.")

	edit.text = root
	recursive.button_pressed = false
	workstation._apply_resource_settings(true, false)

	assert_eq(workstation.get_resource_root_dir(), root, "Settings should apply the resource directory.")
	assert_false(workstation.is_resource_recursive(), "Settings should apply the recursive toggle.")
	assert_eq(workstation.get_resource_index().get_resource_files("terrain").size(), 0, "Non-recursive scan should not include nested terrain files.")


func test_workspace_open_uses_resource_browser_with_browse_fallback() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	var root := _make_resource_fixture("resource_browser_terrain")
	workstation.set_editor(editor)
	workstation._resource_recursive = true
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should use the configured resource directory.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsHost"), "Open Terrain...")
	assert_not_null(open_button, "Terrain workspace should expose Open Terrain.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Open should create the in-editor resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as ItemList
	var browse := dialog.find_child("ResourceBrowserBrowseFilesButton", true, false) as Button
	var dir_label := dialog.find_child("ResourceBrowserDirectoryLabel", true, false) as Label
	var settings_shortcut := dialog.find_child("ResourceBrowserSettingsButton", true, false) as Button
	assert_true(dialog.visible, "Resource browser should open instead of going straight to native file browsing.")
	assert_not_null(list, "Resource browser should include a list.")
	assert_not_null(browse, "Resource browser should keep a native Browse Files fallback.")
	assert_not_null(dir_label, "Resource browser should show the active resource directory.")
	assert_not_null(settings_shortcut, "Resource browser should include a Settings shortcut node.")
	if list != null:
		assert_eq(list.item_count, 1, "Terrain browser should list TRN files from the resource directory.")
		assert_string_contains(list.get_item_text(0), "alpha", "Resource rows should show the matching terrain.")
	if dir_label != null:
		assert_string_contains(dir_label.text, root, "Directory label should show the configured root.")
	if settings_shortcut != null:
		assert_false(settings_shortcut.visible, "Settings shortcut should hide when resources are available.")
	assert_true(dialog.get_ok_button().disabled, "Open should stay disabled until a resource is selected.")


func test_workspace_open_without_resource_dir_shows_empty_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	workstation.set_editor(editor)
	assert_eq(workstation._set_resource_root_dir("", false, false), OK, "Test should clear the resource directory without persisting it.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsHost"), "Open Terrain...")
	assert_not_null(open_button, "Terrain workspace should expose Open Terrain.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Open should still create the resource browser without a root.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as ItemList
	var hint := dialog.find_child("ResourceBrowserHint", true, false) as Label
	var settings_shortcut := dialog.find_child("ResourceBrowserSettingsButton", true, false) as Button
	if list != null:
		assert_eq(list.item_count, 0, "No resource directory should produce no rows.")
	if hint != null:
		assert_string_contains(hint.text, "No resource directory selected", "Empty state should name the missing resource directory.")
	if settings_shortcut != null:
		assert_true(settings_shortcut.visible, "Settings shortcut should be visible when no resource directory is configured.")
	assert_true(dialog.get_ok_button().disabled, "Open should stay disabled without a selected resource.")


func test_environment_sun_popup_exposes_env_document_controls() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation._environment_workspace.set_environment_editor(environment_editor)

	var sun_button: Button = workstation.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%EnvironmentPopup")
	var actions_host: VBoxContainer = workstation.get_node("%EnvironmentActionsHost")
	var save_button := _find_button_by_text(actions_host, "Save Environment")
	var inspector_host: Control = workstation.get_node("%EnvironmentInspectorHost")
	var inspector := inspector_host.get_child(inspector_host.get_child_count() - 1)
	assert_true(popup.visible, "The sun button should show the environment popup.")
	assert_true(sun_button.button_pressed, "The sun button should stay pressed while the popup is visible.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "Opening environment should not switch the active workspace.")
	assert_eq(workstation.get_node("%ProjectLabel").text, "Terrain", "Terrain should keep shell title ownership when no terrain editor is set.")
	assert_eq(workstation.get_node("%EnvironmentPopupTitle").text, "untitled", "Environment should own the popup title.")
	assert_eq(_workspace_action_texts(actions_host), ["New Environment", "Open Environment...", "Save Environment", "Save Environment As..."], "Environment popup should expose document actions without a separate export.")
	assert_not_null(save_button, "Environment popup should expose Save Environment.")
	assert_true(save_button.disabled, "Clean new environments should not enable Save until changed.")
	assert_null(_find_button_by_text(actions_host, "Export Environment..."), "Environment should not advertise export separately from save.")
	assert_true(inspector.get_script() == EnvironmentInspectorScript, "Environment popup should build its inspector instead of a placeholder.")

	environment_editor.env_file.set_env_name("storm_test")
	workstation.sync_from_editor_state()

	assert_eq(workstation.get_node("%EnvironmentPopupTitle").text, "storm_test*", "Environment edits should dirty the popup document title.")
	assert_false(save_button.disabled, "Dirty environments should enable Save.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.ENVIRONMENT)
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "The old Environment workspace id should open the popup instead of changing workspaces.")


func test_environment_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	var root := _make_resource_fixture("resource_browser_environment")
	environment_editor.create_default_environment(false)
	workstation._environment_workspace.set_environment_editor(environment_editor)
	workstation._resource_recursive = true
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index environment files.")

	var sun_button: Button = workstation.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame
	var open_button := _find_button_by_text(workstation.get_node("%EnvironmentActionsHost"), "Open Environment...")
	assert_not_null(open_button, "Environment popup should expose Open Environment.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Environment Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as ItemList
	assert_not_null(list, "Environment resource browser should include a list.")
	if list != null:
		assert_eq(list.item_count, 1, "Environment browser should list ENV files from the resource directory.")
		assert_string_contains(list.get_item_text(0), "alpha", "Environment resource rows should show the matching file.")


func test_resource_settings_persist_in_editor_state() -> void:
	var root := _make_resource_fixture("resource_settings_persist")
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation._resource_recursive = true
	assert_eq(workstation._set_resource_root_dir(root, true, true), OK, "Persisted resource directory should scan successfully.")

	var next_workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(next_workstation.get_resource_root_dir(), root, "New workstation instances should load the persisted resource directory.")
	assert_true(next_workstation.is_resource_recursive(), "Recursive setting should persist with its default value.")


func test_resource_root_inside_user_data_is_rejected_on_load() -> void:
	# A resource root that points inside the app user-data dir (e.g. a temp/test
	# path that leaked into the persisted state) must not be adopted on load, so
	# the resource browser shows a clean "no directory" state instead of a dead
	# internal path.
	var bogus := OS.get_user_data_dir().path_join("resource_settings_persist_bogus_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(bogus)
	var w1 = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(w1._set_resource_root_dir(bogus, true, false), OK, "Persisting the root should succeed.")

	var w2 = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(w2.get_resource_root_dir(), "", "A resource root inside the app user-data dir should be rejected on load.")
	DirAccess.remove_absolute(bogus)


func test_camera_button_exposes_global_viewport_settings() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")

	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_eq(camera_button.get_parent(), environment_button.get_parent(), "Camera and environment should live in the same viewport button rail.")
	assert_true(camera_button.get_index() < environment_button.get_index(), "Camera should sit immediately before Environment.")

	camera_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%CameraPopup")
	var settings_host: Control = workstation.get_node("%CameraSettingsHost")
	var settings_panel: Control = settings_host.get_child(0)
	var fly_speed_spin: SpinBox = settings_panel.get_node("%FlySpeedSpin")
	var near_plane_spin: SpinBox = settings_panel.get_node("%NearPlaneSpin")
	var far_plane_spin: SpinBox = settings_panel.get_node("%FarPlaneSpin")
	assert_true(popup.visible, "The camera button should show the camera popup.")
	assert_true(camera_button.button_pressed, "The camera button should stay pressed while the popup is visible.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "Opening camera settings should not switch workspaces.")

	fly_speed_spin.value_changed.emit(72.0)
	near_plane_spin.value_changed.emit(0.25)
	far_plane_spin.value_changed.emit(2600.0)

	assert_eq(editor.camera.fly_speed, 72.0, "Camera popup should update flight speed.")
	assert_eq(editor.camera.near, 0.25, "Camera popup should update the near plane.")
	assert_eq(editor.camera.far, 2600.0, "Camera popup should update the far plane.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	camera_button.toggled.emit(true)
	await get_tree().process_frame

	assert_true(popup.visible, "Camera settings should remain available from Mission.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION, "Opening camera settings from Mission should not switch workspaces.")


func test_camera_button_targets_object_preview_camera_when_object_is_active() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")
	var host: Control = workstation.get_node("%ViewportHost")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	await get_tree().process_frame

	var preview := _find_node_by_name(host, "ObjectPreview")
	var object_camera := _find_node_by_type(preview, "Camera3D") as Camera3D
	var terrain_camera := editor.camera
	assert_not_null(preview, "Object workspace should mount its preview in the shared viewport host.")
	assert_not_null(object_camera, "Object preview should expose a fly camera for global camera settings.")
	assert_not_null(terrain_camera, "Terrain editor should still keep its camera while Object is active.")
	if object_camera == null or terrain_camera == null:
		return
	var original_terrain_speed: float = terrain_camera.fly_speed

	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	camera_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%CameraPopup")
	var settings_host: Control = workstation.get_node("%CameraSettingsHost")
	var settings_panel: Control = settings_host.get_child(0)
	var fly_speed_spin: SpinBox = settings_panel.get_node("%FlySpeedSpin")
	assert_true(popup.visible, "Camera settings should open while Object is active.")

	fly_speed_spin.value_changed.emit(43.0)

	assert_eq(object_camera.fly_speed, 43.0, "Global camera popup should edit the Object preview camera when Object is active.")
	assert_eq(terrain_camera.fly_speed, original_terrain_speed, "Object camera edits should not mutate the inactive Terrain camera.")


func test_object_workspace_uses_only_global_environment_viewport_button() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")
	var host: Control = workstation.get_node("%ViewportHost")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	await get_tree().process_frame

	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_not_null(environment_button, "Global environment button should stay available from Object.")
	assert_null(_find_node_by_name(host, "ObjectEnvironmentButton"), "Object preview should not add a second environment button over the viewport.")

	environment_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%EnvironmentPopup")
	assert_true(popup.visible, "Global environment button should open the shared environment popup from Object.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.OBJECT, "Opening global environment controls should not switch out of Object.")


func test_viewport_popups_are_mutually_exclusive_and_escape_closes_active_popup() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation._environment_workspace.set_environment_editor(environment_editor)

	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	var camera_popup: PanelContainer = workstation.get_node("%CameraPopup")
	var environment_popup: PanelContainer = workstation.get_node("%EnvironmentPopup")

	camera_button.toggled.emit(true)
	await get_tree().process_frame
	environment_button.toggled.emit(true)
	await get_tree().process_frame

	assert_false(camera_popup.visible, "Opening Environment should close Camera.")
	assert_false(camera_button.button_pressed, "Camera button should release when its popup closes.")
	assert_true(environment_popup.visible, "Environment should be visible after its button is pressed.")

	var escape := InputEventKey.new()
	escape.pressed = true
	escape.keycode = KEY_ESCAPE
	workstation._unhandled_input(escape)

	assert_false(environment_popup.visible, "Escape should close the visible viewport popup.")
	assert_false(environment_button.button_pressed, "Environment button should release after Escape closes the popup.")


func test_terrain_workspace_exposes_project_save_and_export_actions() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)

	var actions_host: VBoxContainer = workstation.get_node("%WorkspaceActionsHost")
	var save_button := _find_button_by_text(actions_host, "Save Project")
	var export_button := _find_button_by_text(actions_host, "Export Terrain...")
	assert_eq(_workspace_action_texts(actions_host), ["New Terrain", "Open Terrain...", "Save Project", "Save Project As...", "Export Terrain..."], "Terrain should expose project actions and a distinct export action.")
	assert_not_null(save_button, "Terrain should expose Save Project.")
	assert_not_null(export_button, "Terrain should expose Export Terrain.")
	assert_false(save_button.disabled, "Dirty terrain projects should enable Save Project.")
	assert_false(export_button.disabled, "Terrain export should be available when the editor is idle.")


func test_switching_workspaces_preserves_terrain_dirty_state() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)

	assert_true(editor.is_dirty, "Switching placeholder domains should not reset terrain document state.")
	assert_eq(workstation.get_node("%ProjectLabel").text, "untitled*", "Returning to Terrain should restore the terrain project title and dirty marker.")
	assert_true(workstation.get_node("%AssetDock").visible, "Terrain properties should return when Terrain is active.")


func test_workstation_mounts_workspace_specific_right_docks() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	workstation.set_editor(editor)

	var dock: Control = workstation.get_node("%AssetDock")
	assert_true(dock.visible, "Terrain should show the shared right dock host.")
	assert_not_null(_find_node_by_name(dock, "TerrainAssetDock"), "Terrain should mount its asset dock inside the shared right dock host.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)

	assert_false(dock.visible, "Object Preview should hide the shared right dock host.")
	assert_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object Preview should not mount an empty detail dock.")
	assert_null(_find_node_by_name(dock, "TerrainAssetDock"), "Switching to Object should remove Terrain's dock content.")
	assert_not_null(_find_node_by_name(workstation.get_node("%ViewportHost"), "ObjectPreview"), "Object preview should stay in the center viewport.")

	var materials_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Materials")
	assert_not_null(materials_button, "Object workspace should expose Materials mode.")
	if materials_button != null:
		materials_button.pressed.emit()
	assert_true(dock.visible, "Object Materials should show the shared right dock host.")
	assert_not_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object Materials should mount its detail dock.")
	assert_not_null(_find_node_by_name(dock, "MaterialDetailPanel"), "Object Materials should expose right-pane material details.")

	var parts_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Part Anims")
	assert_not_null(parts_button, "Object workspace should expose Part Anims mode.")
	if parts_button != null:
		parts_button.pressed.emit()
	assert_true(dock.visible, "Object Part anims should show the shared right dock host.")
	assert_not_null(_find_node_by_name(dock, "PartAnimDetailsEmpty"), "Object Part anims should expose right-pane animation details.")

	var lights_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Lights")
	assert_not_null(lights_button, "Object workspace should expose Lights mode.")
	if lights_button != null:
		lights_button.pressed.emit()
	assert_true(dock.visible, "Object Lights should show the shared right dock host.")
	assert_not_null(_find_node_by_name(dock, "LightDetailPanel"), "Object Lights should expose right-pane light details.")

	var lods_button := _find_button_by_text(workstation.get_node("%ModeRail"), "LODs")
	assert_not_null(lods_button, "Object workspace should expose LODs mode.")
	if lods_button != null:
		lods_button.pressed.emit()
	assert_false(dock.visible, "Object LODs should hide the shared right dock host until they have a real detail editor.")
	assert_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object LODs should not mount placeholder right-pane content.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	assert_false(dock.visible, "Mission should hide the right dock host.")
	assert_eq(dock.get_child_count(), 0, "Workspaces without a right dock should leave the shared host empty.")


func test_workspace_switching_mounts_terrain_and_mission_viewports() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")
	var host: Control = workstation.get_node("%ViewportHost")

	assert_eq(host.get_child_count(), 1, "Terrain should own the viewport host by default.")
	assert_eq(host.get_child(0).name, "TerrainViewport", "Terrain should mount through TerrainViewport.")
	assert_true(editor.is_viewport_active(), "Terrain editor rendering should be active while Terrain owns the viewport.")
	assert_true(editor.is_viewport_edit_input_active(), "Terrain should enable terrain edit input.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	await get_tree().process_frame

	assert_eq(host.get_child_count(), 1, "Mission should replace Terrain as the only viewport owner.")
	assert_eq(host.get_child(0).name, "MissionViewport", "Mission should mount its read-only terrain viewport.")
	assert_true(editor.is_viewport_active(), "Mission should keep the loaded terrain visible.")
	assert_false(editor.is_viewport_edit_input_active(), "Mission should disable terrain brush and shortcut input.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	await get_tree().process_frame

	assert_eq(host.get_child_count(), 1, "Returning to Terrain should still leave one viewport owner.")
	assert_eq(host.get_child(0).name, "TerrainViewport", "TerrainViewport should remount when Terrain becomes active again.")
	assert_true(editor.is_viewport_active(), "Terrain editor rendering should reactivate when Terrain owns the viewport.")
	assert_true(editor.is_viewport_edit_input_active(), "Terrain edit input should reactivate when Terrain owns the viewport.")


func test_workstation_tracks_mode_from_editor_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.brush_radius = 20.0
	editor.brush_strength = 0.8
	editor.brush_hardness = 0.3

	workstation.set_editor(editor)
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.SCATTER, "Workstation should switch to Foliage mode when the editor tool is foliage paint.")

	editor.current_tool = TerrainEditorScript.Tool.TILE_STAMP
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.STAMP, "Workstation should switch to Tile mode when the editor tool becomes tile placement.")


func test_prompt_unsaved_changes_updates_custom_copy() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation.prompt_unsaved_changes("quit")

	var prompt_host: Control = workstation.get_node("%PromptHost")
	var prompt_card: PanelContainer = workstation.get_node("%PromptCard")
	var lead: Label = workstation.get_node("%PromptLead")
	var info: Label = workstation.get_node("%PromptInfoLabel")
	var info_panel: PanelContainer = workstation.get_node("%PromptInfoPanel")
	var primary: Button = workstation.get_node("%PromptPrimaryButton")
	var secondary: Button = workstation.get_node("%PromptSecondaryButton")
	var tertiary: Button = workstation.get_node("%PromptTertiaryButton")
	assert_true(prompt_host.visible, "Unsaved prompt should show the custom modal host.")
	assert_true(prompt_card.visible, "Unsaved prompt should show the custom modal card.")
	assert_eq(lead.text, "Save changes?", "Unsaved prompt should keep the lead short.")
	assert_eq(info.text, "", "Unsaved prompt should no longer show a next-step callout.")
	assert_false(info_panel.visible, "Unsaved prompt should hide the info panel when there is no extra copy.")
	assert_eq(primary.text, "Save", "Unsaved prompt should keep save as the primary confirmation button.")
	assert_eq(secondary.text, "Cancel", "Unsaved prompt should let the user stay in the editor explicitly.")
	assert_eq(tertiary.text, "Discard", "Unsaved prompt should keep discard visible as a secondary destructive action.")


func test_export_flavor_dialog_updates_format_copy() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation._show_export_flavor_dialog("C:/Exports/TestTerrain")

	var prompt_host: Control = workstation.get_node("%PromptHost")
	var prompt_card: PanelContainer = workstation.get_node("%PromptCard")
	var lead: Label = workstation.get_node("%PromptLead")
	var body: Label = workstation.get_node("%PromptBody")
	var info: Label = workstation.get_node("%PromptInfoLabel")
	var format_bhd: Button = workstation.get_node("%PromptFormatBHD")
	var format_cdep: Button = workstation.get_node("%PromptFormatCDEP")
	var primary: Button = workstation.get_node("%PromptPrimaryButton")
	var secondary: Button = workstation.get_node("%PromptSecondaryButton")
	assert_true(prompt_host.visible, "Export prompt should show the custom modal host.")
	assert_true(prompt_card.visible, "Export prompt should show the custom modal card.")
	assert_eq(lead.text, "Export", "Export prompt should keep the lead minimal.")
	assert_eq(body.text, "", "Export prompt should not show extra body copy.")
	assert_false(body.visible, "Export prompt should hide the body when there is no extra copy.")
	assert_eq(info.text, "", "Export prompt should not show the folder path.")
	assert_false(workstation.get_node("%PromptInfoPanel").visible, "Export prompt should not show a folder summary.")
	assert_eq(format_bhd.text, "BHD", "Export choices should keep BHD simple.")
	assert_eq(format_cdep.text, "JO/DFX", "Export choices should keep JO/DFX simple.")
	assert_eq(primary.text, "Export", "Export prompt should keep Export as the primary action.")
	assert_eq(secondary.text, "Cancel", "Export prompt should keep Cancel as the secondary action.")
	assert_null(workstation.get_node_or_null("%PromptFormatLabel"), "Export prompt should no longer show a separate format label.")


func test_asset_dock_builds_preview_cards_for_shared_maps() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_true(dock._slot_previews.has("charmap"), "Asset dock should build a preview card for the surface-type map.")
	assert_true(dock._slot_previews.has("foliagemap"), "Asset dock should build a preview card for the foliage map.")
	assert_true(dock._slot_previews.has("tilestrip"), "Asset dock should build a preview card for the tile atlas.")


func test_asset_dock_uses_shared_slot_labels() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_true(_has_label_text(dock, "Detail A"), "Asset dock should use shared detail slot labels.")
	assert_true(_has_label_text(dock, "Shading 1 / near"), "Asset dock should use shared auxiliary slot labels.")
	assert_true(_has_label_text(dock, "Tile atlas"), "Asset dock should use shared map-data slot labels.")


func test_asset_dock_syncs_slot_filename_labels() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.texture_files = {
		"tilestrip": "atlas_01.pcx",
	}

	dock.set_editor(editor)
	dock.sync_from_editor_state()

	var filename_label: Label = dock._slot_filename_labels["tilestrip"]
	assert_eq(filename_label.text, "atlas_01.pcx", "Asset dock should show the current texture filename for shared asset slots.")


func test_asset_dock_uses_properties_tab_and_removes_old_toggles() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())
	var tabs: TabContainer = dock.get_node("%Tabs")

	assert_eq(tabs.get_tab_count(), 1, "Asset dock should only expose Properties.")
	assert_eq(tabs.get_tab_title(0), "Properties", "The first dock tab should be renamed to Properties.")
	assert_null(dock.get_node_or_null("%CameraTab"), "Camera controls should move to the global viewport popup.")
	assert_null(dock.get_node_or_null("Tabs/Document"), "The old Document tab should be removed.")
	assert_null(dock.get_node_or_null("%ViewTab"), "The old View tab should be removed.")
	assert_null(dock.get_node_or_null("%WaterVisibleToggle"), "The water-plane toggle should move out of the dock.")
	assert_null(dock.get_node_or_null("%HorizonSpin"), "Horizon controls should be removed from the dock.")
	assert_null(dock.get_node_or_null("%ColormapToggle"), "Colormap visibility toggle should be removed from the dock.")
	assert_null(dock.get_node_or_null("%WireframeToggle"), "Wireframe toggle should be removed from the dock.")


func test_asset_dock_and_inspectors_do_not_poll_when_idle() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_false(dock.is_processing(), "Asset dock should sync from editor state changes instead of idle polling.")


func test_sculpt_inspector_syncs_from_editor_ui_state_signal() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := SculptInspector.new()
	var editor = autofree(TerrainEditorScript.new())

	inspector.build_main(host)
	inspector.set_editor(editor)
	editor.set_brush_radius_value(37.0)

	var radius_spin: SpinBox = inspector._brush.get_radius_spin()
	assert_eq(radius_spin.value, 37.0, "Editor UI state changes should update subscribed inspectors without per-frame polling.")


func test_workstation_uses_clip_text_for_long_labels() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var project_label: Label = workstation.get_node("%ProjectLabel")
	var status_context_label: Label = workstation.get_node("%StatusContextLabel")
	var status_camera_label: Label = workstation.get_node("%StatusCameraLabel")

	assert_true(project_label.clip_text, "Project label should clip rather than forcing the top bar wider.")
	assert_true(status_context_label.clip_text, "Status context should clip instead of forcing horizontal overflow.")
	assert_true(status_camera_label.clip_text, "Status camera text should clip instead of forcing horizontal overflow.")


func test_pressing_layout_mode_restores_edit_sectors_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	workstation.set_editor(editor)
	workstation._on_workflow_pressed(TerrainWorkspaceScript.Workflow.LAYOUT)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Selecting Layout should restore the sector editing tool.")


func test_layout_inspector_is_trimmed_to_board_and_legend() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := LayoutInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	inspector.build_main(host)
	inspector.set_editor(editor)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Layout inspector should force sector editing when it becomes active.")
	assert_not_null(inspector._legend_grid, "Layout inspector should keep a passive legend.")
	assert_gt(inspector._legend_grid.get_child_count(), 0, "The legend should be populated with sector swatches.")
	assert_not_null(inspector._sector_overlay_toggle, "Layout inspector should expose a sector overlay toggle under the map layout board.")


func test_layout_inspector_sector_overlay_toggle_syncs_with_editor() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := LayoutInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.set_sector_overlay_visible(true)

	inspector.build_main(host)
	inspector.set_editor(editor)

	assert_true(inspector._sector_overlay_toggle.button_pressed, "Layout inspector should reflect the editor's current sector overlay visibility.")

	inspector._on_sector_overlay_toggled(false)
	assert_false(editor.is_sector_overlay_visible(), "Toggling sector overlay off in Layout should update editor state.")


func test_foliage_inspector_selection_updates_editor_selection() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := ScatterInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT

	inspector.build_main(host)
	inspector.set_editor(editor)
	inspector._on_list_selected(1)

	assert_eq(editor.get_selected_foliage_def_index(), 1, "Selecting a foliage list item should update the editor selection used by paint.")
	assert_true(editor.get_selected_foliage_def() != null, "The selected foliage def should be available after selecting it in the inspector.")


func test_foliage_inspector_reflects_editor_selection_and_add_remove() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := ScatterInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.set_selected_foliage_def_index(1)

	inspector.build_main(host)
	inspector.set_editor(editor)

	var list: ItemList = inspector._list
	assert_true(list.is_selected(1), "The foliage inspector should highlight the editor's selected foliage def.")

	inspector._on_add_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 2, "Adding a foliage def should leave the new foliage type selected in editor state.")

	inspector._on_remove_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 1, "Removing the selected foliage def should clamp selection to the remaining valid index.")
	assert_true(list.is_selected(1), "The foliage inspector should stay aligned with the clamped editor selection after removal.")


func test_stamp_inspector_removes_entry_list_and_apply_workflow() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	inspector.build_main(host)

	assert_not_null(inspector._selection_done, "Tile inspector should expose a direct exit action for selection mode.")
	assert_not_null(inspector._selection_delete, "Tile inspector should expose a direct delete action for the selected tile.")


func test_stamp_inspector_atlas_click_replaces_selected_tile_immediately() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(host)
	inspector.set_editor(editor)
	inspector._on_atlas_selected(5)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after replacing from the atlas.")
	assert_eq(entry.get_tile_index(), 5, "Atlas clicks should replace the selected tile immediately.")


func test_stamp_inspector_atlas_focus_follows_selected_tile() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	editor._document.new_tileinfo()
	# Seed a dummy tilestrip so the atlas has enough tiles for index 2 to be selectable.
	var strip_image := Image.create(256, 64, false, Image.FORMAT_RGBA8)
	strip_image.fill(Color(0.5, 0.5, 0.5, 1.0))
	editor._document.data.set_tilestrip_tex(ImageTexture.create_from_image(strip_image))
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(host)
	inspector.set_editor(editor)
	inspector.sync_from_editor()

	var atlas_status: Label = inspector._atlas_status
	var atlas_list: ItemList = inspector._atlas_list
	assert_string_contains(atlas_status.text, "editing 002", "Atlas status should reflect the selected tile when replace-on-click is active.")
	assert_true(atlas_list.is_selected(2), "Atlas selection should follow the selected tile while a placed tile is active.")


func test_stamp_inspector_reuses_tile_preview_icons_until_atlas_changes() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	var strip_image := Image.create(256, 64, false, Image.FORMAT_RGBA8)
	strip_image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var tilestrip := ImageTexture.create_from_image(strip_image)
	editor._document.data.set_tilestrip_tex(tilestrip)

	inspector.build_main(host)
	inspector.set_editor(editor)
	var cache_size: int = inspector._tile_icon_cache.size()
	var first: Texture2D = inspector._build_icon(tilestrip, 2, 4)
	var second: Texture2D = inspector._build_icon(tilestrip, 2, 4)

	assert_true(first == second, "Tile preview icons should be cached while the tilestrip is unchanged.")
	assert_eq(inspector._tile_icon_cache.size(), cache_size, "Repeated tile icon requests should not allocate duplicate AtlasTextures.")


func test_stamp_inspector_flag_toggle_updates_selected_tile_immediately() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(host)
	inspector.set_editor(editor)
	inspector._on_flip_x(true)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after toggling a transform flag.")
	assert_true((entry.get_flags() & NovaTerrainTileInfo.FLAG_FLIP_X) != 0, "Tile transform toggles should update the selected tile immediately.")


func test_stamp_inspector_shows_quiet_empty_selection_state() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()

	inspector.build_main(host)
	inspector.set_editor(editor)

	var done_button: Button = inspector._selection_done
	var summary: Label = inspector._selection_summary
	var delete_button: Button = inspector._selection_delete
	assert_eq(summary.text, "No tile selected.", "Tile inspector should use a quiet empty-state until the user selects a placed tile.")
	assert_true(done_button.disabled, "Done should stay disabled until a tile is selected.")
	assert_true(delete_button.disabled, "Delete should stay disabled until a tile is selected.")


func test_stamp_inspector_done_clears_selection() -> void:
	var host = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(host)
	inspector.set_editor(editor)
	inspector._on_selection_done_pressed()

	assert_false(editor.has_selected_tileinfo_entry(), "Done should leave Tile mode in placement state with no selected tile.")


func test_quadrant_board_cycles_left_click_and_clears_right_click() -> void:
	var board = add_child_autofree(QuadrantBoardScript.new())
	var values := PackedInt32Array()
	values.resize(256)
	board.set_grid_state(2, 2, values)

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 1, "First left-click should cycle Empty to NW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 3, "Second left-click should cycle NW to NE.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 2, "Third left-click should cycle NE to SW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 4, "Fourth left-click should cycle SW to SE.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 1, "Fifth left-click should wrap SE back to NW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_RIGHT)
	assert_eq(board.grid[0], 0, "Right-click should clear the cell to Empty.")
