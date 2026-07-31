extends GutTest

## Shell-integration tests: the Strings workspace appears in the rail, mounts its
## table in the viewport mount, and exposes its document actions.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")

const STATE_PATH := "user://strings_editor_state.cfg"


func before_each() -> void:
	# Session restore reads this on activate; stale state from another test (or a
	# real editor run sharing user://) would change what opens.
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))


func after_all() -> void:
	if FileAccess.file_exists(STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_PATH))


func test_strings_workspace_in_rail() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var rail: BoxContainer = workstation.get_node("%WorkspaceRail")
	var row_texts := []
	for child in rail.get_children():
		if child is Button:
			var bar_label := child.find_child("BarButtonLabel", true, false) as Label
			row_texts.append(bar_label.text if bar_label != null else "")
	assert_true(row_texts.has("Strings"), "Strings should join the workspace nav.")
	# Rail order: Mission, Terrain, Object, Avatars (World), Fonts, Credits, Strings (Interface)...
	assert_eq(row_texts[6], "Strings", "Strings sits after the World group + Fonts/Credits in the rail.")


func test_strings_workspace_mounts_self_contained_view() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var mount: Control = workstation.get_node("%ViewportMount")
	assert_eq(mount.get_child_count(), 1, "Strings should own the viewport mount while active.")
	var view: Control = mount.get_child(0)
	assert_eq(view.name, "StringsEditorView", "Strings should mount its self-contained editor view.")
	# The view bundles the table and the detail editor (no separate right dock).
	assert_not_null(view.find_child("StringsTableView", true, false), "The view should contain the entry table.")
	assert_not_null(view.find_child("StringsDetailPanel", true, false), "The view should contain the detail editor.")

	# The shared right asset dock stays hidden for Strings.
	var asset_dock: Control = workstation.get_node("%AssetDock")
	assert_false(asset_dock.visible, "Strings should not use the right asset dock.")


func test_strings_workspace_exposes_document_actions() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var actions: BoxContainer = workstation.get_node("%WorkspaceActionsMount")
	assert_not_null(_find_button_by_text(actions, "Open Strings..."), "Strings should expose an Open action.")
	assert_not_null(_find_button_by_text(actions, "New Strings"), "Strings should expose a New action.")


func test_open_populates_table() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame

	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_not_null(ws, "the strings workspace adapter should exist")
	var err: int = ws.open_file("res://fixtures/strings/menu.bin")
	assert_eq(err, OK, "opening the fixture should succeed")
	await get_tree().process_frame

	var mount: Control = workstation.get_node("%ViewportMount")
	var tree: Tree = mount.get_child(0).find_child("StringsTree", true, false)
	assert_not_null(tree, "the table Tree should exist")
	var root := tree.get_root()
	assert_not_null(root, "the table should have a root")
	assert_eq(root.get_child_count(), 6, "all six fixture entries should be listed after open")


func test_inspector_offers_used_by_for_the_open_table_without_index_build() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	await get_tree().process_frame

	var strip = workstation.find_child("StringsUsedByStrip", true, false)
	assert_not_null(strip, "the inspector mounts a Used-by strip for the open table")
	if strip == null:
		return
	assert_true(strip.visible, "an open table targets the strip")
	assert_true(strip.find_button.visible,
		"the strip waits for an explicit ask instead of scanning the whole root")
	assert_false(workstation.get_reference_index().is_built(),
		"opening a table must never trigger the whole-root reference scan")


class UsedByShell:
	extends Node

	class IndexStub:
		extends RefCounted
		var queries: Array = []
		func referrers_of(name: String) -> Array:
			queries.append(name)
			return []
		func is_built() -> bool:
			return false

	var index := IndexStub.new()

	func get_reference_index() -> IndexStub:
		return index

	func open_in_workspace(_kind: String, _path: String, _focus: FocusPayload = null) -> Error:
		return OK


