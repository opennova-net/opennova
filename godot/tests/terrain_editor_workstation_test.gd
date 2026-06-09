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


# Minimal editor double for prompt routing tests: the workstation only needs the
# three pending-action hooks, so this avoids set_editor()'s full workspace bind.
class PromptEditorStub:
	extends Node
	var saved := false
	var discarded := false
	var cancelled := false

	func confirm_pending_action_save() -> void:
		saved = true

	func confirm_pending_action_discard() -> void:
		discarded = true

	func cancel_pending_action() -> void:
		cancelled = true


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


func _norm_path(path: String) -> String:
	return path.rstrip("/").replace("\\", "/").to_lower()


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


# AcceptDialog/ConfirmationDialog keep their action buttons in an internal child
# container, so a normal get_children() walk misses them; this variant includes
# internal children for asserting custom dialog buttons (e.g. Discard).
func _find_button_by_text_deep(root: Node, text: String) -> Button:
	if root is Button and (root as Button).text == text:
		return root as Button
	for child in root.get_children(true):
		var found := _find_button_by_text_deep(child, text)
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


func _find_label_by_text(root: Node, text: String) -> Label:
	if root is Label and (root as Label).text == text:
		return root as Label
	for child in root.get_children():
		var found := _find_label_by_text(child, text)
		if found != null:
			return found
	return null


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


func _direct_child_count_of_type(root: Node, type_name: String) -> int:
	var count := 0
	for child in root.get_children():
		if child.is_class(type_name):
			count += 1
	return count


