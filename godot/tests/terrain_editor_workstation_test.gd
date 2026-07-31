extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const TerrainWorkspaceScript = preload("res://modtools/terrain/terrain_workspace.gd")
const TerrainEditorAssetDockScene = preload("res://modtools/terrain/ui/editor_asset_dock.tscn")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const EnvironmentInspectorScript = preload("res://modtools/environment/environment_inspector.gd")
const QuadrantBoardScript = preload("res://modtools/terrain/ui/widgets/quadrant_board.gd")
const MissionInspectorScript = preload("res://modtools/mission/mission_inspector.gd")
const LinkPayloadScript = preload("res://modtools/framework/links/link_payload.gd")
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const FIXTURE_CACHE_DIR := "opennova_test"

var _saved_state_config := PackedByteArray()
var _had_state_config := false


# Minimal terrain-editor double for the dirty-replace guard tests: dirty, no
# project directory yet, records saves/opens. Avoids set_editor()'s full bind
# cost being the point of the test.
class DirtyTerrainStub:
	extends Node
	var is_dirty := true
	var opened := PackedStringArray()
	var saved_dirs := PackedStringArray()

	func is_export_running() -> bool:
		return false

	func set_viewport_active(_active: bool, _edit_input: bool) -> void:
		pass

	func open_trn(path: String) -> Error:
		opened.append(path)
		return OK

	func new_terrain() -> void:
		pass

	func save_project_to_current_dir() -> Error:
		# No project directory yet - the shell's save_then must fall back to Save As.
		return ERR_INVALID_PARAMETER

	func save_project(dir_path: String) -> Error:
		saved_dirs.append(dir_path)
		return OK

	func has_current_project_dir() -> bool:
		return false

	func get_current_project_dir() -> String:
		return ""

	func get_last_save_dir() -> String:
		return ""


# Records begin_export_terrain calls: pins the export-confirm to the workspace
# that opened the flavor dialog (never the tab active at confirmation).
class ExportRecordingTerrainStub:
	extends Node
	var exports: Array = []  # [dir_path, flavor] per call
	var is_dirty := false  # read by the workspace's unsaved-state checks

	func is_export_running() -> bool:
		return false

	func set_viewport_active(_active: bool, _edit_input: bool) -> void:
		pass

	func begin_export_terrain(dir_path: String, flavor: int) -> Error:
		exports.append([dir_path, flavor])
		return OK

	func get_last_export_dir() -> String:
		return ""

	func has_current_project_dir() -> bool:
		return false

	func get_current_project_dir() -> String:
		return ""

	func get_last_save_dir() -> String:
		return ""


func before_each() -> void:
	_had_state_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_state_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) if _had_state_config else PackedByteArray()


func after_each() -> void:
	# A shell freed by GUT's autofree dies AFTER this hook - and a floating
	# panel's _exit_tree persists state, which would re-pollute the config we
	# restore below (a runtime error mid-test skips the in-body teardown). Kill
	# any lingering children NOW so their exit-time writes land first.
	for child in get_children():
		if child is EditorApp or child is TerrainEditor \
				or (child is Control and child.get_script() == EditorWorkstationScript):
			child.queue_free()
	await get_tree().process_frame
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


func _workspace_action_texts(mount: Node) -> Array:
	var texts := []
	for child in mount.get_children():
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


func _overflow_popup(mount: Node) -> PopupMenu:
	var more := mount.find_child("MoreActionsButton", true, false) as MenuButton
	return null if more == null else more.get_popup()


func _overflow_item_texts(popup: PopupMenu) -> Array:
	var texts := []
	for i in popup.item_count:
		texts.append(popup.get_item_text(i))
	return texts


func _overflow_item_disabled(popup: PopupMenu, text: String) -> bool:
	for i in popup.item_count:
		if popup.get_item_text(i) == text:
			return popup.is_item_disabled(i)
	return true


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
	_write_fixture_file(root.path_join("alpha.ptl"), "ptl")
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
	assert_false(top_bar.is_ancestor_of(workspace_rail), "The workspace dock bar moved out of the top bar into the body.")
	var workspace_bar := workstation.get_node_or_null("%WorkspaceBar") as PanelContainer
	assert_not_null(workspace_bar, "Shell should expose the vertical workspace dock bar.")
	if workspace_bar != null:
		assert_true(workspace_bar.is_ancestor_of(workspace_rail), "The workspace rail lives inside the dock bar.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION, "Mission should be the default workspace.")
	var mission_workspace = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.MISSION)
	assert_true(bool(mission_workspace.get("_mission_preview_context_active")),
		"The default workspace must receive activate(), just like a workspace selected later.")

	var row_texts := []
	for child in workspace_rail.get_children():
		if child is Button:
			var bar_label := child.find_child("BarButtonLabel", true, false) as Label
			row_texts.append(bar_label.text if bar_label != null else "")
	assert_eq(row_texts, ["Mission", "Terrain", "Object", "Avatars", "Fonts", "Credits", "Strings", "Menus", "HUD", "Music", "Particles", "Sound"],
		"The bar should list every viewport workspace; Environment stays on its top-bar toggle, not the bar.")

	assert_false(_has_label_text(workspace_rail, "World"), "Workspace groups should use separators, not inline category words.")
	assert_false(_has_label_text(workspace_rail, "Interface"), "Workspace groups should not read like a sentence.")
	assert_gte(_direct_child_count_of_type(workspace_rail, "HSeparator"), 2,
		"The vertical bar should keep visual separation between World, Interface, Audio, and Atmosphere.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.CREDITS)
	await get_tree().process_frame

	var actions_mount: BoxContainer = workstation.get_node("%WorkspaceActionsMount")
	assert_true(top_bar.is_ancestor_of(actions_mount), "Workspace document actions should live in the top bar.")
	assert_not_null(top_bar.find_child("ContextSpacer", true, false),
		"Top bar uses an expanding spacer to push the document actions and global buttons to the right.")
	var global_buttons := workstation.get_node("%GlobalButtonRail") as BoxContainer
	assert_true(actions_mount.get_index() < global_buttons.get_index(),
		"Document actions should sit immediately before the global icon buttons on the right.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.CREDITS,
		"Credits should become the active workspace.")
	assert_eq(workstation.get_node("%ContextDocLabel").text, "untitled",
		"Fresh Credits workspace should show its untitled document in the context header.")
	assert_true(_workspace_action_texts(actions_mount).has("Open Credits..."),
		"Credits should expose an open action.")
	var credits_overflow := _overflow_popup(actions_mount)
	assert_not_null(credits_overflow, "Credits should fold secondary actions into the More menu.")
	assert_true(_overflow_item_texts(credits_overflow).has("Save Credits As..."),
		"Credits should keep save-as reachable from the More menu.")


func test_every_workspace_rail_button_has_an_icon() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var workspace_rail: BoxContainer = workstation.get_node("%WorkspaceRail")
	var checked := 0
	for child in workspace_rail.get_children():
		if child is Button:
			var icon_rect := child.find_child("BarButtonIcon", true, false) as TextureRect
			assert_not_null(icon_rect, "each dock button stacks a TextureRect icon over its label")
			if icon_rect != null:
				assert_not_null(icon_rect.texture, "the dock button icon should resolve to a workspace texture")
			checked += 1
	assert_eq(checked, 12, "all twelve workspace rows checked")


func test_icon_library_resolves_every_registered_icon_id() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	for def_v in workstation._workspace_defs():
		var def: WorkspaceDef = def_v
		assert_true(def.icon_id != &"", "every workspace registry row declares an icon id")
		assert_not_null(EditorIconLibrary.resolve(def.icon_id),
			"icon id '%s' should resolve to a texture" % String(def.icon_id))
	assert_null(EditorIconLibrary.resolve(&"no_such_icon"),
		"an unknown id degrades to text-only, never errors")
	assert_null(EditorIconLibrary.resolve(&""), "the empty id resolves to null")


func test_action_and_toggle_buttons_have_icons() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	# Action buttons rebuild on a workspace switch (_refresh_workspace_surface).
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.CREDITS)
	await get_tree().process_frame
	var buttons: Dictionary = workstation._top_action_bar.buttons()
	assert_gt(buttons.size(), 0, "the active workspace exposes at least one action button")
	for action_id in buttons:
		var btn := buttons[action_id] as Button
		assert_not_null(btn.icon, "action button '%s' should carry its action icon" % btn.text)
	assert_not_null(workstation._settings_toggle_button.icon, "settings toggle keeps an icon")
	assert_not_null(workstation._camera_toggle_button.icon, "camera toggle keeps an icon")
	assert_not_null(workstation._browser_toggle_button.icon, "browser toggle carries its icon")


