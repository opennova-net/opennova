extends GutTest

# M6 gate: the Menus ONED workspace. Covers the editor document lifecycle, the
# widget tree, the read-only property inspector, the edit_mode preview canvas
# (single-screen visibility + letterbox fit), the editor selection wiring, and
# the workspace adapter (actions + inspector population).

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuWorkspaceScript = preload("res://modtools/mnu/mnu_workspace.gd")
const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
const TEMP_DIR := "user://test_mnu_workspace"


func before_each() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEMP_DIR))


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)


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


# Concatenate every Label.text under a node (read-only inspector verification).
func _collect_label_text(node: Node) -> String:
	var out := ""
	if node is Label:
		out += (node as Label).text + "\n"
	for child in node.get_children():
		out += _collect_label_text(child)
	return out


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
	assert_not_null(preview, "Canvas hosts a NovaMnuMenu preview.")
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

	# Uniform letterbox fit: min(320/640, 240/480) == 0.5.
	assert_almost_eq(preview.scale.x, 0.5, 0.01, "Preview scales to fit the canvas (letterbox).")

	canvas.show_screen_named("OPTIONS")
	var options_visible := false
	for child in preview.get_children():
		if child is NovaMnuScreen and child.get_screen_name() == "OPTIONS":
			options_visible = child.visible
	assert_true(options_visible, "Switching to OPTIONS makes it the visible screen.")


func test_property_inspector_shows_widget_and_screen() -> void:
	var doc := _load_resource()
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame

	inspector.show_widget(doc, _first_root_child(doc, 1))  # StartBtn
	await get_tree().process_frame
	var widget_text := _collect_label_text(inspector)
	assert_string_contains(widget_text, "StartBtn", "Inspector shows the widget name.")
	assert_string_contains(widget_text, "btn_up.tga", "Inspector shows a texture slot.")

	inspector.show_widget(doc, _first_root_child(doc, 2))  # SoundChk (CHECKED)
	await get_tree().process_frame
	assert_string_contains(_collect_label_text(inspector), "Checked", "Inspector decodes the Checked flag.")

	inspector.show_widget(doc, doc.get_screen_ids()[0])  # screen container
	await get_tree().process_frame
	var screen_text := _collect_label_text(inspector)
	assert_string_contains(screen_text, "Screen", "Screen heading shown for a screen id.")
	assert_string_contains(screen_text, "MAIN", "Screen name shown.")
	assert_string_contains(screen_text, "menutxt.BIN", "Screen text resource shown.")


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
	assert_eq(ws.get_open_resource_kind(), "", "Empty kind -> native file dialog fallback.")

	assert_eq(ws.open_file(FIXTURE), OK, "Adapter opens a .mnu file.")
	assert_eq(ws.get_project_title(), "widgets", "Title shows the loaded basename.")
	assert_string_contains(ws.get_status_context(), "2 screen", "Status summarizes the screen count.")

	var host := Control.new()
	host.size = Vector2(800, 480)
	add_child_autofree(host)
	ws.mount_viewport(host)
	await get_tree().process_frame
	assert_gt(host.get_child_count(), 0, "Adapter mounts the editor in the viewport.")

	var inspector_host := Control.new()
	add_child_autofree(inspector_host)
	ws.build_inspector(inspector_host)
	await get_tree().process_frame
	assert_gt(inspector_host.get_child_count(), 0, "Adapter mounts the property inspector.")
	assert_string_contains(_collect_label_text(inspector_host), "MAIN",
		"Inspector shows the default-selected first screen.")

	var start_id := _first_root_child(ws._document.resource, 1)
	ws._on_widget_selected(start_id)
	await get_tree().process_frame
	assert_string_contains(_collect_label_text(inspector_host), "StartBtn",
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
	var host := Control.new()
	host.size = Vector2(800, 480)
	add_child_autofree(host)

	ws.mount_viewport(host)
	await get_tree().process_frame
	var ed = ws._editor
	assert_eq(host.get_child_count(), 1, "Editor mounts once.")

	ws.unmount_viewport(host)
	assert_eq(host.get_child_count(), 0, "Unmount removes the editor without freeing it.")

	ws.mount_viewport(host)
	await get_tree().process_frame
	assert_same(ws._editor, ed, "Remount reuses the same editor instance.")
	assert_eq(host.get_child_count(), 1, "Editor is re-parented exactly once.")
	assert_eq(ed.widget_selected.get_connections().size(), 1,
		"widget_selected stays connected once, not re-connected on remount.")


func test_adapter_release_viewport_tears_down() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var host := Control.new()
	host.size = Vector2(800, 480)
	add_child_autofree(host)
	var inspector_host := Control.new()
	add_child_autofree(inspector_host)
	ws.mount_viewport(host)
	ws.build_inspector(inspector_host)
	await get_tree().process_frame

	ws.release_viewport()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(host.get_child_count(), 0, "release_viewport frees the editor.")
	assert_eq(inspector_host.get_child_count(), 0, "release_viewport frees the inspector.")
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

	var host := Control.new()
	host.size = Vector2(800, 480)
	add_child_autofree(host)
	ws.mount_viewport(host)
	await get_tree().process_frame
	# With no resource root the fixture's .tga textures cannot resolve.
	assert_gt(ws._editor.get_unresolved_asset_count(), 0,
		"Fixture textures are unresolved without a resource root.")
	assert_string_contains(ws.get_status_context(), "unresolved asset",
		"Status surfaces the unresolved-asset count after mount.")


func test_inspector_resyncs_to_first_screen_after_reload() -> void:
	var ws = autofree(MnuWorkspaceScript.new())
	assert_eq(ws.open_file(FIXTURE), OK)
	var host := Control.new()
	host.size = Vector2(800, 480)
	add_child_autofree(host)
	var inspector_host := Control.new()
	add_child_autofree(inspector_host)
	ws.mount_viewport(host)
	ws.build_inspector(inspector_host)
	await get_tree().process_frame

	# Push a widget selection, then reload the document.
	ws._on_widget_selected(_first_root_child(ws._document.resource, 1))
	await get_tree().process_frame
	assert_string_contains(_collect_label_text(inspector_host), "StartBtn", "Inspector shows the selection.")

	assert_eq(ws._document.open_mnu(FIXTURE), OK)
	await get_tree().process_frame
	# The editor resets to the first screen on reload and pushes it to the inspector.
	assert_string_contains(_collect_label_text(inspector_host), "MAIN",
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
