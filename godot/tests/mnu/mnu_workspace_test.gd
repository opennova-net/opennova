extends GutTest

# M6 + M7 gate: the Menus ONED workspace. Covers the editor document lifecycle,
# the widget tree, the editable property inspector, the edit_mode preview canvas
# (single-screen visibility + letterbox fit), the editor selection wiring, the
# workspace adapter (actions + inspector population), and M7 property editing
# (inspector commits funneling through the editor, fine-grained undo/redo, save
# round-trip, %VAR% color preservation).

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuWorkspaceScript = preload("res://modtools/mnu/mnu_workspace.gd")
const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")
const StringsWorkspaceScript = preload("res://modtools/strings/strings_workspace.gd")

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
const TEMP_DIR := "user://test_mnu_workspace"


func before_each() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEMP_DIR))
	_remove_session_file()


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)
	_remove_session_file()


# Workspace open/save/close now persist the tab session; keep it out of the
# shared user:// state (B4 convention) so tests neither leak into the dev
# editor nor into each other.
func _remove_session_file() -> void:
	var state := MnuWorkspaceScript.STATE_PATH
	if FileAccess.file_exists(state):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(state))


func _cleanup_dir(dir_path: String) -> void:
	var abs := ProjectSettings.globalize_path(dir_path)
	var da := DirAccess.open(abs)
	if da == null:
		return
	da.list_dir_begin()
	var fname := da.get_next()
	while fname != "":
		if not da.current_is_dir():
			da.remove(fname)
		fname = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs)


func _load_resource() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


# Concatenate text under a node from labels AND editable controls (LineEdit,
# Button/CheckBox), so the editable inspector can be verified by content.
func _collect_text(node: Node) -> String:
	var out := ""
	if node is Label:
		out += (node as Label).text + "\n"
	elif node is LineEdit:
		out += (node as LineEdit).text + "\n"
	elif node is OptionButton:
		var option := node as OptionButton
		if option.selected >= 0:
			out += option.get_item_text(option.selected) + "\n"
	elif node is Button:
		out += (node as Button).text + "\n"
	for child in node.get_children():
		out += _collect_text(child)
	return out


# Find the first CheckBox with the given label text (flag rows).
func _find_check(node: Node, label: String) -> CheckBox:
	if node is CheckBox and (node as CheckBox).text == label:
		return node
	for child in node.get_children():
		var found := _find_check(child, label)
		if found != null:
			return found
	return null


# Find the first LineEdit under a node (the inspector's first editable field).
func _first_line_edit(node: Node) -> LineEdit:
	if node is LineEdit:
		return node
	for child in node.get_children():
		var found := _first_line_edit(child)
		if found != null:
			return found
	return null


func _first_root_child(doc: NovaMnuDocument, index: int) -> int:
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	return doc.get_child_ids(root)[index]


func test_document_open_dirty_save_round_trip() -> void:
	var doc = autofree(MnuEditorDocumentScript.new())
	assert_eq(doc.open_mnu(FIXTURE), OK, "Menus document should open a fixture .mnu.")
	assert_false(doc.is_dirty, "Opening should leave the document clean.")
	assert_eq(doc.current_path, FIXTURE, "Opening should set current_path.")
	assert_eq(doc.resource.get_screen_count(), 2, "Fixture has two screens.")

	var start_id := _first_root_child(doc.resource, 1)  # StartBtn
	doc.resource.set_widget_name(start_id, "PlayBtn")
	assert_true(doc.is_dirty, "Mutating the document should dirty it via the changed signal.")

	assert_eq(doc.save_as(TEMP_DIR), OK, "Menus document should save as .mnu.")
	assert_false(doc.is_dirty, "Successful save should mark clean.")
	assert_true(doc.current_path.ends_with(".mnu"), "save_as should choose a .mnu filename.")

	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(doc.current_path), OK, "Saved .mnu should reopen.")
	assert_eq(reloaded.resource.get_widget_name(_first_root_child(reloaded.resource, 1)), "PlayBtn",
		"Edited widget name should survive save/reload.")


func test_widget_tree_mirrors_document_hierarchy() -> void:
	var doc := _load_resource()
	var tree = MnuWidgetTreeScript.new()
	add_child_autofree(tree)
	tree.set_document(doc)
	await get_tree().process_frame

	var root := tree.get_root()
	assert_not_null(root, "Tree should have a (hidden) root.")
	assert_eq(root.get_child_count(), 2, "Two screen items.")

	var main := root.get_child(0)
	assert_string_contains(main.get_text(0), "MAIN", "First screen item is MAIN.")
	assert_eq(int(main.get_metadata(0)), doc.get_screen_ids()[0], "Screen item carries the screen id.")
	assert_eq(main.get_child_count(), 1, "Screen item parents its root window.")

	var root_win := main.get_child(0)
	assert_string_contains(root_win.get_text(0), "ROOT", "Root window item is ROOT.")
	assert_eq(root_win.get_child_count(), 5, "Root window has five widget children.")
	assert_string_contains(root_win.get_child(1).get_text(0), "StartBtn", "Second child is StartBtn.")


func test_widget_tree_selection_emits_id() -> void:
	var doc := _load_resource()
	var tree = MnuWidgetTreeScript.new()
	add_child_autofree(tree)
	tree.set_document(doc)
	await get_tree().process_frame

	var root_win := tree.get_root().get_child(0).get_child(0)
	var start_item := root_win.get_child(1)
	var start_id := int(start_item.get_metadata(0))

	watch_signals(tree)
	start_item.select(0)
	await get_tree().process_frame
	# Note: the 4th arg of assert_signal_emitted_with_parameters is the emission
	# index, not a message, so it is omitted here.
	assert_signal_emitted_with_parameters(tree, "widget_selected", [start_id])


func test_widget_tree_select_id_is_silent_and_sets_selection() -> void:
	var doc := _load_resource()
	var tree = MnuWidgetTreeScript.new()
	add_child_autofree(tree)
	tree.set_document(doc)
	await get_tree().process_frame

	var start_id := _first_root_child(doc, 1)
	watch_signals(tree)
	tree.select_id(start_id)
	assert_signal_emit_count(tree, "widget_selected", 0,
		"Programmatic select_id should not re-emit widget_selected.")
	assert_eq(tree.get_selected_id(), start_id, "select_id should set the active selection.")


func test_canvas_builds_inert_preview_with_single_screen() -> void:
	var doc := _load_resource()
	var canvas = MnuCanvasScript.new()
	add_child_autofree(canvas)
	canvas.size = Vector2(320, 240)
	await get_tree().process_frame
	canvas.set_menu(doc, null, null)
	await get_tree().process_frame

	var preview := canvas.get_node_or_null("Preview")
	assert_not_null(preview, "Canvas holds a NovaMnuMenu preview.")
	assert_true(preview is NovaMnuMenu, "Preview is a NovaMnuMenu.")
	assert_true(preview.get_edit_mode(), "Preview is in edit_mode (inert: no nav/audio/cursor).")

	var screens := 0
	var visible := 0
	for child in preview.get_children():
		if child is NovaMnuScreen:
			screens += 1
			if child.visible:
				visible += 1
	assert_eq(screens, 2, "Both screens are built.")
	assert_eq(visible, 1, "Exactly one screen is visible in the preview.")

	# Anamorphic fill: the 800x600 board fills the 320x240 canvas -> 320/800 == 240/600 == 0.4.
	assert_almost_eq(preview.scale.x, 0.4, 0.01, "Preview scales to fill the canvas (anamorphic).")

	canvas.show_screen_named("OPTIONS")
	var options_visible := false
	for child in preview.get_children():
		if child is NovaMnuScreen and child.get_screen_name() == "OPTIONS":
			options_visible = child.visible
	assert_true(options_visible, "Switching to OPTIONS makes it the visible screen.")


func test_editor_preview_uses_menu_stylesheet_from_resource_root() -> void:
	var dir := OS.get_temp_dir().path_join("mnu_style_preview_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir.path_join("menu_style.mns"), FileAccess.WRITE)
	assert_not_null(f, "Test should create a menu stylesheet fixture.")
	if f == null:
		return
	f.store_string("DEF_FONTNAME Gunpl22b.fnt\nDEF_TEXT_FG FFFFFFFF\n")
	f.close()

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK, "Temp resource root should open.")

	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame

	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(FIXTURE)
	ed.set_resource_root(root)
	ed.set_document(editordoc)
	await get_tree().process_frame

	var preview := ed._canvas.get_node_or_null("Preview") as NovaMnuMenu
	assert_not_null(preview, "Editor canvas should mount a NovaMnuMenu preview.")
	if preview == null:
		return
	var sheet := preview.get_stylesheet()
	assert_not_null(sheet, "Editor preview should use menu_style.mns from the resource root.")
	if sheet != null:
		assert_eq(sheet.substitute("%DEF_FONTNAME%"), "Gunpl22b.fnt",
			"Preview stylesheet should be the canonical menu stylesheet.")
	DirAccess.remove_absolute(dir.path_join("menu_style.mns"))
	DirAccess.remove_absolute(dir)


func test_property_inspector_shows_widget_and_screen() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame

	inspector.show_widget(doc, _first_root_child(doc, 1))  # StartBtn
	await get_tree().process_frame
	var widget_text := _collect_text(inspector)
	assert_string_contains(widget_text, "StartBtn", "Inspector shows the widget name.")
	assert_string_contains(widget_text, "btn_up.tga", "Inspector shows a texture slot.")

	inspector.show_widget(doc, _first_root_child(doc, 2))  # SoundChk (CHECKED)
	await get_tree().process_frame
	var checked := _find_check(inspector, "Checked")
	assert_not_null(checked, "Inspector shows a Checked flag toggle.")
	if checked != null:
		assert_true(checked.button_pressed, "The Checked flag toggle is on for a CHECKED widget.")

	inspector.show_widget(doc, doc.get_screen_ids()[0])  # screen container
	await get_tree().process_frame
	var screen_text := _collect_text(inspector)
	assert_string_contains(screen_text, "Screen", "Screen heading shown for a screen id.")
	assert_string_contains(screen_text, "MAIN", "Screen name shown.")
	assert_string_contains(screen_text, "menutxt.BIN", "Screen text resource shown.")