func test_mission_workspace_exposes_document_actions_and_inspector() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	var actions_mount: BoxContainer = workstation.get_node("%WorkspaceActionsMount")
	var inspector_mount: Control = workstation.get_node("%InspectorMount")
	var asset_dock: Control = workstation.get_node("%AssetDock")
	assert_eq(workstation.get_node("%ContextWorkspaceLabel").text, "Mission", "An unloaded Mission workspace names itself in the context header.")
	assert_eq(workstation.get_node("%ContextDocLabel").text, "", "With no mission open the doc half stays empty (no 'Mission / Mission').")
	assert_true(asset_dock.visible, "Mission authoring parks its per-selection editor + Mission form in the shared right dock.")
	assert_null(workstation.get_node_or_null("%FileMenu"), "Global File menu should be removed.")
	assert_null(workstation.get_node_or_null("%SaveButton"), "Global Save button should be removed.")
	assert_null(workstation.get_node_or_null("%ExportButton"), "Global Export button should be removed.")
	# Authoring lands New (create-from-scratch) + Save / Save As alongside Open. The shell builds
	# these buttons up front (before a mission is loaded); Save / Save As sit disabled until there
	# is a loaded / dirtied mission. The real MissionInspector replaces the old "Coming soon" stub:
	# its left pane (mode tabs + lists) mounts in %InspectorMount, while its Selection | Mission
	# editor reparents into the shared right dock.
	assert_true(actions_mount.visible, "Mission should expose its document actions.")
	assert_eq(_workspace_action_texts(actions_mount), ["New Mission", "Open Mission...", "Save Mission", "More"],
		"Mission exposes New + Open + Save as buttons, with the secondary actions folded into More.")
	assert_true(_overflow_item_texts(_overflow_popup(actions_mount)).has("Save Mission As..."),
		"Mission keeps Save As reachable from the More menu.")
	# The left pane mounts the real MissionInspector node (its Selection | Mission editor reparents
	# into the dock). The prior workspace's inspector children are queue_free'd, which is deferred,
	# so they can still coexist this same frame; find the inspector by script rather than by index.
	var inspector: Node = null
	for child in inspector_mount.get_children():
		if child.get_script() == MissionInspectorScript:
			inspector = child
			break
	assert_not_null(inspector, "Mission should mount its real inspector panel in the left pane, not a placeholder.")
	assert_false(_has_label_text(inspector_mount, "Coming soon"), "The coming-soon stub should be gone.")
	assert_gt(asset_dock.get_child_count(), 0, "Mission authoring mounts its Selection + Mission editor in the right dock.")


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
	assert_eq(index.get_resource_files("particle").size(), 1, "PTL files should be indexed as particle resources.")
	assert_eq(index.get_resource_files("strings").size(), 1, "RTXT BIN files should be indexed as strings resources.")
	assert_eq(index.get_resource_files("glb").size(), 0, "GLB files should no longer be indexed.")
	assert_eq(index.get_resource_files("all").size(), 9, "All openable resources should exclude GLB and non-RTXT BIN blobs.")
	assert_eq(String((index.get_resource_files("object_model")[0] as Dictionary).get("relative_path", "")), "alpha.3di", "Object model entries should keep root-relative paths.")
	assert_eq(String((index.get_resource_files("particle")[0] as Dictionary).get("relative_path", "")), "alpha.ptl", "Particle entries should keep root-relative paths.")
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
	workstation._settings_panel.apply_resource_settings(true, false)

	assert_eq(workstation.get_resource_root_dir(), root, "Settings should apply the resource directory.")
	assert_eq(workstation.get_resource_index().get_resource_files("terrain").size(), 1, "Flat scans should include top-level terrain files.")


# The resource browser renders rows in a Tree (Name / Type / Size / Modified).
# Collect the visible row items in display order.
func _browser_rows(list: Tree) -> Array:
	var rows: Array = []
	if list == null:
		return rows
	var root := list.get_root()
	if root == null:
		return rows
	var child := root.get_first_child()
	while child != null:
		rows.append(child)
		child = child.get_next()
	return rows


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
	workstation._settings_panel._populate_recent_dirs()

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
	workstation._settings_panel._populate_recent_dirs()
	var row: HBoxContainer = workstation.get_node("%SettingsRecentRow")
	assert_false(row.visible, "Recent row stays hidden when there is no history to offer.")


func test_settings_recent_dropdown_clear_list_empties_history() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var a := _make_resource_fixture("recent_clear_a")
	var b := _make_resource_fixture("recent_clear_b")
	assert_eq(workstation._set_resource_root_dir(a, true, false), OK, "Apply dir A.")
	assert_eq(workstation._set_resource_root_dir(b, true, false), OK, "Apply dir B.")

	var recent: OptionButton = workstation.get_node("%SettingsRecentOption")
	workstation._settings_panel._populate_recent_dirs()
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
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should use the configured resource directory.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsMount"), "Open Terrain...")
	assert_not_null(open_button, "Terrain workspace should expose Open Terrain.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Open should create the in-editor resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as Tree
	var dir_label := dialog.find_child("ResourceBrowserDirectoryLabel", true, false) as Label
	var settings_shortcut := dialog.find_child("ResourceBrowserSettingsButton", true, false) as Button
	assert_true(dialog.visible, "Resource browser should open instead of going straight to native file browsing.")
	assert_not_null(list, "Resource browser should include a list.")
	assert_null(dialog.find_child("ResourceBrowserBrowseFilesButton", true, false), "Resource browser must not expose a native filesystem escape hatch.")
	assert_not_null(dir_label, "Resource browser should show the active resource directory.")
	assert_not_null(settings_shortcut, "Resource browser should include a Settings shortcut node.")
	var rows := _browser_rows(list)
	assert_eq(rows.size(), 1, "Terrain browser should list TRN files from the resource directory.")
	if rows.size() > 0:
		assert_string_contains((rows[0] as TreeItem).get_text(0).to_lower(), "alpha", "Resource rows should show the matching terrain.")
	if dir_label != null:
		assert_string_contains(dir_label.text, root, "Directory label should show the configured root.")
	if settings_shortcut != null:
		assert_false(settings_shortcut.visible, "Settings shortcut should hide when resources are available.")
	assert_false(dialog.get_ok_button().disabled, "Open should be enabled once a row is auto-selected.")


func test_resource_browser_open_button_opens_selection() -> void:
	# Regression: AcceptDialog hides itself BEFORE emitting `confirmed`, so the OK
	# ("Open") button must still open the auto-selected row. A visibility-gated
	# guard once swallowed this, leaving only double-click (item_activated) working.
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	var root := _make_resource_fixture("resource_browser_ok_button")
	workstation.set_editor(editor)
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK)
	var workspace = workstation._get_active_workspace()
	assert_not_null(workspace, "Terrain workspace should be active by default.")
	if workspace == null:
		return
	var picked := {"path": ""}
	workstation._resource_browser.open(workspace, func(p: String) -> void:
		picked["path"] = p
	)
	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Browser should open.")
	if dialog == null:
		return
	assert_true(dialog.visible, "Browser should be visible before confirming.")
	dialog.get_ok_button().pressed.emit()
	assert_false(picked["path"].is_empty(), "The Open button should open the auto-selected resource.")
	assert_string_contains(picked["path"].to_lower(), "alpha", "Open should pass the selected resource path.")