func _make_resource_fixture(name: String) -> String:
	# Build fixtures under the OS cache dir (not user://): a resource library
	# never lives inside the app user-data dir, so this keeps fixtures out of the
	# editor's real state and compatible with _is_valid_resource_root().
	var root := OS.get_cache_dir().path_join(FIXTURE_CACHE_DIR).path_join("%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(root)
	DirAccess.make_dir_recursive_absolute(root.path_join("ignored"))
	_write_fixture_file(root.path_join("alpha.bms"), "bms")
	_write_fixture_file(root.path_join("alpha.trn"), "trn")
	_write_fixture_file(root.path_join("alpha.env"), "env")
	_write_fixture_file(root.path_join("alpha.glb"), "glb")
	_write_fixture_file(root.path_join("alpha.3dp"), "3dp")
	_write_fixture_file(root.path_join("alpha.3di"), "3di")
	_write_fixture_file(root.path_join("alpha.ase"), "ase")
	_write_fixture_file(root.path_join("alpha.fnt"), "fnt")
	_write_fixture_file(root.path_join("alpha.bin"), "RTXTstrings")
	_write_fixture_file(root.path_join("raw.bin"), "raw")
	_write_fixture_file(root.path_join("ignored/nested.trn"), "nested")
	return root


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func test_workstation_starts_with_domain_workspaces() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var top_bar := workstation.get_node_or_null("%TopBar") as PanelContainer
	assert_not_null(top_bar, "Shell should expose a top bar for global workspace controls.")
	var workspace_rail: BoxContainer = workstation.get_node("%WorkspaceRail")
	assert_true(top_bar.is_ancestor_of(workspace_rail), "Workspace navigation should live in the top bar.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "Terrain should remain the default workspace.")

	var row_texts := []
	for child in workspace_rail.get_children():
		if child is Button:
			row_texts.append((child as Button).text)
	assert_eq(row_texts, ["Terrain", "Object", "Mission", "Fonts", "Credits", "Strings", "Music", "Sound", "Environment"],
		"The nav should list every workspace as a full-width row, with Environment promoted from the sun button.")

	assert_false(_has_label_text(workspace_rail, "World"), "Top-bar workspace groups should use separators, not inline category words.")
	assert_false(_has_label_text(workspace_rail, "Interface"), "Top-bar workspace groups should not read like a sentence.")
	assert_gte(_direct_child_count_of_type(workspace_rail, "VSeparator"), 2,
		"Top-bar workspace groups should keep visual separation between World, Interface, Audio, and Atmosphere.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.CREDITS)
	await get_tree().process_frame

	var actions_host: BoxContainer = workstation.get_node("%WorkspaceActionsHost")
	assert_true(top_bar.is_ancestor_of(actions_host), "Workspace document actions should live in the top bar.")
	assert_null(top_bar.find_child("TopSpacer", true, false),
		"Top bar should let the workspace scroller absorb empty width instead of inserting a gap between actions and global buttons.")
	var global_buttons := workstation.get_node("%GlobalButtonRail") as BoxContainer
	assert_true(actions_host.get_index() < global_buttons.get_index(),
		"Document actions should sit immediately before the global icon buttons on the right.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.CREDITS,
		"Credits should become the active workspace.")
	assert_eq(workstation.get_node("%ProjectLabel").text, "untitled",
		"Fresh Credits workspace should own the shell title while active.")
	assert_true(_workspace_action_texts(actions_host).has("Open Credits..."),
		"Credits should expose an open action.")
	assert_true(_workspace_action_texts(actions_host).has("Save Credits As..."),
		"Credits should expose save-as for a fresh resource.")


func test_workspace_ribbon_exposes_scroll_affordance_when_overflowing() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_size(Vector2(900, 600))
	await get_tree().process_frame
	await get_tree().process_frame

	var scroll := workstation.get_node("%WorkspaceScroll") as ScrollContainer
	var left := workstation.get_node_or_null("%WorkspaceScrollLeftButton") as Button
	var right := workstation.get_node_or_null("%WorkspaceScrollRightButton") as Button
	assert_not_null(left, "Overflowing workspace ribbon should expose a left scroll arrow.")
	assert_not_null(right, "Overflowing workspace ribbon should expose a right scroll arrow.")
	if left == null or right == null:
		return

	assert_true(left.visible, "Overflow arrows should be visible when the workspace ribbon is clipped.")
	assert_true(right.visible, "Overflow arrows should make hidden workspaces discoverable.")
	assert_true(left.disabled, "Left arrow should start disabled at the beginning of the ribbon.")
	assert_false(right.disabled, "Right arrow should be enabled when there are hidden workspaces to the right.")

	var before := scroll.scroll_horizontal
	right.pressed.emit()
	await get_tree().process_frame

	assert_gt(scroll.scroll_horizontal, before, "Right arrow should advance the workspace ribbon scroll position.")
	assert_false(left.disabled, "Left arrow should enable after scrolling right.")


func test_workspace_ribbon_hides_scroll_affordance_when_everything_fits() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_size(Vector2(2200, 900))
	await get_tree().process_frame
	await get_tree().process_frame

	var left := workstation.get_node_or_null("%WorkspaceScrollLeftButton") as Button
	var right := workstation.get_node_or_null("%WorkspaceScrollRightButton") as Button
	assert_not_null(left, "Workspace ribbon should include a left scroll arrow node.")
	assert_not_null(right, "Workspace ribbon should include a right scroll arrow node.")
	if left == null or right == null:
		return

	assert_false(left.visible, "Left overflow arrow should hide when all workspaces fit.")
	assert_false(right.visible, "Right overflow arrow should hide when all workspaces fit.")


func test_workspace_ribbon_does_not_force_top_bar_to_clip_at_mid_width() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_size(Vector2(1366, 768))
	await get_tree().process_frame
	await get_tree().process_frame

	var settings := workstation.get_node("%SettingsToggleButton") as Button
	var shell_right: float = workstation.get_global_rect().end.x
	assert_lte(settings.get_global_rect().end.x, shell_right + 0.5,
		"The top bar should keep global controls visible by letting the workspace ribbon scroll first.")


func test_mission_workspace_exposes_document_actions_and_inspector() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	var actions_host: BoxContainer = workstation.get_node("%WorkspaceActionsHost")
	var inspector_host: Control = workstation.get_node("%InspectorHost")
	assert_eq(workstation.get_node("%ProjectLabel").text, "Mission", "An unloaded Mission workspace owns the shell title while active.")
	assert_false(workstation.get_node("%AssetDock").visible, "Terrain properties should hide outside the terrain workspace.")
	assert_null(workstation.get_node_or_null("%FileMenu"), "Global File menu should be removed.")
	assert_null(workstation.get_node_or_null("%SaveButton"), "Global Save button should be removed.")
	assert_null(workstation.get_node_or_null("%ExportButton"), "Global Export button should be removed.")
	# Authoring (Phase 1) lands Save / Save As alongside Open. The shell builds these
	# buttons up front (before a mission is loaded); they sit disabled until there is a
	# loaded / dirtied mission. The inspector replaces the old "Coming soon" stub.
	assert_true(actions_host.visible, "Mission should expose its document actions.")
	assert_eq(_workspace_action_texts(actions_host), ["Open Mission...", "Save Mission", "Save Mission As..."],
		"Mission exposes Open + Save + Save As once authoring lands.")
	assert_true(_has_label_text(inspector_host, "Mission"), "Mission should show its inspector panel.")
	assert_false(_has_label_text(inspector_host, "Coming soon"), "The coming-soon stub should be gone.")


func test_resource_index_lists_object_resources_without_glb_models() -> void:
	var root := _make_resource_fixture("resource_index_godot")
	var index := NovaResourceIndex.new()

	assert_eq(index.scan(root), OK, "Resource index should scan a filesystem directory.")
	assert_eq(index.get_resource_files("mission").size(), 1, "BMS files should be indexed as mission resources.")
	assert_eq(index.get_resource_files("terrain").size(), 1, "TRN files should be indexed as terrain resources.")
	assert_eq(index.get_resource_files("environment").size(), 1, "ENV files should be indexed as environment resources.")
	assert_eq(index.get_resource_files("object_project").size(), 1, "3DP files should be indexed as object workspaces.")
	assert_eq(index.get_resource_files("object_model").size(), 1, "3DI files should be indexed as object model resources.")
	assert_eq(index.get_resource_files("object_scene").size(), 1, "ASE files should be indexed as importable object scenes.")
	assert_eq(index.get_resource_files("font").size(), 1, "FNT files should be indexed as font resources.")
	assert_eq(index.get_resource_files("strings").size(), 1, "RTXT BIN files should be indexed as strings resources.")
	assert_eq(index.get_resource_files("glb").size(), 0, "GLB files should no longer be indexed.")
	assert_eq(index.get_resource_files("all").size(), 8, "All openable resources should exclude GLB and non-RTXT BIN blobs.")
	assert_eq(String((index.get_resource_files("object_model")[0] as Dictionary).get("relative_path", "")), "alpha.3di", "Object model entries should keep root-relative paths.")
	assert_eq(String((index.get_resource_files("strings")[0] as Dictionary).get("relative_path", "")), "alpha.bin", "Strings entries should keep root-relative paths.")


func test_settings_viewport_popup_edits_resource_directory() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("settings_resource_dir")
	assert_eq(workstation._set_resource_root_dir("", false, false), OK, "Test should start with no configured resource directory.")

	var top_bar := workstation.get_node("%TopBar") as PanelContainer
	var viewport_lane := workstation.get_node("%ViewportLane") as Control
	var settings_button: Button = workstation.get_node("%SettingsToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_eq(settings_button.get_parent(), environment_button.get_parent(), "Settings should live beside the other global top-bar controls.")
	assert_true(top_bar.is_ancestor_of(settings_button), "Settings should live in the top bar, not over the viewport.")
	assert_null(viewport_lane.find_child("ViewportButtonRail", true, false), "Viewport should not own the global button rail.")
	assert_true(environment_button.get_index() < settings_button.get_index(), "Settings should sit beside Camera and Environment.")

	settings_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%SettingsPopup")
	var edit: LineEdit = workstation.get_node("%SettingsResourceDirEdit")
	assert_true(popup.visible, "Settings button should open the top-bar settings popup.")
	assert_true(settings_button.button_pressed, "Settings button should stay pressed while open.")
	assert_null(workstation.get_node_or_null("%SettingsRecursiveToggle"), "The resource root settings should not expose a recursive scan toggle.")

	edit.text = root
	workstation._apply_resource_settings(true, false)

	assert_eq(workstation.get_resource_root_dir(), root, "Settings should apply the resource directory.")
	assert_eq(workstation.get_resource_index().get_resource_files("terrain").size(), 1, "Flat scans should include top-level terrain files.")


func _recent_option_index_for_path(recent: OptionButton, path: String) -> int:
	for i in recent.item_count:
		var meta = recent.get_item_metadata(i)  # null for the separator row
		if typeof(meta) == TYPE_STRING and meta == path:
			return i
	return -1


func _recent_option_index_for_text(recent: OptionButton, text: String) -> int:
	for i in recent.item_count:
		if recent.get_item_text(i) == text:
			return i
	return -1


func test_settings_recent_dropdown_lists_other_dirs_and_switches() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var a := _make_resource_fixture("recent_a")
	var b := _make_resource_fixture("recent_b")
	# persist=true records each applied dir in the shared recents list.
	assert_eq(workstation._set_resource_root_dir(a, true, false), OK, "Applying dir A should succeed.")
	assert_eq(workstation._set_resource_root_dir(b, true, false), OK, "Applying dir B should succeed.")

	var recent: OptionButton = workstation.get_node("%SettingsRecentOption")
	var row: HBoxContainer = workstation.get_node("%SettingsRecentRow")
	workstation._populate_recent_dirs()

	assert_true(row.visible, "Recent row shows when there is another directory to switch to.")
	var a_index := _recent_option_index_for_path(recent, a)
	assert_gt(a_index, 0, "Dir A should be offered as a recent entry.")
	assert_eq(_recent_option_index_for_path(recent, b), -1, "The active dir B should be excluded from the list.")
	assert_eq(recent.get_item_text(a_index), a.get_file(), "Recent entries show the folder name.")

	# Picking A switches the active resource directory to A.
	recent.item_selected.emit(a_index)
	assert_eq(workstation.get_resource_root_dir(), a, "Picking a recent entry switches the resource directory.")


func test_settings_recent_dropdown_hidden_without_history() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(workstation._set_resource_root_dir("", false, false), OK, "Start with no configured resource directory.")
	workstation._resource_library.clear_recent_dirs()
	workstation._populate_recent_dirs()
	var row: HBoxContainer = workstation.get_node("%SettingsRecentRow")
	assert_false(row.visible, "Recent row stays hidden when there is no history to offer.")


func test_settings_recent_dropdown_clear_list_empties_history() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var a := _make_resource_fixture("recent_clear_a")
	var b := _make_resource_fixture("recent_clear_b")
	assert_eq(workstation._set_resource_root_dir(a, true, false), OK, "Apply dir A.")
	assert_eq(workstation._set_resource_root_dir(b, true, false), OK, "Apply dir B.")

	var recent: OptionButton = workstation.get_node("%SettingsRecentOption")
	workstation._populate_recent_dirs()
	var clear_index := _recent_option_index_for_text(recent, "Clear list")
	assert_gt(clear_index, 0, "A Clear list entry should be present when there are recents.")

	recent.item_selected.emit(clear_index)
	assert_eq(workstation._resource_library.get_recent_dirs().size(), 0, "Clear list empties the recent directories.")
	assert_false((workstation.get_node("%SettingsRecentRow") as HBoxContainer).visible, "Recent row hides after clearing the list.")


func test_workspace_open_uses_resource_browser_without_filesystem_escape() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	var root := _make_resource_fixture("resource_browser_terrain")
	workstation.set_editor(editor)
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
	var dir_label := dialog.find_child("ResourceBrowserDirectoryLabel", true, false) as Label
	var settings_shortcut := dialog.find_child("ResourceBrowserSettingsButton", true, false) as Button
	assert_true(dialog.visible, "Resource browser should open instead of going straight to native file browsing.")
	assert_not_null(list, "Resource browser should include a list.")
	assert_null(dialog.find_child("ResourceBrowserBrowseFilesButton", true, false), "Resource browser must not expose a native filesystem escape hatch.")
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


func test_fonts_workspace_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("resource_browser_fonts")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index font files.")
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.FONTS)
	await get_tree().process_frame

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsHost"), "Open Font...")
	assert_not_null(open_button, "Fonts workspace should expose Open Font.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Font Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as ItemList
	assert_not_null(list, "Font resource browser should include a list.")
	if list != null:
		assert_eq(list.item_count, 1, "Font browser should list FNT files from the resource directory.")
		assert_string_contains(list.get_item_text(0), "alpha", "Font resource rows should show the matching file.")


func test_strings_workspace_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("resource_browser_strings")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index strings files.")
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsHost"), "Open Strings...")
	assert_not_null(open_button, "Strings workspace should expose Open Strings.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Strings Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as ItemList
	assert_not_null(list, "Strings resource browser should include a list.")
	if list != null:
		assert_eq(list.item_count, 1, "Strings browser should list RTXT BIN files from the resource directory.")
		assert_string_contains(list.get_item_text(0), "alpha", "Strings resource rows should show the matching file.")


func test_workstation_opens_font_workspace_by_credits_font_name() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := ProjectSettings.globalize_path("res://../fixtures/fnt")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK,
		"Credits font integration should use the configured resource root.")

	assert_eq(workstation.open_font_workspace("Serpen24"), OK,
		"Credits integration should open a Nova font by name from the configured resource root.")
	await get_tree().process_frame

	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.FONTS,
		"Opening a credits font should switch to the Fonts workspace.")
	assert_eq(workstation.get_node("%ProjectLabel").text, "Serpen24",
		"The Fonts workspace title should show the opened font.")


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
	assert_null(dialog.find_child("ResourceBrowserBrowseFilesButton", true, false), "Missing resource roots should be fixed through Settings, not arbitrary file browsing.")
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
	assert_eq(workstation._set_resource_root_dir(root, true, true), OK, "Persisted resource directory should scan successfully.")

	var next_workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(next_workstation.get_resource_root_dir(), root, "New workstation instances should load the persisted resource directory.")


func test_terrain_editor_path_state_preserves_resource_directory() -> void:
	var root := _make_resource_fixture("resource_settings_path_preserve")
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(workstation._set_resource_root_dir(root, true, true), OK, "Persisted resource directory should scan successfully.")

	var editor = autofree(TerrainEditorScript.new())
	editor._remember_open_path(root.path_join("alpha.trn"))

	var next_workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(next_workstation.get_resource_root_dir(), root, "Saving terrain editor path state must preserve the shared resource directory.")


func test_startup_applies_persisted_resource_dir_to_shared_resource_root() -> void:
	# Regression: a fresh editor launch must push the PERSISTED resource dir into
	# the shared resource-root object on startup so authoring tools resolve files
	# through the same flat root that runtime loading uses.
	var root := _make_resource_fixture("veg_search_roots_startup")
	var w1 = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(w1._set_resource_root_dir(root, true, false), OK, "Persisting the resource dir should succeed.")

	var w2 = add_child_autofree(EditorWorkstationScene.instantiate())
	var resource_root: NovaResourceRoot = w2.get_resource_root()
	assert_eq(w2.get_resource_root_dir(), root, "Editor startup should load the persisted resource directory.")
	assert_eq(_norm_path(resource_root.get_root_dir()), _norm_path(root), "The shared resource root should be the persisted directory.")
	assert_eq(_norm_path(resource_root.resolve_file("alpha.3di")), _norm_path(root.path_join("alpha.3di")), "The startup resource root should resolve top-level files.")


func test_resource_root_inside_user_data_is_rejected_on_load() -> void:
	# A resource root that points inside the app user-data dir (e.g. a temp/test
	# path that leaked into the persisted state) must not be adopted on load, so
	# the resource browser shows a clean "no directory" state instead of a dead
	# internal path.
	var bogus := OS.get_user_data_dir().path_join("resource_settings_persist_bogus_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(bogus)
	NovaResourceDirSettings.set_resource_dir(bogus)

	var w2 = add_child_autofree(EditorWorkstationScene.instantiate())
	assert_eq(w2.get_resource_root_dir(), "", "A resource root inside the app user-data dir should be rejected on load.")
	DirAccess.remove_absolute(bogus)


func test_camera_button_exposes_global_viewport_settings() -> void:
	var editor: TerrainEditor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var workstation: EditorWorkstation = editor.get_node("CanvasLayer/EditorWorkstation")

	var top_bar := workstation.get_node("%TopBar") as PanelContainer
	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_eq(camera_button.get_parent(), environment_button.get_parent(), "Camera and environment should live in the same top-bar button rail.")
	assert_true(top_bar.is_ancestor_of(camera_button), "Camera settings should be launched from the top bar.")
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

	assert_false(environment_popup.visible, "Escape should close the visible top-bar popup.")
	assert_false(environment_button.button_pressed, "Environment button should release after Escape closes the popup.")


func test_terrain_workspace_exposes_project_save_and_export_actions() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)

	var actions_host: BoxContainer = workstation.get_node("%WorkspaceActionsHost")
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


func test_unsaved_changes_opens_native_confirmation_dialog() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation.prompt_unsaved_changes("quit")

	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Unsaved changes should open a native confirmation dialog.")
	if dialog == null:
		return
	assert_true(dialog.visible, "Unsaved confirmation should be shown.")
	assert_eq(dialog.dialog_text, "Save changes?", "Unsaved confirmation should ask the short question.")
	assert_eq(dialog.get_ok_button().text, "Save", "Save should stay the primary confirmation.")
	assert_eq(dialog.get_cancel_button().text, "Cancel", "Cancel should let the user keep editing.")
	assert_not_null(_find_button_by_text_deep(dialog, "Discard"), "Discard should remain a destructive custom action.")
	assert_null(workstation.get_node_or_null("%PromptHost"), "The hand-built prompt card should be gone.")
	assert_eq(dialog.theme, workstation.theme, "The dialog should resolve the shell theme explicitly.")


func test_unsaved_dialog_cancel_keeps_editing() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var stub := PromptEditorStub.new()
	autofree(stub)
	workstation.editor = stub

	workstation.prompt_unsaved_changes("quit")
	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Unsaved changes should open a native confirmation dialog.")
	if dialog == null:
		return
	dialog.canceled.emit()

	assert_true(stub.cancelled, "Cancel/Escape should cancel the pending action (keep editing).")
	assert_false(stub.discarded, "Cancel should not discard.")
	assert_false(stub.saved, "Cancel should not save.")


func test_unsaved_dialog_confirm_saves_and_discard_action_discards() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var stub := PromptEditorStub.new()
	autofree(stub)
	workstation.editor = stub

	workstation.prompt_unsaved_changes("quit")
	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Unsaved changes should open a native confirmation dialog.")
	if dialog == null:
		return

	dialog.confirmed.emit()
	assert_true(stub.saved, "Confirm should save the pending action.")
	assert_false(stub.discarded, "Confirm should not discard.")

	dialog.custom_action.emit(&"discard")
	assert_true(stub.discarded, "The Discard custom action should discard the pending action.")


func test_export_flavor_opens_native_dialog_with_format_toggles() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation._show_export_flavor_dialog("C:/Exports/TestTerrain")

	var dialog = workstation.find_child("ExportFlavorDialog", true, false)
	assert_not_null(dialog, "Export should open a native flavor dialog.")
	if dialog == null:
		return
	assert_true(dialog.visible, "Export flavor dialog should be shown.")
	assert_eq(dialog.get_ok_button().text, "Export", "Export should stay the primary action.")
	assert_eq(dialog.get_cancel_button().text, "Cancel", "Cancel should stay the secondary action.")
	var bhd := _find_button_by_text(dialog, "BHD")
	var jodfx := _find_button_by_text(dialog, "JO/DFX")
	assert_not_null(bhd, "Export should offer the BHD format.")
	assert_not_null(jodfx, "Export should offer the JO/DFX format.")
	if jodfx != null:
		assert_true(jodfx.button_pressed, "JO/DFX should be the default export flavor.")
	if bhd != null:
		assert_false(bhd.button_pressed, "BHD should not be selected by default.")
	assert_eq(dialog.get_flavor(), 1, "Default flavor should map to DFX_JO (1).")
	assert_null(workstation.get_node_or_null("%PromptHost"), "The hand-built prompt card should be gone.")
	assert_eq(dialog.theme, workstation.theme, "The dialog should resolve the shell theme explicitly.")


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
	assert_null(dock.get_node_or_null("%CameraTab"), "Camera controls should move to the global top-bar popup.")
	assert_null(dock.get_node_or_null("Tabs/Document"), "The old Document tab should be removed.")
	assert_null(dock.get_node_or_null("%ViewTab"), "The old View tab should be removed.")
	assert_null(dock.get_node_or_null("%WaterVisibleToggle"), "The water-plane toggle should move out of the dock.")
	assert_null(dock.get_node_or_null("%HorizonSpin"), "Horizon controls should be removed from the dock.")
	assert_null(dock.get_node_or_null("%ColormapToggle"), "Colormap visibility toggle should be removed from the dock.")
	assert_null(dock.get_node_or_null("%WireframeToggle"), "Wireframe toggle should be removed from the dock.")


func test_asset_dock_and_inspectors_do_not_poll_when_idle() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_false(dock.is_processing(), "Asset dock should sync from editor state changes instead of idle polling.")


func test_asset_dock_slot_card_title_does_not_share_row_with_buttons() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	# At the ~360px dock width the title and the Load/Reset buttons cannot share a
	# horizontal row without squeezing the title into one-token-per-line wrapping.
	# The title should own a full-width row so multi-word slot labels stay readable.
	var title := _find_label_by_text(dock, "Shading 1 / near")
	assert_not_null(title, "Slot card should expose the slot title label.")
	if title == null:
		return
	var parent := title.get_parent()
	assert_not_null(parent, "Slot title should be parented.")
	var has_button_sibling := false
	for sibling in parent.get_children():
		if sibling is Button:
			has_button_sibling = true
			break
	assert_false(has_button_sibling, "Slot title should sit on its own row, not share it with the Load/Reset buttons.")


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


# --- Phase 2: dialog / popover consistency ---

func test_corner_popovers_share_popover_panel_and_close_button() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	for popup_name in ["%CameraPopup", "%EnvironmentPopup", "%SettingsPopup"]:
		var popup: Node = workstation.get_node(popup_name)
		assert_true(popup is PopoverPanel, "%s should be a shared PopoverPanel." % popup_name)

	for close_name in ["%CameraPopupClose", "%EnvironmentPopupClose", "%SettingsPopupClose"]:
		var close_button := workstation.get_node(close_name) as Button
		assert_eq(close_button.text, PopoverPanel.CLOSE_GLYPH, "%s should use the standard close glyph." % close_name)
		assert_eq(close_button.focus_mode, Control.FOCUS_NONE, "%s should not steal focus." % close_name)


func test_cdep_violations_opens_native_dialog_and_flattens_on_confirm() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var flattened := [false]
	var fix := func() -> void:
		flattened[0] = true

	workstation.prompt_cdep_violations(3, fix)

	var dialog := workstation.find_child("CdepFlattenDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "CDEP violations should open a native confirmation dialog.")
	if dialog == null:
		return
	assert_true(dialog.visible, "CDEP confirmation should be shown.")
	assert_eq(dialog.title, "Flatten before export?", "CDEP confirmation should ask whether to flatten.")
	assert_string_contains(dialog.dialog_text, "3 areas exceed", "CDEP confirmation should report the violation count.")
	assert_string_contains(dialog.dialog_text, "BHD exports are unaffected", "CDEP confirmation should note BHD is safe.")
	assert_eq(dialog.get_ok_button().text, "Flatten automatically", "CDEP primary should flatten.")
	assert_eq(dialog.get_cancel_button().text, "Leave as-is", "CDEP secondary should leave the terrain as-is.")

	dialog.confirmed.emit()
	assert_true(flattened[0], "Confirming CDEP should run the supplied fix callback.")


func test_resource_browser_uses_theme_not_handcoded_styleboxes() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	var root := _make_resource_fixture("resource_browser_theme")
	workstation.set_editor(editor)
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index the configured resource directory.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsHost"), "Open Terrain...")
	assert_not_null(open_button, "Terrain workspace should expose Open Terrain.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Open should create the resource browser.")
	if dialog == null:
		return
	assert_false(dialog.borderless, "Resource browser should use the native themed window frame, not a borderless workaround.")
	assert_false(dialog.has_theme_stylebox_override("panel"), "Resource browser should rely on the theme, not a hand-coded panel stylebox.")
	assert_null(dialog.find_child("ResourceBrowserTitleBar", true, false), "Resource browser should drop its custom title bar in favor of the native one.")
	assert_false(dialog.title.is_empty(), "The native dialog should carry the open title.")


func test_popover_close_button_dismisses_via_shared_signal() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var settings_button: Button = workstation.get_node("%SettingsToggleButton")
	settings_button.toggled.emit(true)
	await get_tree().process_frame

	var settings_popup := workstation.get_node("%SettingsPopup") as PopoverPanel
	assert_true(settings_popup.visible, "Settings popover should open from its toolbar button.")
	var close := workstation.get_node("%SettingsPopupClose") as Button
	close.pressed.emit()

	assert_false(settings_popup.visible, "Pressing the shared close button should dismiss the popover.")
	assert_false(settings_button.button_pressed, "The toolbar toggle should release when the popover closes.")


func test_file_and_dir_dialogs_share_one_native_dialog() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation._open_file_dialog("Open file", PackedStringArray(), func(_p): pass)
	workstation._open_dir_dialog("Open dir", func(_p): pass)

	var dialogs := []
	for child in workstation.get_children():
		if child is FileDialog:
			dialogs.append(child)
	assert_eq(dialogs.size(), 1, "File and directory pickers should share one cached native dialog instead of one per open.")
	if dialogs.size() == 1:
		var dialog := dialogs[0] as FileDialog
		assert_eq(dialog.file_mode, FileDialog.FILE_MODE_OPEN_DIR, "The shared dialog should switch to directory mode for _open_dir_dialog.")
		dialog.hide()


# --- Phase 1: resizable + responsive shell ---

func test_body_row_uses_nested_hsplit_containers_for_resizable_docks() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var body: Node = workstation.get_node_or_null("WorkstationLayout/BodyRow")
	assert_not_null(body, "Shell should keep a BodyRow row.")
	assert_true(body is HSplitContainer, "BodyRow should be an HSplitContainer so the left dock can be dragged.")
	var center_right: Node = workstation.get_node_or_null("%CenterRightSplit")
	assert_true(center_right is HSplitContainer, "The viewport/right-dock split should be an HSplitContainer.")


func test_shell_panels_still_resolve_by_unique_name_after_split_refactor() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var left: Node = workstation.get_node_or_null("%LeftLane")
	var viewport_lane: Node = workstation.get_node_or_null("%ViewportLane")
	var viewport_host: Node = workstation.get_node_or_null("%ViewportHost")
	var dock: Node = workstation.get_node_or_null("%AssetDock")
	assert_not_null(left, "%LeftLane should still resolve after the split refactor.")
	assert_not_null(viewport_lane, "%ViewportLane should still resolve after the split refactor.")
	assert_not_null(viewport_host, "%ViewportHost should still resolve after the split refactor.")
	assert_not_null(dock, "%AssetDock should still resolve after the split refactor.")
	if viewport_lane != null and viewport_host != null:
		assert_true((viewport_lane as Node).is_ancestor_of(viewport_host), "ViewportHost should stay parented under ViewportLane.")


func test_split_size_flags_route_window_growth_to_viewport() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var left := workstation.get_node("%LeftLane") as Control
	var viewport_lane := workstation.get_node("%ViewportLane") as Control
	var dock := workstation.get_node("%AssetDock") as Control
	var center_right := workstation.get_node_or_null("%CenterRightSplit") as Control
	assert_not_null(center_right, "Shell should wrap the viewport and right dock in a center/right split.")
	if center_right == null:
		return
	assert_eq(left.size_flags_horizontal, Control.SIZE_FILL, "Left lane should keep its dragged width, not absorb window growth.")
	assert_eq(center_right.size_flags_horizontal, Control.SIZE_EXPAND_FILL, "The center/right split should absorb body width on resize.")
	assert_eq(viewport_lane.size_flags_horizontal, Control.SIZE_EXPAND_FILL, "The viewport should absorb resize within the center/right split.")
	assert_eq(dock.size_flags_horizontal, Control.SIZE_FILL, "The asset dock should keep its dragged width.")


func test_left_lane_and_asset_dock_have_smaller_minimum_floors() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var left := workstation.get_node("%LeftLane") as Control
	var dock := workstation.get_node("%AssetDock") as Control
	assert_eq(left.custom_minimum_size.x, 240.0, "Left lane should shrink to a 240px floor for small windows.")
	assert_eq(dock.custom_minimum_size.x, 280.0, "Asset dock should shrink to a 280px floor for small windows.")


func test_split_offsets_round_trip_through_layout_state() -> void:
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))

	var lib = EditorResourceLibrary.new()
	var fresh = lib.load_layout_state()
	assert_false(bool(fresh["has_left"]), "A fresh config should report an unset left split offset.")
	assert_false(bool(fresh["has_right"]), "A fresh config should report an unset right split offset.")

	lib.save_layout_state(123, -207)
	var lib2 = EditorResourceLibrary.new()
	var loaded = lib2.load_layout_state()
	assert_true(bool(loaded["has_left"]), "Saved left split offset should be reported as present.")
	assert_eq(int(loaded["left"]), 123, "Saved left split offset should round-trip through the layout state.")
	assert_true(bool(loaded["has_right"]), "Saved right split offset should be reported as present.")
	assert_eq(int(loaded["right"]), -207, "A negative right split offset should round-trip (offsets can be negative).")


func test_split_layout_applies_persisted_offsets_on_load() -> void:
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))

	# Lay out a first shell, simulate the user dragging both dividers to valid
	# (clamped) offsets, then persist them the way drag_ended would.
	var first = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	await get_tree().process_frame
	var first_body := first.get_node("%BodyRow") as SplitContainer
	var first_right := first.get_node("%CenterRightSplit") as SplitContainer
	first_body.split_offset = first_body.split_offset + 40
	first_body.clamp_split_offset()
	first_right.split_offset = first_right.split_offset - 30
	first_right.clamp_split_offset()
	var target_left: int = first_body.split_offset
	var target_right: int = first_right.split_offset
	first._save_split_layout()

	# A fresh shell at the same size should restore those dragged widths.
	var second = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	await get_tree().process_frame
	var second_body := second.get_node("%BodyRow") as SplitContainer
	var second_right := second.get_node("%CenterRightSplit") as SplitContainer
	assert_eq(second_body.split_offset, target_left, "A new shell should restore the persisted left split offset.")
	assert_eq(second_right.split_offset, target_right, "A new shell should restore the persisted right split offset.")