# Regression: the inspector must be rooted in a container. As a plain Control its
# make_inspector_box margin/scroll/box collapsed to zero size and the whole panel
# rendered blank inside the shell's PanelContainer dock -- the symptom that read as
# "there are no properties". Mount it the way the shell does (in a sized
# PanelContainer) and assert the content box is actually laid out.
func test_inspector_content_is_laid_out_non_zero() -> void:
	var doc := _load_resource()
	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(260, 600)
	panel.size = Vector2(260, 600)
	add_child_autofree(panel)
	var inspector = MnuPropertyInspectorScript.new()
	panel.add_child(inspector)
	await get_tree().process_frame
	inspector.show_widget(doc, _first_root_child(doc, 1))
	await get_tree().process_frame
	await get_tree().process_frame
	var box := inspector.find_child("Box", true, false)
	assert_not_null(box, "Inspector built its content box.")
	if box != null:
		assert_gt(box.size.x, 0.0, "Inspector content has non-zero width (not collapsed).")
		assert_gt(box.size.y, 0.0, "Inspector content has non-zero height (rows laid out).")


func test_color_helper_parses_literal_and_variable() -> void:
	assert_null(MnuUiHelpersScript.color_from_mnu("%DEF_TEXT_FG%"), "A %VAR% reference is unresolved.")
	assert_null(MnuUiHelpersScript.color_from_mnu(""), "Empty is unresolved.")
	var c = MnuUiHelpersScript.color_from_mnu("FF8000")
	assert_not_null(c, "A literal hex color parses.")
	if c != null:
		assert_almost_eq(c.r, 1.0, 0.01, "Red channel.")
		assert_almost_eq(c.g, 0.5, 0.02, "Green channel.")
		assert_almost_eq(c.b, 0.0, 0.01, "Blue channel.")


func test_editor_default_selection_and_emit() -> void:
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(FIXTURE)
	ed.set_document(editordoc)
	await get_tree().process_frame

	assert_eq(ed.get_selected_id(), editordoc.resource.get_screen_ids()[0],
		"Editor defaults selection to the first screen.")

	var start_id := _first_root_child(editordoc.resource, 1)
	watch_signals(ed)
	ed._on_tree_selected(start_id)
	# A tree selection re-emits through the editor for the adapter (4th arg of the
	# assert is the emission index, not a message).
	assert_signal_emitted_with_parameters(ed, "widget_selected", [start_id])
	assert_eq(ed.get_selected_id(), start_id, "Editor tracks the selected id.")


func test_editor_selection_drives_visible_screen() -> void:
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(FIXTURE)
	ed.set_document(editordoc)
	await get_tree().process_frame

	# Select BackBtn (lives on OPTIONS); the preview should switch to OPTIONS.
	var screen1: int = editordoc.resource.get_screen_ids()[1]
	var root2: int = editordoc.resource.get_screen_root_id(screen1)
	var back_id: int = editordoc.resource.get_child_ids(root2)[0]
	ed.select_widget(back_id)
	await get_tree().process_frame

	assert_eq(ed.get_visible_screen_name(), "OPTIONS",
		"Selecting a widget on OPTIONS makes OPTIONS the visible screen.")


func test_workspace_adapter_actions_and_inspector() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.get_workspace_label(), "Menus", "Adapter labels itself for the rail.")
	assert_eq(ws.get_open_dialog_filters(), PackedStringArray(["*.mnu,*.MNU ; Nova menus"]),
		"Open dialog targets Nova .mnu files.")
	assert_eq(ws.get_open_resource_kind(), "menu",
		"Open uses the indexed quick-open browser (kind 'menu'), not the native dialog.")

	assert_eq(ws.open_file(FIXTURE), OK, "Adapter opens a .mnu file.")
	assert_eq(ws.get_project_title(), "widgets", "Title shows the loaded basename.")
	assert_string_contains(ws.get_status_context(), "2 screen", "Status summarizes the screen count.")

	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	assert_gt(mount.get_child_count(), 0, "Adapter mounts the editor in the viewport.")

	var inspector_mount := Control.new()
	add_child_autofree(inspector_mount)
	ws.build_inspector(inspector_mount)
	await get_tree().process_frame
	assert_gt(inspector_mount.get_child_count(), 0, "Adapter mounts the property inspector.")
	assert_string_contains(_collect_text(inspector_mount), "MAIN",
		"Inspector shows the default-selected first screen.")

	var start_id := _first_root_child(ws._document.resource, 1)
	ws._on_widget_selected(start_id)
	await get_tree().process_frame
	assert_string_contains(_collect_text(inspector_mount), "StartBtn",
		"Selecting a widget updates the right-dock inspector.")


func test_document_new_resets_to_empty_clean() -> void:
	var doc = autofree(MnuEditorDocumentScript.new())
	assert_eq(doc.open_mnu(FIXTURE), OK)
	doc.resource.set_widget_name(_first_root_child(doc.resource, 1), "X")
	assert_true(doc.is_dirty, "Editing dirties the document.")

	watch_signals(doc)
	assert_eq(doc.create_new(), OK, "New creates a fresh document.")
	assert_eq(doc.resource.get_screen_count(), 1, "A fresh document has one screen.")
	assert_eq(doc.current_path, "", "New clears the current path.")
	assert_false(doc.is_dirty, "A fresh document is clean.")
	assert_signal_emit_count(doc, "resource_loaded", 1, "New emits resource_loaded once.")
	assert_signal_emit_count(doc, "state_changed", 1, "New emits state_changed once.")


func test_document_save_current_overwrites_and_guards() -> void:
	var doc = autofree(MnuEditorDocumentScript.new())
	# Branch 1: a document with no current path cannot save_current.
	assert_eq(doc.save_current(), ERR_UNAVAILABLE, "save_current guards an empty path.")

	assert_eq(doc.open_mnu(FIXTURE), OK)
	assert_eq(doc.save_as(TEMP_DIR), OK)
	var path: String = doc.current_path
	doc.resource.set_widget_name(_first_root_child(doc.resource, 1), "PlayBtn")
	assert_true(doc.is_dirty)

	# Branch 2: save_current overwrites the current path and clears dirty.
	assert_eq(doc.save_current(), OK, "save_current overwrites the current path.")
	assert_false(doc.is_dirty, "save_current marks clean.")
	assert_eq(doc.current_path, path, "save_current keeps the current path.")

	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(path), OK)
	assert_eq(reloaded.resource.get_widget_name(_first_root_child(reloaded.resource, 1)), "PlayBtn",
		"Edit persisted via save_current.")


func test_adapter_save_predicates() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	# A fresh adapter owns an empty (create_empty) resource: save-as is available,
	# but there is nothing to Save and no unsaved changes.
	assert_false(ws.can_save(), "A clean, path-less document cannot Save.")
	assert_false(ws.has_unsaved_changes(), "A fresh document has no unsaved changes.")
	assert_true(ws.can_save_as(), "Save As is available whenever a resource exists.")

	assert_eq(ws.open_file(FIXTURE), OK)
	ws._document.resource.set_widget_name(_first_root_child(ws._document.resource, 1), "PlayBtn")
	assert_true(ws.can_save(), "A dirty document with a path can Save.")
	assert_true(ws.has_unsaved_changes(), "A dirty document reports unsaved changes.")

	assert_eq(ws.save_as(TEMP_DIR), OK)
	assert_false(ws.can_save(), "Saving clears the dirty flag.")


func test_adapter_remount_reuses_single_editor() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)

	ws.mount_viewport(mount)
	await get_tree().process_frame
	var ed = ws._editor
	# The merged workspace mounts two editors into the viewport mount: the menu
	# editor (visible) and the Mns editor (parked invisible until build_inspector
	# reparents it into the Styles tab).
	assert_eq(mount.get_child_count(), 2, "Menu editor and parked Mns editor mount once each.")

	ws.unmount_viewport(mount)
	assert_eq(mount.get_child_count(), 0, "Unmount removes both editors without freeing them.")

	ws.mount_viewport(mount)
	await get_tree().process_frame
	assert_same(ws._editor, ed, "Remount reuses the same editor instance.")
	assert_eq(mount.get_child_count(), 2, "Both editors re-parent exactly once.")
	assert_eq(ed.widget_selected.get_connections().size(), 1,
		"widget_selected stays connected once, not re-connected on remount.")


func test_adapter_release_viewport_tears_down() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	var inspector_mount := Control.new()
	add_child_autofree(inspector_mount)
	ws.mount_viewport(mount)
	ws.build_inspector(inspector_mount)
	await get_tree().process_frame

	ws.release_viewport()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(mount.get_child_count(), 0, "release_viewport frees the editor.")
	assert_eq(inspector_mount.get_child_count(), 0, "release_viewport frees the inspector.")
	assert_null(ws._editor, "release_viewport nulls the editor reference.")
	assert_null(ws._inspector, "release_viewport nulls the inspector reference.")

	# A later document change must not fault into freed nodes.
	ws._document.resource.set_widget_name(_first_root_child(ws._document.resource, 1), "Z")
	await get_tree().process_frame
	pass_test("No crash after a document change post-teardown.")


func test_adapter_status_reports_unresolved_assets() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	# Before mount the editor is null, so the unresolved tail is suppressed.
	assert_false(ws.get_status_context().contains("unresolved"),
		"No unresolved tail before mount (the editor-null guard).")

	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	# With no resource root the fixture's .tga textures cannot resolve.
	assert_gt(ws._editor.get_unresolved_asset_count(), 0,
		"Fixture textures are unresolved without a resource root.")
	assert_string_contains(ws.get_status_context(), "unresolved asset",
		"Status surfaces the unresolved-asset count after mount.")