func test_fonts_workspace_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("resource_browser_fonts")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index font files.")
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.FONTS)
	await get_tree().process_frame

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsMount"), "Open Font...")
	assert_not_null(open_button, "Fonts workspace should expose Open Font.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Font Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as Tree
	assert_not_null(list, "Font resource browser should include a list.")
	var rows := _browser_rows(list)
	assert_eq(rows.size(), 1, "Font browser should list FNT files from the resource directory.")
	if rows.size() > 0:
		assert_string_contains((rows[0] as TreeItem).get_text(0).to_lower(), "alpha", "Font resource rows should show the matching file.")


func test_strings_workspace_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := _make_resource_fixture("resource_browser_strings")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index strings files.")
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsMount"), "Open Strings...")
	assert_not_null(open_button, "Strings workspace should expose Open Strings.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Strings Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as Tree
	assert_not_null(list, "Strings resource browser should include a list.")
	var rows := _browser_rows(list)
	assert_eq(rows.size(), 1, "Strings browser should list RTXT BIN files from the resource directory.")
	if rows.size() > 0:
		assert_string_contains((rows[0] as TreeItem).get_text(0).to_lower(), "alpha", "Strings resource rows should show the matching file.")


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
	assert_eq(workstation.get_node("%ContextDocLabel").text, "Serpen24",
		"The Fonts workspace title should show the opened font.")


func test_workstation_opens_menu_workspace_by_action_target() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var root := ProjectSettings.globalize_path("res://../fixtures/mnu")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK,
		"Menu action integration should use the configured resource root.")

	var err: Error = workstation.open_menu_workspace("sp.mnu", "SINGLE_PLAYER")
	assert_eq(err, OK,
		"A cross-menu action should open its declared target menu file.")
	if err != OK:
		return
	await get_tree().process_frame

	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MNU,
		"Opening a cross-menu action should switch to the Menus workspace.")
	assert_eq(workstation.get_node("%ContextDocLabel").text, "jo_sp",
		"The Menus workspace title should show the opened menu.")
	var ws = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.MNU)
	assert_not_null(ws, "The Menus workspace instance exists.")
	if ws == null:
		return
	assert_eq(ws._document.current_path.get_file(), "jo_sp.mnu", "The action target should resolve to the matching menu resource.")
	assert_eq(ws._document.resource.get_screen_name(ws._editor.get_selected_id()), "SINGLE_PLAYER",
		"The target screen is selected in the Menus workspace.")


func test_workspace_open_without_resource_dir_shows_empty_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	assert_eq(workstation._set_resource_root_dir("", false, false), OK, "Test should clear the resource directory without persisting it.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsMount"), "Open Terrain...")
	assert_not_null(open_button, "Terrain workspace should expose Open Terrain.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Open should still create the resource browser without a root.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as Tree
	var hint := dialog.find_child("ResourceBrowserHint", true, false) as Label
	var settings_shortcut := dialog.find_child("ResourceBrowserSettingsButton", true, false) as Button
	assert_null(dialog.find_child("ResourceBrowserBrowseFilesButton", true, false), "Missing resource roots should be fixed through Settings, not arbitrary file browsing.")
	assert_eq(_browser_rows(list).size(), 0, "No resource directory should produce no rows.")
	if hint != null:
		assert_string_contains(hint.text, "No resource directory selected", "Empty state should name the missing resource directory.")
	if settings_shortcut != null:
		assert_true(settings_shortcut.visible, "Settings shortcut should be visible when no resource directory is configured.")
	assert_true(dialog.get_ok_button().disabled, "Open should stay disabled without a selected resource.")


func test_environment_sun_popup_exposes_env_document_controls() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT).set_environment_editor(environment_editor)

	var sun_button: Button = workstation.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%EnvironmentPopup")
	var actions_mount: VBoxContainer = workstation.get_node("%EnvironmentActionsMount")
	var save_button := _find_button_by_text(actions_mount, "Save Environment")
	var inspector_mount: Control = workstation.get_node("%EnvironmentInspectorMount")
	var inspector := inspector_mount.get_child(inspector_mount.get_child_count() - 1)
	assert_true(popup.visible, "The sun button should show the environment popup.")
	assert_true(sun_button.button_pressed, "The sun button should stay pressed while the popup is visible.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION, "Opening environment should not switch the active workspace.")
	assert_eq(workstation.get_node("%ContextWorkspaceLabel").text, "Mission", "Mission keeps its context-header name when no mission is loaded.")
	assert_eq(workstation.get_node("%EnvironmentPopupTitle").text, "untitled", "Environment should own the popup title.")
	assert_eq(_workspace_action_texts(actions_mount), ["New Environment", "Open Environment...", "Save Environment", "Save Environment As..."], "Environment popup should expose document actions without a separate export.")
	assert_not_null(save_button, "Environment popup should expose Save Environment.")
	assert_true(save_button.disabled, "Clean new environments should not enable Save until changed.")
	assert_null(_find_button_by_text(actions_mount, "Export Environment..."), "Environment should not advertise export separately from save.")
	assert_true(inspector.get_script() == EnvironmentInspectorScript, "Environment popup should build its inspector instead of a placeholder.")

	environment_editor.env_file.set_env_name("storm_test")
	workstation.sync_from_editor_state()

	assert_eq(workstation.get_node("%EnvironmentPopupTitle").text, "storm_test*", "Environment edits should dirty the popup document title.")
	# Save now requires a path (mirrors fonts/credits): a dirty-but-unsaved env
	# keeps Save disabled and routes through Save As until a path is set.
	assert_true(save_button.disabled, "Dirty environments without a path should still gate Save behind Save As.")

	environment_editor.set_current_path("user://env_popup_test.env")
	workstation.sync_from_editor_state()
	assert_false(save_button.disabled, "Dirty environments with a path should enable Save.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.ENVIRONMENT)
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION, "The old Environment workspace id should open the popup instead of changing workspaces.")


func test_environment_open_uses_resource_browser() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	var root := _make_resource_fixture("resource_browser_environment")
	environment_editor.create_default_environment(false)
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT).set_environment_editor(environment_editor)
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index environment files.")

	var sun_button: Button = workstation.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame
	var open_button := _find_button_by_text(workstation.get_node("%EnvironmentActionsMount"), "Open Environment...")
	assert_not_null(open_button, "Environment popup should expose Open Environment.")
	if open_button == null:
		return
	open_button.pressed.emit()

	var dialog := workstation.find_child("ResourceBrowserDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Environment Open should use the shared resource browser.")
	if dialog == null:
		return
	var list := dialog.find_child("ResourceBrowserList", true, false) as Tree
	assert_not_null(list, "Environment resource browser should include a list.")
	var rows := _browser_rows(list)
	assert_eq(rows.size(), 1, "Environment browser should list ENV files from the resource directory.")
	if rows.size() > 0:
		assert_string_contains((rows[0] as TreeItem).get_text(0).to_lower(), "alpha", "Environment resource rows should show the matching file.")


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
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation

	var top_bar := workstation.get_node("%TopBar") as PanelContainer
	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_eq(camera_button.get_parent(), environment_button.get_parent(), "Camera and environment should live in the same top-bar button rail.")
	assert_true(top_bar.is_ancestor_of(camera_button), "Camera settings should be launched from the top bar.")
	assert_true(camera_button.get_index() < environment_button.get_index(), "Camera should sit immediately before Environment.")

	camera_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%CameraPopup")
	var settings_mount: Control = workstation.get_node("%CameraSettingsMount")
	var settings_panel: Control = settings_mount.get_child(0)
	var fly_speed_spin: SpinBox = settings_panel.get_node("%FlySpeedSpin")
	var near_plane_spin: SpinBox = settings_panel.get_node("%NearPlaneSpin")
	var far_plane_spin: SpinBox = settings_panel.get_node("%FarPlaneSpin")
	assert_true(popup.visible, "The camera button should show the camera popup.")
	assert_true(camera_button.button_pressed, "The camera button should stay pressed while the popup is visible.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION, "Opening camera settings should not switch workspaces.")

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
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation
	var mount: Control = workstation.get_node("%ViewportMount")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	await get_tree().process_frame

	var preview := _find_node_by_name(mount, "ObjectPreview")
	var object_camera := _find_node_by_type(preview, "Camera3D") as Camera3D
	var terrain_camera := editor.camera
	assert_not_null(preview, "Object workspace should mount its preview in the shared viewport mount.")
	assert_not_null(object_camera, "Object preview should expose a fly camera for global camera settings.")
	assert_not_null(terrain_camera, "Terrain editor should still keep its camera while Object is active.")
	if object_camera == null or terrain_camera == null:
		return
	var original_terrain_speed: float = terrain_camera.fly_speed

	var camera_button: Button = workstation.get_node("%CameraToggleButton")
	camera_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%CameraPopup")
	var settings_mount: Control = workstation.get_node("%CameraSettingsMount")
	var settings_panel: Control = settings_mount.get_child(0)
	var fly_speed_spin: SpinBox = settings_panel.get_node("%FlySpeedSpin")
	assert_true(popup.visible, "Camera settings should open while Object is active.")

	fly_speed_spin.value_changed.emit(43.0)

	assert_eq(object_camera.fly_speed, 43.0, "Global camera popup should edit the Object preview camera when Object is active.")
	assert_eq(terrain_camera.fly_speed, original_terrain_speed, "Object camera edits should not mutate the inactive Terrain camera.")


func test_object_workspace_uses_only_global_environment_viewport_button() -> void:
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation
	var mount: Control = workstation.get_node("%ViewportMount")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	await get_tree().process_frame

	var environment_button: Button = workstation.get_node("%EnvironmentToggleButton")
	assert_not_null(environment_button, "Global environment button should stay available from Object.")
	assert_null(_find_node_by_name(mount, "ObjectEnvironmentButton"), "Object preview should not add a second environment button over the viewport.")

	environment_button.toggled.emit(true)
	await get_tree().process_frame

	var popup: PanelContainer = workstation.get_node("%EnvironmentPopup")
	assert_true(popup.visible, "Global environment button should open the shared environment popup from Object.")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.OBJECT, "Opening global environment controls should not switch out of Object.")