func test_used_by_queries_table_spellings_and_retargets_on_tab_switch() -> void:
	var workspace_script := preload("res://modtools/strings/strings_workspace.gd")
	var shell: UsedByShell = add_child_autofree(UsedByShell.new())
	var ws = autofree(workspace_script.new())
	ws.set_editor_shell(shell)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)

	var mount := Control.new()
	add_child_autofree(mount)
	ws.build_inspector(mount)
	await get_tree().process_frame

	var strip = mount.find_child("StringsUsedByStrip", true, false)
	assert_not_null(strip, "the stub shell offers the index, so the strip mounts")
	if strip == null:
		return
	strip.find_button.pressed.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(shell.index.queries.slice(0, 2), ["menu.bin", "menu"],
		"the explicit ask queries the table's verbatim file AND its bare stem (menus keep the extension)")

	# Tabs: a second table activates and the live strip retargets immediately.
	# (Refresh fan-out may query more than once; what matters is WHICH table.)
	var copy_dir := "user://test_strings_used_by"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(copy_dir))
	var copy_path := copy_dir + "/other.bin"
	DirAccess.copy_absolute(ProjectSettings.globalize_path("res://fixtures/strings/menu.bin"),
		ProjectSettings.globalize_path(copy_path))
	var before := (shell.index.queries as Array).size()
	assert_eq(ws.open_file(copy_path), OK, "a second table opens in its own tab")
	var since: Array = shell.index.queries.slice(before)
	assert_has(since, "other.bin", "the live strip re-queries with the new table's file name")
	assert_has(since, "other", "...and its bare stem")

	before = (shell.index.queries as Array).size()
	ws.activate_document(0)
	since = shell.index.queries.slice(before)
	assert_has(since, "menu.bin", "switching back retargets the strip to the first table")
	assert_has(since, "menu", "...both spellings again")

	DirAccess.remove_absolute(ProjectSettings.globalize_path(copy_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(copy_dir))


class FontRootShell:
	extends Node

	var root: NovaResourceRoot

	func get_resource_root() -> NovaResourceRoot:
		return root


func test_detail_game_preview_renders_selected_entry_through_engine_font() -> void:
	# STR-1 gate (docs/oned/workspace-maturity-program.md): the selected entry
	# renders through the game's draw path (F2 EngineTextPreview) with a font
	# chosen from the mounted game folder's .fnt set.
	var root_dir := OS.get_cache_dir().path_join("opennova_test_strings_preview_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	var fnt_bytes := FileAccess.get_file_as_bytes(
		ProjectSettings.globalize_path("res://../fixtures/fnt/Serpen24.fnt"))
	assert_gt(fnt_bytes.size(), 0, "font fixture bytes should read")
	var out := FileAccess.open(root_dir.path_join("Serpen24.fnt"), FileAccess.WRITE)
	out.store_buffer(fnt_bytes)
	out.close()

	var workspace_script := preload("res://modtools/strings/strings_workspace.gd")
	var shell: FontRootShell = add_child_autofree(FontRootShell.new())
	shell.root = NovaResourceRoot.new()
	assert_eq(shell.root.set_root_dir(root_dir), OK)
	var ws = autofree(workspace_script.new())
	ws.set_editor_shell(shell)

	assert_eq(ws.get_preview_font_names(), PackedStringArray(["Serpen24.fnt"]),
		"the picker capability lists the mounted folder's fonts")
	assert_not_null(ws.load_preview_font("Serpen24.fnt"),
		"fonts load through the runtime path (VFS read -> NovaFntResource -> FontFile)")

	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	var mount := Control.new()
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var preview: Control = mount.find_child("StringsGamePreview", true, false)
	assert_not_null(preview, "the detail panel mounts the engine text preview")
	if preview == null:
		return
	assert_true(preview.has_font(), "the first available font auto-adopts into the panel")
	assert_gt(preview.get_font_file().get_fixed_size(), 0,
		"the adopted FontFile carries the .fnt's fixed pixel size (the engine draw resolution)")
	assert_eq(preview.get_sample_text(), "New Game",
		"the selected entry renders as the game shows it ({hot} marker stripped)")

	var picker: OptionButton = mount.find_child("StringsPreviewFontOption", true, false)
	assert_not_null(picker, "the preview font is chooseable")
	if picker != null:
		assert_false(picker.disabled, "fonts exist, so the picker is live")
		assert_eq(picker.get_item_text(0), "Serpen24.fnt")

	# Selecting another entry re-renders the panel with that entry's display text.
	var detail: Control = mount.find_child("StringsDetailPanel", true, false)
	assert_not_null(detail)
	if detail != null:
		detail.show_entry(5)  # HUD_AMMO -> "Ammo"
		assert_eq(preview.get_sample_text(), "Ammo", "switching entries updates the engine-drawn line")

	ws.release_viewport()
	DirAccess.remove_absolute(root_dir.path_join("Serpen24.fnt"))
	DirAccess.remove_absolute(root_dir)


func test_detail_game_preview_without_a_mounted_root_offers_no_fonts() -> void:
	var workspace_script := preload("res://modtools/strings/strings_workspace.gd")
	var ws = autofree(workspace_script.new())
	assert_eq(ws.get_preview_font_names(), PackedStringArray(),
		"headless / no mounted folder yields no fonts (strings stays shell-only)")

	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	var mount := Control.new()
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var preview: Control = mount.find_child("StringsGamePreview", true, false)
	assert_not_null(preview, "the panel still mounts without fonts")
	if preview != null:
		assert_false(preview.has_font(), "no font claim is rendered without a mounted game folder")
	var picker: OptionButton = mount.find_child("StringsPreviewFontOption", true, false)
	assert_not_null(picker)
	if picker != null:
		assert_true(picker.disabled, "the picker states the gap instead of listing nothing")
	ws.release_viewport()
	autofree(ws.get_document())  # shell-less: the document node has no shell parent to free it


func test_section_filter_scopes_rows() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	await get_tree().process_frame

	var mount: Control = workstation.get_node("%ViewportMount")
	var tree: Tree = mount.get_child(0).find_child("StringsTree", true, false)

	assert_eq(tree.get_root().get_child_count(), 6, "the All filter should show every entry")

	ws.set_section_filter(0)  # menu_main has 3 entries
	await get_tree().process_frame
	assert_eq(tree.get_root().get_child_count(), 3, "filtering to section 0 should scope to its entries")

	ws.set_section_filter(-1)  # back to All
	await get_tree().process_frame
	assert_eq(tree.get_root().get_child_count(), 6, "clearing the filter should restore every entry")


func test_detail_panel_content_fills_panel() -> void:
	# Regression: the detail/inspector roots are bare Controls; their inner
	# make_inspector_box wrapper must be stretched to fill, not collapse to a
	# min-size box in the top-left corner.
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	for _i in 3:
		await get_tree().process_frame

	var mount: Control = workstation.get_node("%ViewportMount")
	var detail: Control = mount.get_child(0).find_child("StringsDetailPanel", true, false)
	assert_not_null(detail, "detail panel should be mounted")
	assert_gt(detail.size.x, 50.0, "detail panel should have a real width from the split")
	var wrapper: Control = detail.get_child(0)
	assert_almost_eq(wrapper.size.x, detail.size.x, 2.0, "content wrapper should fill the panel width")
	assert_gt(wrapper.size.y, 80.0, "content wrapper should fill the panel height, not collapse")


func test_session_restore_reopens_last_file() -> void:
	# First session: open the fixture (which persists the session state).
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	workstation.queue_free()
	await get_tree().process_frame

	# Second session: activating the workspace should reopen the last file.
	var workstation2 = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation2.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws2 = workstation2._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws2.strings_editor.current_path, "res://fixtures/strings/menu.bin",
		"the last opened table should be restored on activate")
	assert_eq(ws2.strings_editor.string_table.get_entry_count(), 6)
	assert_false(ws2.strings_editor.is_dirty, "a restored table starts clean")