func test_inspector_resyncs_to_first_screen_after_reload() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	var inspector_mount := Control.new()
	add_child_autofree(inspector_mount)
	ws.mount_viewport(mount)
	ws.build_inspector(inspector_mount)
	await get_tree().process_frame

	# Push a widget selection, then reload the document.
	ws._on_widget_selected(_first_root_child(ws._document.resource, 1))
	await get_tree().process_frame
	assert_string_contains(_collect_text(inspector_mount), "StartBtn", "Inspector shows the selection.")

	assert_eq(ws._document.open_mnu(FIXTURE), OK)
	await get_tree().process_frame
	# The editor resets to the first screen on reload and pushes it to the inspector.
	assert_string_contains(_collect_text(inspector_mount), "MAIN",
		"Inspector resyncs to the first screen after a document reload.")


func test_inspector_renders_color_swatch_for_variable() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame

	# The MAIN root window carries DEFAULT_FG=%DEF_TEXT_FG% (a stylesheet variable),
	# which must render as an unresolved (transparent) swatch, preserving the token.
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	inspector.show_widget(doc, root_id)
	await get_tree().process_frame

	var swatches := _collect_color_rects(inspector)
	assert_gt(swatches.size(), 0, "Inspector renders a swatch for the color slot.")
	if swatches.size() > 0:
		assert_almost_eq(swatches[0].color.a, 0.0, 0.001,
			"A %VAR% color renders as an unresolved (transparent) swatch.")


func _collect_color_rects(node: Node) -> Array:
	var out: Array = []
	if node is ColorRect:
		out.append(node)
	for child in node.get_children():
		out.append_array(_collect_color_rects(child))
	return out


# --- M7: property editing, undo/redo, save round-trip ---------------------------

# An editor mounted on a fixture document, ready for apply_edit. Returns
# [editor, editor_document].
func _editor_with_fixture() -> Array:
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(FIXTURE)
	ed.set_document(editordoc)
	await get_tree().process_frame
	return [ed, editordoc]


func _find_widget_id(doc: NovaMnuDocument, id: int, wname: String) -> int:
	if doc.get_widget_name(id) == wname:
		return id
	for cid in doc.get_child_ids(id):
		var f := _find_widget_id(doc, cid, wname)
		if f != -1:
			return f
	return -1


# M9.7: the inspector surfaces a marquee's DATASOURCE as an editable scalar that
# routes through the same apply_edit / undo path as the other widget properties.
func test_editor_marquee_datasource_edit_and_undo() -> void:
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu("res://../fixtures/mnu/all_widgets.mnu")
	ed.set_document(editordoc)
	await get_tree().process_frame

	var doc = editordoc.resource
	var mid := _find_widget_id(doc, doc.get_screen_root_id(doc.get_screen_ids()[0]), "Credits")
	assert_gt(mid, 0, "marquee located")
	ed.select_widget(mid)
	assert_eq(doc.get_widget_datasource(mid), "credits.txt", "initial datasource")

	ed.apply_edit({"target": "widget", "id": mid, "prop": "datasource", "value": "new.txt"})
	assert_eq(doc.get_widget_datasource(mid), "new.txt", "datasource edit applied")
	assert_true(ed.can_undo(), "datasource edit is undoable")
	ed.undo()
	assert_eq(doc.get_widget_datasource(mid), "credits.txt", "undo reverts datasource")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_editor_group_edit_and_undo() -> void:
	# The radio/checkbox group id routes through the generic prop-edit path (the
	# inspector only surfaces the row for toggle types, but apply_edit works on any
	# widget). Mirrors the datasource scalar test.
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu("res://../fixtures/mnu/all_widgets.mnu")
	ed.set_document(editordoc)
	await get_tree().process_frame

	var doc = editordoc.resource
	var wid := _find_widget_id(doc, doc.get_screen_root_id(doc.get_screen_ids()[0]), "Credits")
	assert_gt(wid, 0, "widget located")
	ed.select_widget(wid)
	var before: int = doc.get_widget_group(wid)

	ed.apply_edit({"target": "widget", "id": wid, "prop": "group", "value": before + 3})
	assert_eq(doc.get_widget_group(wid), before + 3, "group edit applied")
	assert_true(ed.can_undo(), "group edit is undoable")
	ed.undo()
	assert_eq(doc.get_widget_group(wid), before, "undo reverts group")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_editor_apply_edit_pushes_undo_and_reverts() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var start_id := _first_root_child(editordoc.resource, 1)
	ed.select_widget(start_id)
	assert_false(ed.can_undo(), "No undo before any edit.")

	ed.apply_edit({"target": "widget", "id": start_id, "prop": "name", "value": "PlayBtn"})
	assert_eq(editordoc.resource.get_widget_name(start_id), "PlayBtn", "apply_edit mutates the document.")
	assert_true(editordoc.is_dirty, "An applied edit dirties the document.")
	assert_true(ed.can_undo(), "An applied edit can be undone.")
	assert_false(ed.can_redo(), "Nothing to redo yet.")

	ed.undo()
	assert_eq(editordoc.resource.get_widget_name(start_id), "StartBtn", "Undo reverts the name.")
	assert_true(ed.can_redo(), "Undo enables redo.")
	assert_false(ed.can_undo(), "The only op was undone.")

	ed.redo()
	assert_eq(editordoc.resource.get_widget_name(start_id), "PlayBtn", "Redo re-applies the name.")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_editor_apply_edit_noop_when_unchanged() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var start_id := _first_root_child(editordoc.resource, 1)
	var current: String = editordoc.resource.get_widget_name(start_id)
	ed.apply_edit({"target": "widget", "id": start_id, "prop": "name", "value": current})
	assert_false(ed.can_undo(), "Re-applying the current value records no undo op.")
	assert_false(editordoc.is_dirty, "A no-op edit does not dirty the document.")


func test_editor_rect_edit_round_trips_through_save() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var start_id := _first_root_child(ws._document.resource, 1)
	ws._on_inspector_edit({"target": "widget", "id": start_id, "prop": "rect", "value": Rect2(12, 34, 100, 40)})
	assert_eq(ws._document.resource.get_window_rect(start_id), Rect2(12, 34, 100, 40),
		"A rect edit applies through the adapter -> editor.")

	assert_eq(ws.save_as(TEMP_DIR), OK)
	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(ws._document.current_path), OK)
	assert_eq(reloaded.resource.get_window_rect(_first_root_child(reloaded.resource, 1)), Rect2(12, 34, 100, 40),
		"Edited rect survives save + reload.")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_inspector_emits_edit_requested_on_text_commit() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame
	var start_id := _first_root_child(doc, 1)
	inspector.show_widget(doc, start_id)
	await get_tree().process_frame

	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))

	var name_edit := _first_line_edit(inspector)
	assert_not_null(name_edit, "Inspector has an editable Name field.")
	name_edit.text = "PlayBtn"
	name_edit.text_submitted.emit("PlayBtn")

	assert_eq(captured.size(), 1, "Committing the field emits one edit_requested.")
	if captured.size() == 1:
		var e: Dictionary = captured[0]
		assert_eq(e.get("target"), "widget", "Edit targets the widget.")
		assert_eq(e.get("id"), start_id, "Edit carries the widget id.")
		assert_eq(e.get("prop"), "name", "Edit names the property.")
		assert_eq(e.get("value"), "PlayBtn", "Edit carries the committed value.")


func test_inspector_flag_toggle_emits_recomputed_mask() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame
	var chk_id := _first_root_child(doc, 2)  # SoundChk (CHECKED)
	inspector.show_widget(doc, chk_id)
	await get_tree().process_frame

	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))

	var disabled := _find_check(inspector, "Disabled")
	assert_not_null(disabled, "Inspector shows a Disabled flag toggle.")
	if disabled != null:
		disabled.button_pressed = true  # fires toggled -> emits a flags edit
		assert_eq(captured.size(), 1, "Toggling a flag emits one edit_requested.")
		if captured.size() == 1:
			var e: Dictionary = captured[0]
			assert_eq(e.get("prop"), "flags", "A flag toggle commits the flags property.")
			var mask := int(e.get("value"))
			assert_true((mask & NovaMnuDocument.FLAG_DISABLED) != 0, "The toggled flag is set in the mask.")
			assert_true((mask & NovaMnuDocument.FLAG_CHECKED) != 0, "Existing flags are preserved in the mask.")


func test_adapter_delegates_undo_redo() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_false(ws.can_undo(), "No editor yet -> nothing to undo.")
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var start_id := _first_root_child(ws._document.resource, 1)
	assert_false(ws.can_undo(), "Mounted but no edit -> nothing to undo.")
	ws._on_inspector_edit({"target": "widget", "id": start_id, "prop": "name", "value": "PlayBtn"})
	assert_true(ws.can_undo(), "An inspector edit is undoable through the adapter.")

	ws.undo()
	assert_eq(ws._document.resource.get_widget_name(start_id), "StartBtn", "Adapter undo reverts the edit.")
	assert_true(ws.can_redo(), "Adapter exposes redo after an undo.")
	ws.redo()
	assert_eq(ws._document.resource.get_widget_name(start_id), "PlayBtn", "Adapter redo re-applies the edit.")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_editor_color_edit_preserves_variable_token() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var root_id: int = editordoc.resource.get_screen_root_id(editordoc.resource.get_screen_ids()[0])
	ed.select_widget(root_id)

	ed.apply_edit({"target": "widget", "id": root_id, "prop": "color",
		"slot": NovaMnuDocument.COLOR_DEFAULT_FG, "value": "00FF00"})
	assert_eq(editordoc.resource.get_widget_color(root_id, NovaMnuDocument.COLOR_DEFAULT_FG), "00FF00",
		"A literal hex color applies.")

	ed.apply_edit({"target": "widget", "id": root_id, "prop": "color",
		"slot": NovaMnuDocument.COLOR_DEFAULT_FG, "value": "%CUSTOM_FG%"})
	assert_eq(editordoc.resource.get_widget_color(root_id, NovaMnuDocument.COLOR_DEFAULT_FG), "%CUSTOM_FG%",
		"A %VAR% token is stored verbatim (survives the round-trip).")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_editor_screen_property_edit_and_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var screen_id: int = editordoc.resource.get_screen_ids()[0]
	var before: int = editordoc.resource.get_screen_music_var(screen_id)
	ed.select_widget(screen_id)

	ed.apply_edit({"target": "screen", "id": screen_id, "prop": "music_var", "value": before + 5})
	assert_eq(editordoc.resource.get_screen_music_var(screen_id), before + 5, "Screen music var edits apply.")
	ed.undo()
	assert_eq(editordoc.resource.get_screen_music_var(screen_id), before, "Undo restores the screen music var.")
	await get_tree().process_frame  # flush queue_free'd preview generations