func test_viewport_popups_are_mutually_exclusive_and_escape_closes_active_popup() -> void:
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT).set_environment_editor(environment_editor)

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
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)

	var actions_mount: BoxContainer = workstation.get_node("%WorkspaceActionsMount")
	var save_button := _find_button_by_text(actions_mount, "Save Project")
	var overflow := _overflow_popup(actions_mount)
	assert_eq(_workspace_action_texts(actions_mount), ["New Terrain", "Open Terrain...", "Save Project", "More"], "Terrain should expose primary actions as buttons and fold the rest into More.")
	assert_not_null(save_button, "Terrain should expose Save Project.")
	assert_not_null(overflow, "Terrain should expose a More menu for the secondary actions.")
	assert_eq(_overflow_item_texts(overflow), ["Save Project As...", "Export Terrain..."], "More holds Save As and Export in action order.")
	assert_false(save_button.disabled, "Dirty terrain projects should enable Save Project.")
	# Menu items refresh their gating when the popup is about to show.
	overflow.about_to_popup.emit()
	assert_false(_overflow_item_disabled(overflow, "Save Project As..."), "Save As should be available when the editor is idle.")
	assert_false(_overflow_item_disabled(overflow, "Export Terrain..."), "Terrain export should be available when the editor is idle.")


func test_switching_workspaces_preserves_terrain_dirty_state() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)

	assert_true(editor.is_dirty, "Switching placeholder domains should not reset terrain document state.")
	assert_eq(workstation.get_node("%ContextDocLabel").text, "untitled*", "Returning to Terrain should restore the terrain project title and dirty marker.")
	assert_true(workstation.get_node("%AssetDock").visible, "Terrain properties should return when Terrain is active.")