func test_inspector_lookup_tester_resolves_like_the_game() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.STRINGS)
	await get_tree().process_frame
	var ws = workstation._get_workspace(EditorWorkstationScript.Workspace.STRINGS)
	assert_eq(ws.open_file("res://fixtures/strings/menu.bin"), OK)
	await get_tree().process_frame

	var lookup: LineEdit = workstation.find_child("StringsLookupEdit", true, false)
	var result: RichTextLabel = workstation.find_child("StringsLookupResult", true, false)
	assert_not_null(lookup, "the inspector should mount the lookup tester")
	assert_not_null(result, "the lookup tester should have a result readout")

	lookup.text = "menu_main:BTN_NEW_GAME"
	lookup.text_changed.emit(lookup.text)
	assert_string_contains(result.text, "New Game", "a scoped hit should preview the in-game text ({hot} stripped)")

	lookup.text = "hud:BTN_NEW_GAME"
	lookup.text_changed.emit(lookup.text)
	assert_string_contains(result.text, "??hud:BTN_NEW_GAME??",
		"a scoped miss should show the engine's ?? marker")

	var normalize: Button = workstation.find_child("StringsNormalizeButton", true, false)
	assert_not_null(normalize, "the normalize affordance should exist")
	assert_false(normalize.visible, "normalize stays hidden while the table is grouped")


func _find_button_by_text(root: Node, text: String) -> Button:
	if root is Button and (root as Button).text == text:
		return root
	for child in root.get_children():
		var found := _find_button_by_text(child, text)
		if found != null:
			return found
	return null