func test_inspector_text_commit_skipped_when_detached() -> void:
	# A focus_exited that fires once the field has left the tree (rebuild/teardown)
	# must not commit a stale edit. Guards against deferred-signal-on-dying-control.
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame
	inspector.show_widget(doc, _first_root_child(doc, 1))
	await get_tree().process_frame

	var edit := _first_line_edit(inspector)
	assert_not_null(edit, "Inspector has an editable field.")
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))

	edit.get_parent().remove_child(edit)  # simulate the field leaving the tree
	edit.text = "Late"
	edit.focus_exited.emit()
	assert_eq(captured.size(), 0, "A focus_exited after the field leaves the tree does not commit.")
	edit.free()  # we own the detached node now


# --- M8b: structural ops (add / delete / reparent / screens) + snapshot undo ----

func _main_root(doc: NovaMnuDocument) -> int:
	return doc.get_screen_root_id(doc.get_screen_ids()[0])


func test_editor_add_widget_grows_tree_and_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)
	ed.select_widget(root)
	var before := doc.get_child_ids(root).size()

	var new_id: int = ed.add_widget_action(NovaMnuDocument.TYPE_BUTTON)
	assert_gt(new_id, 0, "add_widget_action returns a new id.")
	assert_eq(doc.get_child_ids(root).size(), before + 1, "The tree grew by one child.")
	assert_eq(ed.get_selected_id(), new_id, "The new widget is selected.")
	assert_true(editordoc.is_dirty, "Adding a widget dirties the document.")
	assert_true(ed.can_undo(), "An add is undoable.")

	ed.undo()
	assert_false(doc.widget_exists(new_id), "Undo removes the added widget.")
	assert_eq(doc.get_child_ids(root).size(), before, "Undo restores the child count.")

	ed.redo()
	assert_true(doc.widget_exists(new_id), "Redo re-adds the widget with the same id.")
	assert_eq(doc.get_child_ids(root).size(), before + 1, "Redo restores the child.")
	await get_tree().process_frame


func test_editor_delete_widget_shrinks_and_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)
	var start_id := _first_root_child(doc, 1)  # StartBtn
	var name: String = doc.get_widget_name(start_id)
	var rect: Rect2 = doc.get_window_rect(start_id)
	var tex: String = doc.get_widget_texture(start_id, NovaMnuDocument.TEX_DEFAULT)
	var before := doc.get_child_ids(root).size()

	ed.select_widget(start_id)
	ed.delete_selection_action()
	assert_eq(doc.get_child_ids(root).size(), before - 1, "Delete shrinks the tree.")
	assert_false(doc.widget_exists(start_id), "The deleted id is gone.")

	ed.undo()
	assert_true(doc.widget_exists(start_id), "Undo restores the deleted widget.")
	assert_eq(doc.get_widget_name(start_id), name, "Undo restores the name.")
	assert_eq(doc.get_window_rect(start_id), rect, "Undo restores the rect.")
	assert_eq(doc.get_widget_texture(start_id, NovaMnuDocument.TEX_DEFAULT), tex, "Undo restores the texture.")
	await get_tree().process_frame


func test_editor_delete_refused_for_screen_and_root() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var screen := doc.get_screen_ids()[0]

	ed.select_widget(screen)
	ed.delete_selection_action()
	assert_false(ed.can_undo(), "Deleting a screen via the widget-delete action is a no-op (no undo).")
	assert_eq(doc.get_screen_count(), 2, "The screen is not deleted.")

	ed.select_widget(doc.get_screen_root_id(screen))
	ed.delete_selection_action()
	assert_false(ed.can_undo(), "Deleting a root window is a no-op.")
	await get_tree().process_frame


func test_editor_reparent_moves_subtree_and_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)
	var children := doc.get_child_ids(root)
	var title_id: int = children[0]
	var sound_id: int = children[2]  # SoundChk

	ed.reparent_action(sound_id, title_id, 0)
	assert_eq(doc.get_parent_id(sound_id), title_id, "SoundChk reparented under Title.")
	assert_eq(doc.get_child_ids(root).size(), 4, "Root lost a child.")

	ed.undo()
	assert_eq(doc.get_parent_id(sound_id), root, "Undo restores the original parent.")
	assert_eq(doc.get_child_ids(root).size(), 5, "Undo restores the child.")
	assert_eq(doc.get_widget_name(sound_id), "SoundChk", "The id (and name) survive the reparent + undo.")
	await get_tree().process_frame


func test_editor_reparent_cycle_refused() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)
	var start_id := _first_root_child(doc, 1)  # StartBtn

	ed.select_widget(start_id)
	var grand: int = ed.add_widget_action(NovaMnuDocument.TYPE_WINDOW)  # child of StartBtn
	ed.reparent_action(start_id, grand, 0)  # move parent into its own descendant
	assert_eq(doc.get_parent_id(start_id), root, "A cycle reparent is refused; the parent is unchanged.")
	await get_tree().process_frame


func test_editor_interleaved_struct_and_prop_undo_redo() -> void:
	# Keystone: add -> rename the added widget -> delete a sibling -> undo x3 ->
	# redo x3. The rename op (which stores a widget id) must stay valid across the
	# structural undo/redo, which only holds if apply_state restores ids exactly.
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)

	ed.select_widget(root)
	var added: int = ed.add_widget_action(NovaMnuDocument.TYPE_BUTTON)
	assert_gt(added, 0, "Add succeeds.")
	ed.apply_edit({"target": "widget", "id": added, "prop": "name", "value": "MyBtn"})
	assert_eq(doc.get_widget_name(added), "MyBtn", "The added widget is renamed.")

	var start_id := _first_root_child(doc, 1)  # StartBtn (sibling)
	ed.select_widget(start_id)
	ed.delete_selection_action()
	assert_false(doc.widget_exists(start_id), "Sibling StartBtn deleted.")
	assert_true(doc.widget_exists(added), "The added widget survives the sibling delete.")

	ed.undo()  # undo delete
	assert_true(doc.widget_exists(start_id), "Undo#1 restores StartBtn.")
	assert_eq(doc.get_widget_name(added), "MyBtn", "The added widget is still 'MyBtn' after undo#1.")
	ed.undo()  # undo rename
	assert_true(doc.widget_exists(added), "The added widget id is still valid after undo#2.")
	assert_ne(doc.get_widget_name(added), "MyBtn", "Undo#2 reverts the rename.")
	ed.undo()  # undo add
	assert_false(doc.widget_exists(added), "Undo#3 removes the added widget.")
	assert_eq(doc.get_child_ids(root).size(), 5, "Back to the original child count.")

	ed.redo()  # re-add
	assert_true(doc.widget_exists(added), "Redo#1 re-adds with the same id.")
	ed.redo()  # re-rename
	assert_eq(doc.get_widget_name(added), "MyBtn", "Redo#2 re-applies the rename.")
	ed.redo()  # re-delete
	assert_false(doc.widget_exists(start_id), "Redo#3 re-deletes StartBtn.")
	await get_tree().process_frame


func test_editor_add_and_delete_screen_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var n := doc.get_screen_count()

	var sid: int = ed.add_screen_action()
	assert_eq(doc.get_screen_count(), n + 1, "Add screen grows the screen count.")
	assert_eq(ed.get_selected_id(), sid, "The new screen is selected.")
	ed.undo()
	assert_eq(doc.get_screen_count(), n, "Undo removes the screen.")
	ed.redo()
	assert_eq(doc.get_screen_count(), n + 1, "Redo re-adds the screen.")

	ed.delete_screen_action()
	assert_eq(doc.get_screen_count(), n, "Delete removes the visible screen.")
	ed.undo()
	assert_eq(doc.get_screen_count(), n + 1, "Undo restores the deleted screen.")
	await get_tree().process_frame


func test_editor_add_screen_uses_unique_name() -> void:
	# Screen visibility/selection/deletion resolve screens by name, so the toolbar's
	# default name must stay unique or two screens would alias (wrong one shown/deleted).
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var s1: int = ed.add_screen_action()
	var s2: int = ed.add_screen_action()
	assert_ne(doc.get_screen_name(s1), doc.get_screen_name(s2), "Two added screens get distinct names.")
	await get_tree().process_frame


func test_editor_reparent_noop_not_recorded() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := _main_root(doc)
	var children := doc.get_child_ids(root)
	var last: int = children[children.size() - 1]

	ed.reparent_action(last, root, children.size())  # drop in the same slot -> no-op
	assert_false(ed.can_undo(), "A no-op reparent records no undo entry.")
	assert_false(editordoc.is_dirty, "A no-op reparent does not dirty the document.")
	await get_tree().process_frame


func test_editor_delete_last_screen_refused() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	editordoc.create_new()  # one screen
	await get_tree().process_frame
	ed.delete_screen_action()
	assert_eq(editordoc.resource.get_screen_count(), 1, "The last screen cannot be deleted.")