func test_workstation_mounts_workspace_specific_right_docks() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)

	var dock: Control = workstation.get_node("%AssetDock")
	assert_true(dock.visible, "Terrain should show the shared right dock mount.")
	assert_not_null(_find_node_by_name(dock, "TerrainAssetDock"), "Terrain should mount its asset dock inside the shared right dock mount.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)

	assert_false(dock.visible, "Object Preview should hide the shared right dock mount.")
	assert_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object Preview should not mount an empty detail dock.")
	assert_null(_find_node_by_name(dock, "TerrainAssetDock"), "Switching to Object should remove Terrain's dock content.")
	assert_not_null(_find_node_by_name(workstation.get_node("%ViewportMount"), "ObjectPreview"), "Object preview should stay in the center viewport.")

	var materials_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Materials")
	assert_not_null(materials_button, "Object workspace should expose Materials mode.")
	if materials_button != null:
		materials_button.pressed.emit()
	assert_true(dock.visible, "Object Materials should show the shared right dock mount.")
	assert_not_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object Materials should mount its detail dock.")
	assert_not_null(_find_node_by_name(dock, "MaterialDetailPanel"), "Object Materials should expose right-pane material details.")

	var parts_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Part Anims")
	assert_not_null(parts_button, "Object workspace should expose Part Anims mode.")
	if parts_button != null:
		parts_button.pressed.emit()
	assert_true(dock.visible, "Object Part anims should show the shared right dock mount.")
	assert_not_null(_find_node_by_name(dock, "PartAnimDetailsEmpty"), "Object Part anims should expose right-pane animation details.")

	var lights_button := _find_button_by_text(workstation.get_node("%ModeRail"), "Lights")
	assert_not_null(lights_button, "Object workspace should expose Lights mode.")
	if lights_button != null:
		lights_button.pressed.emit()
	assert_true(dock.visible, "Object Lights should show the shared right dock mount.")
	assert_not_null(_find_node_by_name(dock, "LightDetailPanel"), "Object Lights should expose right-pane light details.")

	var lods_button := _find_button_by_text(workstation.get_node("%ModeRail"), "LODs")
	assert_not_null(lods_button, "Object workspace should expose LODs mode.")
	if lods_button != null:
		lods_button.pressed.emit()
	assert_false(dock.visible, "Object LODs should hide the shared right dock mount until they have a real detail editor.")
	assert_null(_find_node_by_name(dock, "ObjectDetailDock"), "Object LODs should not mount placeholder right-pane content.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	assert_true(dock.visible, "Mission authoring shows the shared right dock for its per-selection editor + Mission form.")
	assert_gt(dock.get_child_count(), 0, "Mission should mount its inspector detail content in the shared right dock.")
	assert_null(_find_node_by_name(dock, "TerrainAssetDock"), "Switching from Object to Mission should clear the prior dock content.")


func test_workspace_switching_mounts_terrain_and_mission_viewports() -> void:
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation
	var mount: Control = workstation.get_node("%ViewportMount")

	assert_eq(mount.get_child_count(), 1, "Mission should own the viewport mount by default.")
	assert_eq(mount.get_child(0).name, "MissionViewport", "Mission should mount its read-only terrain viewport by default.")
	assert_true(editor.is_viewport_active(), "Mission should keep the loaded terrain visible.")
	assert_false(editor.is_viewport_edit_input_active(), "Mission should disable terrain brush and shortcut input.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	await get_tree().process_frame

	assert_eq(mount.get_child_count(), 1, "Terrain should replace Mission as the only viewport owner.")
	assert_eq(mount.get_child(0).name, "TerrainViewport", "Terrain should mount through TerrainViewport.")
	assert_true(editor.is_viewport_active(), "Terrain editor rendering should be active while Terrain owns the viewport.")
	assert_true(editor.is_viewport_edit_input_active(), "Terrain should enable terrain edit input.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	await get_tree().process_frame

	assert_eq(mount.get_child_count(), 1, "Returning to Mission should still leave one viewport owner.")
	assert_eq(mount.get_child(0).name, "MissionViewport", "MissionViewport should remount when Mission becomes active again.")
	assert_true(editor.is_viewport_active(), "Mission should keep the terrain visible when it owns the viewport again.")
	assert_false(editor.is_viewport_edit_input_active(), "Terrain edit input should stay off while Mission owns the viewport.")


func test_workstation_tracks_mode_from_editor_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.brush_radius = 20.0
	editor.brush_strength = 0.8
	editor.brush_hardness = 0.3

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.SCATTER, "Workstation should switch to Foliage mode when the editor tool is foliage paint.")

	editor.current_tool = TerrainEditorScript.Tool.TILE_STAMP
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.STAMP, "Workstation should switch to Tile mode when the editor tool becomes tile placement.")


# Minimal tileinfo surface for the gizmo capability hooks.
class TileGizmoTerrainStub:
	extends Node

	class Entry:
		extends RefCounted

		func get_tile_index() -> int:
			return 7

		func get_cell_x() -> int:
			return 3

		func get_cell_z() -> int:
			return 4

	var current_tool := TerrainEditor.Tool.TILE_STAMP
	var rotations := 0
	var cleared := 0

	func has_selected_tileinfo_entry() -> bool:
		return true

	func get_selected_tileinfo_entry() -> Variant:
		return Entry.new()

	func get_selected_tileinfo_world_center() -> Vector3:
		return Vector3(10, 0, 20)

	func rotate_selected_tileinfo_clockwise() -> void:
		rotations += 1

	func clear_tileinfo_selection() -> void:
		cleared += 1


func test_tile_gizmo_state_and_actions_ride_the_workspace_hooks() -> void:
	# The shell's in-world gizmo is capability-driven: it reads
	# get_tile_gizmo_state() and routes buttons through run_tile_gizmo_action(),
	# never touching the terrain editor directly.
	var ws: EditorWorkspace = TerrainWorkspaceScript.new()
	var stub: TileGizmoTerrainStub = autofree(TileGizmoTerrainStub.new())
	ws.set_terrain_editor(stub)

	var state := ws.get_tile_gizmo_state()
	assert_eq(state.label, "Editing tile 007 @ (3, 4)",
		"The workspace formats the gizmo label from the selected tile.")
	assert_eq(state.anchor_world, Vector3(10, 2, 20),
		"The anchor floats 2u above the tile's world center.")

	ws.run_tile_gizmo_action(&"rotate")
	assert_eq(stub.rotations, 1, "Gizmo actions route to the terrain editor through the hook.")
	ws.run_tile_gizmo_action(&"done")
	assert_eq(stub.cleared, 1, "Done clears the selection through the hook.")

	stub.current_tool = TerrainEditor.Tool.RAISE
	assert_null(ws.get_tile_gizmo_state(),
		"Outside the Tile workflow the gizmo reports no state.")


func test_unsaved_changes_opens_native_confirmation_dialog() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation.prompt_unsaved_for(func() -> void: pass, func() -> void: pass)

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
	var outcome := {"saved": 0, "discarded": 0, "cancelled": 0}

	workstation.prompt_unsaved_for(
		func() -> void: outcome.saved += 1,
		func() -> void: outcome.discarded += 1,
		func() -> void: outcome.cancelled += 1)
	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Unsaved changes should open a native confirmation dialog.")
	if dialog == null:
		return
	dialog.canceled.emit()

	assert_eq(outcome.cancelled, 1, "Cancel/Escape should run the keep-editing outcome.")
	assert_eq(outcome.discarded, 0, "Cancel should not discard.")
	assert_eq(outcome.saved, 0, "Cancel should not save.")


func test_unsaved_dialog_confirm_saves_and_discard_action_discards() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var outcome := {"saved": 0, "discarded": 0}
	var on_save := func() -> void: outcome.saved += 1
	var on_discard := func() -> void: outcome.discarded += 1

	workstation.prompt_unsaved_for(on_save, on_discard)
	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Unsaved changes should open a native confirmation dialog.")
	if dialog == null:
		return

	dialog.confirmed.emit()
	assert_eq(outcome.saved, 1, "Confirm should run the save outcome.")
	assert_eq(outcome.discarded, 0, "Confirm should not discard.")

	# Outcomes are consumed on dispatch; a fresh prompt rearms the same dialog.
	workstation.prompt_unsaved_for(on_save, on_discard)
	dialog.custom_action.emit(&"discard")
	assert_eq(outcome.discarded, 1, "The Discard custom action should run the discard outcome.")
	assert_eq(outcome.saved, 1, "Discard should not save again.")


func test_dirty_open_prompts_and_save_routes_through_save_as() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var stub: DirtyTerrainStub = autofree(DirtyTerrainStub.new())
	# Bind only the terrain workspace (set_editor's full bind would drag every
	# workspace through a remount this test doesn't exercise).
	var ws: EditorWorkspace = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.TERRAIN)
	ws.set_terrain_editor(stub)

	assert_eq(ws.open_file("C:/maps/next.trn"), OK, "A dirty-guarded open reports OK while the prompt owns the action.")
	assert_eq(stub.opened.size(), 0, "The open must wait for the prompt outcome.")
	var dialog := workstation.find_child("UnsavedChangesDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "Replacing a dirty terrain should prompt.")
	if dialog == null:
		return

	# Save: no project directory yet, so the save routes through the Save As
	# directory dialog and the open stays deferred until the save lands. The OK
	# button hides the dialog before confirmed fires; mirror that order so the
	# follow-up dialog can take the exclusive slot.
	dialog.hide()
	dialog.confirmed.emit()
	assert_eq(stub.saved_dirs.size(), 0, "Without a project dir the save waits for the Save As pick.")
	var save_dialogs: Array = workstation.find_children("", "FileDialog", true, false)
	assert_eq(save_dialogs.size(), 1, "Save should route through the Save As directory dialog.")
	if save_dialogs.size() != 1:
		return
	var file_dialog: FileDialog = save_dialogs[0] as FileDialog
	file_dialog.dir_selected.emit("C:/maps/project")

	assert_eq(stub.saved_dirs, PackedStringArray(["C:/maps/project"]), "The picked directory receives the save.")
	assert_eq(stub.opened, PackedStringArray(["C:/maps/next.trn"]), "The deferred open runs after a successful save.")


func test_export_flavor_opens_native_dialog_with_format_toggles() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation._save_export.show_export_flavor_dialog("C:/Exports/TestTerrain")

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


func test_export_confirm_targets_the_initiating_workspace_after_tab_switch() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var stub: ExportRecordingTerrainStub = autofree(ExportRecordingTerrainStub.new())
	# Bind only the terrain workspace — activating it would push the stub into
	# the typed asset dock/inspector set_editor calls (the dirty-open precedent).
	var ws: EditorWorkspace = workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.TERRAIN)
	ws.set_terrain_editor(stub)

	workstation._save_export.on_export_pressed(ws)
	var file_dialogs: Array = workstation.find_children("", "FileDialog", true, false)
	assert_eq(file_dialogs.size(), 1, "Export should route through the directory dialog.")
	if file_dialogs.size() != 1:
		return
	var file_dialog: FileDialog = file_dialogs[0] as FileDialog
	# A real pick closes the dialog before the signal; mirror that order so the
	# flavor dialog can take the exclusive-window slot.
	file_dialog.hide()
	file_dialog.dir_selected.emit("C:/Exports/Initiator")

	# The tab switch mid-dialog: the confirm below must still export Terrain.
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	var dialog = workstation.find_child("ExportFlavorDialog", true, false)
	assert_not_null(dialog, "The flavor dialog is up while another tab is active.")
	if dialog == null:
		return
	dialog.hide()
	dialog.confirmed.emit()

	assert_eq(stub.exports.size(), 1,
		"exactly one export runs, on the workspace that opened the dialog")
	if stub.exports.size() == 1:
		assert_eq(stub.exports[0][0], "C:/Exports/Initiator",
			"the export receives the directory picked from the initiating workspace")
		assert_eq(stub.exports[0][1], ExportFlavorDialog.FLAVOR_DFX_JO,
			"the export receives the dialog's selected flavor")


func test_asset_dock_builds_preview_cards_for_shared_maps() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_true(dock._slot_previews.has("charmap"), "Asset dock should build a preview card for the surface-type map.")
	assert_true(dock._slot_previews.has("foliagemap"), "Asset dock should build a preview card for the foliage map.")
	assert_true(dock._slot_previews.has("tilestrip"), "Asset dock should build a preview card for the tile atlas.")


func test_asset_dock_uses_shared_slot_labels() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_true(_has_label_text(dock, "Detail A"), "Asset dock should use shared detail slot labels.")
	assert_true(_has_label_text(dock, "Detail coefficient source"), "Asset dock should use shared auxiliary slot labels.")
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
	var title := _find_label_by_text(dock, "Detail coefficient source")
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
	var mount = add_child_autofree(Control.new())
	var inspector := SculptInspector.new()
	var editor = autofree(TerrainEditorScript.new())

	inspector.build_main(mount)
	inspector.set_editor(editor)
	editor.set_brush_radius_value(37.0)

	var radius_spin: SpinBox = inspector._brush.get_radius_spin()
	assert_eq(radius_spin.value, 37.0, "Editor UI state changes should update subscribed inspectors without per-frame polling.")


func test_workstation_uses_clip_text_for_long_labels() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var doc_label: Label = workstation.get_node("%ContextDocLabel")
	var status_context_label: Label = workstation.get_node("%StatusContextLabel")
	var status_camera_label: Label = workstation.get_node("%StatusCameraLabel")

	assert_true(doc_label.clip_text, "Context document label should clip rather than forcing the top bar wider.")
	assert_true(status_context_label.clip_text, "Status context should clip instead of forcing horizontal overflow.")
	assert_true(status_camera_label.clip_text, "Status camera text should clip instead of forcing horizontal overflow.")


func test_context_header_names_workspace_and_leaves_doc_blank_when_empty() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	await get_tree().process_frame

	var ws_label: Label = workstation.get_node("%ContextWorkspaceLabel")
	var doc_label: Label = workstation.get_node("%ContextDocLabel")
	assert_eq(ws_label.text, "Mission", "The default workspace names itself in the context header.")
	assert_eq(doc_label.text, "", "With nothing open the document half stays blank.")
	assert_eq(doc_label.custom_minimum_size.x, 0.0,
		"An empty document label reserves no width instead of a fixed column.")


func test_pressing_layout_mode_restores_edit_sectors_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	workstation._on_workflow_pressed(TerrainWorkspaceScript.Workflow.LAYOUT)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Selecting Layout should restore the sector editing tool.")


func test_layout_inspector_is_trimmed_to_board_and_legend() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := LayoutInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	inspector.build_main(mount)
	inspector.set_editor(editor)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Layout inspector should force sector editing when it becomes active.")
	assert_not_null(inspector._legend_grid, "Layout inspector should keep a passive legend.")
	assert_gt(inspector._legend_grid.get_child_count(), 0, "The legend should be populated with sector swatches.")
	assert_not_null(inspector._sector_overlay_toggle, "Layout inspector should expose a sector overlay toggle under the map layout board.")


func test_layout_inspector_sector_overlay_toggle_syncs_with_editor() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := LayoutInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.set_sector_overlay_visible(true)

	inspector.build_main(mount)
	inspector.set_editor(editor)

	assert_true(inspector._sector_overlay_toggle.button_pressed, "Layout inspector should reflect the editor's current sector overlay visibility.")

	inspector._on_sector_overlay_toggled(false)
	assert_false(editor.is_sector_overlay_visible(), "Toggling sector overlay off in Layout should update editor state.")


func test_foliage_inspector_selection_updates_editor_selection() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := ScatterInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT

	inspector.build_main(mount)
	inspector.set_editor(editor)
	inspector._on_list_selected(1)

	assert_eq(editor.get_selected_foliage_def_index(), 1, "Selecting a foliage list item should update the editor selection used by paint.")
	assert_true(editor.get_selected_foliage_def() != null, "The selected foliage def should be available after selecting it in the inspector.")


func test_foliage_inspector_reflects_editor_selection_and_add_remove() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := ScatterInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.set_selected_foliage_def_index(1)

	inspector.build_main(mount)
	inspector.set_editor(editor)

	var list: ItemList = inspector._list
	assert_true(list.is_selected(1), "The foliage inspector should highlight the editor's selected foliage def.")

	inspector._on_add_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 2, "Adding a foliage def should leave the new foliage type selected in editor state.")

	inspector._on_remove_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 1, "Removing the selected foliage def should clamp selection to the remaining valid index.")
	assert_true(list.is_selected(1), "The foliage inspector should stay aligned with the clamped editor selection after removal.")


func test_stamp_inspector_removes_entry_list_and_apply_workflow() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	inspector.build_main(mount)

	assert_not_null(inspector._selection_done, "Tile inspector should expose a direct exit action for selection mode.")
	assert_not_null(inspector._selection_delete, "Tile inspector should expose a direct delete action for the selected tile.")


func test_stamp_inspector_atlas_click_replaces_selected_tile_immediately() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(mount)
	inspector.set_editor(editor)
	inspector._on_atlas_selected(5)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after replacing from the atlas.")
	assert_eq(entry.get_tile_index(), 5, "Atlas clicks should replace the selected tile immediately.")


func test_stamp_inspector_atlas_focus_follows_selected_tile() -> void:
	var mount = add_child_autofree(Control.new())
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

	inspector.build_main(mount)
	inspector.set_editor(editor)
	inspector.sync_from_editor()

	var atlas_status: Label = inspector._atlas_status
	var atlas_list: ItemList = inspector._atlas_list
	assert_string_contains(atlas_status.text, "editing 002", "Atlas status should reflect the selected tile when replace-on-click is active.")
	assert_true(atlas_list.is_selected(2), "Atlas selection should follow the selected tile while a placed tile is active.")


func test_stamp_inspector_reuses_tile_preview_icons_until_atlas_changes() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	var strip_image := Image.create(256, 64, false, Image.FORMAT_RGBA8)
	strip_image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var tilestrip := ImageTexture.create_from_image(strip_image)
	editor._document.data.set_tilestrip_tex(tilestrip)

	inspector.build_main(mount)
	inspector.set_editor(editor)
	var cache_size: int = inspector._tile_icon_cache.size()
	var first: Texture2D = inspector._build_icon(tilestrip, 2, 4)
	var second: Texture2D = inspector._build_icon(tilestrip, 2, 4)

	assert_true(first == second, "Tile preview icons should be cached while the tilestrip is unchanged.")
	assert_eq(inspector._tile_icon_cache.size(), cache_size, "Repeated tile icon requests should not allocate duplicate AtlasTextures.")


func test_stamp_inspector_flag_toggle_updates_selected_tile_immediately() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(mount)
	inspector.set_editor(editor)
	inspector._on_flip_x(true)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after toggling a transform flag.")
	assert_true((entry.get_flags() & NovaTerrainTileInfo.FLAG_FLIP_X) != 0, "Tile transform toggles should update the selected tile immediately.")


func test_stamp_inspector_shows_quiet_empty_selection_state() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()

	inspector.build_main(mount)
	inspector.set_editor(editor)

	var done_button: Button = inspector._selection_done
	var summary: Label = inspector._selection_summary
	var delete_button: Button = inspector._selection_delete
	assert_eq(summary.text, "No tile selected.", "Tile inspector should use a quiet empty-state until the user selects a placed tile.")
	assert_true(done_button.disabled, "Done should stay disabled until a tile is selected.")
	assert_true(delete_button.disabled, "Delete should stay disabled until a tile is selected.")


func test_stamp_inspector_done_clears_selection() -> void:
	var mount = add_child_autofree(Control.new())
	var inspector := StampInspector.new()
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.build_main(mount)
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
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK, "Resource browser should index the configured resource directory.")

	var open_button := _find_button_by_text(workstation.get_node("%WorkspaceActionsMount"), "Open Terrain...")
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
	workstation._save_export.open_file_dialog("Open file", PackedStringArray(), func(_p): pass)
	workstation._save_export.open_dir_dialog("Open dir", func(_p): pass)

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

	var body: Node = workstation.get_node_or_null("%BodyRow")
	assert_not_null(body, "Shell should keep a BodyRow row.")
	assert_true(body is HSplitContainer, "BodyRow should be an HSplitContainer so the left dock can be dragged.")
	var center_right: Node = workstation.get_node_or_null("%CenterRightSplit")
	assert_true(center_right is HSplitContainer, "The viewport/right-dock split should be an HSplitContainer.")


func test_shell_panels_still_resolve_by_unique_name_after_split_refactor() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var left: Node = workstation.get_node_or_null("%LeftLane")
	var viewport_lane: Node = workstation.get_node_or_null("%ViewportLane")
	var viewport_mount: Node = workstation.get_node_or_null("%ViewportMount")
	var dock: Node = workstation.get_node_or_null("%AssetDock")
	assert_not_null(left, "%LeftLane should still resolve after the split refactor.")
	assert_not_null(viewport_lane, "%ViewportLane should still resolve after the split refactor.")
	assert_not_null(viewport_mount, "%ViewportMount should still resolve after the split refactor.")
	assert_not_null(dock, "%AssetDock should still resolve after the split refactor.")
	if viewport_lane != null and viewport_mount != null:
		assert_true((viewport_lane as Node).is_ancestor_of(viewport_mount), "ViewportMount should stay parented under ViewportLane.")


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
	first._layout.save_split_layout()

	# A fresh shell at the same size should restore those dragged widths.
	var second = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	await get_tree().process_frame
	var second_body := second.get_node("%BodyRow") as SplitContainer
	var second_right := second.get_node("%CenterRightSplit") as SplitContainer
	assert_eq(second_body.split_offset, target_left, "A new shell should restore the persisted left split offset.")
	assert_eq(second_right.split_offset, target_right, "A new shell should restore the persisted right split offset.")


# --- Resource Browser pane (A10) --------------------------------------------------

func test_browser_pane_defaults_hidden_and_toggles_without_closing_popovers() -> void:
	# Fresh-default assertions must not read this machine's persisted layout
	# (before_each restores the real user:// state by design — a session that
	# left the pane open would leak browser_pane_visible=true into "defaults").
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var mount := workstation.get_node("%ResourceBrowserPaneMount") as Control
	var toggle := workstation.get_node("%BrowserToggleButton") as Button
	assert_false(mount.visible, "the pane defaults hidden (protects the 1024x640 window floor)")
	assert_false(toggle.button_pressed, "the toggle starts unpressed")

	workstation._popovers.set_settings_visible(true)
	toggle.button_pressed = true
	assert_true(mount.visible, "the toggle shows the pane")
	assert_true(workstation.get_node("%SettingsPopup").visible,
		"a dock toggle must not close popovers (it is not in the mutual-exclusion chain)")
	workstation._popovers.set_settings_visible(false)

	toggle.button_pressed = false
	assert_false(mount.visible, "the toggle hides the pane again")


func test_browser_pane_lists_and_filters_by_kind() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var root := _make_resource_fixture("pane_filter")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK)
	workstation._layout.set_browser_pane_visible(true)
	var pane = workstation._layout.browser_pane()
	assert_not_null(pane, "showing the pane builds it lazily")
	assert_eq(pane.table.get_visible_count(), 9,
		"the All filter lists every recognized fixture resource")

	var fonts_index := -1
	for i in pane.kind_option.item_count:
		if pane.kind_option.get_item_text(i) == "Fonts":
			fonts_index = i
	assert_gt(fonts_index, -1, "the kind dropdown offers Fonts")
	pane.kind_option.select(fonts_index)
	pane.kind_option.item_selected.emit(fonts_index)
	assert_eq(pane.table.get_visible_count(), 1, "the kind filter narrows to the one font")