func test_struct_edit_then_save_reload_roundtrip() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	var doc: NovaMnuDocument = ws._document.resource
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])

	# Add a Static under root, then reparent StartBtn under Title.
	ws._editor.select_widget(root)
	var static_name: String = NovaMnuDocument.new().get_widget_type_name(NovaMnuDocument.TYPE_STATIC)
	ws._editor.add_widget_action(NovaMnuDocument.TYPE_STATIC)
	var children := doc.get_child_ids(root)
	ws._editor.reparent_action(children[1], children[0], 0)  # StartBtn under Title

	assert_eq(ws.save_as(TEMP_DIR), OK)
	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(ws._document.current_path), OK)
	var rroot: int = reloaded.resource.get_screen_root_id(reloaded.resource.get_screen_ids()[0])
	var rchildren: PackedInt32Array = reloaded.resource.get_child_ids(rroot)

	assert_eq(rchildren.size(), 5, "Root has 4 originals minus StartBtn plus the added Static.")
	var rnames := []
	for c in rchildren:
		rnames.append(reloaded.resource.get_widget_name(c))
	assert_false(rnames.has("StartBtn"), "StartBtn is no longer a direct child of root.")
	assert_true(rnames.has(static_name), "The added Static persisted.")
	# StartBtn persisted as Title's child.
	var title_id: int = rchildren[0]
	assert_eq(reloaded.resource.get_widget_name(title_id), "Title", "Title is still first.")
	var has_start := false
	for c in reloaded.resource.get_child_ids(title_id):
		if reloaded.resource.get_widget_name(c) == "StartBtn":
			has_start = true
	assert_true(has_start, "StartBtn persisted as Title's child after save + reload.")
	await get_tree().process_frame


# --- M10: nested template authoring (item rows + table columns) -----------------

const ALL_WIDGETS := "res://../fixtures/mnu/all_widgets.mnu"


func _editor_with_all_widgets() -> Array:
	var ed = MnuEditorScript.new()
	add_child_autofree(ed)
	ed.size = Vector2(640, 400)
	await get_tree().process_frame
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(ALL_WIDGETS)
	ed.set_document(editordoc)
	await get_tree().process_frame
	return [ed, editordoc]


func _widget_named(doc: NovaMnuDocument, wname: String) -> int:
	for sid in doc.get_screen_ids():
		var f := _find_widget_id(doc, doc.get_screen_root_id(sid), wname)
		if f != -1:
			return f
	return -1


func test_editor_item_add_remove_undo_redo() -> void:
	var pair = await _editor_with_all_widgets()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var list := _widget_named(doc, "MissionList")
	ed.select_widget(list)
	assert_eq(doc.get_item_count(list), 2, "MissionList starts with 2 items.")

	ed.apply_edit({"id": list, "op": "item_add", "row": {"type": "id", "value": "2", "text": "MM_Charlie"}})
	assert_eq(doc.get_item_count(list), 3, "Adding an item grows the list.")
	assert_true(editordoc.is_dirty, "An item add dirties the document.")
	assert_true(ed.can_undo(), "An item add is undoable.")

	ed.undo()
	assert_eq(doc.get_item_count(list), 2, "Undo removes the added item.")
	ed.redo()
	assert_eq(doc.get_item_count(list), 3, "Redo re-adds the item.")
	assert_eq(doc.get_item(list, 2)["text"], "MM_Charlie", "Redo restores the row content.")
	await get_tree().process_frame


func test_editor_item_move_and_undo() -> void:
	var pair = await _editor_with_all_widgets()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var list := _widget_named(doc, "MissionList")
	ed.select_widget(list)

	ed.apply_edit({"id": list, "op": "item_move", "from": 1, "to": 0})
	assert_eq(doc.get_item(list, 0)["text"], "MM_Bravo", "Move reorders the rows.")
	ed.undo()
	assert_eq(doc.get_item(list, 0)["text"], "MM_Alpha", "Undo restores the order.")
	await get_tree().process_frame


func test_editor_item_field_edit_keeps_selection_and_undo() -> void:
	var pair = await _editor_with_all_widgets()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var list := _widget_named(doc, "MissionList")
	ed.select_widget(list)

	ed.apply_edit({"id": list, "op": "item_field", "index": 0, "key": "text", "value": "MM_Zulu"})
	assert_eq(doc.get_item(list, 0)["text"], "MM_Zulu", "A field edit changes the cell.")
	assert_eq(ed.get_selected_id(), list, "A field edit keeps the list selected (no rebuild churn).")
	assert_true(ed.can_undo(), "A field edit is undoable.")

	# An unchanged re-commit (blur after Enter) records no extra undo entry.
	ed.apply_edit({"id": list, "op": "item_field", "index": 0, "key": "text", "value": "MM_Zulu"})
	ed.undo()
	assert_eq(doc.get_item(list, 0)["text"], "MM_Alpha", "Undo reverts the cell once.")
	assert_false(ed.can_undo(), "The unchanged re-commit recorded no extra undo entry.")
	await get_tree().process_frame


func test_editor_combo_item_add_round_trips_through_save() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(ALL_WIDGETS), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	var doc: NovaMnuDocument = ws._document.resource
	var combo := _widget_named(doc, "ServerList")
	assert_eq(doc.get_item_count(combo), 3, "ServerList combo reads its LIST_BOX rows.")

	ws._on_inspector_edit({"id": combo, "op": "item_add", "row": {"value": "3", "text": "Co-op Server"}})
	assert_eq(doc.get_item_count(combo), 4, "An add through the adapter grows the combo.")

	assert_eq(ws.save_as(TEMP_DIR), OK)
	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(ws._document.current_path), OK)
	var rcombo := _widget_named(reloaded.resource, "ServerList")
	assert_eq(reloaded.resource.get_item_count(rcombo), 4, "Added combo row survives save + reload.")
	assert_eq(reloaded.resource.get_item(rcombo, 3)["text"], "Co-op Server", "Combo row content persisted.")
	await get_tree().process_frame


func test_editor_table_header_edit_and_undo() -> void:
	var pair = await _editor_with_all_widgets()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var table := _widget_named(doc, "MissionTable")
	ed.select_widget(table)
	assert_eq(doc.get_table_headers(table).size(), 3, "MissionTable has 3 headers.")

	ed.apply_edit({"id": table, "op": "header_field", "index": 0, "key": "text", "value": "Mission"})
	assert_eq(String(doc.get_table_headers(table)[0]["text"]), "Mission", "Header rename applies.")
	ed.undo()
	assert_eq(String(doc.get_table_headers(table)[0]["text"]), "Name", "Undo reverts the header rename.")

	# Column count routes through the scalar path (no "op" key).
	ed.apply_edit({"target": "widget", "id": table, "prop": "table_count", "value": 5})
	assert_eq(doc.get_table_column_count(table), 5, "Column count edit applies via the scalar path.")
	ed.undo()
	assert_eq(doc.get_table_column_count(table), 3, "Undo restores the column count.")
	await get_tree().process_frame


func test_editor_action_edit_and_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var start := _first_root_child(doc, 1)
	ed.select_widget(start)
	var before: Array = doc.get_widget_actions(start)
	assert_eq(before.size(), 1, "StartBtn starts with one screen action.")

	var after: Array = before.duplicate()
	after.append({"type": "window", "target": "SoundChk", "state": "HIDE", "file": ""})
	ed.apply_edit({"target": "widget", "id": start, "prop": "actions", "value": after})
	assert_eq(doc.get_widget_actions(start).size(), 2, "The action list edit applies.")
	assert_true(editordoc.is_dirty, "An action edit dirties the document.")
	assert_true(ed.can_undo(), "An action edit is undoable.")

	ed.undo()
	assert_eq(doc.get_widget_actions(start).size(), 1, "Undo restores the original action list.")
	ed.redo()
	assert_eq(String(doc.get_widget_actions(start)[1]["target"]), "SoundChk", "Redo restores the added target.")
	await get_tree().process_frame


func test_inspector_renders_item_and_table_sections() -> void:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(ALL_WIDGETS))
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame

	inspector.show_widget(doc, _widget_named(doc, "MissionList"))
	await get_tree().process_frame
	var list_text := _collect_text(inspector)
	assert_string_contains(list_text, "Items", "Inspector shows an Items section for a list.")
	assert_string_contains(list_text, "MM_Alpha", "Inspector shows the existing item rows.")
	assert_string_contains(list_text, "Add color", "Inspector offers an Add-color affordance for empty slots.")

	inspector.show_widget(doc, _widget_named(doc, "MissionTable"))
	await get_tree().process_frame
	var table_text := _collect_text(inspector)
	assert_string_contains(table_text, "Table columns", "Inspector shows a Table columns section.")
	assert_string_contains(table_text, "Name", "Inspector shows the existing header rows.")
	assert_string_contains(table_text, "Substitutions", "Inspector shows a Substitutions section for a table.")
	assert_string_contains(table_text, "ping_lan.tga", "Inspector shows the existing SUBST rows.")


func test_inspector_renders_action_rows() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame

	inspector.show_widget(doc, _first_root_child(doc, 1)) # StartBtn
	await get_tree().process_frame
	var text := _collect_text(inspector)
	assert_string_contains(text, "Actions", "Inspector shows the visual scripting section.")
	assert_string_contains(text, "screen", "Existing action verb is visible.")
	assert_string_contains(text, "OPTIONS", "Existing action target is visible.")
	assert_string_contains(text, "Add action", "Inspector offers an action add affordance.")