func test_browser_pane_double_click_jumps_through_open_in_workspace() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var root := ProjectSettings.globalize_path("res://../fixtures/fnt")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK)
	workstation._layout.set_browser_pane_visible(true)
	var pane = workstation._layout.browser_pane()

	var fonts_index := -1
	for i in pane.kind_option.item_count:
		if pane.kind_option.get_item_text(i) == "Fonts":
			fonts_index = i
	pane.kind_option.select(fonts_index)
	pane.kind_option.item_selected.emit(fonts_index)
	assert_gt(pane.table.get_visible_count(), 0, "the fixtures root lists fonts")

	# The refresh auto-selects the first row; activation must ride the shared
	# cross-jump spine (open_in_workspace), not a private open path.
	pane.table.tree.item_activated.emit()
	await get_tree().process_frame
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.FONTS,
		"activating a font row lands in the Fonts workspace")


func test_browser_pane_visibility_and_split_persist() -> void:
	var first = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	first._layout.set_browser_pane_visible(true)
	first._right_split.split_offset = -123
	first._layout._save_browser_state()
	first.queue_free()
	await get_tree().process_frame

	var second = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	assert_true((second.get_node("%ResourceBrowserPaneMount") as Control).visible,
		"a fresh shell restores the pane's visibility")
	assert_true((second.get_node("%BrowserToggleButton") as Button).button_pressed,
		"...with the toggle pressed to match")
	assert_eq((second.get_node("%RightSplit") as SplitContainer).split_offset, -123,
		"...and the persisted pane split offset")


func test_browser_pane_never_impersonates_the_modal_dialog() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	assert_null(workstation.find_child("ResourceBrowserDialog", true, false),
		"no modal dialog exists until a picker opens")
	workstation._layout.set_browser_pane_visible(true)
	assert_null(workstation.find_child("ResourceBrowserDialog", true, false),
		"showing the pane never builds the modal")
	assert_null(workstation.find_child("ResourceBrowserList", true, false),
		"the pane's list uses its own name; the pinned modal lookups stay unambiguous")

	workstation.open_kind_picker("terrain", "Choose a terrain", func(_path: String) -> void: pass)
	var dialog: Node = workstation.find_child("ResourceBrowserDialog", true, false)
	assert_not_null(dialog, "the modal still builds on demand")
	if dialog != null:
		var modal_tree: Node = dialog.find_child("ResourceBrowserList", true, false)
		assert_not_null(modal_tree, "the modal's tree keeps its pinned name")
		assert_not_null(dialog.find_child("ResourceBrowserSearch", true, false),
			"the modal's search keeps its pinned name")
		if modal_tree != null:
			# Pin the provider itself, not just one empty-list call: a wired
			# provider on a row-less list would also return null and hide the
			# regression.
			assert_false(modal_tree.get_parent()._drag_payload_provider.is_valid(),
				"the modal's table never enables a drag provider (drag-out is the pane's affordance)")
			assert_null(modal_tree.get_parent()._get_tree_drag_data(Vector2.ZERO),
				"and produces no drag data")
		(dialog as Window).hide()


func test_browser_pane_rows_drag_as_link_payloads() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var root := _make_resource_fixture("pane_drag")
	assert_eq(workstation._set_resource_root_dir(root, false, true), OK)
	workstation._layout.set_browser_pane_visible(true)
	var pane = workstation._layout.browser_pane()

	var fonts_index := -1
	for i in pane.kind_option.item_count:
		if pane.kind_option.get_item_text(i) == "Fonts":
			fonts_index = i
	assert_gt(fonts_index, -1, "the kind dropdown offers Fonts")
	pane.kind_option.select(fonts_index)
	pane.kind_option.item_selected.emit(fonts_index)
	assert_eq(pane.table.get_visible_count(), 1, "the Fonts filter shows the fixture font")

	# The refresh auto-selects the row; the headless drag rides that selection.
	var data: Variant = pane.table._get_tree_drag_data(Vector2.ZERO)
	var payload := LinkPayloadScript.from_drag_data(data)
	assert_not_null(payload, "a pane row drags as a LinkPayload")
	if payload != null:
		assert_eq(payload.kind, "font", "the payload carries the row's kind")
		assert_eq(payload.name, "alpha.fnt", "the payload name keeps the extension")
		assert_true(payload.path.to_lower().ends_with("alpha.fnt"), "the path points at the file")

	# An object row pins the vocabulary rule for real: "object_project" is a
	# kind _JUMP_KIND translates for jumps, so a payload dragging as "object"
	# would expose the regression "font" (identical in both vocabularies) cannot.
	pane.kind_option.select(0)  # back to All
	pane.kind_option.item_selected.emit(0)
	var project_row: TreeItem = null
	var row := pane.table.tree.get_root().get_first_child() as TreeItem
	while row != null:
		var entry := row.get_metadata(0) as Dictionary
		if entry != null and String(entry.get("relative_path", "")) == "alpha.3dp":
			project_row = row
			break
		row = row.get_next()
	assert_not_null(project_row, "the All filter lists the fixture object project")
	if project_row == null:
		return
	project_row.select(0)
	var object_payload := LinkPayloadScript.from_drag_data(pane.table._get_tree_drag_data(Vector2.ZERO))
	assert_not_null(object_payload, "the object row drags as a LinkPayload")
	if object_payload != null:
		assert_eq(object_payload.kind, "object_project",
			"the payload keeps the index's reference-kind vocabulary (never the jump alias)")


func test_right_split_hides_when_dock_and_pane_are_both_hidden() -> void:
	# Same persisted-state guard as the pane-defaults test above: the split's
	# initial visibility derives from the pane default, not this machine's cfg.
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var right_split := workstation.get_node("%RightSplit") as Control

	# A dockless workspace with the pane off keeps the pre-pane behavior: one
	# visible CenterRightSplit child, no live divider, persisted offset inert.
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.OBJECT)
	await get_tree().process_frame
	assert_false(right_split.visible,
		"a dockless workspace with the pane hidden hides the right split entirely")

	workstation._layout.set_browser_pane_visible(true)
	assert_true(right_split.visible, "showing the pane brings the split back")
	workstation._layout.set_browser_pane_visible(false)
	assert_false(right_split.visible, "hiding it again re-hides the split")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	await get_tree().process_frame
	assert_true(right_split.visible, "a dock-using workspace shows the split")


func test_wire_browser_pane_twice_stays_single_wired() -> void:
	# Same persisted-state guard as the pane tests above.
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame

	# _ready already wired the pane once; a re-wire must not push a
	# duplicate-connect engine error (the connect is guarded like the split's).
	workstation._layout.wire_browser_pane()
	assert_engine_error_count(0,
		"re-wiring the browser pane must not double-connect the toggle")

	var toggle := workstation.get_node("%BrowserToggleButton") as Button
	var pane_mount := workstation.get_node("%ResourceBrowserPaneMount") as Control
	toggle.button_pressed = true
	assert_true(pane_mount.visible, "one toggle-on shows the pane once")
	toggle.button_pressed = false
	assert_false(pane_mount.visible, "one toggle-off hides it again")


# --- Detachable panels (B6) ---

func _detach_environment(workstation) -> void:
	workstation.get_node("%EnvironmentPopupDetach").pressed.emit()


# Floating panels persist on _exit_tree (quit-while-floating), which would land
# AFTER after_each restores the shared config; tearing down inside the test
# keeps that write under the restore.
func _teardown(workstation) -> void:
	workstation.queue_free()
	await get_tree().process_frame


func _attach_environment_document(workstation) -> Node:
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT).set_environment_editor(environment_editor)
	return environment_editor


func test_environment_detach_button_pops_content_into_window() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_attach_environment_document(workstation)
	workstation._popovers.set_environment_visible(true)
	assert_true(workstation.get_node("%EnvironmentPopup").visible, "popover opens docked first")

	_detach_environment(workstation)
	assert_false(workstation.get_node("%EnvironmentPopup").visible,
		"the popover chrome hides when its content floats")
	var inspector_mount: Control = workstation.get_node("%EnvironmentInspectorMount")
	assert_true(inspector_mount.get_window() != workstation.get_window(),
		"the environment content now lives in its own Window")
	assert_true((workstation.get_node("%EnvironmentToggleButton") as Button).button_pressed,
		"the rail toggle stays pressed while floating")
	assert_gt(workstation.get_node("%EnvironmentActionsMount").get_child_count(), 0,
		"the floating panel carries the document actions")
	assert_gt(inspector_mount.get_child_count(), 0,
		"...and the environment inspector")
	await _teardown(workstation)