# Adding a previously-empty color slot (the inspector's "Add color" affordance)
# routes through the normal prop-edit path: it persists through save + reload and
# reverts on undo.
func test_editor_add_color_slot_persists_through_save_and_undo() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	var doc: NovaMnuDocument = ws._document.resource
	var root := _main_root(doc)
	assert_eq(doc.get_widget_color(root, NovaMnuDocument.COLOR_DEFAULT_BG), "", "Root background color starts empty.")

	ws._on_inspector_edit({"target": "widget", "id": root, "prop": "color",
		"slot": NovaMnuDocument.COLOR_DEFAULT_BG, "value": "FFFFFF"})
	assert_eq(doc.get_widget_color(root, NovaMnuDocument.COLOR_DEFAULT_BG), "FFFFFF", "The add-color edit sets the slot.")
	assert_true(ws._editor.can_undo(), "Adding a color slot is undoable.")

	assert_eq(ws.save_as(TEMP_DIR), OK)
	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(ws._document.current_path), OK)
	var rroot := _main_root(reloaded.resource)
	assert_eq(reloaded.resource.get_widget_color(rroot, NovaMnuDocument.COLOR_DEFAULT_BG), "FFFFFF",
		"The added color slot survives save + reload.")

	ws._editor.undo()
	assert_eq(doc.get_widget_color(root, NovaMnuDocument.COLOR_DEFAULT_BG), "", "Undo clears the added color slot.")
	await get_tree().process_frame


# Table SUBST (value->image) cells are now authorable: the editor reads them, an
# add + field edit applies and stays focus-stable, and both survive save + reload
# (the regression guard for the historic dropped-on-save bug).
func test_editor_table_subst_edit_and_round_trip() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(ALL_WIDGETS), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	var doc: NovaMnuDocument = ws._document.resource
	var table := _widget_named(doc, "MissionTable")
	assert_eq(doc.get_table_substs(table).size(), 1, "MissionTable reads its one SUBST cell.")
	var first: Dictionary = doc.get_table_substs(table)[0]
	assert_eq(int(first["column"]), 2, "SUBST column read.")
	assert_eq(String(first["value"]), "lan", "SUBST value read.")
	assert_true(bool(first["is_file"]), "SUBST FILE flag read.")
	assert_eq(String(first["file"]), "ping_lan.tga", "SUBST file read.")

	ws._on_inspector_edit({"id": table, "op": "subst_add",
		"row": {"column": 2, "value": "co", "is_file": true, "file": "ping_co.tga"}})
	assert_eq(doc.get_table_substs(table).size(), 2, "Adding a SUBST grows the list.")
	ws._on_inspector_edit({"id": table, "op": "subst_field", "index": 0, "key": "file", "value": "ping_lan2.tga"})
	assert_eq(String(doc.get_table_substs(table)[0]["file"]), "ping_lan2.tga", "A SUBST field edit applies.")
	assert_eq(ws._editor.get_selected_id(), table, "A SUBST field edit keeps the table selected.")

	assert_eq(ws.save_as(TEMP_DIR), OK)
	var reloaded = autofree(MnuEditorDocumentScript.new())
	assert_eq(reloaded.open_mnu(ws._document.current_path), OK)
	var rtable := _widget_named(reloaded.resource, "MissionTable")
	var rsubsts: Array = reloaded.resource.get_table_substs(rtable)
	assert_eq(rsubsts.size(), 2, "Both SUBST cells survive save + reload.")
	assert_eq(String(rsubsts[0]["file"]), "ping_lan2.tga", "The edited SUBST file persisted.")
	assert_eq(String(rsubsts[1]["value"]), "co", "The added SUBST persisted.")

	ws._editor.undo()
	assert_eq(String(doc.get_table_substs(table)[0]["file"]), "ping_lan.tga", "Undo reverts the SUBST field edit.")
	await get_tree().process_frame


# --- Phase 4: multi-select (batch undo, selection ripple, summary) --------------

func test_editor_apply_rect_batch_one_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var a := _first_root_child(doc, 1)
	var b := _first_root_child(doc, 0)
	var a0: Rect2 = doc.get_window_rect(a)
	var b0: Rect2 = doc.get_window_rect(b)
	ed.select_widgets(PackedInt32Array([a, b]))
	ed.apply_rect_batch([
		{"id": a, "rect": Rect2(a0.position + Vector2(10, 5), a0.size)},
		{"id": b, "rect": Rect2(b0.position + Vector2(10, 5), b0.size)},
	])
	assert_eq(doc.get_window_rect(a), Rect2(a0.position + Vector2(10, 5), a0.size), "A moved via the batch.")
	assert_eq(doc.get_window_rect(b), Rect2(b0.position + Vector2(10, 5), b0.size), "B moved via the batch.")
	assert_true(ed.can_undo(), "The batch is undoable.")
	ed.undo()
	assert_eq(doc.get_window_rect(a), a0, "One undo reverts A...")
	assert_eq(doc.get_window_rect(b), b0, "...and B in the same single step.")
	assert_false(ed.can_undo(), "The batch was one undo entry.")
	ed.redo()
	assert_eq(doc.get_window_rect(a), Rect2(a0.position + Vector2(10, 5), a0.size), "Redo re-applies A.")
	assert_eq(doc.get_window_rect(b), Rect2(b0.position + Vector2(10, 5), b0.size), "Redo re-applies B.")
	await get_tree().process_frame


func test_editor_select_widgets_emits_selection_changed() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var a := _first_root_child(doc, 1)
	var b := _first_root_child(doc, 0)
	watch_signals(ed)
	ed.select_widgets(PackedInt32Array([a, b]))
	assert_signal_emitted_with_parameters(ed, "selection_changed", [PackedInt32Array([a, b])])
	assert_eq(ed.get_selected_id(), b, "The active id is the last member of the set.")
	await get_tree().process_frame


func test_inspector_show_selection_summary() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame
	var a := _first_root_child(doc, 1)  # StartBtn
	var b := _first_root_child(doc, 0)
	inspector.show_selection(doc, PackedInt32Array([a, b]))
	await get_tree().process_frame
	var text := _collect_text(inspector)
	assert_string_contains(text, "2 widgets selected", "The summary shows the selection count.")
	assert_string_contains(text, "StartBtn", "The summary lists a member by name.")
	# A single-id selection falls back to the normal editable single-widget view.
	inspector.show_selection(doc, PackedInt32Array([a]))
	await get_tree().process_frame
	assert_string_contains(_collect_text(inspector), "Name", "A single-id selection shows the editable rows.")


func test_adapter_routes_multi_selection_to_inspector() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var mount := Control.new()
	mount.size = Vector2(800, 480)
	add_child_autofree(mount)
	var inspector_mount := Control.new()
	add_child_autofree(inspector_mount)
	ws.mount_viewport(mount)
	ws.build_inspector(inspector_mount)
	await get_tree().process_frame
	var doc: NovaMnuDocument = ws._document.resource
	var a := _first_root_child(doc, 1)
	var b := _first_root_child(doc, 0)
	ws._editor.select_widgets(PackedInt32Array([a, b]))
	await get_tree().process_frame
	assert_string_contains(_collect_text(inspector_mount), "2 widgets selected",
		"A multi-selection routes to the inspector summary through the adapter.")


# --- Phase 5: strings integration + cross-workspace jumps ------------------------

# Stub shell capturing the cross-workspace jump calls the adapter makes.
class _StubShell extends Node:
	var strings_calls: Array = []
	var font_calls: Array = []
	var menu_calls: Array = []
	func open_strings_workspace(path: String, key: String) -> int:
		strings_calls.append([path, key])
		return OK
	func open_font_workspace(font: String) -> int:
		font_calls.append(font)
		return OK
	func open_menu_workspace(file: String, screen: String) -> int:
		menu_calls.append([file, screen])
		return OK
	func show_status_message(_text: String, _duration := 4.0, _severity: StringName = &"info") -> void:
		pass


# Stub editor exposing only the resolved-table path the adapter reads for the jump.
class _StubEditor extends Control:
	var path: String = ""
	func get_text_resource_path() -> String:
		return path
	func get_text_resource():
		return null


func test_strings_open_table_focuses_key() -> void:
	var ws = autofree(StringsWorkspaceScript.new())
	ws._ensure_editor()
	add_child_autofree(ws.strings_editor)
	var sec: int = ws.strings_editor.add_section("default")
	ws.strings_editor.add_entry("ALPHA", "Alpha", sec, Vector2i())
	var idx: int = ws.strings_editor.add_entry("BRAVO", "Bravo", sec, Vector2i())
	# Empty path = focus only (the table is already loaded in-memory here).
	assert_eq(ws.open_strings_table("", "BRAVO"), OK, "Focusing an in-memory table returns OK.")
	assert_eq(ws._search, "BRAVO", "The Strings table view is filtered to the key.")
	assert_eq(ws.strings_editor.selected_index, idx, "The key's entry is selected.")


func test_mnu_adapter_routes_string_jump_to_shell() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	var ed := _StubEditor.new()
	ed.path = "C:/assets/menutxt.bin"
	add_child_autofree(ed)
	ws._editor = ed
	var shell := _StubShell.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	ws._on_string_jump("ALPHA")
	assert_eq(shell.strings_calls.size(), 1, "The jump reaches the shell exactly once.")
	if shell.strings_calls.size() == 1:
		assert_eq(shell.strings_calls[0], ["C:/assets/menutxt.bin", "ALPHA"],
			"The jump carries the resolved table path + the string id.")


func test_mnu_adapter_string_jump_noops_without_table() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	var ed := _StubEditor.new()  # path stays empty -> no resolved table
	add_child_autofree(ed)
	ws._editor = ed
	var shell := _StubShell.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	ws._on_string_jump("ALPHA")
	assert_eq(shell.strings_calls.size(), 0,
		"With no resolved table, the jump no-ops (no shell call).")


func test_mnu_adapter_routes_font_jump_to_shell() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	var shell := _StubShell.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	ws._on_font_jump("Gunpl22b.fnt")
	assert_eq(shell.font_calls, ["Gunpl22b.fnt"],
		"The font jump reaches the shell with the font name.")


func test_mnu_adapter_routes_menu_jump_to_shell() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	var shell := _StubShell.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	ws._on_menu_jump("sp.mnu", "SINGLE_PLAYER")
	assert_eq(shell.menu_calls, [["sp.mnu", "SINGLE_PLAYER"]],
		"The menu jump reaches the shell with the target menu and screen.")


# --- MCP seams: add_widgets_batch / named add_screen_action / save_as_path -----
# The menu MCP tools drive the editor through these; they reuse the snapshot
# undo machinery so agent batches behave like single gestures.

func test_add_widgets_batch_is_one_undo_step_with_props() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var before_children := doc.get_child_ids(root).size()

	var results: Array = ed.add_widgets_batch([
		{ "parent": root, "type": NovaMnuDocument.TYPE_BUTTON, "rect": Rect2(10, 10, 100, 24),
			"props": { "name": "BatchBtn", "text": "Press" } },
		{ "parent": root, "type": NovaMnuDocument.TYPE_STATIC, "rect": Rect2(10, 40, 100, 24),
			"props": { "name": "BatchLabel" } },
		{ "parent": 999999, "type": NovaMnuDocument.TYPE_BUTTON, "rect": Rect2(0, 0, 10, 10) },
	])
	assert_eq(results.size(), 3)
	assert_true(bool(results[0]["ok"]))
	assert_true(bool(results[1]["ok"]))
	assert_false(bool(results[2]["ok"]), "A bad parent row reports ok=false without aborting the batch.")
	assert_eq(doc.get_child_ids(root).size(), before_children + 2)
	var btn := int(results[0]["id"])
	assert_eq(doc.get_widget_name(btn), "BatchBtn", "Initial props applied.")
	assert_eq(doc.get_widget_text(btn), "Press")
	assert_almost_eq(doc.get_window_rect(btn).position.x, 10.0, 0.01)
	assert_eq(ed.get_selected_id(), int(results[1]["id"]), "The last added widget is selected.")

	ed.undo()
	assert_false(doc.widget_exists(btn), "ONE undo removes the whole batch (props included).")
	assert_eq(doc.get_child_ids(root).size(), before_children)
	ed.redo()
	assert_true(doc.widget_exists(btn), "Redo restores the batch with the same ids.")
	await get_tree().process_frame


func test_add_screen_action_accepts_a_custom_name() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var named: int = ed.add_screen_action("CUSTOM_SCREEN")
	assert_gt(named, 0)
	assert_eq(doc.get_screen_name(named), "CUSTOM_SCREEN")
	var defaulted: int = ed.add_screen_action()
	assert_gt(defaulted, 0)
	assert_true(doc.get_screen_name(defaulted).begins_with("SCREEN"), "No name keeps the unique default.")
	ed.undo()
	assert_false(doc.widget_exists(defaulted), "Each add is one undo step.")
	assert_true(doc.widget_exists(named))
	await get_tree().process_frame


func test_document_save_as_path_adopts_path_and_validates() -> void:
	var editordoc = MnuEditorDocumentScript.new()
	editordoc.open_mnu(FIXTURE)
	assert_eq(editordoc.save_as_path("wrong.txt"), ERR_INVALID_PARAMETER, "Extension must match the document type.")
	var dir := OS.get_cache_dir().path_join("opennova_mnu_seam_test")
	var path := dir.path_join("renamed_menu.mnu")
	editordoc.mark_dirty()
	assert_eq(editordoc.save_as_path(path), OK)
	assert_true(FileAccess.file_exists(path), "The file lands exactly where named.")
	assert_eq(editordoc.current_path, path, "The path is adopted as current.")
	assert_false(editordoc.is_dirty, "The save marks the document clean.")
	var reloaded := NovaMnuDocument.new()
	assert_eq(reloaded.load_from_bytes(FileAccess.get_file_as_bytes(path)), OK, "The saved menu round-trips.")
	DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)


func test_add_screen_creates_a_game_shaped_root() -> void:
	# The original engine crashes on a screen whose root lacks a full POSITION
	# and an APPEARANCE row; shipped roots are always named MAIN.
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var sid: int = ed.add_screen_action("SHAPED")
	var root := doc.get_screen_root_id(sid)
	assert_eq(doc.get_widget_name(root), "MAIN", "Shipped screens always name the root MAIN.")
	var rect := doc.get_window_rect(root)
	assert_eq(rect, Rect2(0, 0, 800, 600), "Full-canvas root position.")
	assert_eq(doc.get_window_rect_flags(root), 15, "All four POSITION corners explicit.")
	var apps: Array = doc.get_widget_appearances(root)
	assert_eq(apps.size(), 1, "One appearance row on the fresh root.")
	assert_eq(String(apps[0]["type"]), "custom", "Engine-backdrop row by default.")
	assert_eq(String(apps[0]["state"]), "default")
	await get_tree().process_frame


func test_appearances_and_frame_round_trip_through_apply_edit() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var results: Array = ed.add_widgets_batch([
		{ "parent": root, "type": NovaMnuDocument.TYPE_BUTTON, "rect": Rect2(10, 10, 100, 24),
			"props": { "name": "AppBtn", "appearances": [
				{ "state": "default" }, { "state": "mouseover" },
			] } },
	])
	var btn := int(results[0]["id"])
	var rows: Array = doc.get_widget_appearances(btn)
	assert_eq(rows.size(), 2, "Batch props can seed appearance rows.")
	assert_eq(String(rows[0]["state"]), "default")
	assert_eq(String(rows[0]["type"]), "", "Empty-state rows carry no type (the shipped text-button shape).")

	ed.apply_edit({ "target": "widget", "id": btn, "prop": "appearances", "value": [
		{ "state": "default", "type": "image", "value": "btn5.tga", "map_state": 0, "height": 24 },
	] })
	rows = doc.get_widget_appearances(btn)
	assert_eq(rows.size(), 1, "apply_edit replaces the whole row list.")
	assert_eq(String(rows[0]["value"]), "btn5.tga")
	assert_eq(int(rows[0]["map_state"]), 0)
	ed.undo()
	assert_eq((doc.get_widget_appearances(btn) as Array).size(), 2, "Appearances undo per edit.")

	# Use a fresh screen's root — add_screen makes one with NO frame, so the undo
	# target is unambiguous (the fixture root already ships its own frame).
	var fresh := int(ed.add_screen_action("FRAMED"))
	var fresh_root := int(doc.get_screen_root_id(fresh))
	assert_eq(String(doc.get_window_frame(fresh_root)["stencil"]), "", "Fresh roots have no frame.")
	ed.apply_edit({ "target": "widget", "id": fresh_root, "prop": "frame", "value": {
		"stencil": "BORDER2.tga", "stencil_size": 32, "brush": "BOXTILE.tga", "monogram": "MONOGRAM.tga",
	} })
	var frame: Dictionary = doc.get_window_frame(fresh_root)
	assert_eq(String(frame["stencil"]), "BORDER2.tga")
	assert_eq(int(frame["stencil_size"]), 32)
	assert_eq(String(frame["brush"]), "BOXTILE.tga")
	ed.undo()
	assert_eq(String(doc.get_window_frame(fresh_root)["stencil"]), "", "Frame undoes.")
	await get_tree().process_frame


func test_auto_size_rects_omit_extents_on_disk() -> void:
	# Shipped box-art toggles omit RIGHT (the engine stretches art across an
	# explicit width); a negative rect extent authors that shape.
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var editordoc = pair[1]
	var doc: NovaMnuDocument = editordoc.resource
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var results: Array = ed.add_widgets_batch([
		{ "parent": root, "type": NovaMnuDocument.TYPE_CHECKBOX, "rect": Rect2(20, 20, -1, 25),
			"props": { "name": "AutoChk" } },
	])
	var chk := int(results[0]["id"])
	var flags := doc.get_window_rect_flags(chk)
	assert_eq(flags & NovaMnuDocument.RECT_HAS_RIGHT, 0, "Auto width leaves RIGHT unset.")
	assert_ne(flags & NovaMnuDocument.RECT_HAS_BOTTOM, 0, "Explicit height keeps BOTTOM.")

	var dir := OS.get_cache_dir().path_join("opennova_mnu_autosize_test")
	var path := dir.path_join("autosize.mnu")
	editordoc.mark_dirty()
	assert_eq(editordoc.save_as_path(path), OK)
	var text := FileAccess.get_file_as_string(path)
	var at := text.find("AutoChk")
	assert_gt(at, 0)
	# Slice exactly AutoChk's own element (to its closing tag) — a wider window
	# would spill into the next screen's root, which legitimately has RIGHT.
	var block := text.substr(at, text.find("</WINDOW>", at) - at)
	assert_false(block.contains("<RIGHT>"), "The writer omits RIGHT for auto width.")
	assert_true(block.contains("<BOTTOM>"), "Explicit height still writes BOTTOM.")
	DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)
	await get_tree().process_frame


func test_reopening_a_clean_tab_reloads_from_disk() -> void:
	# An externally rewritten file (another tool, a hand edit) must not be
	# shadowed by a stale clean tab; unsaved edits still win.
	var ws = autofree(MnuWorkspaceScript.new())
	var mount := Control.new()
	mount.size = Vector2(800, 600)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var dir := OS.get_cache_dir().path_join("opennova_mnu_reload_test")
	DirAccess.make_dir_recursive_absolute(dir)
	var path := dir.path_join("reload_probe.mnu")
	var v1 := "<SCREEN><NAME>ONE</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION></WINDOW></SCREEN>"
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(v1)
	f.close()
	assert_eq(int(ws.open_file(path)), OK)
	var doc = ws.get("_document")
	assert_eq(String(doc.resource.get_screen_name(doc.resource.get_screen_ids()[0])), "ONE")

	# Rewrite on disk; reopening the CLEAN tab picks up the new content.
	f = FileAccess.open(path, FileAccess.WRITE)
	f.store_string(v1.replace("ONE", "TWO"))
	f.close()
	assert_eq(int(ws.open_file(path)), OK)
	doc = ws.get("_document")
	assert_eq(String(doc.resource.get_screen_name(doc.resource.get_screen_ids()[0])), "TWO",
			"A clean tab reloads from disk.")

	# Dirty the tab; another reopen keeps the unsaved edits.
	var ed = ws.get_editor_document()
	ed.apply_edit({ "target": "screen", "id": doc.resource.get_screen_ids()[0], "prop": "name", "value": "EDITED" })
	assert_true(bool(doc.is_dirty))
	f = FileAccess.open(path, FileAccess.WRITE)
	f.store_string(v1.replace("ONE", "THREE"))
	f.close()
	assert_eq(int(ws.open_file(path)), OK)
	doc = ws.get("_document")
	assert_eq(String(doc.resource.get_screen_name(doc.resource.get_screen_ids()[0])), "EDITED",
			"Unsaved edits win over the on-disk rewrite.")
	DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)
	ws.release_viewport()
	await get_tree().process_frame