func test_detached_panel_is_exempt_from_popover_mutual_exclusion() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_detach_environment(workstation)
	assert_true(workstation._popovers.environment_panel_mount().is_floating())

	workstation._popovers.set_settings_visible(true)
	assert_true(workstation.get_node("%SettingsPopup").visible, "the settings popover opens")
	assert_true(workstation._popovers.environment_panel_mount().is_floating(),
		"opening a sibling popover must not re-dock or hide the floating panel")
	workstation._popovers.set_settings_visible(false)
	await _teardown(workstation)


func test_toggle_focuses_detached_window_instead_of_closing() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_detach_environment(workstation)

	# The rail toggle while floating raises the window; it never closes or
	# re-docks (re-docking has its own gesture: the window close button).
	workstation._popovers.set_environment_visible(true)
	assert_true(workstation._popovers.environment_panel_mount().is_floating(), "still floating after toggle-on")
	workstation._popovers.set_environment_visible(false)
	assert_true(workstation._popovers.environment_panel_mount().is_floating(), "still floating after toggle-off")
	assert_true((workstation.get_node("%EnvironmentToggleButton") as Button).button_pressed,
		"the toggle re-presses to mirror the floating state")
	await _teardown(workstation)


func test_window_close_redocks_environment_content() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_attach_environment_document(workstation)
	workstation._popovers.set_environment_visible(true)
	var actions_before: int = workstation.get_node("%EnvironmentActionsMount").get_child_count()
	assert_gt(actions_before, 0, "the docked popover carries document actions")
	_detach_environment(workstation)
	var window: Window = workstation._popovers.environment_panel_mount().get_window()

	window.close_requested.emit()
	assert_false(workstation._popovers.environment_panel_mount().is_floating(), "the window close re-docks")
	var content: Control = workstation.get_node("%EnvironmentPopupContent")
	assert_eq(content.get_parent().name, "EnvironmentPopupBox",
		"the content returns to the popover box")
	assert_false(workstation.get_node("%EnvironmentPopup").visible,
		"the popover stays closed after a re-dock")
	assert_false((workstation.get_node("%EnvironmentToggleButton") as Button).button_pressed,
		"the toggle releases")

	workstation._popovers.set_environment_visible(true)
	assert_true(workstation.get_node("%EnvironmentPopup").visible, "the toggle reopens it docked")
	assert_eq(workstation.get_node("%EnvironmentActionsMount").get_child_count(), actions_before,
		"reopening must not duplicate the action buttons")
	await _teardown(workstation)


func test_detached_environment_window_title_tracks_project_title() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_detach_environment(workstation)
	workstation._popovers.refresh_environment_state()
	var window: Window = workstation._popovers.environment_panel_mount().get_window()
	assert_string_contains(window.title, "Environment — ",
		"the floating window titles itself with the document name")
	await _teardown(workstation)


func test_panel_state_round_trips_through_panels_section() -> void:
	# Start from a clean slate: the per-test snapshot already protects the real
	# config, and the REAL config may legitimately hold a user's panel state.
	if FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var library = workstation._resource_library

	var fresh: Dictionary = library.load_panel_state("environment")
	assert_true(bool(fresh.get("docked", false)), "panels default docked")
	assert_false(bool(fresh.get("has_rect", true)), "no rect until one is saved")

	library.save_layout_state(123, -207)
	library.save_panel_state("environment", false, Rect2i(-5, -7, 400, 600))
	var loaded: Dictionary = library.load_panel_state("environment")
	assert_false(bool(loaded.get("docked", true)))
	assert_true(bool(loaded.get("has_rect", false)))
	assert_eq(loaded.get("rect"), Rect2i(-5, -7, 400, 600),
		"negative window positions round-trip (multi-monitor)")
	var layout: Dictionary = library.load_layout_state()
	assert_eq(int(layout.get("left", 0)), 123, "panel saves never clobber the layout section")
	assert_eq(int(layout.get("right", 0)), -207)


func test_persisted_floating_preference_applies_on_next_open() -> void:
	var first = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_detach_environment(first)
	assert_true(first._popovers.environment_panel_mount().is_floating())
	# Move the window after the detach-time save: only the EXIT-time save_now
	# can carry this rect forward, which is what pins it.
	var moved_window: Window = first._popovers.environment_panel_mount().get_window()
	moved_window.size = Vector2i(515, 537)
	first.queue_free()
	await get_tree().process_frame

	var second = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	assert_false(second._popovers.environment_panel_mount().is_floating(),
		"a fresh shell always starts docked - no windows at launch")
	second._popovers.set_environment_visible(true)
	assert_true(second._popovers.environment_panel_mount().is_floating(),
		"the remembered floating preference applies on the next open")
	assert_false(second.get_node("%EnvironmentPopup").visible,
		"the popover never flashes on a floating open")
	assert_eq(second._popovers.environment_panel_mount().get_window().size, Vector2i(515, 537),
		"the window reopens at its quit-time size (exit-time save_now + rect reapply)")
	await _teardown(second)


func test_escape_ignores_detached_panels() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	_detach_environment(workstation)
	workstation._popovers.set_settings_visible(true)

	var escape := InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.pressed = true
	workstation._unhandled_input(escape)
	assert_false(workstation.get_node("%SettingsPopup").visible, "Escape closes the docked popover")
	assert_true(workstation._popovers.environment_panel_mount().is_floating(),
		"...but never touches a floating panel")

	workstation._unhandled_input(escape)
	assert_true(workstation._popovers.environment_panel_mount().is_floating(),
		"a second Escape still leaves the floating panel alone")
	await _teardown(workstation)


func test_set_editor_rebuilds_content_inside_detached_window() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var environment_editor = _attach_environment_document(workstation)
	_detach_environment(workstation)
	assert_gt(workstation.get_node("%EnvironmentActionsMount").get_child_count(), 0)

	# set_editor resets the environment popup content; a floating window must
	# get its content rebuilt immediately, not sit empty until the next toggle.
	var editor = autofree(TerrainEditorScript.new())
	editor.environment_editor = environment_editor
	workstation.set_editor(editor)
	assert_true(workstation._popovers.environment_panel_mount().is_floating(), "the panel keeps floating")
	assert_gt(workstation.get_node("%EnvironmentActionsMount").get_child_count(), 0,
		"the rebuilt actions land inside the floating window")
	assert_gt((workstation.get_node("%EnvironmentInspectorMount") as Control).get_child_count(), 0,
		"...with the rebuilt inspector")
	await _teardown(workstation)


func test_floating_environment_still_owns_the_open_marker() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	await get_tree().process_frame
	var environment_editor = _attach_environment_document(workstation)
	environment_editor.set_current_path("user://b6_marker_test.env")
	_detach_environment(workstation)

	assert_eq(workstation._current_resource_path_for_browser("environment"),
		"user://b6_marker_test.env",
		"a floating environment panel still retargets the browser's (open) marker")
	await _teardown(workstation)


func test_camera_panel_detaches_and_force_redocks_keeping_the_preference() -> void:
	# The camera panel shares the mount machinery but has its own shell guards:
	# detach needs a live camera, and losing the camera force-redocks WITHOUT
	# erasing the user's floating preference (transient editor rebinds).
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation

	workstation._popovers.set_camera_visible(true)
	assert_true((workstation.get_node("%CameraPopup") as Control).visible,
		"the camera popover opens docked (the editor scene has a camera)")
	workstation.get_node("%CameraPopupDetach").pressed.emit()
	assert_true(workstation._popovers.camera_panel_mount().is_floating(), "the camera panel floats")
	assert_false((workstation.get_node("%CameraPopup") as Control).visible)
	assert_false(bool(workstation._popovers.panel_restore_for("camera").get("docked", true)),
		"detach remembers the floating preference")

	# The camera disappears (editor rebind): force-redock, preference intact.
	var saved_camera = editor.camera
	editor.camera = null
	workstation._popovers.refresh_camera_state()
	editor.camera = saved_camera
	assert_false(workstation._popovers.camera_panel_mount().is_floating(),
		"losing the camera re-docks the floating panel")
	assert_false(bool(workstation._popovers.panel_restore_for("camera").get("docked", true)),
		"...without overwriting the remembered floating preference")
	await _teardown(editor)