func test_editor_copy_paste_and_duplicate_preserve_subtrees_one_undo_each() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var doc: NovaMnuDocument = pair[1].resource
	var root := int(doc.get_screen_root_id(doc.get_screen_ids()[0]))
	var start := _find_widget_id(doc, root, "StartBtn")
	var nested := int(doc.add_widget(start, NovaMnuDocument.TYPE_STATIC,
		Rect2(2, 3, 20, 10)))
	doc.set_widget_name(nested, "Nested")
	doc.set_widget_text(nested, "Child")
	ed.restore_history({})
	ed.select_widget(start)
	var original_count := doc.get_child_ids(root).size()
	assert_true(ed.copy_selection_action())
	var pasted: int = ed.paste_selection_action()
	assert_gt(pasted, 0)
	assert_eq(doc.get_child_ids(root).size(), original_count + 1)
	assert_eq(String(doc.get_widget_name(pasted)), "StartBtn")
	assert_eq(doc.get_child_ids(pasted).size(), 1, "paste preserves the subtree")
	assert_eq(String(doc.get_widget_name(doc.get_child_ids(pasted)[0])), "Nested")
	assert_eq(doc.get_widget_actions(pasted).size(), 1, "actions survive clipboard capture")
	ed.undo()
	assert_false(doc.widget_exists(pasted), "one undo removes the pasted subtree")
	assert_false(ed.can_undo(), "paste contributed exactly one undo entry")

	ed.select_widget(start)
	var duplicated: int = ed.duplicate_selection_action()
	assert_gt(duplicated, 0)
	assert_eq(doc.get_child_ids(duplicated).size(), 1)
	assert_ne(duplicated, start, "duplicate receives fresh document ids")
	ed.undo()
	assert_false(doc.widget_exists(duplicated), "one undo removes the duplicate")
	assert_false(ed.can_undo(), "duplicate contributed exactly one undo entry")


func test_editor_align_uses_rendered_auto_extent_and_preserves_auto_width() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var doc: NovaMnuDocument = pair[1].resource
	var root := int(doc.get_screen_root_id(doc.get_screen_ids()[0]))
	var auto := int(doc.add_widget(root, NovaMnuDocument.TYPE_STATIC,
		Rect2(30, 300, -1, 24)))
	doc.set_widget_name(auto, "AutoLabel")
	doc.set_widget_text(auto, "A rendered auto-sized label")
	var fixed := int(doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON,
		Rect2(360, 300, 80, 24)))
	doc.set_widget_name(fixed, "RightEdge")
	await get_tree().process_frame
	await get_tree().process_frame
	var rendered_before: Rect2 = ed.get_rendered_widget_rect(auto)
	assert_gt(rendered_before.size.x, 0.0, "the auto widget has a live rendered width")
	var fixed_before: Rect2 = ed.get_rendered_widget_rect(fixed)
	var expected_right := maxf(rendered_before.end.x, fixed_before.end.x)
	var original_x := doc.get_window_rect(auto).position.x
	ed.restore_history({})
	ed.select_widgets(PackedInt32Array([auto, fixed]))
	ed.align_selection("right")
	await get_tree().process_frame
	await get_tree().process_frame
	var rendered_after: Rect2 = ed.get_rendered_widget_rect(auto)
	assert_almost_eq(rendered_after.end.x, expected_right, 0.75,
		"right alignment uses the rendered auto extent")
	assert_eq(doc.get_window_rect_flags(auto) & NovaMnuDocument.RECT_HAS_RIGHT, 0,
		"alignment keeps RIGHT omitted for auto width")
	ed.undo()
	assert_almost_eq(doc.get_window_rect(auto).position.x, original_x, 0.01)
	assert_false(ed.can_undo(), "alignment is one undo entry")


func test_editor_group_z_order_preserves_relative_order_and_one_undo() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var doc: NovaMnuDocument = pair[1].resource
	var root := int(doc.get_screen_root_id(doc.get_screen_ids()[0]))
	var before: PackedInt32Array = doc.get_child_ids(root)
	assert_gt(before.size(), 4)
	var selected := PackedInt32Array([before[0], before[2]])
	var expected := PackedInt32Array()
	for id in before:
		if not selected.has(id):
			expected.append(id)
	for id in before:
		if selected.has(id):
			expected.append(id)
	ed.select_widgets(selected)
	ed.change_z_order("front")
	assert_eq(doc.get_child_ids(root), expected,
		"front moves the selected group without reversing it")
	ed.undo()
	assert_eq(doc.get_child_ids(root), before)
	assert_false(ed.can_undo(), "group z-order is one undo entry")


func test_editor_duplicate_and_reorder_screen_are_undoable() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var doc: NovaMnuDocument = pair[1].resource
	var first := int(doc.get_screen_ids()[0])
	ed.select_widget(first)
	var copied: int = ed.duplicate_screen_action()
	assert_gt(copied, 0)
	assert_eq(doc.get_screen_count(), 3)
	assert_eq(String(doc.get_screen_name(copied)), "MAIN_COPY")
	assert_eq(doc.get_child_ids(doc.get_screen_root_id(copied)).size(),
		doc.get_child_ids(doc.get_screen_root_id(first)).size(),
		"screen duplicate preserves the complete root subtree")
	ed.undo()
	assert_eq(doc.get_screen_count(), 2)
	assert_false(ed.can_undo(), "screen duplicate is one undo entry")

	ed.select_widget(first)
	ed.move_screen_action(1)
	assert_eq(int(doc.get_screen_ids()[1]), first)
	ed.undo()
	assert_eq(int(doc.get_screen_ids()[0]), first)
	assert_false(ed.can_undo(), "screen reorder is one undo entry")


func test_musicvar_presence_zero_is_undoable_without_destroying_value() -> void:
	var pair = await _editor_with_fixture()
	var ed = pair[0]
	var doc: NovaMnuDocument = pair[1].resource
	var sid := int(doc.get_screen_ids()[0])
	doc.set_screen_property(sid, "music_var", 0)
	ed.restore_history({})
	ed.apply_edit({"target": "screen", "id": sid,
		"prop": "has_music_var", "value": false})
	assert_false(doc.get_screen_has_music_var(sid))
	assert_eq(doc.get_screen_music_var(sid), 0, "the explicit zero remains latent")
	assert_false(doc.to_byte_array().get_string_from_utf8().contains("<MUSICVAR>"))
	ed.undo()
	assert_true(doc.get_screen_has_music_var(sid))
	assert_eq(doc.get_screen_music_var(sid), 0)
	assert_true(doc.to_byte_array().get_string_from_utf8().contains(
		"<MUSICVAR>0</MUSICVAR>"))
	assert_false(ed.can_undo(), "presence toggle is one undo entry")


func test_interactive_lock_blocks_menu_inspector_and_styles_mutations() -> void:
	var ws = MnuWorkspaceScript.new()
	var mount := Control.new()
	mount.size = Vector2(800, 600)
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	assert_eq(ws.open_file(FIXTURE), OK)
	var dock := Control.new()
	add_child_autofree(dock)
	ws.build_inspector(dock)
	await get_tree().process_frame
	var ed = ws.get_editor_document()
	var doc: NovaMnuDocument = ws.get_menu_resource()
	var start := _find_widget_id(doc,
		doc.get_screen_root_id(doc.get_screen_ids()[0]), "StartBtn")
	ed.select_widget(start)
	assert_true(ed.copy_selection_action(), "seed the clipboard before preview")
	var before_name := String(doc.get_widget_name(start))
	var style_before := String(ws.get_stylesheet_resource().get_source_text())

	ed.set_interactive(true)
	assert_true(ed.is_interactive())
	assert_false(ed.can_undo())
	assert_false(ed.can_redo())
	ed.apply_edit({"target": "widget", "id": start,
		"prop": "name", "value": "Blocked"})
	assert_eq(String(doc.get_widget_name(start)), before_name)
	assert_eq(ed.paste_selection_action(), -1)
	assert_eq(ed.duplicate_selection_action(), -1)
	ws.apply_stylesheet_edit(
		{"op": "add", "name": "BLOCKED_INSPECTOR", "value": "00FF00"})
	assert_eq(String(ws.get_stylesheet_resource().get_source_text()), style_before,
		"the Styles authoring funnel is locked")
	var inspector_line := _first_line_edit(dock)
	assert_not_null(inspector_line)
	if inspector_line != null:
		assert_false(inspector_line.editable, "menu inspector controls are locked")

	ed.set_interactive(false)
	assert_false(ed.is_interactive())
	ed.apply_edit({"target": "widget", "id": start,
		"prop": "name", "value": "Unlocked"})
	assert_eq(String(doc.get_widget_name(start)), "Unlocked",
		"leaving preview restores authoring")
	ws.apply_stylesheet_edit(
		{"op": "add", "name": "UNLOCKED_STYLE", "value": "FF00FF"})
	assert_true(ws.get_stylesheet_resource().has_variable("UNLOCKED_STYLE"))
	ws.release_viewport()
